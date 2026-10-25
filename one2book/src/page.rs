//! One OneNote page -> one infinite X0urnal++ page.
//!
//! UNITS. OneNote positions page elements in half-inch steps from the page origin
//! (`offset_horizontal/vertical`, MS-ONE 2.3.18/2.3.19) and draws ink in HIMETRIC, 0.01 mm
//! (`InkStroke::absolute_path`, already scaled by the stroke's InkScaling). Xournal++ uses points.
//! So: half-inch x 36 = pt, and HIMETRIC x 72/2540 = pt. This was checked by eye on a real page
//! (Data Structures, "TEST REVIEW"): ink drawn over PDF printouts lands on the printed lines.
//!
//! WHAT CARRIES OVER
//! - ink on the page                     -> strokes at their exact positions
//! - pictures and printout pages         -> images at their exact positions and sizes
//! - typed text, lists, tables           -> plain text blocks at the text box's position
//! - pictures/ink inside a text box      -> placed in the box's flow (OneNote stores no position)
//! WHAT DOES NOT (each one is counted and reported, never dropped silently)
//! - attachments (embedded files), bold/italic/mixed fonts within a paragraph, equations
//!   as typeset maths, note tags, hyperlinks

use crate::colors;
use crate::images::{self, Kind, MetafileRenderer};
use crate::xopp::{Element, Page as XPage, Tool};
use onenote_parser::contents::{EmbeddedObject, Image, Ink, Outline, OutlineElement, OutlineItem, RichText, Table};
use onenote_parser::page::Page;
use std::collections::BTreeMap;
use std::io::Read;

const HALF_INCH: f64 = 36.0;
const HIMETRIC: f64 = 72.0 / 2540.0;

/// Xournal++'s A4 page, the size every new X0urnal++ page starts at; infinite pages grow from
/// it, so a converted page is never smaller than this.
const A4_W: f64 = 595.275590551181;
const A4_H: f64 = 841.889763779528;

/// Room left past the last mark on the right and bottom, so the page doesn't end flush against
/// the ink (X0urnal++ grows the page as you write near the edge anyway).
const MARGIN: f64 = 72.0;

/// OneNote's default text: Calibri 11 pt. Pango falls back to the system sans if Calibri is
/// missing on the machine that opens the file.
const DEFAULT_FONT: &str = "Calibri";
const DEFAULT_SIZE: f64 = 11.0;
/// OneNote's page-title style is 20 pt; the file only stores a size when it was changed.
const TITLE_SIZE: f64 = 20.0;

/// A text box with no stored width wraps at OneNote's default outline width, about 6.5 in.
const DEFAULT_BOX_WIDTH: f64 = 6.5 * 72.0;
/// One indent step for nested outline elements and list items.
const INDENT: f64 = 18.0;

/// What one page (or a whole section, when summed) turned into -- the verification counts.
#[derive(Default, Clone)]
pub struct Stats {
    pub pages: usize,
    pub strokes: usize,
    pub points: usize,
    pub images: usize,
    pub texts: usize,
    /// things that could not be carried over, by kind -> count
    pub skipped: BTreeMap<String, usize>,
    /// things carried over only approximately, by kind -> count
    pub approximate: BTreeMap<String, usize>,
}

impl Stats {
    pub fn add(&mut self, o: &Stats) {
        self.pages += o.pages;
        self.strokes += o.strokes;
        self.points += o.points;
        self.images += o.images;
        self.texts += o.texts;
        for (k, v) in &o.skipped {
            *self.skipped.entry(k.clone()).or_default() += v;
        }
        for (k, v) in &o.approximate {
            *self.approximate.entry(k.clone()).or_default() += v;
        }
    }
    fn skip(&mut self, what: &str) {
        *self.skipped.entry(what.to_string()).or_default() += 1;
    }
    fn approx(&mut self, what: &str) {
        *self.approximate.entry(what.to_string()).or_default() += 1;
    }
}

struct Ctx<'a> {
    els: Vec<Element>,
    stats: Stats,
    meta: &'a mut MetafileRenderer,
    /// the size text takes when OneNote stored none (bigger for the page title)
    default_size: f64,
}

