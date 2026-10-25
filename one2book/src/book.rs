//! Writing a X0urnal++ notebook (`.book`).
//!
//! FORMAT VERSION 2 (the default): a FOLDER, the synced-notebook layout every device keeps as a
//! git clone (../book-sync/README.md "The book folder"):
//!   notebook.xml        the tree, version="2"
//!   sections/<id>.xopp  one gzipped .xopp per page
//! one2book writes exactly these two and nothing else: `.gitattributes`, `.gitignore` and the
//! git repository itself belong to `book-sync setup`, and the import (one2book-sync) copies a
//! finished conversion into its git clone rather than converting into the clone.
//!
//! FORMAT VERSION 1 (`--zip`, kept for sharing and for Windows): a zip of `mimetype`
//! ("application/x-x0urnal-notebook"), `notebook.xml` (version="1") and the same `sections/*.xopp`
//! (src/core/control/notebook/NotebookArchive.cpp). X0urnal++ imports one into a folder book when it
//! is opened. Unzip it and every page opens in stock xournalpp too.
//!
//! Both get the same stable ids (src/ids.rs) and the same notebook.xml text apart from the
//! version number.
//!
//! The mapping from OneNote keeps its organisation exactly:
//!   OneNote notebook      -> the book
//!   section group         -> chapter
//!   section               -> chapter
//!   page                  -> section (one infinite page)
//!   page with subpages    -> chapter named after the page, holding the page itself first and
//!                            then its subpages (a X0urnal++ section cannot contain others)
//!
//! Pages are written as they are converted, and notebook.xml last, so a 300 MB OneNote section
//! never has to sit in memory as a whole book. Everything goes into `<out>.partial` beside the
//! target and only replaces `<out>` once complete: a failed run never leaves half a book.

use crate::xopp::escape;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use zip::CompressionMethod;
use zip::write::SimpleFileOptions;

pub const MIMETYPE: &str = "application/x-x0urnal-notebook";

pub enum Node {
    Chapter { id: String, title: String, children: Vec<Node> },
    Section { id: String, title: String },
}

/// The path, inside the book, of the page file for a section with this id.
pub fn section_path(id: &str) -> String {
    format!("sections/{id}.xopp")
}

/// notebook.xml as text. Stable on purpose, because git diffs it and book-sync's notebook merge
/// driver reads it: one node per line, two-space indent, attributes always in the order
/// `id title file`, titles on one line (OneNote titles can hold line breaks; a tree title is one
/// line in the sidebar anyway), an empty chapter as one self-closing line.
pub fn notebook_xml(version: u32, title: &str, tree: &[Node]) -> String {
    let mut xml = String::from("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml.push_str(&format!("<notebook version=\"{version}\" title=\"{}\">\n", attr(title)));
    write_nodes(&mut xml, tree, 1);
    xml.push_str("</notebook>\n");
    xml
}

fn write_nodes(xml: &mut String, nodes: &[Node], depth: usize) {
    let pad = "  ".repeat(depth);
    for n in nodes {
        match n {
            Node::Chapter { id, title, children } if children.is_empty() => {
                xml.push_str(&format!("{pad}<chapter id=\"{}\" title=\"{}\"/>\n", attr(id), attr(title)));
            }
            Node::Chapter { id, title, children } => {
                xml.push_str(&format!("{pad}<chapter id=\"{}\" title=\"{}\">\n", attr(id), attr(title)));
                write_nodes(xml, children, depth + 1);
                xml.push_str(&format!("{pad}</chapter>\n"));
            }
            Node::Section { id, title } => {
                xml.push_str(&format!(
                    "{pad}<section id=\"{}\" title=\"{}\" file=\"{}\"/>\n",
                    attr(id),
                    attr(title),
                    attr(&section_path(id))
                ));
            }
        }
    }
}

/// An attribute value: whitespace runs (line breaks included) folded to one space, then escaped.
fn attr(s: &str) -> String {
    escape(&s.split_whitespace().collect::<Vec<_>>().join(" "))
}

enum Sink {
    /// version 2: `partial` is the folder being filled
    Folder,
    /// version 1: `partial` is the zip being written (boxed: a ZipWriter is large)
    Zip(Box<zip::ZipWriter<BufWriter<File>>>),
}

pub struct BookWriter {
    out: PathBuf,
    partial: PathBuf,
    sink: Sink,
    done: bool,
}

/// `<path>` with `suffix` added to its file name: the scratch names beside the target.
fn sibling(path: &Path, suffix: &str) -> PathBuf {
    let mut name = path.file_name().map(|n| n.to_os_string()).unwrap_or_default();
    name.push(suffix);
    path.with_file_name(name)
}

/// Remove a file or folder if it is there.
fn remove_any(path: &Path) -> std::io::Result<()> {
    match fs::symlink_metadata(path) {
        Ok(m) if m.is_dir() => fs::remove_dir_all(path),
        Ok(_) => fs::remove_file(path),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e),
    }
}

