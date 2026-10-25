# book-sync

Keeps X0urnal++ notebooks ("books") in step across devices through any git server, the way
OneNote syncs its notebooks. Every device has a full git clone of every book; X0urnal++ calls
`book-sync` on its own, and you can run it from a terminal too.

Python 3.11 or newer, standard library only. Runs on Linux (x86_64 and aarch64) and Windows.
MIT licensed (see `LICENSE`).

## What OneNote does, and what this copies

| OneNote | here |
|---|---|
| no Save button; every change is kept | X0urnal++ saves a page ~2 s after the last edit, on page switch, on focus-out and on close; never asks |
| full offline copy on every device | every device has a git clone of every book |
| incremental sync of changed parts | git: only changed pages travel |
| two devices, same page: merged per element; a **conflict page** only when that fails | per-page 3-way merge by stroke (`xopp-merge`); a sibling page "… (conflict from laptop, 25 Oct 14:02)" when it can't decide |
| sync status, Sync Now | a status line and a Sync button in the Notebook sidebar tab |
| page versions | `git log` of the page file |
| recycle bin | removed pages move to `trash/` |

Sync runs on events, never on a timer: a burst of saves, opening a book, switching pages, the
window getting focus back, closing, the Sync button. A device that is asleep or offline keeps its
commits and sends them at its next event.

## Setting it up

1. **Install.** Put `book-sync` on PATH; the other files stay beside it.
   - Linux: `x0urnal/build-linux.sh --tools` links them into `~/.local/bin`, or by hand:

         for f in book-sync xopp-merge notebook-merge xopp-filter; do ln -sf "$PWD/$f" ~/.local/bin/; done

     (the Arch package installs them for you).
   - Windows: `x0urnal\build-windows.ps1` copies them into `%USERPROFILE%\bin`, with
     `book-sync.cmd` for terminals. Install Python 3.11+ so `python` and `pythonw` are on PATH.

2. **Choose where the books live.** One repo per book, `<base>/book-<slug>.git`, on any git
   server you can push to over ssh or https without a password prompt (an ssh key, or a
   credential helper). There is no default; set the base once per device:

       git config --global book-sync.remote git@github.com:YOU
       # or: git@git.example.com:YOU, ssh://host/srv/git, /mnt/usb/books, ...
       # or per run: BOOK_SYNC_REMOTE_BASE=...

   **Keep book repos private** -- they hold your notes. Servers with push-to-create (Forgejo,
   Gitea, GitLab) make the repo on the first push. On GitHub, create an empty private repo first:

       gh repo create YOU/book-notes --private

   Without a remote everything still works: books are saved on the device and the sidebar says
   "not set up to sync".