/// Convert one page.
pub fn convert(page: &Page, meta: &mut MetafileRenderer) -> (XPage, Stats) {
    let mut cx = Ctx { els: Vec::new(), stats: Stats { pages: 1, ..Stats::default() }, meta, default_size: DEFAULT_SIZE };

    // The title area holds several outlines -- the title itself, then the date and the time --
    // usually without positions of their own. OneNote stacks them, so they flow downward from
    // the title's position, each starting below the one before.
    if let Some(title) = page.title() {
        let tx = title.offset_horizontal() as f64 * HALF_INCH;
        let mut ty = title.offset_vertical() as f64 * HALF_INCH;
        for (i, outline) in title.contents().iter().enumerate() {
            let x = outline.offset_horizontal().map(|v| v as f64 * HALF_INCH).unwrap_or(tx);
            let y = outline.offset_vertical().map(|v| v as f64 * HALF_INCH).unwrap_or(ty).max(ty);
            // the first outline is the title itself; the date and time under it are normal size
            cx.default_size = if i == 0 { TITLE_SIZE } else { DEFAULT_SIZE };
            ty = layout_outline(&mut cx, outline, x, y);
        }
        cx.default_size = DEFAULT_SIZE;
    }

    for pc in page.contents() {
        if let Some(outline) = pc.outline() {
            let ox = outline.offset_horizontal().unwrap_or(0.0) as f64 * HALF_INCH;
            let oy = outline.offset_vertical().unwrap_or(0.0) as f64 * HALF_INCH;
            layout_outline(&mut cx, outline, ox, oy);
        } else if let Some(img) = pc.image() {
            let x = img.offset_horizontal().unwrap_or(0.0) as f64 * HALF_INCH;
            let y = img.offset_vertical().unwrap_or(0.0) as f64 * HALF_INCH;
            place_image(&mut cx, img, x, y);
        } else if let Some(ink) = pc.ink() {
            ink_strokes(&mut cx, ink, 0.0, 0.0);
        } else if pc.embedded_file().is_some() {
            cx.stats.skip("attachment");
        } else {
            cx.stats.skip("unknown page element");
        }
    }

    // Paint order: images at the bottom (printouts are paper you write on), then text, then ink.
    cx.els.sort_by_key(|e| match e {
        Element::Image { .. } => 0,
        Element::Text { .. } => 1,
        Element::Stroke { .. } => 2,
    });

    // X0urnal++ pages start at 0,0 and only grow right/down, and nothing at a negative
    // coordinate is shown -- but OneNote content can sit left of or above its origin (a picture
    // at x = -1.3 half-inches in "arrays"). Shift the whole page so everything is on it.
    let mut b = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
    for e in &cx.els {
        if let Some((x0, y0, x1, y1)) = e.bounds() {
            b = (b.0.min(x0), b.1.min(y0), b.2.max(x1), b.3.max(y1));
        }
    }
    let dx = if b.0 < 0.0 { -b.0 + HALF_INCH } else { 0.0 };
    let dy = if b.1 < 0.0 { -b.1 + HALF_INCH } else { 0.0 };
    if dx != 0.0 || dy != 0.0 {
        for e in cx.els.iter_mut() {
            e.translate(dx, dy);
        }
    }

    let page = XPage {
        width: (b.2 + dx + MARGIN).max(A4_W),
        height: (b.3 + dy + MARGIN).max(A4_H),
        background: colors::VOID,
        elements: cx.els,
    };
    (page, cx.stats)
}

