#!/usr/bin/env python3
"""booklib -- the parts book-sync and its git merge drivers share.

Everything here is pure Python 3 standard library (>= 3.11) so the same files run unchanged on
Linux (x86_64 and aarch64) and Windows. The contract these functions implement is ./README.md;
where this file had to pick a reading of it, the choice is in the README's "Notes" section.

What lives here:
  - names: the device name, the remote's book slug, ids, the conflict page names
  - the page file formats: gzip / plain detection, the xopp filter's clean and smudge, and the
    "is this file complete?" check save/push use before committing a page
  - xopp merging (one page file, three versions -> one, or "cannot decide")
  - notebook.xml: parse, write in the one-node-per-line form, three-way merge of the tree
  - the pending records the drivers leave for book-sync (conflict pages, lost titles)
"""

import copy
import datetime
import gzip
import json
import os
import re
import secrets
import socket
import subprocess
import time
import xml.etree.ElementTree as ET
import xml.parsers.expat
import zlib
from collections import defaultdict, deque

# ---- names -------------------------------------------------------------------------------------

DEFAULT_NOTEBOOKS = "~/Documents/notebooks"

# git config key naming the remote base when BOOK_SYNC_REMOTE_BASE is not set (README "Remote").
REMOTE_CONFIG_KEY = "book-sync.remote"
NO_REMOTE_HINT = ("set BOOK_SYNC_REMOTE_BASE, or run `git config --global %s <base>` "
                  "(e.g. git@example.com:me)" % REMOTE_CONFIG_KEY)

_NO_WINDOW = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}

# The chapter recovered pages go into. A fixed id, not a generated one, so two devices that both
# recover something end up with ONE "Recovered" chapter after they merge (the tree merges by id).
RECOVERED_ID = "recovered"
RECOVERED_TITLE = "Recovered"

# Where a book keeps local, never-committed state (.gitignore has `.book-sync/`).
STATE_DIR = ".book-sync"
PENDING_CONFLICTS = "conflicts.jsonl"   # written by the drivers, read by book-sync sync
PENDING_TITLES = "titles.jsonl"         # titles notebook-merge had to overrule


def device_name(hostname=None):
    """The name this machine goes by in ids and commits: the lowercased hostname.

    BOOK_SYNC_DEVICE overrides it (to give a machine a shorter name, and for the tests, which run
    several "devices" on one machine). A dotted hostname (an FQDN) counts by its first label:
    "desktop.lan" is desktop.
    """
    forced = os.environ.get("BOOK_SYNC_DEVICE", "").strip()
    if forced:
        return forced.lower()
    h = (hostname if hostname is not None else socket.gethostname()).strip().lower()
    return h.split(".")[0] or "unknown"


def book_slug(book_dir):
    """The remote name of a book: folder name without .book, lowercased, [^a-z0-9-] -> '-'."""
    name = os.path.basename(os.path.normpath(book_dir))
    if name.endswith(".book"):
        name = name[: -len(".book")]
    return re.sub(r"[^a-z0-9-]", "-", name.lower())


def remote_base():
    """Where book repos live: BOOK_SYNC_REMOTE_BASE, else git config book-sync.remote, else None.
    There is no default: without one, books stay local until a remote is configured."""
    base = os.environ.get("BOOK_SYNC_REMOTE_BASE", "").strip()
    if not base:
        try:
            r = subprocess.run(["git", "config", "--get", REMOTE_CONFIG_KEY], stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=30, **_NO_WINDOW)
            base = r.stdout.decode("utf-8", "replace").strip() if r.returncode == 0 else ""
        except (OSError, subprocess.SubprocessError):
            base = ""
    return base.rstrip("/") or None


def remote_url(slug):
    """The repo of book <slug> under the remote base, or None when no remote is configured."""
    base = remote_base()
    return "%s/book-%s.git" % (base, slug) if base else None


def notebooks_dir():
    return os.path.abspath(os.path.expanduser(os.environ.get("BOOK_SYNC_NOTEBOOKS") or DEFAULT_NOTEBOOKS))


_B36 = "0123456789abcdefghijklmnopqrstuvwxyz"


def base36(n):
    n = int(n)
    if n <= 0:
        return "0"
    out = ""
    while n:
        n, r = divmod(n, 36)
        out = _B36[r] + out
    return out


def new_id(device=None):
    """An id of the X0urnal++ form <device>-<base36 ms>-<4 base36 random>."""
    rand = "".join(secrets.choice(_B36) for _ in range(4))
    return "%s-%s-%s" % (device or device_name(), base36(int(time.time() * 1000)), rand)


def now_iso():
    return datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")


def short_when(epoch):
    """'25 Oct 14:02', local time, the form the conflict page titles use. Built by hand rather
    than with %-d so it does not depend on glibc's strftime extensions; %b is the C locale's
    month abbreviation because Python never calls setlocale on its own."""
    t = datetime.datetime.fromtimestamp(epoch)
    return "%d %s %s" % (t.day, t.strftime("%b"), t.strftime("%H:%M"))


def conflict_id(orig_id, device, epoch_ms):
    return "%s-conflict-%s-%s" % (orig_id, device, base36(epoch_ms))


