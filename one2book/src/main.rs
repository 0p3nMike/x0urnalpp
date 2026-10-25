//! one2book: a OneNote notebook -> a X0urnal++ notebook (.book), handwriting kept as strokes.
//!
//!   one2book [--zip] <notebook folder | Notebook.onetoc2 | Notebook.onepkg | Section.one> <out.book>
//!
//! Writes the book FOLDER `<out.book>/` (format version 2: notebook.xml + sections/<id>.xopp,
//! the synced layout of ../book-sync/README.md), or with `--zip` the single-file zip book of
//! version 1, for sharing and for Windows. Ids are stable (src/ids.rs) and the output is
//! byte-identical for unchanged input, so re-importing a notebook changes only what changed in
//! OneNote.
//!
//! Prints what every section turned into (pages, strokes, images, text blocks, and anything
//! carried over only approximately or not at all), so a conversion can be checked by its numbers.
//! Exit status: 0 = every section converted; 2 = the book was written but some sections could
//! not be read (they are named in the output); 1 = nothing was written.
//!
//! See README.md for the mapping, the colour rule and the limits.

mod book;
mod colors;
mod ids;
mod images;
mod page;
mod xopp;

use book::{BookWriter, Node};
use ids::Ids;
use onenote_parser::Parser;
use onenote_parser::section::{Section, SectionEntry};
use page::Stats;
use std::path::{Path, PathBuf};
use typed_path::TypedPath;

const CREATOR: &str = "one2book";

fn main() {
    let (mut zip, mut unknown) = (false, false);
    let mut paths = Vec::new();
    for a in std::env::args().skip(1) {
        match a.as_str() {
            "--zip" => zip = true,
            _ if a.starts_with('-') => unknown = true,
            _ => paths.push(a),
        }
    }
    if unknown || paths.len() != 2 {
        eprintln!("usage: one2book [--zip] <notebook folder | .onetoc2 | .onepkg | .one> <out.book>");
        std::process::exit(1);
    }
    match run(Path::new(&paths[0]), Path::new(&paths[1]), zip) {
        Ok(failed) => std::process::exit(if failed > 0 { 2 } else { 0 }),
        Err(e) => {
            eprintln!("one2book: {e}");
            std::process::exit(1);
        }
    }
}

/// What the input is, and the notebook it holds.
enum Input {
    Notebook { title: String, entries: Vec<SectionEntry>, failures: Vec<String> },
    Section(Section),
}

fn open(input: &Path) -> Result<Input, String> {
    let parser = Parser::new();
    let ext = input.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    let stem = |p: &Path| p.file_stem().and_then(|s| s.to_str()).unwrap_or("Notebook").to_string();
    let notebook = |title: String, nb: onenote_parser::notebook::Notebook| Input::Notebook {
        title,
        failures: nb.report().warnings().iter().map(|w| w.message().to_string()).collect(),
        entries: nb.entries().to_vec(),
    };
    let tp = |p: &Path| TypedPath::derive(p.to_str().unwrap_or_default()).to_path_buf();

    if input.is_dir() {
        // a notebook folder: its table of contents is the one .onetoc2 inside
        let toc = std::fs::read_dir(input)
            .map_err(|e| format!("{}: {e}", input.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .find(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("onetoc2")))
            .ok_or_else(|| format!("{}: no .onetoc2 (notebook table of contents) in this folder", input.display()))?;
        let title = input.file_name().and_then(|s| s.to_str()).unwrap_or("Notebook").to_string();
        let nb = parser.parse_notebook(tp(&toc).to_path()).map_err(|e| format!("{}: {e}", toc.display()))?;
        let mut opened = notebook(title, nb);
        add_unlisted_sections(input, &parser, &mut opened);
        Ok(opened)
    } else if ext == "onetoc2" {
        let title = input.parent().and_then(|p| p.file_name()).and_then(|s| s.to_str()).unwrap_or("Notebook").to_string();
        let nb = parser.parse_notebook(tp(input).to_path()).map_err(|e| format!("{}: {e}", input.display()))?;
        Ok(notebook(title, nb))
    } else if ext == "onepkg" {
        let nb = parser.parse_package(tp(input).to_path()).map_err(|e| format!("{}: {e}", input.display()))?;
        Ok(notebook(stem(input), nb))
    } else if ext == "one" {
        let s = parser.parse_section(tp(input).to_path()).map_err(|e| format!("{}: {e}", input.display()))?;
        Ok(Input::Section(s))
    } else {
        Err(format!("{}: not a notebook folder, .onetoc2, .onepkg or .one", input.display()))
    }
}

/// Sections that sit in the notebook folder but are missing from its .onetoc2. OneNote rewrites
/// the table of contents lazily -- one tested notebook's was last written 22 minutes before its newest
/// section was -- and it shows every section file in the folder anyway, the unlisted ones after
/// the rest. Do the same, so the book holds what OneNote shows.
fn add_unlisted_sections(dir: &Path, parser: &Parser, opened: &mut Input) {
    let Input::Notebook { entries, failures, .. } = opened else { return };
    let listed: Vec<String> = entries
        .iter()
        .filter_map(|e| match e {
            SectionEntry::Section(s) => Some(s.display_name().to_string()),
            SectionEntry::SectionGroup(_) => None,
        })
        .collect();
    let Ok(read) = std::fs::read_dir(dir) else { return };
    let mut extra: Vec<PathBuf> = read
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("one")))
        .filter(|p| {
            let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            // already in the book, or already reported as unreadable
            !listed.iter().any(|l| l == stem) && !failures.iter().any(|f| f.contains(&format!("{stem}.one")))
        })
        .collect();
    extra.sort();
    for p in extra {
        let tp = TypedPath::derive(p.to_str().unwrap_or_default()).to_path_buf();
        println!("  (adding {}: in the folder but not in OneNote's table of contents)", p.display());
        match parser.parse_section(tp.to_path()) {
            Ok(s) => entries.push(SectionEntry::Section(s)),
            Err(e) => failures.push(format!("failed to import section {}: {e}", p.display())),
        }
    }
}

