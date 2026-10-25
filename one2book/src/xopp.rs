//! Writing one X0urnal++ page as a `.xopp` file (gzipped XML, Xournal++ fileversion 4).
//!
//! The element model here is deliberately small: images, strokes and plain text are the only
//! things a converted OneNote page needs. Coordinates are PostScript points (1/72 in), the unit
//! Xournal++ uses for everything on a page.
//!
//! Format facts this writer relies on (read from the pinned Xournal++ 1.3.7 SaveHandler /
//! LoadHandler and the X0urnal++ patch, see ../README.md "File format"):
//! - colours are `#RRGGBBAA`; the loader throws the alpha byte away, so opacity is the tool's job
//! - a stroke needs at least two points, or the WHOLE file fails to load
//! - `<page infinite="true">` is X0urnal++'s own flag; stock xournalpp ignores it and still
//!   opens every stroke
//! - `<image>` takes base64 of any gdk-pixbuf format and stretches it to left/top/right/bottom

use flate2::{Compression, GzBuilder};
use std::fmt::Write as _;
use std::io::Write as _;

/// An sRGB colour, no alpha (see the module note on why alpha is never written as anything but ff).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb(pub u8, pub u8, pub u8);

/// The Xournal++ tool a stroke belongs to. The highlighter draws at a fixed 0.47 opacity.
#[derive(Clone, Copy, Debug)]
pub enum Tool {
    Pen,
    Highlighter,
}

pub enum Element {
    /// A raster image stretched to the given rectangle. `data` is the encoded file (PNG/JPEG/...).
    Image { left: f64, top: f64, right: f64, bottom: f64, data: Vec<u8> },
    /// A constant-width stroke (OneNote stores no pen pressure, so there are no per-point widths).
    Stroke { tool: Tool, color: Rgb, width: f64, points: Vec<(f64, f64)> },
    /// Plain text; `x, y` is the top-left corner of the text block, lines split on '\n'.
    Text { x: f64, y: f64, font: String, size: f64, color: Rgb, text: String },
}

impl Element {
    /// The element's extent as (min_x, min_y, max_x, max_y), used to size and place the page.
    pub fn bounds(&self) -> Option<(f64, f64, f64, f64)> {
        match self {
            Element::Image { left, top, right, bottom, .. } => Some((*left, *top, *right, *bottom)),
            Element::Stroke { points, width, .. } => {
                let half = width / 2.0;
                let mut b = (f64::MAX, f64::MAX, f64::MIN, f64::MIN);
                for &(x, y) in points {
                    b = (b.0.min(x - half), b.1.min(y - half), b.2.max(x + half), b.3.max(y + half));
                }
                (!points.is_empty()).then_some(b)
            }
            Element::Text { x, y, size, text, .. } => {
                // A rough box: Xournal++ lays the text out with Pango at draw time, so the real
                // width is only known there. Half the font size per character is close enough to
                // keep the page big enough to hold it.
                let lines = text.lines().count().max(1) as f64;
                let longest = text.lines().map(|l| l.chars().count()).max().unwrap_or(0) as f64;
                Some((*x, *y, x + longest * size * 0.5, y + lines * size * 1.2))
            }
        }
    }

    /// Move the element by (dx, dy).
    pub fn translate(&mut self, dx: f64, dy: f64) {
        match self {
            Element::Image { left, top, right, bottom, .. } => {
                *left += dx;
                *right += dx;
                *top += dy;
                *bottom += dy;
            }
            Element::Stroke { points, .. } => {
                for p in points.iter_mut() {
                    p.0 += dx;
                    p.1 += dy;
                }
            }
            Element::Text { x, y, .. } => {
                *x += dx;
                *y += dy;
            }
        }
    }
}

/// One page. `background` is the solid paper colour; the grid style is graph paper, the style
/// X0urnal++ tiles past the page edge on infinite pages.
pub struct Page {
    pub width: f64,
    pub height: f64,
    pub background: Rgb,
    pub elements: Vec<Element>,
}

fn hex(c: Rgb) -> String {
    format!("#{:02x}{:02x}{:02x}ff", c.0, c.1, c.2)
}