def parse_conflict_id(cid):
    """(original id, device, epoch seconds) for a conflict page id, or None.

    Split at the LAST '-conflict-' so a conflict of a conflict still names its direct original,
    and take the time off the end, so a device name with a dash in it survives."""
    k = cid.rfind("-conflict-")
    if k <= 0:
        return None
    orig, rest = cid[:k], cid[k + len("-conflict-"):]
    if "-" not in rest:
        return None
    dev, t = rest.rsplit("-", 1)
    try:
        ms = int(t, 36)
    except ValueError:
        return None
    return orig, dev, ms / 1000.0


def conflict_title(title, device, epoch):
    return "%s (conflict from %s, %s)" % (title, device, short_when(epoch))


def section_file(sid):
    return "sections/%s.xopp" % sid


# ---- page file formats -------------------------------------------------------------------------

GZIP_MAGIC = b"\x1f\x8b"


def is_gzip(data):
    return data[:2] == GZIP_MAGIC


def gunzip_or_none(data):
    """The decompressed bytes, or None if the gzip stream is broken or cut short. A file X0urnal++
    is still writing ends without the gzip trailer, which is exactly what this rejects."""
    try:
        return gzip.decompress(data)
    except (OSError, EOFError, zlib.error, ValueError):
        return None


def clean_bytes(data):
    """filter=xopp clean: what git stores. gzip -> the plain XML inside it; anything else (plain XML
    already, or a gzip that will not decompress) unchanged, so a broken file is stored as it is
    rather than refused. Deterministic: decompressing the same bytes gives the same bytes."""
    if is_gzip(data):
        plain = gunzip_or_none(data)
        if plain is not None:
            return plain
    return data


def smudge_bytes(data):
    """filter=xopp smudge: what the working tree gets. Plain XML -> gzip, written with mtime 0 and no
    file name so the same blob always smudges to the same bytes. Already gzipped (a blob stored
    before the filter existed) -> unchanged."""
    if is_gzip(data):
        return data
    return gzip.compress(data, compresslevel=6, mtime=0)


def xml_root_tag(data):
    """The root element's name if `data` is one complete, well-formed XML document, else None.

    expat without any character handlers is fast even for pages full of base64 images; a file cut
    off mid-write fails with "no element found" or "unclosed token"."""
    p = xml.parsers.expat.ParserCreate()
    root = []

    def start(name, attrs):
        if not root:
            root.append(name)

    p.StartElementHandler = start
    try:
        p.Parse(data, True)
    except (xml.parsers.expat.ExpatError, ValueError):
        return None
    return root[0] if root else None


def page_file_ok(data):
    """True if a page file is complete: gzip that decompresses (CRC and length checked) or plain,
    holding a well-formed <xournal> document. save/push commit only files that pass."""
    if is_gzip(data):
        data = gunzip_or_none(data)
        if data is None:
            return False
    return xml_root_tag(data) == "xournal"


def notebook_file_ok(data):
    return xml_root_tag(data) == "notebook"


def needs_check(path):
    """Paths save/push verify before committing: page files and the tree."""
    return path == "notebook.xml" or (path.endswith(".xopp") and (path.startswith("sections/") or path.startswith("trash/")))


def file_ok(path, data):
    return notebook_file_ok(data) if path == "notebook.xml" else page_file_ok(data)


def write_atomic(path, data):
    """Write via a temporary file in the same folder and rename it over the target, so a reader
    (X0urnal++, git) never sees half a file."""
    d = os.path.dirname(path) or "."
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, ".%s.%s.tmp" % (os.path.basename(path), secrets.token_hex(4)))
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ---- xopp merge --------------------------------------------------------------------------------
#
# A page file is <xournal> with <title>/<preview> and one or more <page>, each holding a
# <background> and <layer>s; a layer holds the elements (stroke, text, image, teximage, and
# whatever a later Xournal++ adds). The merge works per page and per layer on elements identified
# by a canonical key, as the README says: tag + sorted attributes + text, numbers rounded to 0.01
# because Xournal++ writes %.8g and the importer two decimals.


class CannotDecide(Exception):
    """The merge would have to guess; the caller keeps ours and saves theirs as a conflict page."""


# One number, written so that a string can match it in exactly one way (no backtracking blow-up on
# long base64 text), and a whole value that is nothing but numbers separated by whitespace.
_NUM_RE = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
_NUM = re.compile(_NUM_RE)
_NUMS = re.compile(r"\s*%s(?:\s+%s)*\s*" % (_NUM_RE, _NUM_RE))


def canon_value(v):
    """A value with its numbers rounded to 0.01 -- but only when the WHOLE value is a list of
    numbers ("12.5", "1.41 1.2 1.3", stroke coordinates). Colours, fonts and words are compared
    exactly; rounding the digits inside "#00ff00ff" or a sentence would make different things
    equal. The rounded numbers are kept as a tuple of floats (10, 10.0 and 10.004 are all 10.0),
    which is much faster than formatting each one, and a page can hold millions."""
    if v and _NUMS.fullmatch(v):
        return tuple([round(x, 2) + 0.0 for x in map(float, v.split())])
    return v


