# X0urnal++

**A OneNote replacement built on [Xournal++](https://github.com/xournalpp/xournalpp).** Handwritten
notes on an infinite canvas, in notebooks with chapters and pages, saved as you write and synced
across your devices through any git server -- and your OneNote notebooks come along, with the
handwriting still editable.

X0urnal++ is a fork of Xournal++ 1.3.7. Everything Xournal++ does still works (PDF annotation,
layers, LaTeX, export); this adds what was missing to leave OneNote:

- **Infinite canvas.** Journal → Infinite Canvas turns a page into a whiteboard that grows as you
  write near its edge (right and down). Only the part on screen is rendered, so a huge page costs
  about as much memory as an A4 one. It exports to PDF at the size of what is drawn on it.
- **Notebooks with chapters and pages.** File → New Notebook / Open Notebook. A Notebook tab in
  the sidebar shows the tree: tap a page to open it, tap a chapter to fold it, press and hold a row
  to rename it or move it. A book switcher at the top lists every notebook on the device.
- **Autosave, no Save button.** A page is saved two seconds after you stop writing, when you
  switch pages, when the window loses focus and when you close it. Nothing ever asks.
- **Sync across devices over git.** Every device keeps a full copy; [book-sync](../book-sync/)
  merges edits per stroke, so two devices writing on the same page both keep their ink. When it
  really can't decide, you get a conflict page next to the original, as in OneNote. Works with
  GitHub, GitLab, Forgejo/Gitea, or a folder on a server or a USB stick.
- **OneNote import.** [one2book](../one2book/) converts OneNote notebooks (folders of `.one`
  files, or a `.onepkg` export) into X0urnal++ notebooks: ink as real pen strokes, pictures and
  PDF printouts in place, typed text as text, the same sections and page order.
- **Touch and pen friendly.** The menus open with a finger or a pen (they are popovers), sidebar
  rows are finger-sized, and X0urnal++ starts on the notebook and page you were last on.

Notebooks are plain folders of ordinary `.xopp` files: every page opens in stock Xournal++ too, so
nothing is locked in.

## The theme

X0urnal++ ships with a dark theme of its own, and it is the default: black graph paper with a
blue grid, off-white ink, square 1px-ruled chrome with Win98-style bevels, and the
[BigBlueTerm437 Nerd Font](https://www.nerdfonts.com/font-downloads) for the interface and for
typed text. Install that font (Arch: `ttf-bigblueterminal-nerd`; elsewhere from Nerd Fonts) or
the interface falls back to a default font.

- New pages are black graph paper; a page's colour and background stay per page (Journal → Paper
  Color / Paper Background), and documents with white pages keep them.
- The theme's colours can follow a theme switcher: X0urnal++ reads `~/.config/sw4tch/house-map`
  (lines of `<theme hex> <replacement hex>`, as written by
  [sw4tch](https://github.com/0p3nMike/sw4tch)) and re-colours live when it changes. Without the
  file it looks as shipped.
- `X0URNAL_FRAMELESS=1` hides the title bar, for tiling window managers that draw their own.

## Build and install

### Linux

Install Xournal++'s build dependencies (from [readme/LinuxBuild.md](../readme/LinuxBuild.md)):

```sh
# Arch
sudo pacman -S --needed base-devel cmake ninja git gtk3 libxml2 portaudio libsndfile poppler-glib \
  gettext libzip lua gtksourceview4 help2man qpdf librsvg python
# Debian / Ubuntu
sudo apt-get install build-essential cmake ninja-build git libgtk-3-dev libpoppler-glib-dev \
  portaudio19-dev libsndfile-dev libxml2-dev liblua5.3-dev libzip-dev librsvg2-dev gettext \
  libgtksourceview-4-dev help2man libqpdf-dev python3
# Fedora
sudo dnf install gcc-c++ cmake ninja-build git gtk3-devel libxml2-devel portaudio-devel \
  libsndfile-devel poppler-glib-devel gettext libzip-devel librsvg2-devel lua-devel \
  gtksourceview4-devel help2man qpdf-devel python3
```

Then build and install into `/opt/X0urnal++`, beside any Xournal++ your distribution ships
(settings live in `~/.config/X0urnal++`, so the two never mix):

```sh
git clone https://github.com/0p3nMike/x0urnalpp.git && cd x0urnalpp
x0urnal/build-linux.sh --tools      # --tools also links book-sync into ~/.local/bin
/opt/X0urnal++/bin/X0urnal++
```

On Arch, `x0urnal/arch/PKGBUILD` builds a package instead (`cd x0urnal/arch && makepkg -si`): the
app in `/opt/X0urnal++`, a launcher entry, and `book-sync` on PATH.

### Windows

1. Install [MSYS2](https://www.msys2.org) (to `C:\msys64`, or set `MSYS2_ROOT`),
   [PowerShell 7](https://aka.ms/powershell) and [Python 3.11+](https://www.python.org) (for
   sync; `python` and `pythonw` on PATH), and the BigBlueTerm437 Nerd Font.
2. In PowerShell 7, from a clone of this repository:

   ```powershell
   pwsh -File x0urnal\build-windows.ps1
   ```

   It installs the MSYS2 packages it needs, builds, bundles the GTK runtime into
   `%USERPROFILE%\X0urnal++` and copies book-sync into `%USERPROFILE%\bin`.
3. Start `%USERPROFILE%\X0urnal++\bin\X0urnal++.exe`.

## Notebooks

File → New Notebook asks for a name and creates `Documents/notebooks/<Name>.book`, a folder:

    Notes.book/
      notebook.xml          the tree of chapters and pages
      sections/<id>.xopp    one page per file
      trash/                removed pages

Pages added with **+ Page** are infinite black graph paper. Removing a page or chapter moves it
to the book's `trash/`; with sync on, every version of every page also stays in the book's git
history. Started without a file, X0urnal++ opens the notebook you were last in, on the page you
were last on. Ordinary `.xopp` and PDF files open as always.

## Sync across devices

Pick a git server and give book-sync the place for the book repos once per device (one private
repo per book, `book-<name>`):

```sh
git config --global book-sync.remote git@github.com:YOU      # or git@git.example.com:YOU, ...
```

On GitHub, create each book's repo first as an empty **private** repo (`gh repo create
YOU/book-notes --private`); Forgejo, Gitea and GitLab create it on the first push. Then:

```sh
book-sync init ~/Documents/notebooks/Notes.book    # on the first device (New Notebook does this)
book-sync clone notes                              # once on every other device
```

From then on X0urnal++ syncs by itself: after you write, when you switch pages or windows, and
from the Sync button in the Notebook tab, which also shows the sync status. Offline is fine: the
changes go out at the next sync. Details, the merge rules and every command:
[book-sync/README.md](../book-sync/README.md).

## Import from OneNote

1. Get the notebook out of OneNote: a notebook stored on the computer is a folder of `.one` files
   (by default in `Documents\OneNote Notebooks`); one stored in OneDrive is exported with
   File → Export → Notebook → OneNote Package (`.onepkg`).
2. Convert it ([Rust](https://rustup.rs) needed):

   ```sh
   cd one2book && cargo build --release
   target/release/one2book "/path/to/My Notebook" ~/Documents/notebooks/My_Notebook.book
   ```

   It prints what every section became, including anything it could only approximate.
3. Open it in X0urnal++ (File → Open Notebook), and `book-sync init My_Notebook` to sync it.

Still using OneNote while you switch? `one2book/one2book-sync` converts again whenever you run it
and merges the changes into the synced book instead of replacing it, so you can write on both
sides. See [one2book/README.md](../one2book/README.md).

## Following Xournal++ releases

The `x0urnal` branch is Xournal++'s release tag with the X0urnal++ commits on top, so it moves to
a new release with one rebase. Every change inside an upstream file is marked with a
`X0urnal++` comment, which makes conflicts easy to place.

```sh
git remote add upstream https://github.com/xournalpp/xournalpp.git   # once
git fetch upstream --tags
git switch x0urnal
git rebase --onto v1.3.8 v1.3.7          # from the release it is on, to the new one
git grep -n 'X0urnal++' -- src ui         # every fork hunk, to check after conflicts
x0urnal/build-linux.sh && (cd book-sync && ./run-tests) && (cd one2book && cargo test)
git tag x0urnal-1.3.8-1
git push --force-with-lease origin x0urnal x0urnal-1.3.8-1
```

`book-sync/` and `one2book/` are folders of their own, so a rebase never touches them.

## What is where

| | |
|---|---|
| `src/`, `ui/`, `palettes/`, ... | Xournal++ with the X0urnal++ changes |
| `x0urnal/` | build scripts for Linux and Windows, the Arch package |
| `book-sync/` | the sync tool and its git merge drivers (Python) |
| `one2book/` | the OneNote converter (Rust) |
| `README.md` | Xournal++'s own README |

## Licences

- X0urnal++ (everything outside `book-sync/` and `one2book/`) is a modified Xournal++ and, like
  it, licensed under the GNU General Public License version 2 or later (`LICENSE`).
- `book-sync/` and `one2book/` are MIT licensed (their `LICENSE` files).
- `one2book/vendor/onenote_parser` is [onenote.rs](https://github.com/msiemens/onenote.rs) 2.1.0
  with a parser fix, under its own licence, the Mozilla Public License 2.0.

X0urnal++ is not affiliated with the Xournal++ project or with Microsoft. OneNote is a trademark
of Microsoft.
