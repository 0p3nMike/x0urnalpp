"""End-to-end tests: real git repos, book-sync run as a program, the drivers and the filter
called by git. The "forge" is a folder of bare repos (BOOK_SYNC_REMOTE_BASE); "devices" are
separate homes with BOOK_SYNC_DEVICE set. Nothing here touches the network."""

import gzip
import json
import os
import stat
import subprocess
import time
import unittest

from fixtures import BOOK_SYNC, BS, tool, S1, S2, S3, T1, TOOLS, BookTest, gz, page_xml, stroke, text, tree_xml
import booklib as bl


class Setup(BookTest):
    def test_setup_idempotent(self):
        book = os.path.join(self.nbdir("tablet"), "Fresh.book")
        os.makedirs(os.path.join(book, "sections"))
        self.write_page(book, "p1", [[S1]])
        r = self.bs("tablet", "setup", book)
        self.assertEqual(r["state"], "local")
        snap = {}
        for f in (".git/config", ".gitattributes", ".gitignore"):
            with open(os.path.join(book, f), "rb") as fh:
                snap[f] = (fh.read(), os.stat(os.path.join(book, f)).st_mtime_ns)
        self.bs("tablet", "setup", book)
        self.bs("tablet", "setup", book)
        for f, (data, mtime) in snap.items():
            with open(os.path.join(book, f), "rb") as fh:
                self.assertEqual(fh.read(), data, f)
            self.assertEqual(os.stat(os.path.join(book, f)).st_mtime_ns, mtime, f + " was rewritten")
        with open(os.path.join(book, ".gitattributes")) as fh:
            self.assertEqual(fh.read(), "sections/*.xopp  merge=xopp  filter=xopp\n"
                                        "trash/*.xopp     merge=xopp  filter=xopp\n"
                                        "notebook.xml     merge=notebook\n")
        with open(os.path.join(book, ".gitignore")) as fh:
            self.assertEqual(fh.read().split(), ["*~", ".*.autosave.xopp", "*.tmp", ".book-sync/"])
        cfg = self.git(book, "config", "--local", "--list")
        self.assertIn("user.name=tablet", cfg)
        self.assertIn("merge.xopp.driver=%s %%O %%A %%B %%P" % tool("xopp-merge"), cfg)
        self.assertIn("filter.xopp.process=%s process" % tool("xopp-filter"), cfg)

    @unittest.skipIf(os.name == "nt", "symlinks need admin on Windows, and the driver command there is '<python> <script>'")
    def test_setup_through_symlinks_writes_real_paths(self):
        """Installed as symlinks in ~/bin: the config must name the real files."""
        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir)
        for n in ("book-sync", "xopp-merge", "notebook-merge", "xopp-filter"):
            os.symlink(os.path.join(TOOLS, n), os.path.join(bindir, n))
        book = os.path.join(self.nbdir("laptop"), "Linked.book")
        os.makedirs(book)
        r = subprocess.run([os.path.join(bindir, "book-sync"), "setup", book], stdout=subprocess.PIPE,
                           env=self.env("laptop"), timeout=60)
        self.assertEqual(json.loads(r.stdout)["state"], "local")
        cfg = self.git(book, "config", "--get", "merge.notebook.driver")
        self.assertTrue(cfg.startswith(os.path.join(TOOLS, "notebook-merge")), cfg)
        self.assertNotIn(bindir, self.git(book, "config", "--local", "--list"))

    def test_existing_gitattributes_kept(self):
        book = os.path.join(self.nbdir("tablet"), "Other.book")
        os.makedirs(book)
        with open(os.path.join(book, ".gitattributes"), "w") as f:
            f.write("# mine\nsections/*.xopp merge=xopp filter=xopp\n*.png binary\n")
        self.bs("tablet", "setup", book)
        with open(os.path.join(book, ".gitattributes")) as f:
            got = f.read()
        self.assertTrue(got.startswith("# mine\nsections/*.xopp merge=xopp filter=xopp\n*.png binary\n"))
        self.assertIn("trash/*.xopp     merge=xopp  filter=xopp", got)
        self.assertIn("notebook.xml     merge=notebook", got)
        self.assertEqual(got.count("sections/*.xopp"), 1)