# Elements whose text is content, not coordinates: typed text, base64 picture data. Their text is
# compared exactly ("3.14159" and "3.14" typed on a page are different text).
_VERBATIM_TEXT = {"text", "image", "teximage"}


def canon_text(e):
    t = e.text or ""
    return t if e.tag in _VERBATIM_TEXT else canon_value(t)


_KEYS = {}   # id(element) -> (element, key) during one merge_xopp call; see ekey


def ekey(e):
    """The canonical key of an element (README "Merge drivers"). Remembered for the duration of a
    merge: the same element's key is wanted several times (whole-document checks, then per layer),
    and for a 20,000-stroke page computing it once is most of the work. The element itself is
    kept in the memo so its id cannot be reused by another object while the memo lives."""
    hit = _KEYS.get(id(e))
    if hit is not None and hit[0] is e:
        return hit[1]
    k = (
        e.tag,
        tuple(sorted((k, canon_value(v)) for k, v in e.attrib.items())),
        canon_text(e),
        tuple(ekey(c) for c in e),
    )
    _KEYS[id(e)] = (e, k)
    return k


def _anchors(e):
    """What makes two versions of an element "the same element, edited".

    Xournal++ keeps no element ids, so an edit shows up as one element gone and another one come.
    To tell "the same text box edited differently on both sides" (a conflict) from "two separate
    changes" (both kept), an element removed from the base is paired with a new element of the same
    kind that shares one of these anchors:
      text      the same position (edited words), or the same words (moved)
      stroke    the same points (recoloured, new width, other tool); a moved stroke is NOT paired,
                so it simply counts as erased + drawn, which the counting merge handles exactly
      image     the same picture data (moved/resized), or the same position (replaced)
      teximage  the same LaTeX, the same picture data, or the same position
      other     the same text
    """
    a = {k: canon_value(v) for k, v in e.attrib.items()}
    t = e.tag
    out = []
    if t == "text":
        out = [("pos", a.get("x"), a.get("y")), ("words", e.text or "")]
    elif t == "stroke":
        out = [("points", canon_text(e))]
    elif t == "image":
        out = [("data", (e.text or "").strip()), ("pos", a.get("left"), a.get("top"))]
    elif t == "teximage":
        out = [("tex", a.get("text")), ("data", (e.text or "").strip()), ("pos", a.get("left"), a.get("top"))]
    elif (e.text or "").strip():
        out = [("text", canon_text(e))]
    return [x for x in out if all(v not in (None, "") for v in x[1:])]


def fkey(e):
    """The exact key: tag, attributes and text as written. Two elements with the same exact key
    have the same canonical key too, so matching on this first and on ekey only for what is left
    gives the README's result -- at a fraction of the cost, because a page's unchanged strokes
    (nearly all of them) are matched by one string hash each instead of rounding every number.
    A 20,000-stroke OneNote page merges in about a second instead of ten."""
    return (e.tag, tuple(sorted(e.attrib.items())), e.text or "", tuple(fkey(c) for c in e))


def _match(base, side):
    """Pair each side element with an identical base element (same canonical key), in order.
    Returns (side index -> base index, base indices with no partner, side indices with none)."""
    pool = defaultdict(deque)
    for i, e in enumerate(base):
        pool[fkey(e)].append(i)
    same = {}
    rest = []
    for j, e in enumerate(side):
        q = pool.get(fkey(e))
        if q:
            same[j] = q.popleft()
        else:
            rest.append(j)
    left = sorted(i for q in pool.values() for i in q)
    if not (left and rest):
        return same, left, rest
    # what the exact key could not match: written differently, maybe only by precision
    pool = defaultdict(deque)
    for i in left:
        pool[ekey(base[i])].append(i)
    added = []
    for j in rest:
        q = pool.get(ekey(side[j]))
        if q:
            same[j] = q.popleft()
        else:
            added.append(j)
    removed = sorted(i for q in pool.values() for i in q)
    return same, removed, added


def _pair_edits(base, removed, side, added):
    """base index -> side index for removed base elements that were edited rather than erased."""
    index = defaultdict(list)
    for j in added:
        for an in _anchors(side[j]):
            index[(side[j].tag, an)].append(j)
    used = set()
    pairs = {}
    for i in removed:
        b = base[i]
        for an in _anchors(b):
            for j in index.get((b.tag, an), ()):
                if j not in used:
                    pairs[i] = j
                    used.add(j)
                    break
            if i in pairs:
                break
    return pairs


def _merge3(vb, va, vt, what):
    """Three-way merge of one value compared by canon_value: a change on one side wins; the same
    change on both is fine; different changes on both cannot be decided."""
    cb, ca, ct = (canon_value(x) if x is not None else None for x in (vb, va, vt))
    if ca == cb:
        return vt
    if ct == cb or ca == ct:
        return va
    raise CannotDecide(what)


