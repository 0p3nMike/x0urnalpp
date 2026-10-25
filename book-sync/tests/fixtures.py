"""Small synthetic books for the book-sync tests, built in code (no binary fixtures to go stale).

A page here is a few strokes and text boxes, written the way X0urnal++ writes them: gzipped,
with a real mtime in the gzip header (so the bytes change on every save even when the XML does
not -- exactly what the clean filter has to see through).
"""

try:
    import fcntl
except ImportError:          # Windows
    fcntl = None
    import msvcrt
import gzip
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import booklib  # noqa: E402

BOOK_SYNC = os.path.join(TOOLS, "book-sync")
# how to start it: Windows runs a script only through its interpreter
BS = [sys.executable, BOOK_SYNC] if os.name == "nt" else [BOOK_SYNC]
WINDOWS = os.name == "nt"


def tool(name):
    """The command book-sync writes into a book's git config for one of its drivers (book-sync
    tool_cmd): the script itself, or on Windows "<python> <script>" with forward slashes."""
    import shlex
    p = os.path.join(TOOLS, name)
    if WINDOWS:
        return "%s %s" % (shlex.quote(sys.executable.replace("\\", "/")), shlex.quote(p.replace("\\", "/")))
    return p


def stroke(points, color="#000000ff", width="1.41", tool="pen"):
    pts = " ".join("%g" % p for p in points)
    return '<stroke tool="%s" color="%s" width="%s">%s</stroke>' % (tool, color, width, pts)


def text(words, x=10, y=20, color="#000000ff", size=12):
    return '<text font="Sans" size="%s" x="%s" y="%s" color="%s">%s</text>' % (size, x, y, color, words)


def page_xml(layers, width=595.27559, height=841.88976, title="Xournal++ document - see https://xournalpp.github.io/"):
    """layers: a list of lists of element strings (one page). Returns the XML as bytes."""
    out = ['<?xml version="1.0" standalone="no"?>',
           '<xournal creator="xournalpp 1.3.7" fileversion="4">',
           "<title>%s</title>" % title,
           '<page width="%s" height="%s">' % (width, height),
           '<background type="solid" color="#ffffffff" style="graph"/>']
    for layer in layers:
        out.append("<layer>")
        out.extend(layer)
        out.append("</layer>")
    out += ["</page>", "</xournal>", ""]
    return "\n".join(out).encode()


def gz(data):
    # like X0urnal++: real mtime in the header, so equal XML gives different bytes
    return gzip.compress(data, mtime=int(time.time()) + random.randint(0, 10 ** 6))


S1 = stroke([10, 10, 20, 20, 30, 30])
S2 = stroke([40, 40, 50, 50, 60, 66.5])
S3 = stroke([100, 100, 110, 120, 130, 140], color="#ff0000ff")
T1 = text("hello", x=10, y=200)


def tree_xml(title, chapters):
    """chapters: [(chapter id, chapter title, [(section id, section title), ...]), ...]"""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', '<notebook version="2" title="%s">' % title]
    for cid, ctitle, secs in chapters:
        lines.append('  <chapter id="%s" title="%s">' % (cid, ctitle))
        for sid, stitle in secs:
            lines.append('    <section id="%s" title="%s" file="sections/%s.xopp"/>' % (sid, stitle, sid))
        lines.append("  </chapter>")
    lines += ["</notebook>", ""]
    return "\n".join(lines).encode()


