# one2book

Converts OneNote notebooks into X0urnal++ notebooks ("books"), with the **handwriting kept as
real strokes** you can select, erase and recolour -- not pictures of ink. Typed text, pictures and
PDF printouts come along too, in the same chapters and order as in OneNote.

    one2book [--zip] <notebook folder | Notebook.onetoc2 | Notebook.onepkg | Section.one> <out.book>
    ./one2book-sync [--no-copy] [NAME=SOURCE ...]   # convert again and merge into a synced book

Rust (edition 2024). MIT licensed (see `LICENSE`); the vendored OneNote parser keeps its own
licence, MPL-2.0 (`vendor/onenote_parser/LICENSE`).

## Quick start

    cargo build --release
    target/release/one2book "/path/to/My Notebook" ~/Documents/notebooks/My_Notebook.book

then open it in X0urnal++ (File → Open Notebook), and to sync it to your other devices,
`book-sync init My_Notebook` (see `../book-sync/README.md`).

**Where OneNote keeps notebooks.** A notebook saved on the computer is a folder of `.one` files
(one per section) and a `.onetoc2`, by default under `Documents\OneNote Notebooks`. A notebook
that lives in OneDrive or SharePoint has no local folder: export it from OneNote with
File → Export → Notebook → OneNote Package (`.onepkg`) and convert that. Copy the folder or the
`.onepkg` to Linux, or run one2book on Windows directly.

`one2book` prints what every section became (pages, strokes, images, text blocks, plus everything
carried over only approximately or not at all). Exit status: 0 every section converted; 2 the
book was written but some sections could not be read (they are named); 1 nothing was written.

`--zip` writes the single-file zip book instead of a folder, for sending a notebook to someone.
X0urnal++ unpacks a zip book into a folder book when it opens one.

## What it does to a page

One OneNote page becomes one **infinite** X0urnal++ page (`<page infinite="true">`), sized to
its content and never smaller than A4. X0urnal++ pages start at 0,0 and grow only right/down,
so a page with anything left of or above OneNote's origin is shifted as a whole.

| OneNote | X0urnal++ |
|---|---|
| ink | pen strokes, exact positions, OneNote's pen width; one width per stroke (OneNote stores no pressure) |
| highlighter (see-through ink) | highlighter strokes (Xournal++ draws them at its own fixed 0.47 opacity) |
| pasted pictures, PDF printout pages | images at their exact positions and sizes; printouts are the ~144 dpi page images OneNote keeps (the original PDF is not in the notebook) |
| EMF/WMF pictures (older printouts) | rendered to PNG with LibreOffice (`soffice --headless`) if it is installed |
| typed text, lists | plain text blocks at the text box position, wrapped at roughly the box width, bullets as • |
| page title, date, time | text at the top, title at OneNote's 20 pt |
| tables | one text line per row, cells separated by `|` |
| attachments | not carried over (counted in the report) |

**Units.** OneNote positions elements in half-inches and draws ink in HIMETRIC (0.01 mm); Xournal++
uses points. half-inch × 36 = pt, HIMETRIC × 72/2540 = pt. Ink written over printouts lands on
the printed lines, and three other OneNote readers (one2html, Joplin, OfficeIMO) place ink with
the same numbers.

**Colours.** The pages get X0urnal++'s whiteboard look: the dark ground `#05070a` with the blue
graph grid. Xournal++ draws ink colours literally, so black and near-black ink and text (every
channel ≤ 0x40) become the theme's off-white `#d8d4c4`, or they would vanish. Every other colour
is kept exactly.

**Approximate, and reported as such:** pictures and ink inside a text box (OneNote keeps no
position for them, so they follow the text), handwriting written inline in a typed paragraph, and
mixed formatting within one paragraph (a Xournal++ text element has one font, so the paragraph
takes its first run's).

## Book layout

Same organisation as in OneNote: notebook → book, section group → chapter, section → chapter,
page → section (one `.xopp`), and a page with subpages → a chapter named after it holding the
page first, then its subpages. Sections appear in OneNote's tab order. A section file that is in
the notebook folder but missing from OneNote's table of contents (OneNote rewrites the
`.onetoc2` lazily) is added after the rest, which is where OneNote shows it too. The recycle bin
is left out.

    My_Notebook.book/
      notebook.xml          the tree, version="2"
      sections/<id>.xopp    one page per file (gzipped .xopp; every page opens in stock Xournal++ too)

