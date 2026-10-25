/*
 * X0urnal++ -- reading and writing books: the folder book (format version 2)
 * and the zipped `.book` (format version 1). See NotebookModel.h.
 *
 * A zipped book holds `mimetype`, `notebook.xml` (the tree) and one ordinary
 * `.xopp` per section under `sections/`. xournalpp's Document, LoadHandler and
 * SaveHandler all work on a filesystem path, so a zipped book's sections are
 * extracted to a folder and opened as perfectly normal documents; that folder is
 * a folder book, which is what X0urnal++ edits.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <string>

#include "filesystem.h"

namespace x0urnal {

class NotebookModel;

class NotebookArchive {
public:
    /**
     * Open a `.book`, filling `model` and extracting its sections into
     * `workDir`. Returns false and sets `error` on failure.
     */
    static bool open(const fs::path& archive, NotebookModel& model, const fs::path& workDir, std::string& error);

    /**
     * Write `model` and everything in `workDir` back to the archive.
     *
     * Written to a temporary file and then renamed over the original, so an
     * interrupted save cannot leave a half-written notebook where the real one
     * was.
     */
    static bool save(const fs::path& archive, const NotebookModel& model, const fs::path& workDir, std::string& error);

    /**
     * Create a new, empty notebook on disk with one chapter and one section,
     * and fill `model` to match.
     */
    static bool create(const fs::path& archive, NotebookModel& model, const fs::path& workDir, std::string& error);

    /// Where a section's extracted file lives while the notebook is open.
    static fs::path workPathFor(const fs::path& workDir, const std::string& sectionPath);

    /* ---- folder books (format version 2; book-sync/README.md) ----
     * A book is a folder edited in place -- notebook.xml + sections/<id>.xopp,
     * usually in a git clone kept in step with a remote by book-sync. The zip
     * above is only for importing zipped books and for sharing. */

    /// True for a folder holding notebook.xml.
    static bool isBookFolder(const fs::path& dir);
    /// True for an old zipped `.book` file.
    static bool isZippedBook(const fs::path& file);
    /// Read `dir`/notebook.xml into `model`; model.path() becomes `dir`.
    static bool openFolder(const fs::path& dir, NotebookModel& model, std::string& error);
    /// Write `dir`/notebook.xml (version 2): to notebook.xml.tmp, then renamed over it.
    static bool writeIndex(const fs::path& dir, const NotebookModel& model, std::string& error);
    /// Unpack a zipped `.book` into a NEW folder book at `dir`.
    static bool importZip(const fs::path& archive, const fs::path& dir, std::string& error);
    /// A new folder book at `dir` (must not exist): one chapter holding one page.
    static bool createFolder(const fs::path& dir, NotebookModel& model, std::string& error);
    /// An empty page at `p`: A4, infinite, black graph paper.
    static bool writeBlankPage(const fs::path& p, const std::string& title, std::string& error);

    /* Internal: written into the zip as a `mimetype` entry and checked on open,
       the way .odt and .xopp do it. Never shown to anyone, so it keeps the
       vendor-prefixed form even though the extension is the plain `.book`. */
    static constexpr const char* MIMETYPE = "application/x-x0urnal-notebook";
    static constexpr const char* INDEX_ENTRY = "notebook.xml";
};

}  // namespace x0urnal