def _merge_element(b, a, t):
    """One element edited on both sides: per attribute, the side that changed it wins; the same
    attribute (or the text) changed differently on both sides cannot be decided."""
    out = ET.Element(a.tag)
    names = list(a.attrib) + [n for n in t.attrib if n not in a.attrib]
    names += [n for n in b.attrib if n not in a.attrib and n not in t.attrib]
    for n in names:
        v = _merge3(b.get(n), a.get(n), t.get(n), "the %s of a %s was changed on both sides" % (n, a.tag))
        if v is not None:
            out.set(n, v)
    tb, ta, tt = canon_text(b), canon_text(a), canon_text(t)
    if ta == tb:
        out.text = t.text
    elif tt == tb or ta == tt:
        out.text = a.text
    else:
        raise CannotDecide("the same %s was edited differently on both sides" % a.tag)
    kb, ka, kt = (tuple(ekey(c) for c in x) for x in (b, a, t))
    if ka == kb:
        src = t
    elif kt == kb or ka == kt:
        src = a
    else:
        raise CannotDecide("the inside of a %s was changed on both sides" % a.tag)
    for c in src:
        out.append(copy.deepcopy(c))
    out.tail = a.tail
    return out


def merge_layer_elements(lb, la, lt):
    """Merge the elements of one layer (README: count = ours + theirs - base, never below 0; order =
    ours, then what only theirs added, on top), refined by edit pairing so that
      - an element edited on one side only is that edit, kept where it was in the stacking order,
      - the same element edited on both sides is merged per attribute, or raises CannotDecide,
      - the identical edit made on both sides appears once, not twice."""
    B, A, T = list(lb), list(la), list(lt)
    sameA, remA, addA = _match(B, A)
    sameT, remT, addT = _match(B, T)
    editA = _pair_edits(B, remA, A, addA)
    editT = _pair_edits(B, remT, T, addT)
    remT_set = set(remT)

    replace_ours = {}   # ours index -> element to write instead (both sides edited it)
    skip_theirs = set()  # theirs indices already accounted for
    for i in sorted(set(remA) & remT_set):
        ja, jt = editA.get(i), editT.get(i)
        if ja is not None and jt is not None:
            skip_theirs.add(jt)
            if ekey(A[ja]) != ekey(T[jt]):
                replace_ours[ja] = _merge_element(B[i], A[ja], T[jt])
        # edited on one side and erased on the other: the edit is simply kept by the counting
        # below (never lose writing); erased on both: gone.

    out = []
    for j, e in enumerate(A):
        if j in sameA:                      # ours left this base element alone
            i = sameA[j]
            if i in remT_set:               # ... theirs erased or edited it
                jt = editT.get(i)
                if jt is not None:
                    out.append(T[jt])       # their edit, in the element's old place
                    skip_theirs.add(jt)
                continue
            out.append(e)
        else:                               # ours drew or edited this
            out.append(replace_ours.get(j, e))
    for j in addT:
        if j not in skip_theirs:
            out.append(T[j])                # only theirs drew this: on top
    return out


def _attrs_merge(eb, ea, et, larger=()):
    """Three-way merge of an element's attributes (pages, layers). Attributes in `larger` take the
    numerically larger of ours and theirs (page size)."""
    res = {}
    names = list(ea.attrib) + [n for n in et.attrib if n not in ea.attrib]
    names += [n for n in eb.attrib if n not in ea.attrib and n not in et.attrib]
    for n in names:
        va, vt = ea.get(n), et.get(n)
        if n in larger and va is not None and vt is not None:
            try:
                res[n] = va if float(va) >= float(vt) else vt
                continue
            except ValueError:
                pass
        v = _merge3(eb.get(n), va, vt, "the %s of a %s was changed on both sides" % (n, ea.tag))
        if v is not None:
            res[n] = v
    return res


def _children_merge(cb, ca, ct, what):
    """Three-way merge of a list of elements taken as one value (a page's background)."""
    kb, ka, kt = ([ekey(c) for c in x] for x in (cb, ca, ct))
    if ka == kb:
        return ct
    if kt == kb or ka == kt:
        return ca
    raise CannotDecide(what)


def _merge_page(pb, pa, pt):
    attrs = _attrs_merge(pb, pa, pt, larger=("width", "height"))
    lb = [c for c in pb if c.tag == "layer"]
    la = [c for c in pa if c.tag == "layer"]
    lt = [c for c in pt if c.tag == "layer"]
    if not (len(lb) == len(la) == len(lt)):
        raise CannotDecide("the number of layers on a page differs")
    others = _children_merge([c for c in pb if c.tag != "layer"], [c for c in pa if c.tag != "layer"],
                             [c for c in pt if c.tag != "layer"], "the page background was changed on both sides")
    page = ET.Element("page", attrs)
    page.text = "\n"
    for c in others:
        c = copy.deepcopy(c)
        c.tail = "\n"
        page.append(c)
    for b, a, t in zip(lb, la, lt):
        layer = ET.Element("layer", _attrs_merge(b, a, t))
        elems = merge_layer_elements(b, a, t)
        layer.text = "\n" if elems else None
        for e in elems:
            e.tail = "\n"
            layer.append(e)
        layer.tail = "\n"
        page.append(layer)
    page.tail = "\n"
    return page


_SENTINEL = "\x00book-sync: no common base\x00"


