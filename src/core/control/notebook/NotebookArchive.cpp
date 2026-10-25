/*
 * X0urnal++ -- reading and writing books.
 * See NotebookArchive.h for the design, NotebookModel.h for the format.
 *
 * @license GNU GPLv2 or later
 */

#include "NotebookArchive.h"

#include "util/StringUtils.h"  // char_cast: u8string() is char8_t in C++20

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <zip.h>
#include <zlib.h>  // gzopen: a fresh section is written as a gzipped .xopp

#include "model/notebook/NotebookModel.h"

namespace x0urnal {

namespace {

/* ---- small zip helpers ---------------------------------------------------- */

bool addEntry(zip_t* z, const std::string& name, const std::string& data, std::string& error) {
    /* zip_source_buffer does NOT copy by default; the 4th argument asks libzip
       to take ownership of a malloc'd block, so the buffer must outlive the
       zip_close. Copying into malloc here is simpler and safer than keeping a
       parallel container alive until close. */
    void* buf = malloc(data.size());
    if (!buf && !data.empty()) {
        error = "out of memory writing " + name;
        return false;
    }
    if (!data.empty()) {
        memcpy(buf, data.data(), data.size());
    }
    zip_source_t* src = zip_source_buffer(z, buf, data.size(), 1 /* libzip frees it */);
    if (!src) {
        free(buf);
        error = std::string("zip_source_buffer: ") + zip_strerror(z);
        return false;
    }
    if (zip_file_add(z, name.c_str(), src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
        zip_source_free(src);
        error = std::string("zip_file_add(") + name + "): " + zip_strerror(z);
        return false;
    }
    return true;
}

bool readEntry(zip_t* z, const std::string& name, std::string& out, std::string& error) {
    zip_stat_t st;
    if (zip_stat(z, name.c_str(), 0, &st) != 0) {
        error = "missing entry: " + name;
        return false;
    }
    zip_file_t* f = zip_fopen(z, name.c_str(), 0);
    if (!f) {
        error = std::string("zip_fopen(") + name + "): " + zip_strerror(z);
        return false;
    }
    out.resize(static_cast<size_t>(st.size));
    const zip_int64_t got = out.empty() ? 0 : zip_fread(f, out.data(), st.size);
    zip_fclose(f);
    if (got < 0 || static_cast<zip_uint64_t>(got) != st.size) {
        error = "short read: " + name;
        return false;
    }
    return true;
}

bool writeFile(const fs::path& p, const std::string& data, std::string& error) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) {
        error = "cannot write " + p.string();
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

/**
 * An empty infinite-canvas page, as a gzipped .xopp.
 *
 * Shared by create() and save() so a notebook never contains a section entry
 * without a file behind it.
 */
bool writeBlankSection(const fs::path& p, const std::string& title, std::string& error) {
    /* An A4 infinite page on the void with the blue graph grid -- the same paper
       the converted OneNote books use (one2book), so a new page in a book matches
       the pages around it. It grows as you write, like every infinite page. */
    constexpr double A4_W = 595.275590551181;
    constexpr double A4_H = 841.889763779528;

    /* A raw string literal: the payload is XML full of quotes, and escaping it
       by hand invites real newlines inside string literals. The title is user text, so it is
       escaped: a page called "R&D <draft>" must not produce a broken file. */
    xmlChar* escapedTitle = xmlEncodeSpecialChars(nullptr, BAD_CAST title.c_str());
    std::ostringstream xopp;
    xopp << R"(<?xml version="1.0" standalone="no"?>)"
         << "\n"
         << R"(<xournal creator="X0urnal++" fileversion="4">)"
         << "\n<title>" << reinterpret_cast<const char*>(escapedTitle) << "</title>\n"
         << R"(<page width=")" << A4_W << R"(" height=")" << A4_H << R"(" infinite="true">)"
         << "\n"
         << R"(<background type="solid" color="#05070aff" style="graph"/>)"
         << "\n<layer/>\n</page>\n</xournal>\n";
    xmlFree(escapedTitle);

    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    gzFile gz = gzopen(char_cast(p.u8string().c_str()), "wb");
    if (!gz) {
        error = "cannot create " + p.string();
        return false;
    }
    const std::string data = xopp.str();
    const bool wrote = gzwrite(gz, data.data(), static_cast<unsigned>(data.size())) == static_cast<int>(data.size());
    gzclose(gz);
    if (!wrote) {
        error = "cannot write " + p.string();
        return false;
    }
    return true;
}

bool readFile(const fs::path& p, std::string& out, std::string& error) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        error = "cannot read " + p.string();
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

/* ---- notebook.xml --------------------------------------------------------- */

void writeNode(xmlNodePtr parent, const NotebookNode& node) {
    for (const auto& child: node.children) {
        xmlNodePtr e = xmlNewChild(parent, nullptr,
                                   BAD_CAST(child->isChapter() ? "chapter" : "section"), nullptr);
        xmlNewProp(e, BAD_CAST "id", BAD_CAST child->id.c_str());
        xmlNewProp(e, BAD_CAST "title", BAD_CAST child->title.c_str());
        if (!child->isChapter()) {
            xmlNewProp(e, BAD_CAST "file", BAD_CAST child->sectionPath.c_str());
        }
        writeNode(e, *child);
    }
}

/* libxml2's formatted dump puts one node per line with a two-space indent and
   keeps attributes in the order they were added (id, title, file) -- exactly the
   stable text book-sync/README.md asks of notebook.xml, so `git diff` and the
   merge driver only ever see real changes. */
std::string serialiseIndex(const NotebookModel& model, const char* version = "1") {
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr root = xmlNewNode(nullptr, BAD_CAST "notebook");
    xmlNewProp(root, BAD_CAST "version", BAD_CAST version);
    xmlNewProp(root, BAD_CAST "title", BAD_CAST model.title().c_str());
    xmlDocSetRootElement(doc, root);
    writeNode(root, model.root());

    xmlChar* buf = nullptr;
    int len = 0;
    xmlDocDumpFormatMemoryEnc(doc, &buf, &len, "UTF-8", 1);
    std::string out(reinterpret_cast<const char*>(buf), static_cast<size_t>(len));
    xmlFree(buf);
    xmlFreeDoc(doc);
    return out;
}

std::string attr(xmlNodePtr n, const char* name) {
    xmlChar* v = xmlGetProp(n, BAD_CAST name);
    if (!v) {
        return {};
    }
    std::string s(reinterpret_cast<const char*>(v));
    xmlFree(v);
    return s;
}

void parseNode(xmlNodePtr xml, NotebookModel& model, NotebookNode* parent) {
    for (xmlNodePtr c = xml->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) {
            continue;
        }
        const bool isChapter = xmlStrcmp(c->name, BAD_CAST "chapter") == 0;
        const bool isSection = xmlStrcmp(c->name, BAD_CAST "section") == 0;
        if (!isChapter && !isSection) {
            continue;  // unknown element: ignore, do not fail
        }
        NotebookNode* node = model.add(isChapter ? NotebookNode::Kind::Chapter : NotebookNode::Kind::Section,
                                       attr(c, "title"), parent);
        /* Keep the id and file from the FILE rather than the freshly generated
           ones, so a reopened notebook keeps pointing at the same .xopp. */
        const std::string id = attr(c, "id");
        if (!id.empty()) {
            node->id = id;
        }
        const std::string file = attr(c, "file");
        if (isSection && !file.empty()) {
            node->sectionPath = file;
        }
        parseNode(c, model, node);
    }
}

}  // namespace

fs::path NotebookArchive::workPathFor(const fs::path& workDir, const std::string& sectionPath) {
    return workDir / fs::u8path(sectionPath);
}

bool NotebookArchive::open(const fs::path& archive, NotebookModel& model, const fs::path& workDir,
                           std::string& error) {
    int err = 0;
    zip_t* z = zip_open(char_cast(archive.u8string().c_str()), ZIP_RDONLY, &err);
    if (!z) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, err);
        error = std::string("cannot open notebook: ") + zip_error_strerror(&ze);
        zip_error_fini(&ze);
        return false;
    }

    std::string mimetype;
    if (readEntry(z, "mimetype", mimetype, error) && mimetype.rfind(MIMETYPE, 0) != 0) {
        error = "not a X0urnal++ notebook (wrong mimetype)";
        zip_close(z);
        return false;
    }

    std::string indexXml;
    if (!readEntry(z, INDEX_ENTRY, indexXml, error)) {
        zip_close(z);
        return false;
    }

    xmlDocPtr doc = xmlReadMemory(indexXml.data(), static_cast<int>(indexXml.size()), "notebook.xml", nullptr,
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        error = "notebook.xml is not valid XML";
        zip_close(z);
        return false;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    model.setTitle(attr(root, "title"));
    parseNode(root, model, nullptr);
    xmlFreeDoc(doc);

    /* Extract every section that the index claims exists. A section listed in
       the tree but absent from the archive is reported rather than silently
       producing an empty document. */
    std::error_code ec;
    fs::create_directories(workDir, ec);
    for (const NotebookNode* section: model.sections()) {
        std::string data;
        if (!readEntry(z, section->sectionPath, data, error)) {
            error = "section '" + section->title + "' is missing from the notebook (" + section->sectionPath + ")";
            zip_close(z);
            return false;
        }
        if (!writeFile(workPathFor(workDir, section->sectionPath), data, error)) {
            zip_close(z);
            return false;
        }
    }

    model.setPath(archive);
    zip_close(z);
    return true;
}