3. **The first device sends a book:** `book-sync init ~/Documents/notebooks/Notes.book`.
   (X0urnal++'s File → New Notebook runs this for you.)

4. **Every other device gets it once:** `book-sync clone notes`. The folder is named from the
   book's title. From then on X0urnal++ keeps it in step.

`book-sync sync-all` syncs every book on the device, for a cron job or a login script if you
want one.

## Commands

    book-sync setup  <book>            idempotent: git init if needed, merge drivers + filters in
                                       .git/config, .gitattributes, .gitignore, user.name = device
    book-sync init   <book>            setup + first commit + push to the remote
    book-sync clone  <slug> [<dest>]   clone <base>/book-<slug>.git into ~/Documents/notebooks/
    book-sync save   <book>            commit local changes, no network (fast; safe to call often)
    book-sync push   <book>            save + push, NEVER touches the working tree: if the remote has
                                       newer commits it doesn't merge, it answers state "behind"
    book-sync sync   <book>            save, fetch, merge, push
    book-sync status <book>            what the last sync found + what is waiting; no network
    book-sync sync-all                 every *.book folder under ~/Documents/notebooks/
    book-sync merge  <book> <ref>      merge a local ref (one2book-sync's `onenote` branch) with
                                       everything sync does around a merge; no network

`<book>` is a path to a book folder, or the name of one under `~/Documents/notebooks/` (`Notes`
or `Notes.book`). The slug is the folder name without `.book`, lowercased, anything outside
`[a-z0-9-]` turned into `-`.

**JSON line** (always exactly one on stdout, also on failure):

    {"state": "synced|offline|conflict|error|local|behind", "time": "2024-01-31T14:02:11",
     "changed": ["sections/on-6f1c….xopp", "notebook.xml"],   # paths the merge changed in the working tree
     "conflicts": ["<id of each conflict page created>"],
     "message": "Merged from laptop"}

`state`: `synced` pushed and up to date; `offline` server unreachable, local commits kept;
`conflict` merged but made conflict pages; `behind` the remote has newer commits (from `push`,
or when a page mid-write blocks a merge); `local` the book has no remote (yet); `error` anything
else (the message says what). Exit status 0 for all but `error`. Extra fields: `book`, `ahead`
(commits not on the server), `skipped`; `status` adds `dirty` and `busy` (a sync holds the book
now) and does not wait for the lock; `sync-all` reports the worst state and per-book results in
`books`.

**Environment:** `BOOK_SYNC_REMOTE_BASE` (else `git config book-sync.remote`; no default),
`BOOK_SYNC_NOTEBOOKS` (default `~/Documents/notebooks`), `BOOK_SYNC_DEVICE` (default: the
hostname's first label, lowercased), `BOOK_SYNC_LOCK_TIMEOUT` (30 s), `BOOK_SYNC_GRACE` (10 s),
`BOOK_SYNC_THEIR_DEVICE` (names the other side in conflict pages), `BOOK_SYNC_STARTED_BY`
(X0urnal++ on Windows sets it: this sync is its own, so a book it holds open is merged).

## The book folder (format version 2)

    Notes.book/
      notebook.xml          the tree (below)
      sections/<id>.xopp    one page per file, the gzipped .xopp X0urnal++ writes
      trash/<id>.xopp       removed pages (git history keeps every version anyway)
      .gitattributes        written by `book-sync setup`
      .gitignore            *~  .*.autosave.xopp  *.tmp  .book-sync/

Every page is an ordinary `.xopp` that stock Xournal++ opens. `notebook.xml`:

    <?xml version="1.0" encoding="UTF-8"?>
    <notebook version="2" title="Notes">
      <chapter id="on-chap-…" title="Discrete">
        <section id="on-6f1c…" title="truth tables" file="sections/on-6f1c….xopp"/>
      </chapter>
    </notebook>

- The writer writes `notebook.xml.tmp` and renames it over `notebook.xml` (never half-written).
- One node per line, two-space indent, attributes in the order `id title file`: stable text, so
  `git diff` and the merge driver see real changes only.
- Format version 1 is the same tree and pages in a single zip file (`.book` as a file); X0urnal++
  turns one into a folder book when it opens it.

### Ids

Ids never change once given, and are unique across devices without coordination:

| made by | form | example |
|---|---|---|
| X0urnal++, a new page or chapter | `<device>-<base36 ms since epoch>-<4 base36 random>` | `tablet-mg8k2x1a-7q3c` |
| the OneNote import, a page | `on-<OneNote page GUID, lowercase, no braces>` | `on-6f1c0b2e-…` |
| the OneNote import, a section / section group | `on-sec-<16 hex of sha256(path in notebook)>` | `on-sec-3b9d…` |
| zipped books | `n<k>` | kept as they are |

`<device>` is `BOOK_SYNC_DEVICE`, or the hostname's first label, lowercased (`Desktop.lan` ->
`desktop`); X0urnal++ uses the same rule. A section's file is always `sections/<id>.xopp`.

## Merging

**Locking:** `flock` on `.git/book-sync.lock` (msvcrt's lock on Windows; waits up to 30 s), so
X0urnal++, `sync-all` and the OneNote import never run git in the same book at once.

**Commits:** author = device, message `"<device>: <n> pages"` (or `"onenote import"`); a merge
commit adds a line "Merged from <device>".

**Never loses data:** a page removed on one side but edited on the other is kept; a page file in
`sections/` that `notebook.xml` doesn't mention is put back into the tree under a chapter
"Recovered"; the merge drivers fall back to conflict pages, never to "theirs wins".

Configured by `setup` in `.git/config` and `.gitattributes`:

    sections/*.xopp  merge=xopp  filter=xopp
    trash/*.xopp     merge=xopp  filter=xopp
    notebook.xml     merge=notebook

- **`filter=xopp`** (`xopp-filter`): clean = gunzip (git stores plain XML, which deltas, so a
  book's history stays small); smudge = gzip. It also speaks git's long-running `process`
  protocol, so a clone of a big book starts Python once, not once per page.
- **`xopp-merge %O %A %B %P`** (base, ours, theirs, path): reads gzip or plain XML. Per page and
  layer, every element (stroke / text / image / teximage) gets a canonical key: tag + sorted
  attributes + its text with numbers rounded to 0.01. Result count of each key = ours + theirs −
  base (never below 0); order = ours, then elements only theirs added, on top. Page size = the
  larger. An attribute changed on one side only takes that change. **Cannot decide** (the same
  attribute changed differently on both sides, page or layer counts differ, either side
  unparsable): write ours, write theirs as a conflict page (below), exit 0.
- **`notebook-merge %O %A %B`**: union of nodes by id; a node removed on one side and unchanged
  on the other is removed; renamed or moved on one side takes that side; changed differently on
  both, ours wins and the other title goes in the conflict page's title.
- **Conflict page:** theirs is saved as `sections/<their-id>-conflict-<device>-<base36 time>.xopp`
  and added to `notebook.xml` right after the original, titled
  `<title> (conflict from <device>, <25 Oct 14:02>)`. Removing it (sidebar → Remove) resolves it.

The drivers are written into each book's `.git/config` by their **real** absolute path (symlinks
resolved), not by name: X0urnal++ started from a desktop launcher may not have `~/.local/bin` on
PATH, and a missing driver would make git text-merge a page. Every run re-checks those paths, so
moving the book-sync folder heals at the next sync.

## X0urnal++ and book-sync

- X0urnal++ opens a `.book` **folder** in place. A zip `.book` is unpacked into
  `~/Documents/notebooks/<Name>.book/` first.
- One window per book: a lock on `.git/x0urnal.lock` (flock on Linux, LockFileEx on byte 0 on
  Windows, the byte msvcrt tests). A sync started from anywhere else -- a terminal, `sync-all` --
  only sends a book that is open in X0urnal++ and never merges it, because the window would save
  its older copy of a page back over the merge. X0urnal++'s own syncs merge; on Windows they say
  so in `BOOK_SYNC_STARTED_BY`, since there is no `/proc` to ask.
- `save` whenever (local, cheap); `push` after a burst of saves (~20 s idle) and on focus-out;
  `sync` only when the editor can reload: opening a book, page switch (if the last sync was over a
  minute ago), focus-in after a minute, the Sync button. Input is switched off for the moment a
  `sync` takes. If `changed` names the open page it is reloaded; if it names `notebook.xml` the
  tree is reloaded.
- The open page is written out before `sync`/`push`: only complete files are committed (a
  half-written one is left out and named in `skipped`).
- New page: file first, then notebook.xml. Remove: node out of the tree AND the file moved to
  `trash/` -- a file left in `sections/` without a node comes back under "Recovered".

## The OneNote import (../one2book)

`one2book` writes folder books with the import ids above and byte-identical output for unchanged
input. `one2book-sync` commits each conversion on a branch `onenote`, then
`BOOK_SYNC_THEIR_DEVICE=onenote` merges it into `main` with the same drivers and pushes: pages
changed only in OneNote arrive, pages changed only in X0urnal++ stay, both -> merged or a conflict
page. A plain `git merge onenote` works with the drivers too; the following `book-sync sync` puts
conflict pages into the tree and keeps pages OneNote dropped but X0urnal++ edited.

## Tests

    ./run-tests            everything (~90 s; local bare repos only, no network)
    ./run-tests -k text    only tests whose name matches

Scratch repos go under `$BOOKSYNC_TEST_TMP` (default: the system temp dir) and are removed after
each test unless `BOOKSYNC_KEEP=1`. Nothing touches the network or `~/Documents/notebooks`. On
Windows two tests skip (symlinks, SIGTERM).

## Notes (choices where the spec left room)

- `save` and `push` never touch the working tree, so putting conflict pages into the tree, the
  "Recovered" chapter and taking files back out of `trash/` happen in `sync` (and `merge`/`init`).
  `sync` leaves alone orphan/trash files younger than 10 s that the merge did not bring (X0urnal++
  may be halfway through creating or removing a page).
- `status`: `time` is the last sync's; `state` is the last sync's result, or `conflict` while
  conflict pages are in the tree, or `behind` if the last fetch saw newer commits.
- Canonical keys round only values that are wholly numbers (coordinates, widths, x/y); colours,
  fonts, typed text and base64 are compared exactly (rounding "3.14159" typed on a page to "3.14"
  would merge two different texts).
- Xournal++ has no element ids, so "the same element edited" is recognised by anchors: text by
  position or words, stroke by its points, image by data or position, teximage by LaTeX, data or
  position. Edited on both sides -> merged per attribute, or a conflict page if the same attribute
  or text differs; edited on one side -> keeps its stacking place; the same edit on both sides ->
  once. A stroke moved on both sides becomes two strokes.
- `<title>`/`<preview>` and other non-page parts: theirs if only theirs changed, else ours, never a
  conflict. Add/add (no base): what both sides share is the base.
- notebook-merge cannot reach the page files, so the drivers leave records in `.book-sync/` and
  sync inserts the conflict pages. When both sides renamed a page, theirs titles its conflict page
  if there is one, else it is only named in `message`. If our notebook.xml is unreadable,
  theirs is taken (our pages return under "Recovered") -- the one "theirs" case.
- Conflict page `<device>` = author of their last commit to that page (`BOOK_SYNC_THEIR_DEVICE`
  overrides); the time is that commit's, in local time. A conflict of a page in the trash stays
  in the trash. "Recovered" has the fixed id `recovered`, so two devices end up with one chapter.
- An empty chapter is written as an opening and a closing line, like one2book. book-sync
  rewrites notebook.xml only when it changes the tree.
- `init` creates the bare repo when the remote base is a local folder (push-to-create's
  stand-in), and merges if the remote already has the book.
- Every git call runs with hooks, signing and rerere off and ssh `ConnectTimeout=15`/`BatchMode`
  (unless an own ssh command is set). SIGTERM is honoured between git commands, never by killing
  git mid-way; git lock files over a minute old are cleared once book-sync holds the book.
- Windows: locks are msvcrt's instead of flock, git runs with CREATE_NO_WINDOW (X0urnal++ starts
  book-sync through pythonw, which has no console), drivers are `<python> <script>`, and each book
  sets `core.autocrlf=false`. X0urnal++ takes the script from `%USERPROFILE%\bin\book-sync`, never
  from PATH: PATH answers `book-sync.cmd`, which pythonw cannot run.