/// The state of one conversion, threaded through the tree walk.
struct Conv {
    w: BookWriter,
    meta: images::MetafileRenderer,
    ids: Ids,
    total: Stats,
    sections: usize,
}

fn run(input: &Path, out: &Path, zip: bool) -> Result<usize, String> {
    let opened = open(input)?;
    // BookWriter fills <out>.partial and only puts it at <out> in finish(), so a run that fails
    // part way never replaces a good book with half of one
    let w = BookWriter::create(out, zip)?;
    let meta = images::MetafileRenderer::new().map_err(|e| e.to_string())?;
    let mut cx = Conv { w, meta, ids: Ids::default(), total: Stats::default(), sections: 0 };
    let kind = if zip { "zip book, format 1" } else { "book folder, format 2" };

    let (title, tree, failures) = match &opened {
        Input::Notebook { title, entries, failures } => {
            println!("one2book: {title} -> {} ({kind})", out.display());
            let tree = entries_to_nodes(entries, &mut cx, "", "")?;
            (title.clone(), tree, failures.clone())
        }
        Input::Section(s) => {
            println!("one2book: {} -> {} ({kind})", s.display_name(), out.display());
            let node = section_to_node(s, s.display_name(), &mut cx, "")?;
            cx.sections = 1;
            (s.display_name().to_string(), vec![node], Vec::new())
        }
    };

    for f in &failures {
        println!("  FAILED {f}");
    }
    for warning in &cx.ids.warnings {
        println!("  WARNING {warning}");
    }
    let Conv { w, total, sections, .. } = cx;
    w.finish(&title, &tree)?;
    println!(
        "  total: {} sections ({} could not be read), {} pages, {} strokes ({} points), {} images, {} text blocks",
        sections + failures.len(),
        failures.len(),
        total.pages,
        total.strokes,
        total.points,
        total.images,
        total.texts
    );
    Ok(failures.len())
}

/// The nodes for a list of sections and section groups whose parent sits at `parent` in the
/// notebook ("" at the top, "Group A" inside a group) -- the path names a section's id.
fn entries_to_nodes(entries: &[SectionEntry], cx: &mut Conv, parent: &str, indent: &str) -> Result<Vec<Node>, String> {
    let mut nodes = Vec::new();
    for entry in entries {
        match entry {
            SectionEntry::Section(s) => {
                let path = ids::join_path(parent, s.display_name());
                nodes.push(section_to_node(s, &path, cx, indent)?);
                cx.sections += 1;
            }
            SectionEntry::SectionGroup(g) => {
                println!("{indent}  group {}", g.display_name());
                let path = ids::join_path(parent, g.display_name());
                let id = cx.ids.claim(ids::section_id(&path), &format!("section group {path}"));
                let deeper = format!("{indent}  ");
                let children = entries_to_nodes(g.entries(), cx, &path, &deeper)?;
                nodes.push(Node::Chapter { id, title: g.display_name().to_string(), children });
            }
        }
    }
    Ok(nodes)
}

/// One page waiting to be placed in the tree: its OneNote level (1 = page, 2+ = subpage).
struct PageItem {
    level: i32,
    id: String,
    title: String,
}