bool NotebookArchive::save(const fs::path& archive, const NotebookModel& model, const fs::path& workDir,
                           std::string& error) {
    /* Write beside the target, then rename over it. A crash mid-write then
       costs the temporary file, not the notebook. */
    const fs::path tmp = fs::path(archive).concat(".tmp");
    std::error_code ec;
    fs::remove(tmp, ec);

    int err = 0;
    zip_t* z = zip_open(char_cast(tmp.u8string().c_str()), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!z) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, err);
        error = std::string("cannot create notebook: ") + zip_error_strerror(&ze);
        zip_error_fini(&ze);
        return false;
    }

    bool ok = addEntry(z, "mimetype", MIMETYPE, error) && addEntry(z, INDEX_ENTRY, serialiseIndex(model), error);

    if (ok) {
        for (const NotebookNode* section: model.sections()) {
            const fs::path p = workPathFor(workDir, section->sectionPath);
            std::string data;
            if (!fs::exists(p)) {
                /*
                 * A section in the tree with no file yet -- added and never
                 * opened. Write an empty infinite-canvas page for it rather
                 * than skipping: skipping stores a notebook whose own index
                 * points at an entry that is not there, and reopening it then
                 * fails with "section is missing".
                 */
                if (!writeBlankSection(p, section->title, error)) {
                    ok = false;
                    break;
                }
            }
            if (!readFile(p, data, error) || !addEntry(z, section->sectionPath, data, error)) {
                ok = false;
                break;
            }
        }
    }

    if (zip_close(z) != 0 && ok) {
        error = "failed to finish writing the notebook";
        ok = false;
    }
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }

    fs::rename(tmp, archive, ec);
    if (ec) {
        /* Windows will not rename over an existing file. */
        fs::remove(archive, ec);
        fs::rename(tmp, archive, ec);
    }
    if (ec) {
        error = "could not replace the notebook file: " + ec.message();
        return false;
    }
    return true;
}