/// XML-escape text content and attribute values.
pub fn escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            // XML 1.0 forbids most control characters; OneNote text occasionally carries them
            // (vertical tab as a soft line break, for one). Map the soft break to a newline and
            // drop the rest rather than write a file the loader rejects.
            '\u{0b}' => out.push('\n'),
            c if (c as u32) < 0x20 && c != '\n' && c != '\t' && c != '\r' => {}
            c => out.push(c),
        }
    }
    out
}

/// Serialise the page as a complete gzipped `.xopp` document.
pub fn write(page: &Page, creator: &str) -> std::io::Result<Vec<u8>> {
    use base64::Engine as _;
    let b64 = base64::engine::general_purpose::STANDARD;

    let mut xml = String::new();
    let _ = writeln!(xml, r#"<?xml version="1.0" standalone="no"?>"#);
    let _ = writeln!(xml, r#"<xournal creator="{}" fileversion="4">"#, escape(creator));
    let _ = writeln!(xml, "<title>Xournal++ document - see https://xournalpp.github.io/</title>");
    let _ = writeln!(xml, r#"<page width="{:.4}" height="{:.4}" infinite="true">"#, page.width, page.height);
    let _ = writeln!(xml, r#"<background type="solid" color="{}" style="graph"/>"#, hex(page.background));
    let _ = writeln!(xml, "<layer>");
    for el in &page.elements {
        match el {
            Element::Image { left, top, right, bottom, data } => {
                let _ = writeln!(
                    xml,
                    r#"<image left="{left:.4}" top="{top:.4}" right="{right:.4}" bottom="{bottom:.4}">{}</image>"#,
                    b64.encode(data)
                );
            }
            Element::Stroke { tool, color, width, points } => {
                let tool = match tool {
                    Tool::Pen => "pen",
                    Tool::Highlighter => "highlighter",
                };
                let _ = write!(xml, r#"<stroke tool="{tool}" color="{}" width="{width:.4}" capStyle="round">"#, hex(*color));
                for (i, (x, y)) in points.iter().enumerate() {
                    if i > 0 {
                        xml.push(' ');
                    }
                    let _ = write!(xml, "{x:.2} {y:.2}");
                }
                xml.push_str("</stroke>\n");
            }
            Element::Text { x, y, font, size, color, text } => {
                let _ = writeln!(
                    xml,
                    r#"<text font="{}" size="{size:.2}" x="{x:.4}" y="{y:.4}" color="{}">{}</text>"#,
                    escape(font),
                    hex(*color),
                    escape(text)
                );
            }
        }
    }
    let _ = writeln!(xml, "</layer>");
    let _ = writeln!(xml, "</page>");
    let _ = writeln!(xml, "</xournal>");

    // The same page must give the same bytes on every run (the books are git repos, and a
    // re-import must not turn up as every page changed). The XML above is
    // deterministic -- fixed element order, fixed-precision numbers -- and so is the gzip as long
    // as its header carries no time or name: mtime 0, no file name, OS byte 255 ("unknown").
    // flate2's defaults already are these; spelled out so nobody "fixes" them.
    let mut gz = GzBuilder::new().mtime(0).operating_system(255).write(Vec::new(), Compression::default());
    gz.write_all(xml.as_bytes())?;
    gz.finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn same_page_same_bytes_and_a_bare_gzip_header() {
        let page = Page {
            width: 595.275590551181,
            height: 841.889763779528,
            background: Rgb(5, 7, 10),
            elements: vec![
                Element::Stroke { tool: Tool::Pen, color: Rgb(1, 2, 3), width: 0.6, points: vec![(1.0 / 3.0, 2.0), (-0.0, 1e-9)] },
                Element::Text { x: 1.5, y: 2.25, font: "Calibri".into(), size: 11.0, color: Rgb(9, 9, 9), text: "a & b".into() },
            ],
        };
        let a = write(&page, "one2book").unwrap();
        assert_eq!(a, write(&page, "one2book").unwrap());
        // ID1 ID2 CM FLG MTIME(4) XFL OS: no FNAME/FCOMMENT flags, mtime 0, OS unknown
        assert_eq!(&a[..4], &[0x1f, 0x8b, 8, 0]);
        assert_eq!(&a[4..8], &[0, 0, 0, 0]);
        assert_eq!(a[9], 255);
    }
}