/// Ink strokes from one ink object (and its nested groups), offset by its container's origin.
fn ink_strokes(cx: &mut Ctx, ink: &Ink, ox: f64, oy: f64) {
    let gx = ox + ink.offset_horizontal().unwrap_or(0.0) as f64 * HALF_INCH;
    let gy = oy + ink.offset_vertical().unwrap_or(0.0) as f64 * HALF_INCH;
    for s in ink.ink_strokes() {
        let mut points: Vec<(f64, f64)> =
            s.absolute_path().iter().map(|p| (gx + p.x() as f64 * HIMETRIC, gy + p.y() as f64 * HIMETRIC)).collect();
        if points.is_empty() {
            continue;
        }
        if points.len() == 1 {
            // a dot: Xournal++ refuses a one-point stroke (and the whole file with it)
            points.push(points[0]);
        }
        // A highlighter is the one pen OneNote stores as see-through (transparency 127 on the
        // chisel tip in the notebooks tested); 0 and 255 both mean an ordinary opaque pen.
        let tool = match s.transparency() {
            Some(t) if t > 0 && t < 255 => Tool::Highlighter,
            _ => Tool::Pen,
        };
        // Pen tip size: the parser exposes width and height (swapped relative to the stored
        // InkWidth/InkHeight); a round pen has both equal, a highlighter's chisel is long and thin
        // and draws as thick as its long side, so the larger of the two is the stroke width.
        let width = (s.width().max(s.height()) as f64 * HIMETRIC).max(0.2);
        let color = colors::ink(s.color());
        cx.stats.strokes += 1;
        cx.stats.points += points.len();
        cx.els.push(Element::Stroke { tool, color, width, points });
    }
    for child in ink.child_groups() {
        ink_strokes(cx, child, gx, gy);
    }
}

/// Bytes of an image, converted to something Xournal++ loads; None (and a count) if impossible.
fn image_bytes(cx: &mut Ctx, img: &Image) -> Option<Vec<u8>> {
    let mut data = Vec::new();
    img.read()?.read_to_end(&mut data).ok()?;
    match images::sniff(&data) {
        Kind::Png | Kind::Jpeg | Kind::Gif | Kind::Bmp => Some(data),
        k @ (Kind::Emf | Kind::Wmf) => {
            let png = cx.meta.to_png(&data, k);
            if png.is_none() {
                cx.stats.skip("metafile image that could not be rendered");
            }
            png
        }
        Kind::Unknown => {
            cx.stats.skip("image in an unknown format");
            None
        }
    }
}

/// The size OneNote shows an image at, in points.
fn image_size(img: &Image, data: &[u8]) -> (f64, f64) {
    let w = img.picture_width().or(img.layout_max_width());
    let h = img.picture_height().or(img.layout_max_height());
    match (w, h) {
        (Some(w), Some(h)) => (w as f64 * HALF_INCH, h as f64 * HALF_INCH),
        // no stored size: show it at 96 dpi, the way OneNote sizes a pasted picture
        _ => images::png_size(data).map(|(pw, ph)| (pw as f64 * 0.75, ph as f64 * 0.75)).unwrap_or((144.0, 144.0)),
    }
}

/// An image whose top-left corner is at (x, y). Returns its height, for flowing text below it.
fn place_image(cx: &mut Ctx, img: &Image, x: f64, y: f64) -> f64 {
    let Some(data) = image_bytes(cx, img) else { return 0.0 };
    let (w, h) = image_size(img, &data);
    cx.stats.images += 1;
    cx.els.push(Element::Image { left: x, top: y, right: x + w, bottom: y + h, data });
    h
}

/// Lay out a text box (outline) whose top-left corner is at (x, y), in reading order.
/// Returns where the box ends vertically.
fn layout_outline(cx: &mut Ctx, outline: &Outline, x: f64, y: f64) -> f64 {
    let width = outline.layout_max_width().map(|w| w as f64 * HALF_INCH).unwrap_or(DEFAULT_BOX_WIDTH);
    let mut cursor = y;
    layout_items(cx, outline.items(), x, width, 0, &mut cursor);
    cursor
}

fn layout_items(cx: &mut Ctx, items: &[OutlineItem], x: f64, width: f64, depth: usize, cursor: &mut f64) {
    for item in items {
        match item {
            OutlineItem::Group(g) => layout_items(cx, g.outlines(), x, width, depth + 1, cursor),
            OutlineItem::Element(e) => {
                layout_element(cx, e, x, width, depth, cursor);
                layout_items(cx, e.children(), x, width, depth + 1, cursor);
            }
        }
    }
}