def _synth_base(ra, rt):
    """A stand-in base for an add/add merge (both sides created the same file, so git has no base).
    Elements both sides have count as base (so identical content is not doubled); attributes both
    sides agree on are base; anything they disagree on gets a value neither side has, so a
    difference there cannot be decided instead of silently going one way."""
    root = ET.Element(ra.tag, dict(ra.attrib))
    pa = [c for c in ra if c.tag == "page"]
    pt = [c for c in rt if c.tag == "page"]
    for c in ra:
        if c.tag != "page":
            root.append(c)
    if len(pa) != len(pt):
        raise CannotDecide("the number of pages differs")

    def common_attrs(x, y):
        out = {}
        for n in set(x.attrib) | set(y.attrib):
            out[n] = x.get(n) if x.get(n) == y.get(n) else _SENTINEL
        return out

    for a, t in zip(pa, pt):
        page = ET.SubElement(root, "page", common_attrs(a, t))
        oa = [c for c in a if c.tag != "layer"]
        ot = [c for c in t if c.tag != "layer"]
        if [ekey(c) for c in oa] == [ekey(c) for c in ot]:
            for c in oa:
                page.append(c)
        else:
            ET.SubElement(page, "background", {"book-sync": _SENTINEL})
        la = [c for c in a if c.tag == "layer"]
        lt = [c for c in t if c.tag == "layer"]
        if len(la) != len(lt):
            raise CannotDecide("the number of layers on a page differs")
        for x, y in zip(la, lt):
            layer = ET.SubElement(page, "layer", common_attrs(x, y))
            pool = defaultdict(int)
            for e in y:
                pool[ekey(e)] += 1
            for e in x:
                k = ekey(e)
                if pool[k] > 0:
                    pool[k] -= 1
                    layer.append(e)
    return root


_XML_DECL = re.compile(rb"^\s*(<\?xml[^>]*\?>)")


def _xml_decl(raw):
    m = _XML_DECL.match(raw)
    return m.group(1).decode("ascii", "replace") if m else '<?xml version="1.0" standalone="no"?>'


def _decl_encoding(decl):
    m = re.search(r"""encoding\s*=\s*["']([A-Za-z0-9._-]+)["']""", decl)
    enc = m.group(1) if m else "utf-8"
    try:
        "".encode(enc)
    except LookupError:
        enc = "utf-8"
    return enc


def merge_xopp(base, ours, theirs):
    """Merge one page file. `base` may be None or empty (add/add).

    Returns (result bytes, None) on success, or (ours, reason) when it cannot decide -- the caller
    then saves theirs as a conflict page. The result is gzipped if ours was gzipped and plain if
    ours was plain (with filter=xopp, git hands the drivers plain XML and stores what they write
    as the blob)."""
    _KEYS.clear()
    try:
        return _merge_xopp(base, ours, theirs)
    finally:
        _KEYS.clear()


def _merge_xopp(base, ours, theirs):
    ours_gz = is_gzip(ours)
    plain = []
    for data in (base, ours, theirs):
        if data is None or len(data) == 0:
            plain.append(None)
            continue
        p = gunzip_or_none(data) if is_gzip(data) else data
        plain.append(p)
    pb, pa, pt = plain
    if pa is None or pt is None:
        return ours, "a version of the page cannot be read"
    # The trivial cases first: no parsing, and byte-for-byte keeps of what a side wrote.
    if pa == pt or (pb is not None and pt == pb):
        return ours, None
    if pb is not None and pa == pb:
        return (smudge_bytes(pt) if ours_gz else pt), None
    try:
        ra = ET.fromstring(pa)
        rt = ET.fromstring(pt)
        rb = ET.fromstring(pb) if pb is not None else None
    except ET.ParseError:
        return ours, "a version of the page is not valid XML"
    try:
        if rb is None:
            rb = _synth_base(ra, rt)
        if fkey(rt) == fkey(rb):
            return ours, None
        if fkey(ra) == fkey(rb):
            return (smudge_bytes(pt) if ours_gz else pt), None
        pages_b = [c for c in rb if c.tag == "page"]
        pages_a = [c for c in ra if c.tag == "page"]
        pages_t = [c for c in rt if c.tag == "page"]
        if not (len(pages_b) == len(pages_a) == len(pages_t)):
            raise CannotDecide("the number of pages differs")
        merged = [_merge_page(b, a, t) for b, a, t in zip(pages_b, pages_a, pages_t)]
    except CannotDecide as e:
        return ours, str(e)
    # <title>, <preview> and anything else outside the pages: theirs if only theirs changed it,
    # else ours. Never worth a conflict page -- X0urnal++ rewrites the preview on every save.
    nb = [c for c in rb if c.tag != "page"]
    na = [c for c in ra if c.tag != "page"]
    nt = [c for c in rt if c.tag != "page"]
    head = nt if [ekey(c) for c in na] == [ekey(c) for c in nb] else na
    root = ET.Element(ra.tag, dict(ra.attrib))   # root attributes from ours (README)
    root.text = "\n"
    for c in head:
        c = copy.deepcopy(c)
        c.tail = "\n"
        root.append(c)
    for p in merged:
        root.append(p)
    decl = _xml_decl(pa)
    text = decl + "\n" + ET.tostring(root, encoding="unicode") + "\n"
    out = text.encode(_decl_encoding(decl), "xmlcharrefreplace")
    return (smudge_bytes(out) if ours_gz else out), None