bool NotebookArchive::create(const fs::path& archive, NotebookModel& model, const fs::path& workDir,
                             std::string& error) {
    model.setTitle(std::string(char_cast(archive.stem().u8string().c_str())));
    NotebookNode* chapter = model.add(NotebookNode::Kind::Chapter, "Chapter 1", nullptr);
    NotebookNode* section = model.add(NotebookNode::Kind::Section, "Page 1", chapter);

    /* The first section is a real, empty page written now rather than left for
       the editor, so a fresh notebook never opens onto a missing file. */
    std::string blankError;
    if (!writeBlankSection(workPathFor(workDir, section->sectionPath), section->title, blankError)) {
        error = blankError;
        return false;
    }

    model.setPath(archive);
    return save(archive, model, workDir, error);
}

/* ---- folder books (format version 2) ----------------------------------------
 *
 * See book-sync/README.md. A book is a FOLDER that X0urnal++ edits in place:
 * notebook.xml plus sections/<id>.xopp, usually inside a git clone that book-sync
 * keeps in step with a remote. Every page is saved where it lives, so an
 * autosaved page survives the window closing, a crash or a reboot.
 */

bool NotebookArchive::isBookFolder(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::is_regular_file(dir / INDEX_ENTRY, ec);
}

bool NotebookArchive::isZippedBook(const fs::path& file) {
    std::error_code ec;
    return file.extension() == ".book" && fs::is_regular_file(file, ec);
}