/// A folder book can replace a folder at `out`, but never a git clone (its `.git` is the book's
/// history and sync state -- the import copies into clones itself) and never a file (most
/// likely a v1 zip book, maybe one somebody wrote in).
fn check_folder_target(out: &Path) -> Result<(), String> {
    if out.join(".git").exists() {
        return Err(format!(
            "{}: is a git clone; one2book writes whole book folders only, never into a clone. Convert to \
             another path and copy notebook.xml and sections/ in (one2book-sync does this)",
            out.display()
        ));
    }
    if out.exists() && !out.is_dir() {
        return Err(format!(
            "{}: exists and is not a folder (a zip book from one2book 0.1?); move it away, or pass --zip",
            out.display()
        ));
    }
    Ok(())
}

impl BookWriter {
    /// Start a book at `out`: a folder (version 2) or, with `zip`, a zip file (version 1).
    pub fn create(out: &Path, zip: bool) -> Result<Self, String> {
        let partial = sibling(out, ".partial");
        let err = |p: &Path, e: &dyn std::fmt::Display| format!("{}: {e}", p.display());
        // whatever a failed run left behind is ours to clear
        remove_any(&partial).map_err(|e| err(&partial, &e))?;
        let sink = if zip {
            if out.is_dir() {
                return Err(format!("{}: is a folder; a zip book needs a file name of its own", out.display()));
            }
            let mut z = zip::ZipWriter::new(BufWriter::new(File::create(&partial).map_err(|e| err(&partial, &e))?));
            // mimetype first and uncompressed, the way ODF-style containers do it
            z.start_file("mimetype", SimpleFileOptions::default().compression_method(CompressionMethod::Stored))
                .and_then(|_| z.write_all(MIMETYPE.as_bytes()).map_err(Into::into))
                .map_err(|e| err(&partial, &e))?;
            Sink::Zip(Box::new(z))
        } else {
            check_folder_target(out)?;
            fs::create_dir_all(partial.join("sections")).map_err(|e| err(&partial, &e))?;
            Sink::Folder
        };
        Ok(Self { out: out.to_path_buf(), partial, sink, done: false })
    }

    /// Store one page's .xopp bytes (already gzipped, so a zip stores rather than deflates them).
    pub fn add_section(&mut self, id: &str, xopp: &[u8]) -> Result<(), String> {
        let path = section_path(id);
        match &mut self.sink {
            Sink::Folder => {
                let p = self.partial.join(&path);
                fs::write(&p, xopp).map_err(|e| format!("{}: {e}", p.display()))
            }
            Sink::Zip(z) => {
                let opts = SimpleFileOptions::default()
                    .compression_method(CompressionMethod::Stored)
                    .large_file(xopp.len() > u32::MAX as usize / 2);
                z.start_file(path, opts)
                    .and_then(|_| z.write_all(xopp).map_err(Into::into))
                    .map_err(|e| format!("{}: {e}", self.partial.display()))
            }
        }
    }

    /// Write notebook.xml and put the finished book in place at `out`.
    pub fn finish(mut self, title: &str, tree: &[Node]) -> Result<(), String> {
        let err = |p: &Path, e: &dyn std::fmt::Display| format!("{}: {e}", p.display());
        match std::mem::replace(&mut self.sink, Sink::Folder) {
            Sink::Zip(mut z) => {
                let xml = notebook_xml(1, title, tree);
                z.start_file("notebook.xml", SimpleFileOptions::default().compression_method(CompressionMethod::Deflated))
                    .and_then(|_| z.write_all(xml.as_bytes()).map_err(Into::into))
                    .and_then(|_| z.finish().and_then(|mut f| f.flush().map_err(Into::into)))
                    .map_err(|e| err(&self.partial, &e))?;
                fs::rename(&self.partial, &self.out).map_err(|e| err(&self.out, &e))?;
            }
            Sink::Folder => {
                // notebook.xml.tmp renamed over notebook.xml, the way every writer of a book
                // folder does it (the contract), even though this whole folder is still scratch
                let xml = notebook_xml(2, title, tree);
                let tmp = self.partial.join("notebook.xml.tmp");
                let mut f = File::create(&tmp).map_err(|e| err(&tmp, &e))?;
                f.write_all(xml.as_bytes()).and_then(|_| f.sync_all()).map_err(|e| err(&tmp, &e))?;
                fs::rename(&tmp, self.partial.join("notebook.xml")).map_err(|e| err(&tmp, &e))?;
                self.swap_in()?;
            }
        }
        self.done = true;
        Ok(())
    }

