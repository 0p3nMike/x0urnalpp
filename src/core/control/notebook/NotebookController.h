/*
 * X0urnal++ -- the open notebook: which one, which page, saving and syncing.
 *
 * "Flipping into the notebook" is OPENING A DIFFERENT DOCUMENT in the same
 * window, with the notebook tree in the sidebar. Not a separate window
 * (duplicating MainWindow and the near-singleton Control is enormous), and not
 * a view toggle on the current document (an A4 PDF and an infinite whiteboard
 * are genuinely different page geometry).
 *
 * SYNCED BOOKS (the design is book-sync/README.md).
 *
 * A book is a FOLDER, edited in place: notebook.xml + sections/<id>.xopp in a git
 * clone of the book's repo on a git server. Each page is an ordinary .xopp opened as
 * the editor's document, so the whole existing load/save/undo path is reused.
 * This class adds what OneNote does around it:
 *
 *   - Never asks to save. A page saves itself AUTOSAVE_MS after the last edit,
 *     and before anything that would leave it (switching pages, closing, quitting,
 *     the window losing focus). Control::close asks saveInsteadOfAsking() first.
 *   - Sync runs on events, never on a timer that ticks while nobody is looking:
 *       * PUSH (`book-sync push`): after a burst of saves (PUSH_MS idle) and on
 *         focus-out. It commits and sends, and never touches a file on disk, so it
 *         runs while you keep writing.
 *       * FULL (`book-sync sync`): fetch, merge, push. It may rewrite the open page
 *         or notebook.xml, so it runs only where that is safe -- opening the book,
 *         switching pages (before the next page is loaded), the window coming back
 *         after a minute away, the Sync button, and after a push answered "behind"
 *         while you were idle -- with the page's input switched off for the second
 *         it takes. What it changed is reloaded.
 *   - One window per book: a lock in the book's .git folder.
 *
 * book-sync not installed, or a book with no remote, still works: everything is
 * saved locally and the status line says so.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glib.h>

#include "filesystem.h"

#include "model/notebook/NotebookModel.h"
#include "undo/UndoRedoHandler.h"  // UndoRedoListener

class Control;

namespace x0urnal {

class NotebookController: public UndoRedoListener {
public:
    explicit NotebookController(Control* control);
    ~NotebookController() override;

    bool isOpen() const { return notebookOpen; }
    NotebookModel& model() { return *notebookModel; }
    const NotebookModel& model() const { return *notebookModel; }

    /// The page currently loaded as the document, or empty.
    const std::string& currentSectionId() const { return currentSection; }

    /// A book folder, an old zipped `.book` (imported on open), or a page file inside a book.
    static bool isNotebookPath(const fs::path& p);
    /**
     * Control's open path asks this. True for a book, or for a page opened from outside (Recent
     * files) -- false for the page this controller is itself opening right now, which has to load
     * as an ordinary document. (Without that exception, opening a page would re-enter the book,
     * which opens the page, and so on without end.)
     */
    bool shouldOpenAsNotebook(const fs::path& p) const;
    /// The book a page file belongs to (`<book>/sections/<id>.xopp`), or empty.
    static fs::path bookOf(const fs::path& file);
    /// The book open last on this device, if it is still there; X0urnal++ started without a
    /// file opens it, so you pick up where you left off.
    static fs::path lastNotebook();
    /// The book on this device written in most recently (lastNotebook's fallback), or empty.
    static fs::path newestBook();
    /// Where books live on every device: ~/Documents/notebooks.
    static fs::path notebooksDir();

    /// Ask for a name and create a new book in notebooksDir().
    void askToCreate();
    /// Ask for a book folder and open it.
    void askToOpen();

    /// Open a book folder in place (an old zipped `.book` is imported first).
    bool openNotebook(const fs::path& path);

    /// Load a page as the editor's document. Saves the current one first, never asks.
    void openSection(const std::string& sectionId);

    /// Save, send, release the book, and return the editor to an empty document.
    void closeNotebook();

    /// True when the editor's document is a page of the open book.
    bool ownsCurrentDocument() const;

    /**
     * Control::updateWindowTitle asks this first. A page's file is named by its id, so upstream's
     * title, "on-e3571593-....xopp - X0urnal++", would say nothing in an app switcher or a dock.
     * For a page of the open book this gives "<page title> — <book title>" (Control adds
     * " - X0urnal++"); for any other document it is empty and upstream's title stays.
     */
    std::string windowTitle() const;

    /**
     * Control::close calls this before it would ask "save changes?". For a page
     * of the open book the answer is always yes, so it saves (silently) and
     * calls `then(saved)`. Returns false if this document is not ours to save, or
     * if the last save of it failed -- then the normal question is asked, so a
     * page that cannot be written is never silently thrown away.
     */
    bool saveInsteadOfAsking(std::function<void(bool)> then);

    /// Control::quit, after the document is closed: commit locally, send in the background.
    void onQuit();

    /// The Sync button: save, then a full sync now.
    void syncNow();

    /// One line for the sidebar: "Synced 14:02", "Saved on this device · offline", ...
    const std::string& statusText() const { return status; }

    /**
     * The chapters open in the sidebar, remembered per book and per device (never synced: how you
     * fold the tree HERE). A book with nothing remembered gives the empty set, so every chapter
     * starts closed.
     */
    std::set<std::string> openChapters() const;
    void rememberOpenChapters(const std::set<std::string>& ids) const;

    /// Tree edits. Each writes notebook.xml at once and schedules a push.
    NotebookNode* addChapter(const std::string& title, const std::string& parentId);
    NotebookNode* addSection(const std::string& title, const std::string& parentId);
    bool removeNode(const std::string& id);
    void renameNode(const std::string& id, const std::string& title);
    /// Into `newParentId` ("" = the top level), before the child now at `index` (-1 = last).
    bool moveNode(const std::string& id, const std::string& newParentId, int index);
    /// One place up (-1) or down (+1) among its siblings.
    bool moveBy(const std::string& id, int delta);

    /// Every book on this device: the folders in notebooksDir(), by name. The sidebar's
    /// book switcher lists these.
    static std::vector<fs::path> books();
    /// The open book's folder, or empty.
    const fs::path& currentBook() const { return bookDir; }

    /// Called by the sidebar so it can rebuild after the model changes.
    void setOnChanged(std::function<void()> fn) { onChanged = std::move(fn); }
    void fireChanged() const {
        if (onChanged) {
            onChanged();
        }
    }
    /// Called by the sidebar so it can redraw the status line.
    void setOnStatus(std::function<void()> fn) { onStatus = std::move(fn); }

    // UndoRedoListener: every edit restarts the autosave countdown.
    void undoRedoChanged() override;
    void undoRedoPageChanged(PageRef page) override;