class BookTest(unittest.TestCase):
    """Each test gets its own scratch folder: a "forge" of bare repos and one home per device."""

    def setUp(self):
        parent = os.environ.get("BOOKSYNC_TEST_TMP") or None
        if parent:
            os.makedirs(parent, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="bs-%s-" % self._testMethodName[:30], dir=parent)
        self.forge = os.path.join(self.tmp, "forge")
        os.makedirs(self.forge)

    def tearDown(self):
        if os.environ.get("BOOKSYNC_KEEP"):
            return
        shutil.rmtree(self.tmp, ignore_errors=True)

    # -- environment of one device
    def home(self, device):
        return os.path.join(self.tmp, "home-" + device)

    def nbdir(self, device):
        return os.path.join(self.home(device), "Documents", "notebooks")

    def env(self, device, **extra):
        e = dict(os.environ)
        for k in list(e):
            if k.startswith("GIT_") or k.startswith("BOOK_SYNC_"):
                del e[k]
        e.update({
            "HOME": self.home(device),
            # git's global config is $XDG_CONFIG_HOME/git/config when ~/.gitconfig is missing:
            # point it into the device's home too, so no test reads or writes the real one
            "XDG_CONFIG_HOME": os.path.join(self.home(device), ".config"),
            "GIT_CONFIG_NOSYSTEM": "1",
            "BOOK_SYNC_DEVICE": device,
            "BOOK_SYNC_REMOTE_BASE": self.forge,
            "BOOK_SYNC_NOTEBOOKS": self.nbdir(device),
            "BOOK_SYNC_GRACE": "0",
            "GIT_AUTHOR_NAME": "test", "GIT_AUTHOR_EMAIL": "test@test",
            "GIT_COMMITTER_NAME": "test", "GIT_COMMITTER_EMAIL": "test@test",
        })
        e.update({k: str(v) for k, v in extra.items()})
        os.makedirs(self.home(device), exist_ok=True)
        return e

    def bs(self, device, *args, expect_rc=None, **env):
        """Run book-sync as `device`; check it printed exactly one JSON line; return it."""
        r = subprocess.run(BS + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=self.env(device, **env), timeout=120)
        lines = r.stdout.decode().splitlines()
        self.assertEqual(len(lines), 1, "stdout must be exactly one JSON line, got %r (stderr %s)" % (lines, r.stderr.decode()))
        res = json.loads(lines[0])
        for k in ("state", "time", "changed", "conflicts", "message"):
            self.assertIn(k, res)
        self.assertEqual(r.returncode, 1 if res["state"] == "error" else 0, res)
        if expect_rc is not None:
            self.assertEqual(r.returncode, expect_rc, res)
        res["_stderr"] = r.stderr.decode()
        return res

    def git(self, book, *args, device="test"):
        r = subprocess.run(["git", "-C", book] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=self.env(device), timeout=60)
        if r.returncode != 0:
            raise AssertionError("git %s failed: %s" % (" ".join(args), r.stderr.decode()))
        return r.stdout.decode()

    # -- books
    def make_book(self, device="tablet", name="Test_Book", pages=None, chapters=None):
        """A folder book on `device` with chapter Ch1 = p1 (S1, S2, T1) and p2 (S3), set up and sent
        to the forge with `init`."""
        book = os.path.join(self.nbdir(device), name + ".book")
        os.makedirs(os.path.join(book, "sections"))
        pages = pages if pages is not None else {"p1": [[S1, S2, T1]], "p2": [[S3]]}
        for sid, layers in pages.items():
            self.write_page(book, sid, layers)
        chapters = chapters if chapters is not None else [("c1", "Ch1", [(s, "page " + s) for s in pages])]
        with open(os.path.join(book, "notebook.xml"), "wb") as f:
            f.write(tree_xml(name, chapters))
        res = self.bs(device, "init", book)
        self.assertEqual(res["state"], "synced", res)
        return book

    def clone(self, device, slug="test-book"):
        res = self.bs(device, "clone", slug)
        self.assertEqual(res["state"], "synced", res)
        return res["book"]

    def pair(self):
        a = self.make_book("tablet")
        b = self.clone("laptop")
        return a, b

    @staticmethod
    def write_page(book, sid, layers, folder="sections", raw=None, **kw):
        path = os.path.join(book, folder, sid + ".xopp")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(raw if raw is not None else gz(page_xml(layers, **kw)))
        return path

    @staticmethod
    def read_page(book, sid, folder="sections"):
        with open(os.path.join(book, folder, sid + ".xopp"), "rb") as f:
            data = f.read()
        return booklib.clean_bytes(data).decode()

    @staticmethod
    def tree(book):
        with open(os.path.join(book, "notebook.xml"), "rb") as f:
            return booklib.parse_notebook(f.read())

    def tree_ids(self, book):
        idx, order = booklib.index_notebook(self.tree(book))
        return idx, order

    @staticmethod
    def hold_lock(book, name="book-sync.lock"):
        """Hold a book's lock the way book-sync (or X0urnal++, name="x0urnal.lock") does."""
        fd = os.open(os.path.join(book, ".git", name), os.O_RDWR | os.O_CREAT, 0o644)
        if fcntl is not None:
            fcntl.flock(fd, fcntl.LOCK_EX)
        else:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_LOCK, 1)
        return fd