def xopp_title(data):
    """The <title> of a page file, unless it is Xournal++'s boilerplate; None if there is none."""
    try:
        p = gunzip_or_none(data) if is_gzip(data) else data
        root = ET.fromstring(p)
    except (ET.ParseError, TypeError):
        return None
    t = root.find("title")
    if t is None or not (t.text or "").strip():
        return None
    s = t.text.strip()
    if s.startswith("Xournal++ document"):
        return None
    return s


# ---- notebook.xml ------------------------------------------------------------------------------


class Node:
    """A <chapter> or <section>. attrs keep their order; children are Nodes or Unknowns."""

    __slots__ = ("tag", "attrs", "children")

    def __init__(self, tag, attrs, children=None):
        self.tag = tag
        self.attrs = dict(attrs)
        self.children = children if children is not None else []

    @property
    def id(self):
        return self.attrs.get("id")

    def title(self):
        return self.attrs.get("title", "")


class Unknown:
    """An element notebook.xml has that this code does not know: kept as it is, where it is."""

    __slots__ = ("elem",)

    def __init__(self, elem):
        self.elem = elem

    def key(self):
        return ekey(self.elem)


class Notebook:
    __slots__ = ("attrs", "children")

    def __init__(self, attrs, children=None):
        self.attrs = dict(attrs)
        self.children = children if children is not None else []


def empty_notebook(title=""):
    return Notebook({"version": "2", "title": title})


def parse_notebook(data):
    """notebook.xml bytes -> Notebook. Empty input -> an empty tree (git's base for add/add).
    Raises ValueError for anything that is not a <notebook> document."""
    if data is None or not data.strip():
        return empty_notebook()
    try:
        root = ET.fromstring(data)
    except ET.ParseError as e:
        raise ValueError("notebook.xml is not valid XML: %s" % e)
    if root.tag != "notebook":
        raise ValueError("notebook.xml has <%s> at the top, not <notebook>" % root.tag)

    def conv(elem):
        if elem.tag in ("chapter", "section"):
            attrs = dict(elem.attrib)
            if not attrs.get("id") and elem.tag == "section":
                # an id-less section still has a file; its name is the id (README: a section's
                # file is always sections/<id>.xopp)
                f = attrs.get("file", "")
                m = re.fullmatch(r"sections/(.+)\.xopp", f)
                if m:
                    attrs = {"id": m.group(1), **attrs}
            if attrs.get("id"):
                return Node(elem.tag, attrs, [conv(c) for c in elem])
        e = copy.deepcopy(elem)
        e.tail = None
        return Unknown(e)

    return Notebook(root.attrib, [conv(c) for c in root])


def _esc(s):
    out = []
    for ch in s:
        if ch == "&":
            out.append("&amp;")
        elif ch == "<":
            out.append("&lt;")
        elif ch == ">":
            out.append("&gt;")
        elif ch == '"':
            out.append("&quot;")
        elif ch == "\n":
            out.append("&#10;")
        elif ch == "\r":
            out.append("&#13;")
        elif ch == "\t":
            out.append("&#9;")
        elif ord(ch) < 0x20:
            continue   # not allowed in XML 1.0 at all
        else:
            out.append(ch)
    return "".join(out)


def _attr_text(attrs, first):
    names = [n for n in first if n in attrs] + [n for n in attrs if n not in first]
    return "".join(' %s="%s"' % (n, _esc(attrs[n])) for n in names)