That is all one2book writes. `.gitattributes`, `.gitignore`, `trash/` and the git repo belong to
`book-sync` and X0urnal++; one2book refuses to write over a git clone (the import copies a
finished conversion into its clone itself) and over a file (a zip book). It fills
`<out>.book.partial/` and only swaps it in once complete, so a failed run never leaves half a book;
an existing book folder at `<out>` is replaced whole. `notebook.xml` is written as
`notebook.xml.tmp` and renamed, one node per line, two-space indent, attributes in the order
`id title file` -- stable text, so `git diff` and the merge driver see real changes only.

### Ids

Ids never change once given (`src/ids.rs`), so a re-import is a merge, not a replacement:

| node | id |
|---|---|
| page | `on-<OneNote page GUID, lowercase, no braces>` -- the parser's `Page::link_target_id()` is `{6F1C0B2E-...}`, normalised |
| section, section group | `on-sec-<first 16 hex of sha256(its path in the notebook)>`, path = group names and the section name joined by `/` (`Group A/Discrete`) |
| the chapter a page with subpages becomes | `on-chap-<that page's GUID>` |

A page keeps its id through edits and renames in OneNote. A section renamed or moved in OneNote
gets a new id, its pages keep theirs (the merge sees pages move, not vanish). Every id is unique
within the book: a clash (a copied section file holding the same page GUIDs) gets `-2`, `-3`, ...
in document order and a `WARNING duplicate id` line in the report.

### Same input, same bytes

Converting unchanged OneNote files gives byte-identical files, so a re-import changes only what
changed in OneNote: the gzip header has mtime 0, no file name and OS byte 255; numbers have fixed
precision; elements, pages and sections keep OneNote's order. EMF/WMF pictures rendered by
LibreOffice have any tIME/tEXt/zTXt/iTXt chunks dropped.

## Keeping a book in step with OneNote (one2book-sync)

While OneNote is still in use somewhere, `one2book-sync` (Linux, bash) converts it again whenever
you run it and **merges** each conversion into the synced book instead of replacing it: pages
changed only in OneNote arrive, pages changed only in X0urnal++ stay, a page changed on both is
merged stroke by stroke or gets a conflict page next to it.

    ./one2book-sync My_Notebook="/mnt/share/OneNote Notebooks/My Notebook"
    ./one2book-sync Work=~/exports/Work.onepkg

or list them once in `~/.config/one2book/notebooks`, one `NAME|SOURCE` per line, and run
`./one2book-sync` (all) or `./one2book-sync My_Notebook` (one). It needs `cargo`, `git`,
`book-sync` and a remote configured for book-sync. Per notebook:

1. copy the changed OneNote files into a mirror (`~/.cache/one2book/src/<name>/`), each copy
   checked against the source after it lands, because OneNote saves sections in place;
2. if the mirror changed (or one2book did), convert into `~/.cache/one2book/books/<name>.book/`
   (report: `<name>.log` beside it). A conversion in which some section couldn't be read is **not
   imported** -- it would remove that section's pages on every device until the next good import;
3. in the import clone `~/.cache/one2book/repo/<name>.book/` (a clone of the book's repo, set up
   with `book-sync setup` so it has the merge drivers): put the conversion on branch `onenote` and
   commit it if anything changed; bring `main` up to date (`book-sync sync`), merge `onenote` into
   it (conflict pages say "from onenote"), `book-sync sync` again, and push `onenote` too.

The first import of a book makes `main` the conversion and creates the remote repo with
`book-sync init`. A remote repo with `main` but no `onenote` branch wasn't made by the import and
is refused: merging would put every page in twice. A merge that stops is undone; `main` stays as
it was and the import waits on `onenote` for the next run. `--no-copy` skips step 1 and converts
what the mirror has (after a one2book update, or while the source is away).

## Checking a conversion

1. The numbers: one2book's per-section counts.
2. X0urnal++ itself, headless: `X0urnal++ --create-pdf=out.pdf page.xopp` for any page.
3. By eye, against OneNote's own PDF export of the same page.
4. `cargo test`: ids and their normalisation, notebook.xml formatting, the folder swap, the gzip
   header, the PNG strip.

## The parser

`vendor/onenote_parser` is onenote_parser 2.1.0 (https://github.com/msiemens/onenote.rs,
MPL-2.0, licence beside it) with one fix, marked `one2book fix` in the code (under
`src/onestore/desktop/`). 2.1.0 rejects large sections with "unexpected end of file": some have a
file-node list fragment whose ChunkTerminatorFND (the end-of-list marker) is followed by non-zero
leftover bytes, and 2.1.0 never stopped at the marker, so it decoded those leftovers as nodes.
[MS-ONESTORE] 2.4.1 says the stream ends at the marker and the rest is padding that MUST be
ignored. The fix stops there, treats an impossible node header as padding, and honours the
committed-transaction count (2.3.3). Sections that already parsed give byte-identical output
before and after. Same failure as upstream issue #31.