fn layout_element(cx: &mut Ctx, e: &OutlineElement, x: f64, width: f64, depth: usize, cursor: &mut f64) {
    let ix = x + depth as f64 * INDENT;
    let bullet = !e.list_contents().is_empty();
    for c in e.contents() {
        if let Some(rt) = c.rich_text() {
            text_block(cx, rt, ix, width - depth as f64 * INDENT, bullet, cursor);
            // handwriting written inline in a typed paragraph: OneNote keeps no position for it
            for obj in rt.embedded_objects() {
                if let EmbeddedObject::Ink(container) = obj {
                    cx.stats.approx("handwriting inside typed text");
                    let before = cx.els.len();
                    ink_strokes(cx, container.ink(), ix, *cursor);
                    *cursor += drawn_height(&cx.els[before..], *cursor);
                }
            }
        } else if let Some(img) = c.image() {
            cx.stats.approx("picture inside a text box");
            *cursor += place_image(cx, img, ix, *cursor) + 4.0;
        } else if let Some(ink) = c.ink() {
            cx.stats.approx("drawing inside a text box");
            let before = cx.els.len();
            ink_strokes(cx, ink, ix, *cursor);
            *cursor += drawn_height(&cx.els[before..], *cursor);
        } else if let Some(table) = c.table() {
            cx.stats.approx("table (as text rows)");
            table_rows(cx, table, ix, cursor);
        } else if c.embedded_file().is_some() {
            cx.stats.skip("attachment");
        } else {
            cx.stats.skip("unknown text-box content");
        }
    }
}

/// How far below `top` the given elements reach (for advancing the flow past inline ink).
fn drawn_height(els: &[Element], top: f64) -> f64 {
    els.iter().filter_map(|e| e.bounds()).map(|b| b.3 - top).fold(0.0, f64::max) + 4.0
}

/// One paragraph as one Xournal++ text element. Xournal++ text has one font per element and does
/// not wrap, so the paragraph takes its first run's style and is broken into lines at roughly the
/// text box's width.
fn text_block(cx: &mut Ctx, rt: &RichText, x: f64, width: f64, bullet: bool, cursor: &mut f64) {
    let style = rt.text_run_formatting().first().unwrap_or(rt.paragraph_style());
    let size = style.font_size().map(|s| s as f64 / 2.0).unwrap_or(cx.default_size);
    let font = style.font().unwrap_or(DEFAULT_FONT).to_string();
    let color = colors::text(style.font_color());
    let raw = rt.text().trim_end_matches(['\r', '\n']);
    let line_h = size * 1.2;
    if raw.trim().is_empty() {
        *cursor += line_h; // an empty paragraph is still a blank line in OneNote
        return;
    }
    if rt.text_run_formatting().len() > 1 {
        cx.stats.approx("mixed formatting in a paragraph (first run's style used)");
    }
    let text = if bullet { format!("\u{2022} {raw}") } else { raw.to_string() };
    let wrapped = wrap(&text, ((width / (size * 0.5)) as usize).max(10));
    let lines = wrapped.lines().count().max(1) as f64;
    cx.stats.texts += 1;
    cx.els.push(Element::Text { x, y: *cursor, font, size, color, text: wrapped });
    *cursor += lines * line_h + rt.paragraph_space_after() as f64;
}

/// Break text into lines of at most `max` characters at spaces (existing line breaks kept).
fn wrap(text: &str, max: usize) -> String {
    let mut out = String::new();
    for (i, para) in text.split(['\n', '\r', '\u{0b}']).enumerate() {
        if i > 0 {
            out.push('\n');
        }
        let mut len = 0;
        for word in para.split(' ') {
            let wl = word.chars().count();
            if len > 0 && len + 1 + wl > max {
                out.push('\n');
                len = 0;
            } else if len > 0 {
                out.push(' ');
                len += 1;
            }
            out.push_str(word);
            len += wl;
        }
    }
    out
}

/// A table as one text line per row, cells separated by " | ".
fn table_rows(cx: &mut Ctx, table: &Table, x: f64, cursor: &mut f64) {
    for row in table.contents() {
        let cells: Vec<String> = row
            .contents()
            .iter()
            .map(|cell| {
                cell.contents()
                    .iter()
                    .flat_map(|el| el.contents().iter().filter_map(|c| c.rich_text().map(|rt| rt.text().trim().to_string())))
                    .collect::<Vec<_>>()
                    .join(" ")
            })
            .collect();
        let line = cells.join(" | ");
        cx.stats.texts += 1;
        cx.els.push(Element::Text {
            x,
            y: *cursor,
            font: DEFAULT_FONT.to_string(),
            size: DEFAULT_SIZE,
            color: colors::BONE,
            text: line,
        });
        *cursor += DEFAULT_SIZE * 1.2;
    }
}