fn section_to_node(s: &Section, path: &str, cx: &mut Conv, indent: &str) -> Result<Node, String> {
    let chapter_id = cx.ids.claim(ids::section_id(path), &format!("section {path}"));
    let mut items = Vec::new();
    let mut stats = Stats::default();
    for series in s.page_series() {
        for p in series.pages() {
            let (xpage, st) = page::convert(p, &mut cx.meta);
            let title = p.title_text().map(str::trim).filter(|t| !t.is_empty()).unwrap_or("Untitled page").to_string();
            let id = cx.ids.claim(ids::page_id(p.link_target_id()), &format!("page \"{title}\" in {path}"));
            let bytes = xopp::write(&xpage, CREATOR).map_err(|e| e.to_string())?;
            cx.w.add_section(&id, &bytes)?;
            items.push(PageItem { level: p.level(), id, title });
            stats.add(&st);
        }
    }
    println!(
        "{indent}  section {}: {} pages, {} strokes, {} images, {} text blocks",
        s.display_name(),
        stats.pages,
        stats.strokes,
        stats.images,
        stats.texts
    );
    for (what, n) in &stats.approximate {
        println!("{indent}      approximate: {n} x {what}");
    }
    for (what, n) in &stats.skipped {
        println!("{indent}      not carried over: {n} x {what}");
    }
    cx.total.add(&stats);
    let mut i = 0;
    let children = nest(&items, &mut i, i32::MIN, &mut cx.ids);
    Ok(Node::Chapter { id: chapter_id, title: s.display_name().to_string(), children })
}

/// Turn the flat page list (in OneNote order, with levels) into the tree: a page followed by
/// deeper-level pages becomes a chapter (`on-chap-<its GUID>`) holding itself and then its
/// subpages.
fn nest(items: &[PageItem], i: &mut usize, level: i32, taken: &mut Ids) -> Vec<Node> {
    let mut out = Vec::new();
    while *i < items.len() && items[*i].level >= level {
        let it = &items[*i];
        *i += 1;
        let page = Node::Section { id: it.id.clone(), title: it.title.clone() };
        if *i < items.len() && items[*i].level > it.level {
            let id = taken.claim(ids::chapter_id(&it.id), &format!("chapter for page \"{}\"", it.title));
            let mut children = vec![page];
            children.extend(nest(items, i, it.level + 1, taken));
            out.push(Node::Chapter { id, title: it.title.clone(), children });
        } else {
            out.push(page);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn item(level: i32, guid: &str, title: &str) -> PageItem {
        PageItem { level, id: ids::page_id(guid), title: title.to_string() }
    }

    /// (id, title, children) of a tree, for comparing shapes
    fn shape(nodes: &[Node]) -> Vec<(String, String, usize)> {
        let mut out = Vec::new();
        for n in nodes {
            match n {
                Node::Chapter { id, title, children } => {
                    out.push((id.clone(), title.clone(), children.len()));
                    out.extend(shape(children));
                }
                Node::Section { id, title } => out.push((id.clone(), title.clone(), 0)),
            }
        }
        out
    }

    #[test]
    fn a_page_with_subpages_becomes_an_on_chap_chapter() {
        let items = [
            item(1, "{AAAAAAAA-0000-0000-0000-000000000001}", "Intro"),
            item(1, "{BBBBBBBB-0000-0000-0000-000000000002}", "Proofs"),
            item(2, "{CCCCCCCC-0000-0000-0000-000000000003}", "by induction"),
            item(2, "{DDDDDDDD-0000-0000-0000-000000000004}", "by contradiction"),
            item(1, "{EEEEEEEE-0000-0000-0000-000000000005}", "Sets"),
        ];
        let mut taken = Ids::default();
        let mut i = 0;
        let tree = nest(&items, &mut i, i32::MIN, &mut taken);
        let s = |a: &str, b: &str, n| (a.to_string(), b.to_string(), n);
        assert_eq!(
            shape(&tree),
            vec![
                s("on-aaaaaaaa-0000-0000-0000-000000000001", "Intro", 0),
                s("on-chap-bbbbbbbb-0000-0000-0000-000000000002", "Proofs", 3),
                s("on-bbbbbbbb-0000-0000-0000-000000000002", "Proofs", 0),
                s("on-cccccccc-0000-0000-0000-000000000003", "by induction", 0),
                s("on-dddddddd-0000-0000-0000-000000000004", "by contradiction", 0),
                s("on-eeeeeeee-0000-0000-0000-000000000005", "Sets", 0),
            ]
        );
        assert!(taken.warnings.is_empty());
    }
}