private:
    enum class SyncKind { Push, Full };

    /// What one `book-sync` run answered (its JSON line).
    struct SyncResult {
        std::string state;  // synced | offline | behind | conflict | local | error
        std::string time;
        std::string message;
        std::vector<std::string> changed;
        std::vector<std::string> conflicts;
    };

    void loadTree(bool openPage);
    void openSectionNow(const std::string& sectionId);
    void scheduleAutosave();
    void autosaveNow(std::function<void(bool)> then);
    void schedulePush();
    void runSync(SyncKind kind, std::function<void()> then);
    void syncFinished(SyncKind kind, const std::string& out, std::function<void()> then);
    void runBookSync(const std::vector<std::string>& args, std::function<void(const std::string&)> done);
    static SyncResult parseResult(const std::string& json);
    void setInputBlocked(bool blocked, const char* why);
    void writeIndex();
    void setStatus(std::string s);
    void rememberPage() const;
    std::string lastPage() const;
    void rememberNotebook() const;
    bool acquireLock(const fs::path& dir);
    void releaseLock();
    void connectWindow();
    void onFocusIn();
    void onFocusOut();
    bool userIsMidEdit() const;
    fs::path pagePath(const NotebookNode& node) const;
    static std::string findBookSync();
    /// How to start it: the script itself, or on Windows pythonw + the script.
    static std::vector<std::string> bookSyncArgv(const std::string& exe);

    Control* control;
    std::unique_ptr<NotebookModel> notebookModel;
    std::string currentSection;
    bool notebookOpen = false;
    fs::path bookDir;

#ifndef _WIN32
    int lockFd = -1;
#else
    void* lockHandle = nullptr;  ///< a HANDLE; void* keeps windows.h out of this header
#endif
    bool listening = false;       ///< registered as an undo listener / window watcher
    bool showTreeOnOpen = false;  ///< the first page of a freshly opened book shows the sidebar
    std::string wantPage;         ///< a page asked for by path (a recent file), opened once the tree loads
    fs::path openingFile;         ///< the page file openSectionNow() is loading, until it has loaded

    guint autosaveSource = 0;
    guint pushSource = 0;
    bool saving = false;
    bool saveFailed = false;
    gint64 lastEdit = 0;       ///< g_get_monotonic_time() of the last edit
    gint64 lastFullSync = 0;   ///< ... of the last finished full sync
    bool fullSyncDue = false;  ///< a push saw newer changes on the remote

    bool syncing = false;
    bool inputBlocked = false;
    bool pendingAny = false;
    bool pendingFull = false;
    std::vector<std::function<void()>> pendingThens;

    std::string status;
    std::function<void()> onChanged;
    std::function<void()> onStatus;
};

}  // namespace x0urnal