class Filter(BookTest):
    def test_round_trip(self):
        a, b = self.pair()
        # in git: plain XML
        blob = self.git(a, "cat-file", "-p", "HEAD:sections/p1.xopp")
        self.assertTrue(blob.startswith('<?xml version="1.0" standalone="no"?>'), blob[:40])
        # in the working tree of a clone: gzip, deterministic (mtime 0)
        with open(os.path.join(b, "sections", "p1.xopp"), "rb") as f:
            data = f.read()
        self.assertEqual(data[:2], b"\x1f\x8b")
        self.assertEqual(data[4:8], b"\0\0\0\0")
        self.assertEqual(gzip.decompress(data).decode(), blob)
        self.assertEqual(self.git(b, "status", "--porcelain"), "")
        # X0urnal++ re-saves the same page: new gzip bytes, same XML -> nothing to commit
        p = os.path.join(b, "sections", "p1.xopp")
        with open(p, "wb") as f:
            f.write(gzip.compress(gzip.decompress(data), mtime=12345, compresslevel=9))
        os.utime(p, (time.time() + 5, time.time() + 5))
        # the CONTENT is unchanged: git diff runs the clean filter. (Not `git status`: when the file's
        # size differs from the index's, git calls it modified without running the filter. With
        # classic zlib, levels 6 and 9 happen to give this page the same size; with zlib-ng -- Python
        # 3.14 on Windows -- they don't.)
        self.assertEqual(self.git(b, "diff", "--stat"), "")
        r = self.bs("laptop", "save", b)
        self.assertEqual(r["message"], "Nothing to save")
        # the history is plain text, so a one-stroke change is a one-line diff
        self.write_page(b, "p1", [[S1, S2, T1, S3]])
        self.bs("laptop", "save", b)
        diff = self.git(b, "show", "--format=", "HEAD")
        self.assertEqual(sum(1 for l in diff.splitlines() if l.startswith("+<")), 1, diff)

    def test_merge_result_is_smudged(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.write_page(b, "p1", [[S1, S2, T1, stroke([1, 2, 3, 4])]])
        self.bs("tablet", "sync", a)
        self.bs("laptop", "sync", b)
        with open(os.path.join(b, "sections", "p1.xopp"), "rb") as f:
            self.assertEqual(f.read(2), b"\x1f\x8b")
        self.assertTrue(self.git(b, "cat-file", "-p", "HEAD:sections/p1.xopp").startswith("<?xml"))
        self.assertEqual(self.git(b, "status", "--porcelain"), "")


class Merging(BookTest):
    def converge(self, a, b):
        ra = self.bs("tablet", "sync", a)
        rb = self.bs("laptop", "sync", b)
        rc = self.bs("tablet", "sync", a)
        self.assertEqual(self.git(a, "rev-parse", "HEAD"), self.git(b, "rev-parse", "HEAD"))
        return ra, rb, rc

    def test_different_pages(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.write_page(b, "p2", [[S3, S1]])
        ra, rb, rc = self.converge(a, b)
        self.assertEqual([r["state"] for r in (ra, rb, rc)], ["synced"] * 3)
        self.assertEqual(rb["changed"], ["sections/p1.xopp"])
        self.assertEqual(rc["changed"], ["sections/p2.xopp"])
        self.assertEqual(rb["conflicts"], [])
        for book in (a, b):
            self.assertEqual(self.read_page(book, "p1").count("<stroke"), 3)
            self.assertEqual(self.read_page(book, "p2").count("<stroke"), 2)
        self.assertEqual(self.git(b, "log", "-1", "--format=%an %s"), "laptop laptop: 1 pages\n")

    def test_same_page_different_strokes(self):
        a, b = self.pair()
        new_a = stroke([200, 200, 210, 210], color="#00ff00ff")
        new_b = stroke([300, 300, 310, 310], color="#0000ffff")
        self.write_page(a, "p1", [[S1, S2, T1, new_a]])
        self.write_page(b, "p1", [[S1, S2, T1, new_b]])
        ra, rb, rc = self.converge(a, b)
        self.assertEqual(rb["state"], "synced")
        self.assertEqual(rb["conflicts"], [])
        self.assertIn("Merged from tablet", rb["message"])
        for book in (a, b):
            page = self.read_page(book, "p1")
            self.assertIn("#00ff00ff", page)
            self.assertIn("#0000ffff", page)
            self.assertEqual(page.count("<stroke"), 4)
            self.assertEqual(len([x for x in os.listdir(os.path.join(book, "sections")) if "conflict" in x]), 0)

    def test_erase_stays_erased(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S2, T1]])                    # erased S1
        self.write_page(b, "p1", [[S1, S2, T1, S3]])            # drew S3
        self.converge(a, b)
        for book in (a, b):
            page = self.read_page(book, "p1")
            self.assertNotIn(">10 10 20 20 30 30<", page)
            self.assertIn("#ff0000ff", page)

    def test_text_conflict(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, text("hello from tablet", x=10, y=200)]])
        self.write_page(b, "p1", [[S1, S2, text("hello from laptop", x=10, y=200)]])
        ra = self.bs("tablet", "sync", a)
        rb = self.bs("laptop", "sync", b)
        self.assertEqual(rb["state"], "conflict", rb)
        self.assertEqual(len(rb["conflicts"]), 1)
        cid = rb["conflicts"][0]
        self.assertTrue(cid.startswith("p1-conflict-tablet-"), cid)
        self.assertIn("notebook.xml", rb["changed"])
        self.assertIn("sections/%s.xopp" % cid, rb["changed"])
        # the original keeps ours (laptop's), the conflict page holds theirs (tablet's)
        self.assertIn("hello from laptop", self.read_page(b, "p1"))
        self.assertIn("hello from tablet", self.read_page(b, cid))
        idx, order = self.tree_ids(b)
        self.assertEqual(order["c1"], ["p1", cid, "p2"])
        self.assertRegex(idx[cid].node.title(), r"^page p1 \(conflict from tablet, \d+ \w{3} \d\d:\d\d\)$")
        self.assertEqual(self.bs("laptop", "status", b)["state"], "conflict")
        # tablet gets it too
        rc = self.bs("tablet", "sync", a)
        self.assertIn("sections/p1.xopp", rc["changed"])
        self.assertEqual(self.bs("tablet", "status", a)["conflicts"], [cid])
        # resolving: Remove in the sidebar = node out of the tree, file to the trash
        os.makedirs(os.path.join(a, "trash"), exist_ok=True)
        os.rename(os.path.join(a, "sections", cid + ".xopp"), os.path.join(a, "trash", cid + ".xopp"))
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children = [c for c in nb.children[0].children if c.id != cid]
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.assertEqual(self.bs("tablet", "sync", a)["state"], "synced")
        self.assertEqual(self.bs("tablet", "status", a)["state"], "synced")
        self.assertEqual(self.bs("laptop", "sync", b)["state"], "synced")
        self.assertEqual(self.bs("laptop", "status", b)["conflicts"], [])

    def test_deleted_on_a_edited_on_b(self):
        """Both orders: the deleting side syncs first, and the editing side syncs first."""
        for first in ("delete", "edit"):
            with self.subTest(first=first):
                self.tearDown()
                self.setUp()
                a, b = self.pair()
                # tablet deletes p2 (X0urnal++: node out of the tree, file into trash/)
                os.makedirs(os.path.join(a, "trash"))
                os.rename(os.path.join(a, "sections", "p2.xopp"), os.path.join(a, "trash", "p2.xopp"))
                with open(os.path.join(a, "notebook.xml"), "rb") as f:
                    nb = bl.parse_notebook(f.read())
                nb.children[0].children = [c for c in nb.children[0].children if c.id != "p2"]
                with open(os.path.join(a, "notebook.xml"), "wb") as f:
                    f.write(bl.serialize_notebook(nb))
                # laptop edits it
                self.write_page(b, "p2", [[S3, stroke([5, 5, 6, 6], color="#abcdefff")]])
                if first == "delete":
                    self.bs("tablet", "sync", a)
                    r = self.bs("laptop", "sync", b)
                    self.bs("tablet", "sync", a)
                else:
                    self.bs("laptop", "sync", b)
                    r = self.bs("tablet", "sync", a)
                    self.bs("laptop", "sync", b)
                self.assertIn(r["state"], ("synced",), r)
                for book in (a, b):
                    self.assertTrue(os.path.exists(os.path.join(book, "sections", "p2.xopp")), (first, book))
                    self.assertIn("#abcdefff", self.read_page(book, "p2"))
                    idx, order = self.tree_ids(book)
                    self.assertEqual(order["c1"], ["p1", "p2"], first)
                    self.assertFalse(os.path.exists(os.path.join(book, "trash", "p2.xopp")))
                self.assertEqual(self.git(a, "rev-parse", "HEAD"), self.git(b, "rev-parse", "HEAD"))

    def test_trashed_and_edited_differently(self):
        """tablet edits p1's text and then deletes the page; laptop edits the same text another way.
        Both versions must end up in sections/ and in the tree."""
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, text("tablet's words", x=10, y=200)]])
        self.bs("tablet", "save", a)
        os.makedirs(os.path.join(a, "trash"))
        os.rename(os.path.join(a, "sections", "p1.xopp"), os.path.join(a, "trash", "p1.xopp"))
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children = [c for c in nb.children[0].children if c.id != "p1"]
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.write_page(b, "p1", [[S1, S2, text("laptop's words", x=10, y=200)]])
        self.bs("tablet", "sync", a)
        r = self.bs("laptop", "sync", b)
        self.bs("tablet", "sync", a)
        for book in (a, b):
            idx, order = self.tree_ids(book)
            self.assertIn("p1", idx)
            texts = []
            for sid in order["c1"]:
                texts.append(self.read_page(book, sid))
            joined = "".join(texts)
            self.assertIn("laptop's words", joined)
            self.assertIn("tablet's words", joined)
            self.assertEqual(sorted(os.listdir(os.path.join(book, "sections"))),
                             sorted(i + ".xopp" for i in order["c1"]))

    def test_page_file_deleted_outright_edited_other_side(self):
        a, b = self.pair()
        os.unlink(os.path.join(a, "sections", "p2.xopp"))
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children = [c for c in nb.children[0].children if c.id != "p2"]
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.write_page(b, "p2", [[S3, S1]])
        self.bs("laptop", "sync", b)
        r = self.bs("tablet", "sync", a)          # modify/delete: git leaves it unmerged
        self.assertEqual(r["state"], "synced", r)
        self.assertIn("sections/p2.xopp", r["changed"])
        self.assertEqual(self.read_page(a, "p2").count("<stroke"), 2)
        self.assertIn("p2", self.tree_ids(a)[0])

    def test_deleted_unchanged_stays_deleted(self):
        a, b = self.pair()
        os.makedirs(os.path.join(a, "trash"))
        os.rename(os.path.join(a, "sections", "p2.xopp"), os.path.join(a, "trash", "p2.xopp"))
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children = [c for c in nb.children[0].children if c.id != "p2"]
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.write_page(b, "p1", [[S1, S2, T1, S3]])      # laptop edits another page
        self.converge(a, b)
        for book in (a, b):
            self.assertNotIn("p2", self.tree_ids(book)[0])
            self.assertFalse(os.path.exists(os.path.join(book, "sections", "p2.xopp")))
            self.assertTrue(os.path.exists(os.path.join(book, "trash", "p2.xopp")))

    def test_both_add_pages(self):
        a, b = self.pair()
        for book, dev, sid in ((a, "tablet", "tablet-mg8k2x1a-7q3c"), (b, "laptop", "laptop-mg8k2x9z-0abc")):
            self.write_page(book, sid, [[S1]], title="new on " + dev)
            with open(os.path.join(book, "notebook.xml"), "rb") as f:
                nb = bl.parse_notebook(f.read())
            nb.children[0].children.append(bl.Node("section", {"id": sid, "title": "new on " + dev, "file": bl.section_file(sid)}))
            with open(os.path.join(book, "notebook.xml"), "wb") as f:
                f.write(bl.serialize_notebook(nb))
        ra, rb, rc = self.converge(a, b)
        self.assertEqual(rb["state"], "synced")
        for book in (a, b):
            idx, order = self.tree_ids(book)
            self.assertEqual(order["c1"], ["p1", "p2", "laptop-mg8k2x9z-0abc", "tablet-mg8k2x1a-7q3c"])
            self.assertNotIn("recovered", idx)

    def test_rename_one_side(self):
        a, b = self.pair()
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].attrs["title"] = "Chapter One"
        nb.children[0].children[1].attrs["title"] = "renamed"
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.write_page(b, "p1", [[S1, S2, T1, S3]])
        # laptop also adds a page to the tree, so both sides changed notebook.xml (the driver runs)
        self.write_page(b, "laptop-new-0001", [[S2]])
        with open(os.path.join(b, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children.append(bl.Node("section", {"id": "laptop-new-0001", "title": "x", "file": "sections/laptop-new-0001.xopp"}))
        with open(os.path.join(b, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        ra, rb, rc = self.converge(a, b)
        self.assertIn("notebook.xml", rb["changed"])
        for book in (a, b):
            idx, order = self.tree_ids(book)
            self.assertEqual(idx["c1"].node.title(), "Chapter One")
            self.assertEqual(idx["p2"].node.title(), "renamed")
            self.assertEqual(order["c1"], ["p1", "p2", "laptop-new-0001"])
            self.assertEqual(self.read_page(book, "p1").count("<stroke"), 3)

    def test_renamed_differently_on_both(self):
        a, b = self.pair()
        for book, t in ((a, "tablet's name"), (b, "laptop's name")):
            with open(os.path.join(book, "notebook.xml"), "rb") as f:
                nb = bl.parse_notebook(f.read())
            nb.children[0].children[0].attrs["title"] = t
            with open(os.path.join(book, "notebook.xml"), "wb") as f:
                f.write(bl.serialize_notebook(nb))
        self.bs("tablet", "sync", a)
        rb = self.bs("laptop", "sync", b)
        self.assertEqual(self.tree_ids(b)[0]["p1"].node.title(), "laptop's name")
        self.assertIn("tablet's name", rb["message"])

    def test_changed_lists_added_modified_deleted(self):
        a, b = self.pair()
        os.unlink(os.path.join(a, "sections", "p2.xopp"))
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children = [nb.children[0].children[0], bl.Node("section", {"id": "n1", "title": "n", "file": "sections/n1.xopp"})]
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.write_page(a, "n1", [[S1]])
        self.write_page(a, "p1", [[S1]])
        self.bs("tablet", "sync", a)
        r = self.bs("laptop", "sync", b)           # a fast-forward on laptop
        self.assertEqual(r["changed"], ["notebook.xml", "sections/n1.xopp", "sections/p1.xopp", "sections/p2.xopp"])
        self.assertIn("Updated from tablet", r["message"])


class Network(BookTest):
    def fake_ssh(self):
        path = os.path.join(self.tmp, "fake-ssh")
        with open(path, "w") as f:
            f.write("#!/bin/sh\necho 'ssh: connect to host fakeforge port 22: Network is unreachable' >&2\nexit 255\n")
        os.chmod(path, 0o755)
        return path.replace("\\", "/")      # git runs it through sh, which would eat Windows backslashes

    def test_offline_keeps_local_commit(self):
        a = self.make_book("tablet")
        self.git(a, "remote", "set-url", "origin", "fakeforge:books/book-test-book.git")
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        before = self.git(a, "rev-parse", "HEAD")
        r = self.bs("tablet", "sync", a, GIT_SSH_COMMAND=self.fake_ssh())
        self.assertEqual(r["state"], "offline", r)
        self.assertIn("Network is unreachable", r["message"])
        self.assertEqual(r["ahead"], 1)
        self.assertNotEqual(self.git(a, "rev-parse", "HEAD"), before)       # the local commit
        self.assertEqual(self.git(a, "status", "--porcelain"), "")
        self.assertEqual(self.git(a, "log", "-1", "--format=%s"), "tablet: 1 pages\n")
        st = self.bs("tablet", "status", a)
        self.assertEqual((st["state"], st["ahead"]), ("offline", 1))
        r = self.bs("tablet", "push", a, GIT_SSH_COMMAND=self.fake_ssh())
        self.assertEqual(r["state"], "offline")
        # back online: the commit goes out
        self.git(a, "remote", "set-url", "origin", os.path.join(self.forge, "book-test-book.git"))
        r = self.bs("tablet", "sync", a)
        self.assertEqual((r["state"], r["ahead"]), ("synced", 0))

    def test_no_remote_configured(self):
        """No BOOK_SYNC_REMOTE_BASE and no git config: init keeps the book local and says how to
        configure one; clone fails with the same hint."""
        book = os.path.join(self.nbdir("tablet"), "Alone.book")
        os.makedirs(os.path.join(book, "sections"))
        self.write_page(book, "p1", [[S1]])
        with open(os.path.join(book, "notebook.xml"), "wb") as f:
            f.write(tree_xml("Alone", [("c1", "Ch1", [("p1", "page p1")])]))
        r = self.bs("tablet", "init", book, BOOK_SYNC_REMOTE_BASE="")
        self.assertEqual(r["state"], "local", r)
        self.assertIn("book-sync.remote", r["message"])
        self.assertEqual(self.git(book, "remote"), "")
        self.assertEqual(self.git(book, "log", "-1", "--format=%s"), "tablet: 1 pages\n")
        r = self.bs("laptop", "clone", "alone", BOOK_SYNC_REMOTE_BASE="")
        self.assertEqual(r["state"], "error")
        self.assertIn("no remote configured", r["message"])
        # configured afterwards (git config this time): init sends it, another device clones it
        self.git(book, "config", "--global", "book-sync.remote", self.forge, device="tablet")
        r = self.bs("tablet", "init", book, BOOK_SYNC_REMOTE_BASE="")
        self.assertEqual(r["state"], "synced", r)

    def test_status(self):
        book = os.path.join(self.nbdir("tablet"), "Local.book")
        os.makedirs(book)
        r = self.bs("tablet", "status", book)
        self.assertEqual(r["state"], "local")
        self.bs("tablet", "setup", book)
        self.assertEqual(self.bs("tablet", "sync", book)["state"], "local")
        a, b = self.pair()
        st = self.bs("laptop", "status", b)
        self.assertEqual((st["state"], st["ahead"], st["dirty"], st["busy"]), ("synced", 0, False, False))
        self.write_page(b, "p1", [[S1]])
        st = self.bs("laptop", "status", b)
        self.assertTrue(st["dirty"])
        self.bs("laptop", "save", b)
        st = self.bs("laptop", "status", b)
        self.assertEqual((st["ahead"], st["dirty"]), (1, False))
        self.assertIn("1 change not on the server yet", st["message"])
        fd = self.hold_lock(b)
        try:
            self.assertTrue(self.bs("laptop", "status", b)["busy"])
        finally:
            os.close(fd)

    def test_sync_from_outside_only_sends_a_book_open_in_x0urnal(self):
        """X0urnal++ holds .git/x0urnal.lock while a book is open. A sync started by anything but
        that X0urnal++ (here: this test) must not merge under the window -- only send."""
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.assertEqual(self.bs("tablet", "push", a)["state"], "synced")
        self.write_page(b, "p2", [[S3, S1]])
        before = self.read_page(b, "p1") if os.path.exists(os.path.join(b, "sections", "p1.xopp")) else None
        fd = self.hold_lock(b, "x0urnal.lock")
        try:
            r = self.bs("laptop", "sync", b)
            self.assertEqual(r["state"], "behind", r)
            self.assertIn("Open in X0urnal++", r["message"])
            self.assertEqual(r["changed"], [])
            now = self.read_page(b, "p1") if os.path.exists(os.path.join(b, "sections", "p1.xopp")) else None
            self.assertEqual(now, before, "merged under the window")
        finally:
            os.close(fd)                    # closing it drops the lock, on both systems
        r = self.bs("laptop", "sync", b)       # closed: an ordinary sync merges again
        self.assertEqual(r["state"], "synced", r)
        self.assertEqual(self.read_page(b, "p1"), self.read_page(a, "p1"))

    def test_sync_started_by_x0urnal_merges_the_book_it_holds(self):
        """X0urnal++ on Windows says it started book-sync in BOOK_SYNC_STARTED_BY (no /proc there to
        ask). Its own sync of the book it holds open merges as usual."""
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.assertEqual(self.bs("tablet", "push", a)["state"], "synced")
        fd = self.hold_lock(b, "x0urnal.lock")
        try:
            r = self.bs("laptop", "sync", b, BOOK_SYNC_STARTED_BY="X0urnal++")
            self.assertEqual(r["state"], "synced", r)
            self.assertNotIn("Open in X0urnal++", r["message"])
            self.assertEqual(self.read_page(b, "p1"), self.read_page(a, "p1"))
        finally:
            os.close(fd)

    def test_push_never_touches_the_working_tree(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.assertEqual(self.bs("tablet", "push", a)["state"], "synced")
        self.write_page(b, "p2", [[S3, S1]])
        snap = {}
        for root, _, files in os.walk(b):
            if os.sep + ".git" in root or "/.git" in root:
                continue
            for n in files:
                p = os.path.join(root, n)
                st = os.stat(p)
                snap[p] = (st.st_mtime_ns, st.st_ino, st.st_size)
        r = self.bs("laptop", "push", b)
        self.assertEqual(r["state"], "behind", r)
        self.assertEqual(r["message"], "Tablet has newer changes")
        self.assertEqual(r["changed"], [])
        after = {}
        for root, _, files in os.walk(b):
            if os.sep + ".git" in root or "/.git" in root:
                continue
            for n in files:
                p = os.path.join(root, n)
                st = os.stat(p)
                after[p] = (st.st_mtime_ns, st.st_ino, st.st_size)
        after = {k: v for k, v in after.items() if os.sep + ".book-sync" + os.sep not in k and "/.book-sync/" not in k}
        snap = {k: v for k, v in snap.items() if os.sep + ".book-sync" + os.sep not in k and "/.book-sync/" not in k}
        self.assertEqual(after, snap)
        self.assertEqual(self.git(b, "log", "-1", "--format=%s"), "laptop: 1 pages\n")   # but committed
        self.assertEqual(self.bs("laptop", "status", b)["state"], "behind")
        r = self.bs("laptop", "sync", b)
        self.assertEqual(r["state"], "synced")
        self.assertEqual(r["changed"], ["sections/p1.xopp"])
        self.assertEqual(self.bs("tablet", "push", a)["state"], "behind")
        self.assertEqual(self.bs("tablet", "sync", a)["changed"], ["sections/p2.xopp"])
        self.assertEqual(self.bs("tablet", "push", a)["message"], "Up to date")

    def test_clone_by_slug(self):
        a = self.make_book("tablet", name="Course_Notes")
        r = self.bs("laptop", "clone", "course-notes")
        self.assertEqual(r["state"], "synced")
        self.assertEqual(r["book"], os.path.join(self.nbdir("laptop"), "Course_Notes.book"))
        self.assertEqual(self.git(r["book"], "config", "user.name"), "laptop\n")
        self.assertEqual(self.bs("laptop", "clone", "course-notes")["state"], "error")       # already there
        r2 = self.bs("laptop", "clone", "course-notes", "Copy")
        self.assertEqual(r2["book"], os.path.join(self.nbdir("laptop"), "Copy.book"))
        r3 = self.bs("laptop", "clone", "nope")
        self.assertEqual(r3["state"], "error")
        self.assertFalse(any(n.startswith(".book-sync-clone") for n in os.listdir(self.nbdir("laptop"))))

    def test_init_when_forge_already_has_the_book(self):
        a = self.make_book("tablet")
        other = os.path.join(self.nbdir("laptop"), "Test_Book.book")
        os.makedirs(os.path.join(other, "sections"))
        self.write_page(other, "laptop-x-0001", [[S2]])
        r = self.bs("laptop", "init", other)          # unrelated history: merged, not overwritten
        self.assertEqual(r["state"], "synced", r)
        idx, _ = self.tree_ids(other)
        self.assertIn("p1", idx)
        self.assertIn("laptop-x-0001", idx)            # recovered from the folder
        self.assertEqual(self.bs("tablet", "sync", a)["state"], "synced")
        self.assertTrue(os.path.exists(os.path.join(a, "sections", "laptop-x-0001.xopp")))


class Locking(BookTest):
    def test_sync_waits_for_the_lock(self):
        a = self.make_book("tablet")
        self.write_page(a, "p1", [[S1]])
        fd = self.hold_lock(a)
        p = subprocess.Popen(BS + ["sync", a], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=self.env("tablet", BOOK_SYNC_LOCK_TIMEOUT=20))
        try:
            time.sleep(1.5)
            self.assertIsNone(p.poll(), "sync ran while the lock was held")
            self.assertEqual(self.git(a, "log", "--format=%s").count("\n"), 1)   # nothing committed yet
        finally:
            os.close(fd)
        out, _ = p.communicate(timeout=60)
        self.assertEqual(json.loads(out)["state"], "synced")

    def test_lock_timeout_is_an_error(self):
        a = self.make_book("tablet")
        fd = self.hold_lock(a)
        try:
            t = time.time()
            r = self.bs("tablet", "sync", a, BOOK_SYNC_LOCK_TIMEOUT=1)
            self.assertGreaterEqual(time.time() - t, 1.0)
            self.assertEqual(r["state"], "error")
            self.assertIn("busy", r["message"])
        finally:
            os.close(fd)

    def test_concurrent_syncs_serialize(self):
        a, b = self.pair()
        procs = []
        for k in range(4):
            self.write_page(b, "laptop-c-%d" % k, [[S1]])
            for book, dev in ((a, "tablet"), (b, "laptop")):
                procs.append(subprocess.Popen(BS + ["sync", book], stdout=subprocess.PIPE,
                                              stderr=subprocess.PIPE, env=self.env(dev)))
            procs.append(subprocess.Popen(BS + ["save", b], stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, env=self.env("laptop")))
        for p in procs:
            out, err = p.communicate(timeout=120)
            lines = out.decode().splitlines()
            self.assertEqual(len(lines), 1, err)
            self.assertIn(json.loads(lines[0])["state"], ("synced", "local"), (lines, err.decode()))
        for book in (a, b):
            self.assertFalse(os.path.exists(os.path.join(book, ".git", "index.lock")))
            self.git(book, "fsck", "--no-progress")
        self.bs("laptop", "sync", b)
        self.bs("tablet", "sync", a)
        self.assertEqual(self.git(a, "rev-parse", "HEAD"), self.git(b, "rev-parse", "HEAD"))
        idx, _ = self.tree_ids(a)
        self.assertEqual(sum(1 for k in idx if k.startswith("laptop-c-")), 4)   # recovered, once each


class Signals(BookTest):
    @unittest.skipIf(os.name == "nt", "Windows has no SIGTERM to catch: terminate() is TerminateProcess")
    def test_sigterm_still_prints_json(self):
        a = self.make_book("tablet")
        fd = self.hold_lock(a)
        try:
            p = subprocess.Popen(BS + ["sync", a], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=self.env("tablet"))
            time.sleep(1.0)
            p.terminate()
            out, _ = p.communicate(timeout=30)
        finally:
            os.close(fd)
        self.assertEqual(p.returncode, 1)
        res = json.loads(out)
        self.assertEqual(res["state"], "error")
        self.assertIn("interrupted", res["message"])
        self.assertEqual(self.bs("tablet", "sync", a)["state"], "synced")

    def test_stale_index_lock_is_cleared(self):
        a = self.make_book("tablet")
        lock = os.path.join(a, ".git", "index.lock")
        open(lock, "w").close()
        old = time.time() - 3600
        os.utime(lock, (old, old))
        x0 = os.path.join(a, ".git", "x0urnal.lock")
        open(x0, "w").close()
        os.utime(x0, (old, old))
        self.write_page(a, "p1", [[S1]])
        r = self.bs("tablet", "sync", a)
        self.assertEqual(r["state"], "synced", r)
        self.assertFalse(os.path.exists(lock))
        self.assertTrue(os.path.exists(x0))          # X0urnal++'s window lock is not ours to clear


class Safety(BookTest):
    def test_mid_write_page_is_not_committed(self):
        a = self.make_book("tablet")
        full = gz(page_xml([[S1, S2, T1, S3]]))
        self.write_page(a, "p1", None, raw=full[: len(full) // 2])         # X0urnal++ mid-write
        self.write_page(a, "p2", [[S3, S1]])                               # a complete one
        r = self.bs("tablet", "save", a)
        self.assertEqual(r["skipped"], ["sections/p1.xopp"])
        self.assertIn("being written", r["message"])
        self.assertEqual(self.git(a, "show", "--name-only", "--format=", "HEAD").split(), ["sections/p2.xopp"])
        self.assertEqual(self.git(a, "cat-file", "-p", "HEAD:sections/p1.xopp").count("<stroke"), 2)
        r = self.bs("tablet", "push", a)
        self.assertEqual(r["skipped"], ["sections/p1.xopp"])
        r = self.bs("tablet", "sync", a)
        self.assertEqual(r["skipped"], ["sections/p1.xopp"])
        self.assertNotIn("p1", str(r["changed"]))
        # a new page mid-write is not "recovered" either, and the tree mid-write is not committed
        self.write_page(a, "half", None, raw=full[:40])
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            tree = f.read()
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(tree[:-15])
        r = self.bs("tablet", "sync", a)
        self.assertEqual(r["skipped"], ["notebook.xml", "sections/half.xopp", "sections/p1.xopp"])
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(tree)
        self.write_page(a, "p1", None, raw=full)                           # the write finished
        r = self.bs("tablet", "save", a)
        self.assertEqual(r["skipped"], ["sections/half.xopp"])
        self.assertEqual(self.git(a, "cat-file", "-p", "HEAD:sections/p1.xopp").count("<stroke"), 3)

    def test_mid_write_page_blocks_merge_as_behind(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.bs("tablet", "sync", a)
        full = gz(page_xml([[S1, S2, T1, stroke([9, 9, 8, 8])]]))
        self.write_page(b, "p1", None, raw=full[:30])                 # laptop is writing p1 right now
        self.write_page(b, "p2", [[S3, S1]])
        r = self.bs("laptop", "sync", b)
        self.assertEqual(r["state"], "behind", r)
        self.assertIn("sections/p1.xopp", r["message"])
        self.assertEqual(r["changed"], [])
        with open(os.path.join(b, "sections", "p1.xopp"), "rb") as f:
            self.assertEqual(f.read(), full[:30])                     # not touched
        self.assertEqual(self.git(b, "log", "-1", "--format=%s"), "laptop: 1 pages\n")   # p2 saved
        self.write_page(b, "p1", None, raw=full)                      # the write finished
        r = self.bs("laptop", "sync", b)
        self.assertEqual(r["state"], "synced", r)
        page = self.read_page(b, "p1")
        self.assertIn("#ff0000ff", page)                              # tablet's stroke
        self.assertIn(">9 9 8 8<", page)                              # laptop's stroke

    def test_orphan_page_recovered(self):
        a = self.make_book("tablet")
        self.write_page(a, "lost-1", [[S1]], title="My lost page")
        self.write_page(a, "lost-2", [[S2]])
        r = self.bs("tablet", "sync", a, BOOK_SYNC_GRACE=600)       # just written: left alone
        self.assertEqual(r["changed"], [])
        self.assertNotIn("recovered", self.tree_ids(a)[0])
        r = self.bs("tablet", "save", a)                            # save never touches the tree
        r = self.bs("tablet", "sync", a)
        self.assertIn("notebook.xml", r["changed"])
        self.assertIn("Recovered", r["message"])
        idx, order = self.tree_ids(a)
        self.assertEqual(order["recovered"], ["lost-1", "lost-2"])
        self.assertEqual(idx["recovered"].node.title(), "Recovered")
        self.assertEqual(idx["lost-1"].node.title(), "My lost page")
        self.assertEqual(idx["lost-2"].node.title(), "lost-2")

    def test_tree_page_whose_file_is_in_trash(self):
        a = self.make_book("tablet")
        os.makedirs(os.path.join(a, "trash"))
        os.rename(os.path.join(a, "sections", "p2.xopp"), os.path.join(a, "trash", "p2.xopp"))
        r = self.bs("tablet", "sync", a)
        self.assertTrue(os.path.exists(os.path.join(a, "sections", "p2.xopp")))
        self.assertEqual(r["changed"], ["sections/p2.xopp", "trash/p2.xopp"])

    def test_plain_git_merge_then_sync(self):
        """one2book-sync's way: a branch merged with plain `git merge` and the drivers; the next
        sync puts the conflict page into the tree."""
        a = self.make_book("tablet")
        self.git(a, "checkout", "-q", "-b", "onenote")
        self.write_page(a, "p1", [[S1, S2, text("from onenote", x=10, y=200)]])
        self.write_page(a, "p2", [[S3, stroke([7, 7, 8, 8])]])
        self.git(a, "add", "-A")
        self.git(a, "commit", "-q", "-m", "onenote import")
        self.git(a, "checkout", "-q", "main")
        self.write_page(a, "p1", [[S1, S2, text("from xournal", x=10, y=200)]])
        self.write_page(a, "p2", [[S3, S2]])
        self.bs("tablet", "save", a)
        env = self.env("tablet", BOOK_SYNC_THEIR_DEVICE="onenote")
        r = subprocess.run(["git", "-C", a, "merge", "-q", "--no-edit", "onenote"], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.read_page(a, "p2").count("<stroke"), 3)       # merged per stroke
        conflicts = [n for n in os.listdir(os.path.join(a, "sections")) if "-conflict-onenote-" in n]
        self.assertEqual(len(conflicts), 1)
        r = self.bs("tablet", "sync", a)
        self.assertEqual(r["state"], "conflict", r)
        cid = conflicts[0][:-5]
        self.assertEqual(r["conflicts"], [cid])
        self.assertIn("from onenote", self.read_page(a, cid))
        self.assertIn("from xournal", self.read_page(a, "p1"))
        self.assertEqual(self.tree_ids(a)[1]["c1"], ["p1", cid, "p2"])
        self.assertEqual(self.git(a, "status", "--porcelain"), "")

    def test_merge_command(self):
        a = self.make_book("tablet")
        self.git(a, "checkout", "-q", "-b", "onenote")
        os.unlink(os.path.join(a, "sections", "p2.xopp"))            # gone from OneNote
        with open(os.path.join(a, "notebook.xml"), "rb") as f:
            nb = bl.parse_notebook(f.read())
        nb.children[0].children = nb.children[0].children[:1]
        with open(os.path.join(a, "notebook.xml"), "wb") as f:
            f.write(bl.serialize_notebook(nb))
        self.git(a, "add", "-A")
        self.git(a, "commit", "-q", "-m", "onenote import")
        self.git(a, "checkout", "-q", "main")
        self.write_page(a, "p2", [[S3, S2]])                         # ...but written in X0urnal++
        r = self.bs("tablet", "merge", a, "onenote", BOOK_SYNC_THEIR_DEVICE="onenote")
        self.assertEqual(r["state"], "synced", r)
        self.assertIn("Merged from onenote", r["message"])
        self.assertEqual(self.read_page(a, "p2").count("<stroke"), 2)
        self.assertIn("p2", self.tree_ids(a)[0])
        self.assertEqual(self.bs("tablet", "merge", a, "onenote")["message"].split(";")[0], "Onenote is already merged")


class Commands(BookTest):
    def test_sync_all(self):
        a = self.make_book("tablet", name="One")
        b = self.make_book("tablet", name="Two_Book")
        local = os.path.join(self.nbdir("tablet"), "Three.book")
        os.makedirs(local)
        with open(os.path.join(self.nbdir("tablet"), "Zip.book"), "wb") as f:
            f.write(b"PK\3\4 not a folder")
        self.write_page(a, "p1", [[S1]])
        r = self.bs("tablet", "sync-all")
        self.assertEqual(r["state"], "synced", r)
        self.assertEqual([x["name"] for x in r["books"]], ["One.book", "Three.book", "Two_Book.book"])
        self.assertEqual([x["state"] for x in r["books"]], ["synced", "local", "synced"])
        self.assertEqual(r["message"], "3 books: 2 synced, 1 local")
        # laptop clones both and a change travels
        self.clone("laptop", "one")
        self.clone("laptop", "two-book")
        self.write_page(b, "p2", [[S1, S2]])
        self.bs("tablet", "sync-all")
        r = self.bs("laptop", "sync-all")
        self.assertEqual(r["changed"], ["Two_Book.book/sections/p2.xopp"])

    def test_json_line_always(self):
        r = self.bs("tablet", "sync", "/nonexistent/X.book")
        self.assertEqual(r["state"], "error")
        r = self.bs("tablet", "frobnicate", "x")
        self.assertEqual(r["state"], "error")
        self.assertIn("usage", r["message"])
        r = self.bs("tablet")
        self.assertEqual(r["state"], "error")
        r = self.bs("tablet", "status")
        self.assertEqual(r["state"], "error")
        zipbook = os.path.join(self.tmp, "Old.book")
        with open(zipbook, "wb") as f:
            f.write(b"PK")
        self.assertIn("zip", self.bs("tablet", "sync", zipbook)["message"])

    def test_book_by_name(self):
        self.make_book("tablet", name="Named")
        self.assertEqual(self.bs("tablet", "status", "Named")["state"], "synced")
        self.assertEqual(self.bs("tablet", "status", "Named.book")["state"], "synced")

    def test_interrupted_merge_is_finished(self):
        a, b = self.pair()
        self.write_page(a, "p1", [[S1, S2, T1, S3]])
        self.bs("tablet", "sync", a)
        self.write_page(b, "p2", [[S3, S2]])
        self.bs("laptop", "save", b)
        # a sync killed in the middle of its merge
        self.git(b, "fetch", "-q", "origin")
        subprocess.run(["git", "-C", b, "merge", "--no-ff", "--no-commit", "-q", "origin/main"],
                       env=self.env("laptop"), check=True)
        self.assertEqual(self.bs("laptop", "save", b)["state"], "behind")
        self.assertEqual(self.bs("laptop", "push", b)["state"], "behind")
        r = self.bs("laptop", "sync", b)
        self.assertEqual(r["state"], "synced", r)
        self.assertEqual(self.read_page(b, "p1").count("<stroke"), 3)
        self.assertEqual(self.git(b, "status", "--porcelain"), "")


if __name__ == "__main__":
    import unittest
    unittest.main()