    /// Replace `out` with the finished folder: the old one steps aside to `<out>.old`, the new
    /// one takes its name, the old one goes. There is no moment with half a book at `out`.
    fn swap_in(&self) -> Result<(), String> {
        let err = |p: &Path, e: &dyn std::fmt::Display| format!("{}: {e}", p.display());
        // checked again: the conversion takes minutes, and someone may have cloned there meanwhile
        check_folder_target(&self.out)?;
        if !self.out.exists() {
            return fs::rename(&self.partial, &self.out).map_err(|e| err(&self.out, &e));
        }
        let old = sibling(&self.out, ".old");
        remove_any(&old).map_err(|e| err(&old, &e))?;
        fs::rename(&self.out, &old).map_err(|e| err(&self.out, &e))?;
        if let Err(e) = fs::rename(&self.partial, &self.out) {
            let _ = fs::rename(&old, &self.out); // put the previous book back
            return Err(err(&self.out, &e));
        }
        // the new book is in place; an old copy that won't go is cleared by the next run
        let _ = fs::remove_dir_all(&old);
        Ok(())
    }
}

impl Drop for BookWriter {
    /// A conversion that stopped part way leaves nothing behind but its error message.
    fn drop(&mut self) {
        if !self.done {
            let _ = remove_any(&self.partial);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Vec<Node> {
        vec![
            Node::Chapter {
                id: "on-sec-1cf8f2179a77e83b".into(),
                title: "Discrete".into(),
                children: vec![
                    Node::Section { id: "on-6f1c".into(), title: "truth tables".into() },
                    Node::Chapter {
                        id: "on-chap-77aa".into(),
                        title: "Proofs".into(),
                        children: vec![
                            Node::Section { id: "on-77aa".into(), title: "Proofs".into() },
                            Node::Section { id: "on-88bb".into(), title: "by induction".into() },
                        ],
                    },
                ],
            },
            Node::Chapter { id: "on-sec-0000000000000000".into(), title: "empty".into(), children: vec![] },
        ]
    }

    #[test]
    fn notebook_xml_is_one_node_per_line_in_attribute_order() {
        let want = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<notebook version=\"2\" title=\"Course_Notes\">
  <chapter id=\"on-sec-1cf8f2179a77e83b\" title=\"Discrete\">
    <section id=\"on-6f1c\" title=\"truth tables\" file=\"sections/on-6f1c.xopp\"/>
    <chapter id=\"on-chap-77aa\" title=\"Proofs\">
      <section id=\"on-77aa\" title=\"Proofs\" file=\"sections/on-77aa.xopp\"/>
      <section id=\"on-88bb\" title=\"by induction\" file=\"sections/on-88bb.xopp\"/>
    </chapter>
  </chapter>
  <chapter id=\"on-sec-0000000000000000\" title=\"empty\"/>
</notebook>
";
        assert_eq!(notebook_xml(2, "Course_Notes", &sample()), want);
    }

    #[test]
    fn titles_are_escaped_and_kept_on_one_line() {
        let tree = vec![Node::Section { id: "on-1".into(), title: "  a <b> & \"c\"\r\n second\u{0b}line\t ".into() }];
        let xml = notebook_xml(1, "x & y", &tree);
        assert!(xml.contains("<notebook version=\"1\" title=\"x &amp; y\">"));
        assert!(xml.contains(
            "  <section id=\"on-1\" title=\"a &lt;b&gt; &amp; &quot;c&quot; second line\" file=\"sections/on-1.xopp\"/>\n"
        ));
        assert_eq!(xml.lines().count(), 4);
    }

    #[test]
    fn folder_book_swaps_in_whole_and_refuses_git_clones() {
        let base = std::env::temp_dir().join(format!("one2book-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(&base).unwrap();
        let out = base.join("T.book");

        // first write, then a rewrite that drops a page: the old page file must not survive
        let mut w = BookWriter::create(&out, false).unwrap();
        w.add_section("on-1", b"one").unwrap();
        w.add_section("on-2", b"two").unwrap();
        w.finish("T", &[]).unwrap();
        let mut w = BookWriter::create(&out, false).unwrap();
        w.add_section("on-1", b"one").unwrap();
        w.finish("T", &[]).unwrap();
        assert!(out.join("sections/on-1.xopp").exists());
        assert!(!out.join("sections/on-2.xopp").exists());
        assert!(out.join("notebook.xml").exists() && !out.join("notebook.xml.tmp").exists());
        assert!(!sibling(&out, ".partial").exists() && !sibling(&out, ".old").exists());

        // a run that fails part way leaves the previous book and no scratch behind
        let mut w = BookWriter::create(&out, false).unwrap();
        w.add_section("on-3", b"three").unwrap();
        drop(w);
        assert!(out.join("sections/on-1.xopp").exists() && !out.join("sections/on-3.xopp").exists());
        assert!(!sibling(&out, ".partial").exists());

        // a git clone is never replaced
        fs::create_dir_all(out.join(".git")).unwrap();
        assert!(BookWriter::create(&out, false).is_err());

        // a zip book is a file, and a folder book refuses to replace one
        let z = base.join("Z.book");
        let mut w = BookWriter::create(&z, true).unwrap();
        w.add_section("on-1", b"one").unwrap();
        w.finish("Z", &[]).unwrap();
        assert!(z.is_file());
        assert!(BookWriter::create(&z, false).is_err());
        fs::remove_dir_all(&base).unwrap();
    }
}
