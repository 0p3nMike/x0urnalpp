"""Unit tests: names, the xopp merge, the notebook merge, the filter -- no git repos involved
(except the filter's process protocol, which is driven by hand here)."""

import gzip
import os
import sys
import tempfile
import subprocess
import unittest
import xml.etree.ElementTree as ET

from fixtures import S1, S2, S3, T1, TOOLS, page_xml, stroke, text, tree_xml

import booklib as bl

SCRIPT = [sys.executable] if os.name == "nt" else []    # Windows runs a script only through Python


def strokes_of(xml_bytes):
    root = ET.fromstring(bl.clean_bytes(xml_bytes))
    return [ET.tostring(e, encoding="unicode").strip() for e in root.iter() if e.tag in ("stroke", "text", "image", "teximage", "audio")]


class Names(unittest.TestCase):
    def test_device_from_hostname(self):
        old = os.environ.pop("BOOK_SYNC_DEVICE", None)
        try:
            self.assertEqual(bl.device_name("tablet"), "tablet")
            self.assertEqual(bl.device_name("Laptop-7"), "laptop-7")
            self.assertEqual(bl.device_name("desktop"), "desktop")
            self.assertEqual(bl.device_name("DeskTop.lan"), "desktop")
            os.environ["BOOK_SYNC_DEVICE"] = "Forced"
            self.assertEqual(bl.device_name("tablet"), "forced")
        finally:
            os.environ.pop("BOOK_SYNC_DEVICE", None)
            if old is not None:
                os.environ["BOOK_SYNC_DEVICE"] = old

    def test_slug(self):
        self.assertEqual(bl.book_slug("/x/Course_Notes.book"), "course-notes")
        self.assertEqual(bl.book_slug("/x/Work Notes.book/"), "work-notes")
        self.assertEqual(bl.book_slug("Old_Class_Notes.book"), "old-class-notes")
        self.assertEqual(bl.book_slug("Café.book"), "caf-")

    def test_remote_url(self):
        # no default: unset everywhere -> None; then git config; then the environment, which wins
        keys = ("BOOK_SYNC_REMOTE_BASE", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM")
        old = {k: os.environ.pop(k, None) for k in keys}
        with tempfile.TemporaryDirectory() as tmp:
            gitconfig = os.path.join(tmp, "gitconfig")
            open(gitconfig, "w").close()
            try:
                os.environ["GIT_CONFIG_GLOBAL"] = gitconfig
                os.environ["GIT_CONFIG_NOSYSTEM"] = "1"
                cwd = os.getcwd()
                os.chdir(tmp)       # not inside any repo, so no local config is read
                try:
                    self.assertIsNone(bl.remote_base())
                    self.assertIsNone(bl.remote_url("course-notes"))
                    subprocess.run(["git", "config", "--global", "book-sync.remote", "git@example.com:me/"], check=True)
                    self.assertEqual(bl.remote_url("course-notes"), "git@example.com:me/book-course-notes.git")
                    os.environ["BOOK_SYNC_REMOTE_BASE"] = "/tmp/books/"
                    self.assertEqual(bl.remote_url("x"), "/tmp/books/book-x.git")
                finally:
                    os.chdir(cwd)
            finally:
                for k, v in old.items():
                    os.environ.pop(k, None)
                    if v is not None:
                        os.environ[k] = v

    def test_ids(self):
        self.assertEqual(bl.base36(0), "0")
        self.assertEqual(int(bl.base36(1761400000000), 36), 1761400000000)
        nid = bl.new_id("tablet")
        self.assertRegex(nid, r"^tablet-[0-9a-z]+-[0-9a-z]{4}$")
        cid = bl.conflict_id("on-6f1c-ab", "desktop", 1761400931000)
        self.assertEqual(cid, "on-6f1c-ab-conflict-desktop-" + bl.base36(1761400931000))
        orig, dev, t = bl.parse_conflict_id(cid)
        self.assertEqual((orig, dev, t), ("on-6f1c-ab", "desktop", 1761400931.0))
        # a device name with a dash, and a conflict of a conflict
        self.assertEqual(bl.parse_conflict_id("a-conflict-x-y-" + bl.base36(5))[:2], ("a", "x-y"))
        self.assertEqual(bl.parse_conflict_id(cid + "-conflict-laptop-10")[0], cid)
        self.assertIsNone(bl.parse_conflict_id("tablet-mg8k2x1a-7q3c"))

    def test_conflict_title(self):
        t = bl.conflict_title("truth tables", "desktop", 1761400931)
        self.assertRegex(t, r"^truth tables \(conflict from desktop, \d{1,2} [A-Z][a-z]{2} \d\d:\d\d\)$")


class Formats(unittest.TestCase):
    def test_clean_smudge_deterministic(self):
        xml = page_xml([[S1, S2]])
        a = gzip.compress(xml, mtime=1)
        b = gzip.compress(xml, mtime=99999, compresslevel=9)
        self.assertNotEqual(a, b)
        self.assertEqual(bl.clean_bytes(a), xml)
        self.assertEqual(bl.clean_bytes(b), xml)
        self.assertEqual(bl.clean_bytes(xml), xml)          # plain stays plain
        self.assertEqual(bl.smudge_bytes(xml), bl.smudge_bytes(xml))
        self.assertEqual(bl.smudge_bytes(a), a)              # gzip stays as it is
        self.assertEqual(bl.clean_bytes(bl.smudge_bytes(xml)), xml)
        broken = a[:-5]
        self.assertEqual(bl.clean_bytes(broken), broken)     # never refuses, never loses

    def test_complete_check(self):
        xml = page_xml([[S1]])
        g = gzip.compress(xml)
        self.assertTrue(bl.page_file_ok(g))
        self.assertTrue(bl.page_file_ok(xml))
        self.assertFalse(bl.page_file_ok(g[: len(g) // 2]))     # gzip cut short
        self.assertFalse(bl.page_file_ok(g[:-3]))               # trailer missing
        self.assertFalse(bl.page_file_ok(xml[:-20]))            # XML cut short
        self.assertFalse(bl.page_file_ok(gzip.compress(xml[:-20])))
        self.assertFalse(bl.page_file_ok(b""))
        self.assertFalse(bl.page_file_ok(tree_xml("x", [])))    # not a page
        self.assertTrue(bl.notebook_file_ok(tree_xml("x", [])))
        self.assertFalse(bl.notebook_file_ok(tree_xml("x", [])[:-5]))

    def test_filter_process_protocol(self):
        """Talk git's long-running filter protocol to xopp-filter by hand."""
        def pkt(s):
            if isinstance(s, str):
                s = s.encode()
            return b"%04x" % (len(s) + 4) + s


        big = page_xml([[S1, S2]]) + b"<!--" + os.urandom(1) * 70000 + b"-->"   # make the content span packets
        g = gzip.compress(big, mtime=5)
        msg = pkt("git-filter-client\n") + pkt("version=2\n") + b"0000"
        msg += pkt("capability=clean\n") + pkt("capability=smudge\n") + pkt("capability=delay\n") + b"0000"
        msg += pkt("command=clean\n") + pkt("pathname=sections/a.xopp\n") + b"0000"
        for k in range(0, len(g), 65516):
            msg += pkt(g[k:k + 65516])
        msg += b"0000"
        msg += pkt("command=smudge\n") + pkt("pathname=sections/a.xopp\n") + b"0000" + pkt(b"<x/>") + b"0000"
        msg += pkt("command=clean\n") + pkt("pathname=sections/empty.xopp\n") + b"0000" + b"0000"
        r = subprocess.run(SCRIPT + [os.path.join(TOOLS, "xopp-filter"), "process"], input=msg,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout

        def read(buf, pos):
            n = int(buf[pos:pos + 4], 16)
            if n == 0:
                return None, pos + 4
            return buf[pos + 4:pos + n], pos + n

        def read_list(buf, pos):
            items = []
            while True:
                p, pos = read(buf, pos)
                if p is None:
                    return items, pos
                items.append(p)

        pos = 0
        hello, pos = read_list(out, pos)
        self.assertEqual(hello, [b"git-filter-server\n", b"version=2\n"])
        caps, pos = read_list(out, pos)
        self.assertEqual(caps, [b"capability=clean\n", b"capability=smudge\n"])   # no delay
        results = []
        for _ in range(3):
            status, pos = read_list(out, pos)
            self.assertEqual(status, [b"status=success\n"])
            content, pos = read_list(out, pos)
            tail, pos = read_list(out, pos)
            self.assertEqual(tail, [])
            results.append(b"".join(content))
        self.assertEqual(results[0], big)
        self.assertEqual(gzip.decompress(results[1]), b"<x/>")
        self.assertEqual(results[2], b"")
        self.assertEqual(pos, len(out))

    def test_filter_single_file_mode(self):
        xml = page_xml([[S1]])
        f = os.path.join(TOOLS, "xopp-filter")
        g = subprocess.run(SCRIPT + [f, "smudge"], input=xml, stdout=subprocess.PIPE, check=True).stdout
        self.assertEqual(g[:2], b"\x1f\x8b")
        self.assertEqual(g, subprocess.run(SCRIPT + [f, "smudge"], input=xml, stdout=subprocess.PIPE, check=True).stdout)
        c = subprocess.run(SCRIPT + [f, "clean"], input=g, stdout=subprocess.PIPE, check=True).stdout
        self.assertEqual(c, xml)


class XoppMerge(unittest.TestCase):
    def merge(self, base, ours, theirs):
        res, reason = bl.merge_xopp(base, ours, theirs)
        if reason is None:
            ET.fromstring(bl.clean_bytes(res))   # always valid XML
        return res, reason

    def test_both_add_strokes(self):
        base = page_xml([[S1]])
        res, reason = self.merge(base, page_xml([[S1, S2]]), page_xml([[S1, S3]]))
        self.assertIsNone(reason)
        got = strokes_of(res)
        self.assertEqual(len(got), 3)
        self.assertIn("66.5", got[1])            # ours' stroke where ours put it
        self.assertIn("#ff0000ff", got[2])       # theirs' new stroke on top

    def test_erase_stays_erased(self):
        base = page_xml([[S1, S2]])
        res, reason = self.merge(base, page_xml([[S2]]), page_xml([[S1, S2, S3]]))
        self.assertIsNone(reason)
        got = strokes_of(res)
        self.assertEqual(len(got), 2)
        self.assertFalse(any(">10 10 20 20" in g for g in got))

    def test_erase_on_both_sides(self):
        base = page_xml([[S1, S2, S3]])
        res, reason = self.merge(base, page_xml([[S2, S3]]), page_xml([[S2]]))
        self.assertIsNone(reason)
        self.assertEqual(len(strokes_of(res)), 1)

    def test_text_edited_both_sides_cannot_decide(self):
        base = page_xml([[T1]])
        ours = page_xml([[text("hello world", x=10, y=200)]])
        theirs = page_xml([[text("hello there", x=10, y=200)]])
        res, reason = bl.merge_xopp(base, ours, theirs)
        self.assertIsNotNone(reason)
        self.assertEqual(res, ours)

    def test_text_words_one_side_colour_other(self):
        base = page_xml([[S1, T1]])
        ours = page_xml([[S1, text("hello world", x=10, y=200)]])
        theirs = page_xml([[S1, text("hello", x=10, y=200, color="#ff0000ff")]])
        res, reason = self.merge(base, ours, theirs)
        self.assertIsNone(reason)
        got = strokes_of(res)
        self.assertEqual(len(got), 2)
        self.assertIn("hello world", got[1])
        self.assertIn("#ff0000ff", got[1])

    def test_same_attribute_changed_differently(self):
        base = page_xml([[S1]])
        ours = page_xml([[stroke([10, 10, 20, 20, 30, 30], color="#00ff00ff")]])
        theirs = page_xml([[stroke([10, 10, 20, 20, 30, 30], color="#0000ffff")]])
        _, reason = bl.merge_xopp(base, ours, theirs)
        self.assertIn("color", reason)

    def test_recolour_on_theirs_keeps_place(self):
        base = page_xml([[S1, S2, S3]])
        ours = page_xml([[S1, S2, S3, T1]])
        theirs = page_xml([[S1, stroke([40, 40, 50, 50, 60, 66.5], color="#123456ff"), S3]])
        res, reason = self.merge(base, ours, theirs)
        self.assertIsNone(reason)
        got = strokes_of(res)
        self.assertEqual(len(got), 4)
        self.assertIn("#123456ff", got[1])       # in its old place, not moved to the top

    def test_identical_edit_not_doubled(self):
        base = page_xml([[T1]])
        both = page_xml([[S1, text("hello!", x=10, y=200)]])
        both2 = page_xml([[text("hello!", x=10, y=200), S1]])
        res, reason = self.merge(base, both, both2)
        self.assertIsNone(reason)
        got = strokes_of(res)
        self.assertEqual(sum("hello!" in g for g in got), 1)

    def test_rounding_to_hundredths(self):
        """X0urnal++ writes %.8g, the importer two decimals: the same stroke either way."""
        base = page_xml([[stroke([10.123456, 20.5, 30, 40])]])
        ours = page_xml([[stroke([10.12, 20.5, 30, 40])]])          # re-saved with 2 decimals
        theirs = page_xml([[stroke([10.123456, 20.5, 30, 40]), S3]])
        res, reason = self.merge(base, ours, theirs)
        self.assertIsNone(reason)
        self.assertEqual(len(strokes_of(res)), 2)

    def test_page_size_larger(self):
        base = page_xml([[S1]], width=100, height=100)
        ours = page_xml([[S1, S2]], width=300, height=100)
        theirs = page_xml([[S1, S3]], width=100, height=500)
        res, reason = self.merge(base, ours, theirs)
        self.assertIsNone(reason)
        page = ET.fromstring(res).find("page")
        self.assertEqual((page.get("width"), page.get("height")), ("300", "500"))

    def test_layer_count_differs(self):
        base = page_xml([[S1]])
        _, reason = bl.merge_xopp(base, page_xml([[S1, S2]]), page_xml([[S1], [S3]]))
        self.assertIn("layers", reason)

    def test_unparsable(self):
        base = page_xml([[S1]])
        _, reason = bl.merge_xopp(base, page_xml([[S1, S2]]), b"<xournal><page>")
        self.assertIsNotNone(reason)
        _, reason = bl.merge_xopp(base, page_xml([[S1, S2]]), gzip.compress(page_xml([[S3]]))[:-10])
        self.assertIsNotNone(reason)

    def test_output_form_follows_ours(self):
        base = page_xml([[S1]])
        ours, theirs = page_xml([[S1, S2]]), page_xml([[S1, S3]])
        res, _ = bl.merge_xopp(base, gzip.compress(ours), theirs)
        self.assertEqual(res[:2], b"\x1f\x8b")
        res, _ = bl.merge_xopp(gzip.compress(base), ours, gzip.compress(theirs))
        self.assertTrue(res.startswith(b'<?xml version="1.0" standalone="no"?>\n<xournal creator="xournalpp 1.3.7"'))

    def test_trivial_sides(self):
        base = page_xml([[S1]])
        ours = page_xml([[S1, S2]])
        self.assertEqual(bl.merge_xopp(base, ours, base), (ours, None))
        self.assertEqual(bl.merge_xopp(base, base, ours), (ours, None))

    def test_add_add_without_base(self):
        a = page_xml([[S1, S2]])
        t = page_xml([[S1, S3]])
        res, reason = self.merge(None, a, t)
        self.assertIsNone(reason)
        self.assertEqual(len(strokes_of(res)), 3)      # S1 once, not twice

    def test_unknown_elements_kept(self):
        audio = '<audio fn="rec.ogg" ts="5">x</audio>'
        base = page_xml([[S1]])
        res, reason = self.merge(base, page_xml([[S1, audio]]), page_xml([[S1, S3]]))
        self.assertIsNone(reason)
        self.assertTrue(any("rec.ogg" in g for g in strokes_of(res)))

    def test_images_and_teximages(self):
        img = '<image left="1" top="2" right="30" bottom="40">iVBORw0KGgo007</image>'
        img_moved = '<image left="5" top="6" right="34" bottom="44">iVBORw0KGgo007</image>'
        tex = '<teximage text="x^2" texlength="3" left="1" top="1" right="9" bottom="9">AAAA</teximage>'
        tex_a = '<teximage text="x^3" texlength="3" left="1" top="1" right="9" bottom="9">BBBB</teximage>'
        tex_b = '<teximage text="x^4" texlength="3" left="1" top="1" right="9" bottom="9">CCCC</teximage>'
        base = page_xml([[img, tex]])
        res, reason = self.merge(base, page_xml([[img_moved, tex]]), page_xml([[img, tex, S1]]))
        self.assertIsNone(reason)
        got = strokes_of(res)
        self.assertEqual(len(got), 3)
        self.assertIn('left="5"', got[0])
        _, reason = bl.merge_xopp(base, page_xml([[img, tex_a]]), page_xml([[img, tex_b]]))
        self.assertIsNotNone(reason)

    def test_several_pages(self):
        def two(a, b):
            x = page_xml([a]).decode()
            y = page_xml([b]).decode()
            pa = x[x.index("<page"):x.index("</page>") + 7]
            pb = y[y.index("<page"):y.index("</page>") + 7]
            return x.replace(pa, pa + "\n" + pb).encode()
        base = two([S1], [S2])
        res, reason = self.merge(base, two([S1, S3], [S2]), two([S1], [S2, T1]))
        self.assertIsNone(reason)
        pages = ET.fromstring(res).findall("page")
        self.assertEqual(len(pages), 2)
        self.assertEqual(len(pages[0].find("layer")), 2)
        self.assertEqual(len(pages[1].find("layer")), 2)
        _, reason = bl.merge_xopp(base, two([S1, S3], [S2]), page_xml([[S1]]))
        self.assertIsNotNone(reason)


class NotebookMerge(unittest.TestCase):
    def nb(self, chapters, title="B"):
        return bl.parse_notebook(tree_xml(title, chapters))

    def ids(self, nb):
        idx, order = bl.index_notebook(nb)
        return idx, order

    BASE = [("c1", "Ch1", [("p1", "one"), ("p2", "two")]), ("c2", "Ch2", [("p3", "three")])]

    def test_union_and_order(self):
        a = self.nb([("c1", "Ch1", [("p1", "one"), ("p2", "two"), ("pa", "A")]), ("c2", "Ch2", [("p3", "three")])])
        t = self.nb([("c1", "Ch1", [("p1", "one"), ("p2", "two"), ("pb", "B")]), ("c2", "Ch2", [("p3", "three")])])
        m, titles = bl.merge_notebooks(self.nb(self.BASE), a, t)
        idx, order = self.ids(m)
        self.assertEqual(order["c1"], ["p1", "p2", "pa", "pb"])
        self.assertEqual(titles, [])

    def test_delete_unchanged_vs_changed(self):
        a = self.nb([("c1", "Ch1", [("p1", "one")]), ("c2", "Ch2", [])])            # deleted p2, p3
        t = self.nb([("c1", "Ch1", [("p1", "one"), ("p2", "two")]), ("c2", "Ch2", [("p3", "THREE")])])
        m, _ = bl.merge_notebooks(self.nb(self.BASE), a, t)
        idx, order = self.ids(m)
        self.assertNotIn("p2", idx)          # unchanged on theirs: deleted
        self.assertIn("p3", idx)             # renamed on theirs: kept
        self.assertEqual(idx["p3"].node.title(), "THREE")

    def test_rename_and_move_one_side(self):
        a = self.nb([("c1", "Ch1", [("p1", "uno"), ("p2", "two")]), ("c2", "Ch2", [("p3", "three")])])
        t = self.nb([("c1", "Ch1", [("p1", "one")]), ("c2", "Ch2", [("p3", "three"), ("p2", "two")])])
        m, _ = bl.merge_notebooks(self.nb(self.BASE), a, t)
        idx, order = self.ids(m)
        self.assertEqual(idx["p1"].node.title(), "uno")
        self.assertEqual(idx["p2"].parent, "c2")
        self.assertEqual(order["c2"], ["p3", "p2"])

    def test_reorder_on_theirs(self):
        t = self.nb([("c2", "Ch2", [("p3", "three")]), ("c1", "Ch1", [("p2", "two"), ("p1", "one")])])
        a = self.nb(self.BASE)
        m, _ = bl.merge_notebooks(self.nb(self.BASE), a, t)
        idx, order = self.ids(m)
        self.assertEqual(order[None], ["c2", "c1"])
        self.assertEqual(order["c1"], ["p2", "p1"])

    def test_renamed_differently_ours_wins(self):
        a = self.nb([("c1", "Ch1", [("p1", "ours"), ("p2", "two")]), ("c2", "Ch2", [("p3", "three")])])
        t = self.nb([("c1", "Ch1", [("p1", "theirs"), ("p2", "two")]), ("c2", "Ch2", [("p3", "three")])])
        m, titles = bl.merge_notebooks(self.nb(self.BASE), a, t)
        idx, _ = self.ids(m)
        self.assertEqual(idx["p1"].node.title(), "ours")
        self.assertEqual(titles, [("p1", "ours", "theirs")])

    def test_deleted_chapter_comes_back_for_new_page(self):
        a = self.nb([("c1", "Ch1", [("p1", "one"), ("p2", "two")])])                 # deleted Ch2
        t = self.nb(self.BASE[:1] + [("c2", "Ch2", [("p3", "three"), ("p4", "new")])])
        m, _ = bl.merge_notebooks(self.nb(self.BASE), a, t)
        idx, order = self.ids(m)
        self.assertIn("c2", idx)
        self.assertEqual(order["c2"], ["p4"])        # p3 was unchanged there: deleted with Ch2

    def test_move_loop_is_broken(self):
        base = bl.parse_notebook(b'<notebook version="2" title="B"><chapter id="x" title="X"/><chapter id="y" title="Y"/></notebook>')
        a = bl.parse_notebook(b'<notebook version="2" title="B"><chapter id="y" title="Y"><chapter id="x" title="X"/></chapter></notebook>')
        t = bl.parse_notebook(b'<notebook version="2" title="B"><chapter id="x" title="X"><chapter id="y" title="Y"/></chapter></notebook>')
        m, _ = bl.merge_notebooks(base, a, t)
        idx, _ = self.ids(m)
        self.assertEqual(set(idx), {"x", "y"})       # both still there, no loop

    def test_serialize_form(self):
        nb = bl.Notebook({"version": "2", "title": "T<1>"}, [
            bl.Node("chapter", {"id": "c1", "title": "Ch & 1"},
                    [bl.Node("section", {"id": "p1", "title": 'say "hi"', "file": "sections/p1.xopp"})]),
            bl.Node("chapter", {"id": "c2", "title": "Empty"})])
        out = bl.serialize_notebook(nb).decode()
        self.assertEqual(out, '<?xml version="1.0" encoding="UTF-8"?>\n'
                              '<notebook version="2" title="T&lt;1&gt;">\n'
                              '  <chapter id="c1" title="Ch &amp; 1">\n'
                              '    <section id="p1" title="say &quot;hi&quot;" file="sections/p1.xopp"/>\n'
                              '  </chapter>\n'
                              '  <chapter id="c2" title="Empty">\n'
                              '  </chapter>\n'
                              '</notebook>\n')
        self.assertEqual(bl.serialize_notebook(bl.parse_notebook(out.encode())).decode(), out)

    def test_attribute_order_normalised(self):
        nb = bl.parse_notebook(b'<notebook title="T" version="2"><section file="sections/a.xopp" title="A" id="a" extra="1"/></notebook>')
        out = bl.serialize_notebook(nb).decode()
        self.assertIn('<notebook version="2" title="T">', out)
        self.assertIn('<section id="a" title="A" file="sections/a.xopp" extra="1"/>', out)

    def test_unknown_elements_preserved(self):
        base = bl.parse_notebook(b'<notebook version="2" title="B"><meta k="v"/><chapter id="c" title="C"/></notebook>')
        a = bl.parse_notebook(b'<notebook version="2" title="B"><meta k="v"/><chapter id="c" title="C2"/></notebook>')
        t = bl.parse_notebook(b'<notebook version="2" title="B"><meta k="v"/><chapter id="c" title="C"/><tag x="1"/></notebook>')
        m, _ = bl.merge_notebooks(base, a, t)
        out = bl.serialize_notebook(m).decode()
        self.assertEqual(out.count("<meta"), 1)
        self.assertIn('<tag x="1" />', out)
        self.assertIn('title="C2"', out)


if __name__ == "__main__":
    unittest.main()
