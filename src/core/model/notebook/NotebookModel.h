/*
 * X0urnal++ -- the notebook tree: book -> chapters -> sections.
 *
 * WHAT A NOTEBOOK IS ON DISK
 *
 * A book is a FOLDER (format version 2), edited in place and usually a git
 * clone kept in step by book-sync (book-sync/README.md):
 *
 *     MyNotes.book/
 *       notebook.xml               this tree: chapters, sections, order, titles
 *       sections/<id>.xopp         one page each, a COMPLETE, VALID .xopp
 *       trash/<id>.xopp            removed pages
 *
 * Format version 1 is the same tree and pages in a single zip file, with a
 * `mimetype` entry, the way `.xopp` and `.odt` are zips; X0urnal++ turns one into
 * a folder book when it opens it.
 *
 * The property that matters: every section is an ordinary xournalpp document
 * that stock xournalpp opens. If this fork is ever abandoned, no data is trapped.
 *
 * WHY SECTIONS ARE SEPARATE FILES RATHER THAN ONE BIG DOCUMENT
 *
 * Document is a flat `std::vector<PageRef>` and the `size_t` page index is the
 * universal currency across XournalView::viewPages, LayoutMapper, the sidebar
 * previews, every undo action and the Lua API. Keeping ONE SECTION as the open
 * document means page counts, numbering, previews, undo and save all keep
 * exactly their present meaning, and almost nothing upstream has to change.
 * Putting a whole notebook in one Document would have pushed a hierarchy through
 * all of that.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "filesystem.h"

namespace x0urnal {

/**
 * One node of the notebook tree.
 *
 * Chapters nest (a chapter may hold chapters), which covers OneNote's "section
 * group" without needing a separate kind. A section is a leaf holding a `.xopp`.
 */
struct NotebookNode {
    enum class Kind { Chapter, Section };

    Kind kind = Kind::Section;
    std::string id;     ///< stable, never shown; survives renames
    std::string title;  ///< what the sidebar shows

    /// Section only: path inside the archive, e.g. "sections/01-analysis.xopp".
    std::string sectionPath;

    std::vector<std::unique_ptr<NotebookNode>> children;

    bool isChapter() const { return kind == Kind::Chapter; }
};

/**
 * A whole notebook: the tree plus where it came from.
 *
 * This is pure data with no GTK and no Document knowledge, so it can be unit
 * tested and so the sidebar and the controller can both hold a reference
 * without a dependency cycle.
 */
class NotebookModel {
public:
    NotebookModel();
    ~NotebookModel();

    const fs::path& path() const { return archivePath; }
    void setPath(fs::path p) { archivePath = std::move(p); }

    const std::string& title() const { return notebookTitle; }
    void setTitle(std::string t) { notebookTitle = std::move(t); }

    NotebookNode& root() { return *rootNode; }
    const NotebookNode& root() const { return *rootNode; }

    /// Depth-first walk over every section leaf, in reading order.
    std::vector<const NotebookNode*> sections() const;

    /// Find a node by id, or nullptr. Linear: a notebook is tens of nodes.
    NotebookNode* findById(const std::string& id);

    /**
     * A fresh id that no node currently uses, unique across devices too.
     *
     * Synced books (book-sync/README.md "Ids") are edited on several devices
     * without talking to each other first, so a per-book counter ("n1", "n2",
     * ...) would hand two devices the same id for two different pages. An id is
     * `<device>-<ms since epoch, base 36>-<4 random base 36>`: the device name
     * separates machines, the time and the random tail separate two pages made
     * on one machine, and the loop still steps over any id the tree already
     * holds. Old "n<k>" ids from zipped books are kept as they are.
     */
    std::string newId();

    /// This machine's name in ids: BOOK_SYNC_DEVICE if set, else the hostname (see the .cpp).
    static std::string deviceName();

    /// Add a chapter or section under `parent` (nullptr = the root).
    NotebookNode* add(NotebookNode::Kind kind, const std::string& title, NotebookNode* parent);

    /// Remove a node and everything under it. Returns false if not found.
    bool remove(const std::string& id);
    /// The node that holds `id` (the root for a top-level node), or nullptr.
    NotebookNode* parentOf(const std::string& id);
    /**
     * Move a node, with everything under it, to `newParent` (nullptr = the root),
     * just before the child now at `index` there (counted before the move; -1 or past
     * the end = last). Refused for the root, for
     * a parent that is a page, and for a chapter moved into itself or below itself.
     */
    bool move(const std::string& id, NotebookNode* newParent, int index);

    bool empty() const { return rootNode->children.empty(); }

private:
    fs::path archivePath;
    std::string notebookTitle;
    std::unique_ptr<NotebookNode> rootNode;
};

}  // namespace x0urnal