bool NotebookArchive::openFolder(const fs::path& dir, NotebookModel& model, std::string& error) {
    const fs::path index = dir / INDEX_ENTRY;
    xmlDocPtr doc = xmlReadFile(char_cast(index.u8string().c_str()), nullptr, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        error = "notebook.xml is missing or not valid XML (" + index.string() + ")";
        return false;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || xmlStrcmp(root->name, BAD_CAST "notebook") != 0) {
        xmlFreeDoc(doc);
        error = "notebook.xml has no <notebook> element";
        return false;
    }
    model.setTitle(attr(root, "title"));
    parseNode(root, model, nullptr);
    xmlFreeDoc(doc);
    model.setPath(dir);
    return true;
}

bool NotebookArchive::writeIndex(const fs::path& dir, const NotebookModel& model, std::string& error) {
    /* Beside the real file, then renamed over it: book-sync may read notebook.xml
       at any moment and must never see half of one. */
    const fs::path index = dir / INDEX_ENTRY;
    const fs::path tmp = fs::path(index).concat(".tmp");
    if (!writeFile(tmp, serialiseIndex(model, "2"), error)) {
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, index, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "could not replace notebook.xml: " + ec.message();
        return false;
    }
    return true;
}

bool NotebookArchive::importZip(const fs::path& archive, const fs::path& dir, std::string& error) {
    /* Reuses open(): it reads the index and extracts every section into a
       directory -- which, pointed at the new book folder, is exactly an import.
       The index is then rewritten as version 2. */
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        error = "a notebook folder with that name already exists: " + dir.string();
        return false;
    }
    NotebookModel model;
    if (!open(archive, model, dir, error)) {
        fs::remove_all(dir, ec);
        return false;
    }
    if (!writeIndex(dir, model, error)) {
        return false;
    }
    return true;
}

bool NotebookArchive::createFolder(const fs::path& dir, NotebookModel& model, std::string& error) {
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        error = "a notebook with that name already exists: " + dir.string();
        return false;
    }
    std::string title(char_cast(dir.stem().u8string().c_str()));
    model.setTitle(title);
    NotebookNode* chapter = model.add(NotebookNode::Kind::Chapter, "Chapter 1", nullptr);
    NotebookNode* section = model.add(NotebookNode::Kind::Section, "Page 1", chapter);
    if (!writeBlankPage(dir / fs::u8path(section->sectionPath), section->title, error)) {
        return false;
    }
    model.setPath(dir);
    return writeIndex(dir, model, error);
}

bool NotebookArchive::writeBlankPage(const fs::path& p, const std::string& title, std::string& error) {
    return writeBlankSection(p, title, error);
}

}  // namespace x0urnal