def serialize_notebook(nb):
    """The README form: XML declaration, one node per line, two-space indent, attributes in the
    order id title file. A chapter is an opening and a closing line even when it is empty, as
    one2book writes it, so an empty chapter getting its first page is a one-line diff."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             "<notebook%s>" % _attr_text(nb.attrs, ("version", "title"))]

    def emit(children, depth):
        pad = "  " * depth
        for c in children:
            if isinstance(c, Unknown):
                lines.append(pad + ET.tostring(c.elem, encoding="unicode").strip())
            elif c.tag == "chapter" or c.children:
                lines.append(pad + "<%s%s>" % (c.tag, _attr_text(c.attrs, ("id", "title", "file"))))
                emit(c.children, depth + 1)
                lines.append(pad + "</%s>" % c.tag)
            else:
                lines.append(pad + "<section%s/>" % _attr_text(c.attrs, ("id", "title", "file")))

    emit(nb.children, 1)
    lines.append("</notebook>")
    return ("\n".join(lines) + "\n").encode("utf-8")


class Info:
    __slots__ = ("node", "parent", "pos")

    def __init__(self, node, parent, pos):
        self.node, self.parent, self.pos = node, parent, pos


def index_notebook(nb):
    """id -> Info(node, parent id or None for the top, position among its siblings), and
    parent id -> [child ids in order]."""
    idx = {}
    order = defaultdict(list)

    def walk(children, parent):
        for pos, c in enumerate(children):
            if isinstance(c, Node) and c.id not in idx:
                idx[c.id] = Info(c, parent, pos)
                order[parent].append(c.id)
                walk(c.children, c.id)

    walk(nb.children, None)
    return idx, order


def sections_of(nb):
    """Every section node, in tree order."""
    out = []

    def walk(children):
        for c in children:
            if isinstance(c, Node):
                if c.tag == "section":
                    out.append(c)
                walk(c.children)

    walk(nb.children)
    return out


def merge_notebooks(nb_b, nb_a, nb_t):
    """Three-way merge of the tree (README "notebook-merge").

    Returns (merged Notebook, [(id, our title, their title)] for titles both sides changed).
      - union of nodes by id
      - deleted on one side and unchanged on the other -> deleted; changed on the other -> kept
      - renamed or moved on one side -> that side
      - changed differently on both -> ours, and their title is reported
      - a kept node whose chapter was deleted brings the chapter back
      - order: the order of the side that changed this chapter's order, with the other side's
        new nodes slotted in after the node they follow there."""
    B, oB = index_notebook(nb_b)
    A, oA = index_notebook(nb_a)
    T, oT = index_notebook(nb_t)

    def changed(x, b):
        return x.node.attrs != b.node.attrs or x.parent != b.parent or x.node.tag != b.node.tag

    ids = list(A) + [i for i in T if i not in A]
    keep = {}           # id -> (tag, attrs, parent)
    titles = []
    for nid in ids:
        a, t, b = A.get(nid), T.get(nid), B.get(nid)
        if a and t and b is None:
            # both added the same id (no base to compare with): ours, plus what only theirs has
            attrs = dict(a.node.attrs)
            for n, v in t.node.attrs.items():
                attrs.setdefault(n, v)
            keep[nid] = (a.node.tag, attrs, a.parent)
        elif a and t:
            attrs = {}
            names = list(a.node.attrs) + [n for n in t.node.attrs if n not in a.node.attrs]
            for n in names:
                va, vt, vb = a.node.attrs.get(n), t.node.attrs.get(n), b.node.attrs.get(n)
                if va == vb:
                    v = vt
                elif vt == vb or va == vt:
                    v = va
                else:
                    v = va
                    if n == "title":
                        titles.append((nid, va, vt))
                if v is not None:
                    attrs[n] = v
            parent = t.parent if a.parent == b.parent else a.parent
            keep[nid] = (a.node.tag, attrs, parent)
        elif a:
            if b is None or changed(a, b):
                keep[nid] = (a.node.tag, dict(a.node.attrs), a.parent)
        elif t:
            if b is None or changed(t, b):
                keep[nid] = (t.node.tag, dict(t.node.attrs), t.parent)

    # A kept node needs its chapter: bring back chapters that were deleted on one side while the
    # other side put something in them.
    def bring_back(pid):
        while pid is not None and pid not in keep:
            src = A.get(pid) or T.get(pid)
            if src is None:
                return
            keep[pid] = (src.node.tag, dict(src.node.attrs), src.parent)
            pid = src.parent

    for nid in list(keep):
        bring_back(keep[nid][2])

    # Parents must be kept chapters, and moves on both sides must not make a loop (A moved X into
    # Y while B moved Y into X): a node in a loop goes back to ours' parent, or to the top.
    def fix_parent(nid):
        tag, attrs, parent = keep[nid]
        if parent is not None and (parent not in keep or keep[parent][0] != "chapter"):
            keep[nid] = (tag, attrs, None)

    for nid in list(keep):
        fix_parent(nid)
    for _ in range(len(keep) + 1):
        looped = None
        for nid in keep:
            seen = set()
            p = nid
            while p is not None and p not in seen:
                seen.add(p)
                p = keep[p][2]
            if p is not None:
                looped = p
                break
        if looped is None:
            break
        tag, attrs, _ = keep[looped]
        ours_parent = A[looped].parent if looped in A else None
        if ours_parent == keep[looped][2]:
            ours_parent = None
        keep[looped] = (tag, attrs, ours_parent)
        fix_parent(looped)

    members = defaultdict(set)
    for nid, (_, _, parent) in keep.items():
        members[parent].add(nid)

    def ordered(pid):
        mem = members.get(pid, set())
        seq_a = [x for x in oA.get(pid, []) if x in mem]
        seq_t = [x for x in oT.get(pid, []) if x in mem]
        if pid in oT and oA.get(pid, []) == oB.get(pid, []) and (pid is None or pid in A):
            skeleton, other = seq_t, seq_a     # ours did not touch this chapter's order
        else:
            skeleton, other = seq_a, seq_t
        res = list(skeleton)
        placed = set(res)
        in_other = set(other)
        for k, x in enumerate(other):
            if x in placed:
                continue
            pos = 0
            for y in reversed(other[:k]):
                if y in placed:
                    pos = res.index(y) + 1
                    break
            # past what only the skeleton side has there: both added pages after p2 -> ours'
            # first, then theirs' (the same rule as strokes: ours, then theirs on top)
            while pos < len(res) and res[pos] not in in_other:
                pos += 1
            res.insert(pos, x)
            placed.add(x)
        rest = [x for x in ids if x in mem and x not in placed]
        rest += sorted(x for x in mem if x not in placed and x not in rest)
        return res + rest

    # Unknown elements: ours' stay where they were; ones only theirs added go at the end.
    def unknowns(nb, pid_node):
        if pid_node is None:
            return [(k, c) for k, c in enumerate(nb.children) if isinstance(c, Unknown)]
        return [(k, c) for k, c in enumerate(pid_node.children) if isinstance(c, Unknown)]

    def build(pid):
        kids = []
        for nid in ordered(pid):
            tag, attrs, _ = keep[nid]
            if tag == "section" and not attrs.get("file"):
                attrs["file"] = section_file(nid)
            kids.append(Node(tag, attrs, build(nid)))
        a_node = None if pid is None else (A[pid].node if pid in A else None)
        t_node = None if pid is None else (T[pid].node if pid in T else None)
        b_node = None if pid is None else (B[pid].node if pid in B else None)
        ours_u = unknowns(nb_a, a_node) if (pid is None or a_node is not None) else []
        theirs_u = unknowns(nb_t, t_node) if (pid is None or t_node is not None) else []
        base_u = unknowns(nb_b, b_node) if (pid is None or b_node is not None) else []
        for k, u in ours_u:
            kids.insert(min(k, len(kids)), u)
        have = {u.key() for _, u in ours_u} | {u.key() for _, u in base_u}
        for _, u in theirs_u:
            if u.key() not in have:
                kids.append(u)
        return kids

    attrs = {}
    names = list(nb_a.attrs) + [n for n in nb_t.attrs if n not in nb_a.attrs]
    for n in names:
        va, vt, vb = nb_a.attrs.get(n), nb_t.attrs.get(n), nb_b.attrs.get(n)
        v = vt if va == vb else va
        if v is not None:
            attrs[n] = v
    return Notebook(attrs, build(None)), titles


# ---- git, from inside a driver -----------------------------------------------------------------


def git_out(args, cwd=None):
    """stdout of a git command as text, or None if it failed."""
    try:
        r = subprocess.run(["git"] + args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60,
                           **({"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}))
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    return r.stdout.decode("utf-8", "replace")


def worktree_top(cwd=None):
    top = git_out(["rev-parse", "--show-toplevel"], cwd)
    return top.strip() if top and top.strip() else None


def their_device_and_time(top, path):
    """Who made theirs, and when: for the conflict page's name and title.

    BOOK_SYNC_THEIR_DEVICE wins when set (one2book-sync sets it to "onenote"). Otherwise the author
    of the last commit on their side that touched the path -- book-sync passes their commit as
    BOOK_SYNC_THEIRS; a plain `git merge` leaves MERGE_HEAD, which is tried too."""
    forced = os.environ.get("BOOK_SYNC_THEIR_DEVICE", "").strip().lower()
    ref = os.environ.get("BOOK_SYNC_THEIRS", "").strip()
    if not ref and git_out(["rev-parse", "-q", "--verify", "MERGE_HEAD"], top):
        ref = "MERGE_HEAD"
    name, when = "", time.time()
    if ref:
        out = git_out(["log", "-1", "--format=%an%x00%ct", ref, "--", path], top)
        if not out or "\x00" not in out:
            out = git_out(["log", "-1", "--format=%an%x00%ct", ref], top)
        if out and "\x00" in out:
            n, t = out.strip().split("\x00", 1)
            name = n.strip().lower()
            try:
                when = float(t)
            except ValueError:
                pass
    dev = forced or name or "other"
    dev = re.sub(r"[^a-z0-9_.-]", "-", dev) or "other"
    return dev, when


def make_conflict_page(top, path, theirs, device=None, when=None):
    """Save theirs as a conflict page beside the original and leave a record for book-sync, which
    puts it in notebook.xml right after the original when it finishes the merge.

    `path` is the page's path in the book (sections/<id>.xopp or trash/<id>.xopp). A conflict of a
    page in the trash stays in the trash. Returns the conflict page's id."""
    if device is None or when is None:
        d, w = their_device_and_time(top, path)
        device = device or d
        when = when if when is not None else w
    folder, name = os.path.split(path)
    orig = name[: -len(".xopp")] if name.endswith(".xopp") else name
    ms = int(time.time() * 1000)
    while True:
        cid = conflict_id(orig, device, ms)
        rel = "%s/%s.xopp" % (folder, cid) if folder else "%s.xopp" % cid
        if not os.path.exists(os.path.join(top, rel)):
            break
        ms += 1
    write_atomic(os.path.join(top, rel), smudge_bytes(theirs))
    add_pending(top, PENDING_CONFLICTS, {"id": cid, "of": orig, "file": rel, "path": path,
                                         "device": device, "when": when})
    return cid


def add_pending(top, name, record):
    d = os.path.join(top, STATE_DIR)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "a", encoding="utf-8") as f:
        f.write(json.dumps(record) + "\n")
        f.flush()
        os.fsync(f.fileno())


def read_pending(top, name):
    p = os.path.join(top, STATE_DIR, name)
    out = []
    try:
        with open(p, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except ValueError:
                        pass
    except FileNotFoundError:
        pass
    return out


def clear_pending(top, name):
    try:
        os.unlink(os.path.join(top, STATE_DIR, name))
    except FileNotFoundError:
        pass
