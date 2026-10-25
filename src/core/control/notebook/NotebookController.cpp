/*
 * X0urnal++ -- the open notebook. See NotebookController.h for the design and
 * book-sync/README.md for the sync contract.
 *
 * @license GNU GPLv2 or later
 */

#include "NotebookController.h"

#include "util/StringUtils.h"  // char_cast: u8string() is char8_t in C++20

#include <algorithm>
#include <cctype>

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#ifndef _WIN32
#include <fcntl.h>     // open
#include <sys/file.h>  // flock
#include <unistd.h>    // close, setsid
#endif

#include "control/Control.h"
#include "gui/MainWindow.h"
#include "gui/XournalView.h"
#include "gui/sidebar/Sidebar.h"
#include "model/Document.h"
#include "util/PathUtil.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"

#include "control/notebook/NotebookArchive.h"

#ifdef _WIN32
// last: its macros (DELETE, ERROR, ...) would break upstream headers above
#include <windows.h>  // CreateFileW, LockFileEx
#endif

namespace x0urnal {

namespace {

/* The rhythm (book-sync/README.md). A page saves this long after the last stroke:
   long enough that a pause mid-word doesn't write the file, short enough that
   nothing is ever more than a breath from being on disk. */
constexpr guint AUTOSAVE_MS = 2000;
/* After a burst of saves, the push waits this long for the burst to end, so one
   writing session is one commit rather than one per stroke. */
constexpr guint PUSH_MS = 20000;
/* Switching pages or coming back to the window only fetches from the remote if
   the last full sync is older than this: flipping through pages must not cost a
   network round trip per page. */
constexpr gint64 FULL_SYNC_EVERY_US = 60 * G_USEC_PER_SEC;
/* "Idle" for running a full sync on our own after a push saw newer changes: no
   edit for this long. */
constexpr gint64 IDLE_US = 5 * G_USEC_PER_SEC;

/* ---- a GTK chooser for books -------------------------------------------------
 *
 * Written here rather than added to xoj::OpenDlg: those helpers carry
 * format-specific filters for .xopp/.pdf, and extending them would mean hunks in
 * upstream files for a dialog only this feature uses. Both modes start in
 * ~/Documents/notebooks, where every book lives, so the books are in sight.
 */
void chooseNotebook(GtkWindow* parent, bool creating, const fs::path& start, std::function<void(fs::path)> callback) {
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
            creating ? _("New Notebook") : _("Open Notebook"), parent,
            creating ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_Cancel"),
            GTK_RESPONSE_CANCEL, creating ? _("_Create") : _("_Open"), GTK_RESPONSE_ACCEPT, nullptr);
    std::error_code ec;
    fs::create_directories(start, ec);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), char_cast(start.u8string().c_str()));
    if (creating) {
        /* No pre-filled name: the name typed in becomes the book's name, and
           ".book" is added below if it is left off. */
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), true);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "");
    }

    g_signal_connect(dlg, "response", G_CALLBACK(+[](GtkDialog* d, int response, gpointer data) {
                         auto* cb = static_cast<std::function<void(fs::path)>*>(data);
                         if (response == GTK_RESPONSE_ACCEPT) {
                             gchar* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
                             if (name) {
                                 fs::path p = fs::u8path(name);
                                 g_free(name);
                                 (*cb)(std::move(p));
                             }
                         }
                         delete cb;
                         gtk_widget_destroy(GTK_WIDGET(d));
                     }),
                     new std::function<void(fs::path)>(std::move(callback)));
    gtk_widget_show_all(dlg);
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/* "2024-01-31T14:02:11" -> "14:02". */
std::string hhmm(const std::string& iso) { return iso.size() >= 16 ? iso.substr(11, 5) : iso; }

#ifndef _WIN32
/* The background push started at quit runs in its own session, so it outlives
   X0urnal++ and isn't killed with the window's process group. */
void detach(gpointer) { setsid(); }
#endif

}  // namespace

NotebookController::NotebookController(Control* control):
        control(control), notebookModel(std::make_unique<NotebookModel>()) {}

NotebookController::~NotebookController() {
    if (autosaveSource) {
        g_source_remove(autosaveSource);
    }
    if (pushSource) {
        g_source_remove(pushSource);
    }
    releaseLock();
}

/* ---- where books are ------------------------------------------------------ */

bool NotebookController::isNotebookPath(const fs::path& p) {
    return NotebookArchive::isBookFolder(p) || NotebookArchive::isZippedBook(p) || !bookOf(p).empty();
}

bool NotebookController::shouldOpenAsNotebook(const fs::path& p) const {
    if (!openingFile.empty() && p == openingFile) {
        return false;  // our own page load: an ordinary document
    }
    return isNotebookPath(p);
}

fs::path NotebookController::bookOf(const fs::path& file) {
    /* A page is <book>/sections/<id>.xopp. Opened on its own (from Recent files, say) it would be
       edited outside its book -- no autosave, no sync, no tree -- so it opens as its book instead,
       on that page. */
    if (file.extension() != ".xopp" || file.parent_path().filename() != "sections") {
        return {};
    }
    const fs::path book = file.parent_path().parent_path();
    return NotebookArchive::isBookFolder(book) ? book : fs::path();
}

fs::path NotebookController::lastNotebook() {
    gchar* contents = nullptr;
    const fs::path f = Util::getConfigFile("last-notebook");
    if (!g_file_get_contents(char_cast(f.u8string().c_str()), &contents, nullptr, nullptr)) {
        return newestBook();  // nothing remembered on this device yet
    }
    fs::path p = fs::u8path(g_strstrip(contents));
    g_free(contents);
    if (NotebookArchive::isBookFolder(p)) {
        return p;
    }
    return newestBook();
}

fs::path NotebookController::newestBook() {
    /* Nothing remembered yet on this device (a fresh install, or the book it
       remembers is gone): the book written in most recently, so X0urnal++ never
       opens on an empty page when books are there. */
    fs::path best;
    fs::file_time_type bestTime{};
    for (const fs::path& b: books()) {
        std::error_code ec;
        const auto t = fs::last_write_time(b / "notebook.xml", ec);
        if (!ec && (best.empty() || t > bestTime)) {
            best = b;
            bestTime = t;
        }
    }
    return best;
}

std::vector<fs::path> NotebookController::books() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e: fs::directory_iterator(notebooksDir(), ec)) {
        if (e.is_directory(ec) && NotebookArchive::isBookFolder(e.path())) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        std::string x = char_cast(a.filename().u8string().c_str()), y = char_cast(b.filename().u8string().c_str());
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return std::tolower(c); });
        return x < y;
    });
    return out;
}

void NotebookController::rememberNotebook() const {
    /* Per device, in X0urnal++'s own settings folder: which book to pick back up next time. */
    const fs::path f = Util::getConfigFile("last-notebook");
    g_file_set_contents(char_cast(f.u8string().c_str()), char_cast(bookDir.u8string().c_str()), -1, nullptr);
}

fs::path NotebookController::notebooksDir() {
    const char* docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    fs::path base = docs ? fs::u8path(docs) : fs::u8path(g_get_home_dir()) / "Documents";
    return base / "notebooks";
}

fs::path NotebookController::pagePath(const NotebookNode& node) const {
    return bookDir / fs::u8path(node.sectionPath);
}

bool NotebookController::ownsCurrentDocument() const {
    if (!notebookOpen) {
        return false;
    }
    Document* doc = control->getDocument();
    doc->lock_shared();
    const fs::path p = doc->getFilepath();
    doc->unlock_shared();
    if (p.empty()) {
        return false;
    }
    const fs::path rel = p.lexically_relative(bookDir);
    return !rel.empty() && *rel.begin() != "..";
}

std::string NotebookController::windowTitle() const {
    if (!ownsCurrentDocument()) {
        return {};
    }
    Document* doc = control->getDocument();
    doc->lock_shared();
    const fs::path rel = doc->getFilepath().lexically_relative(bookDir);
    doc->unlock_shared();
    /* The page is found by the document's file, not by currentSection: Control titles the window
       while the page is still loading, and currentSection is set only once it has loaded. */
    const NotebookNode* page = nullptr;
    for (const NotebookNode* n: notebookModel->sections()) {
        if (fs::u8path(n->sectionPath).lexically_normal() == rel) {
            page = n;
            break;
        }
    }
    std::string book = notebookModel->title();
    if (book.empty()) {
        const fs::path dir = bookDir.has_filename() ? bookDir : bookDir.parent_path();
        book = char_cast(dir.stem().u8string().c_str());
    }
    if (!page) {
        return book;  // a file inside the book that is not one of its pages
    }
    return (page->title.empty() ? std::string(_("Untitled page")) : page->title) + " — " + book;  // em dash
}

/* ---- opening and closing --------------------------------------------------- */

void NotebookController::askToCreate() {
    chooseNotebook(control->getGtkWindow(), true, notebooksDir(), [this](fs::path path) {
        if (path.extension() != ".book") {
            path += ".book";
        }
        NotebookModel model;
        std::string error;
        if (!NotebookArchive::createFolder(path, model, error)) {
            XojMsgBox::showErrorToUser(control->getGtkWindow(), _("Could not create the notebook:\n") + error);
            return;
        }
        /* `book-sync init` makes it a git clone and pushes it -- a server with
           push-to-create makes the repo on that first push -- so a new book syncs
           from its first page. Without a configured remote it stays local. */
        const std::string exe = findBookSync();
        if (exe.empty()) {
            openNotebook(path);
            return;
        }
        setStatus(_("Setting up sync…"));
        std::vector<std::string> args = bookSyncArgv(exe);
        args.push_back("init");
        args.push_back(char_cast(path.u8string().c_str()));
        runBookSync(args,
                    [this, path](const std::string&) { openNotebook(path); });
    });
}

void NotebookController::askToOpen() {
    chooseNotebook(control->getGtkWindow(), false, notebooksDir(), [this](fs::path path) { openNotebook(path); });
}

bool NotebookController::openNotebook(const fs::path& path) {
    fs::path dir = fs::absolute(path);

    /* A page file: open its book, on that page. */
    std::string page;
    if (const fs::path book = bookOf(dir); !book.empty()) {
        page = char_cast(dir.stem().u8string().c_str());
        dir = book;
    }

    /* An old zipped book (format 1) is unpacked into the notebooks folder once, and
       that folder is what opens from then on. */
    if (NotebookArchive::isZippedBook(dir)) {
        const fs::path target = notebooksDir() / dir.filename();
        if (!NotebookArchive::isBookFolder(target)) {
            std::string error;
            if (!NotebookArchive::importZip(dir, target, error)) {
                XojMsgBox::showErrorToUser(control->getGtkWindow(), _("Could not open the notebook:\n") + error);
                return false;
            }
        }
        dir = target;
    }
    if (!NotebookArchive::isBookFolder(dir)) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(),
                                   _("That folder is not a notebook (it has no notebook.xml):\n") + dir.string());
        return false;
    }

    std::error_code ec;
    if (notebookOpen && fs::equivalent(dir, bookDir, ec)) {
        if (!page.empty()) {
            openSection(page);  // already open in this window: just go to the page
        }
        return true;
    }
    if (notebookOpen) {
        /* Another book is open in this window: let it go first. Its page is saved
           by the close below (never asked about), then it is sent. */
        autosaveNow([this](bool) { runSync(SyncKind::Push, nullptr); });
        releaseLock();
        notebookOpen = false;
        currentSection.clear();
    }

    if (!acquireLock(dir)) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(),
                                   _("This notebook is already open in another X0urnal++ window."));
        return false;
    }

    connectWindow();
    bookDir = dir;
    notebookModel = std::make_unique<NotebookModel>();
    notebookOpen = true;
    currentSection.clear();
    wantPage = page;
    showTreeOnOpen = true;
    fullSyncDue = false;
    rememberNotebook();

    /* Catch up with the other devices before anything is shown, so the book opens
       as it is everywhere -- then read the tree and open the page you were on. */
    runSync(SyncKind::Full, [this]() { loadTree(true); });
    return true;
}

void NotebookController::loadTree(bool openPage) {
    auto model = std::make_unique<NotebookModel>();
    std::string error;
    if (!NotebookArchive::openFolder(bookDir, *model, error)) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), _("Could not read the notebook:\n") + error);
        return;
    }
    notebookModel = std::move(model);
    fireChanged();
    control->updateWindowTitle();  // a sync may have renamed the open page or the book

    auto isPage = [this](const std::string& id) {
        const NotebookNode* n = id.empty() ? nullptr : notebookModel->findById(id);
        return n && !n->isChapter();
    };
    const auto sections = notebookModel->sections();

    if (openPage) {
        /* The page asked for (a page file was opened), else where you left off on this device,
           else the first page. */
        std::string want = isPage(wantPage) ? wantPage : isPage(currentSection) ? currentSection : lastPage();
        wantPage.clear();
        if (!isPage(want)) {
            want = sections.empty() ? std::string() : sections.front()->id;
        }
        currentSection.clear();
        if (!want.empty()) {
            openSectionNow(want);
        }
    } else if (!currentSection.empty() && !isPage(currentSection)) {
        /* The page you were on was removed on another device. It is in trash/ and
           in the book's history; show the first page instead of a dangling one. */
        currentSection.clear();
        if (!sections.empty()) {
            openSectionNow(sections.front()->id);
        }
    }
}

void NotebookController::openSection(const std::string& sectionId) {
    if (!notebookOpen) {
        return;
    }
    NotebookNode* node = notebookModel->findById(sectionId);
    if (!node || node->isChapter()) {
        return;  // chapters are containers; clicking one selects, it does not open
    }
    if (currentSection == sectionId && ownsCurrentDocument()) {
        return;
    }
    /* 1. Save the page you are leaving, without asking.
       2. If it has been a while, catch up with the other devices -- before the
          next page is loaded, so nothing you are looking at changes under you.
       3. Open the page. */
    autosaveNow([this, sectionId](bool) {
        const bool due = fullSyncDue || g_get_monotonic_time() - lastFullSync > FULL_SYNC_EVERY_US;
        if (due && !findBookSync().empty()) {
            runSync(SyncKind::Full, [this, sectionId]() { openSectionNow(sectionId); });
        } else {
            openSectionNow(sectionId);
        }
    });
}

void NotebookController::openSectionNow(const std::string& sectionId) {
    const NotebookNode* node = notebookModel->findById(sectionId);
    if (!node || node->isChapter()) {
        return;  // gone after a sync; loadTree() already picked another page
    }
    const fs::path file = pagePath(*node);
    if (!fs::exists(file)) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), _("That page is missing from the notebook."));
        return;
    }
    /* openFile closes the current document first; if it still has unsaved edits
       (a stroke landed after the save above), Control::close hands it back to
       saveInsteadOfAsking(), so it is saved, never asked about. */
    openingFile = file;  // Control's open path must load THIS as a page, not reopen the book
    control->openFile(
            file,
            [this, sectionId](bool ok) {
                openingFile.clear();
                if (!ok) {
                    return;
                }
                currentSection = sectionId;
                rememberPage();
                fireChanged();
                if (showTreeOnOpen) {
                    /* A book is navigated from its tree: show it, on the Notebook tab
                       (tab 0, see Sidebar::initTabs), or a book opened with the sidebar
                       hidden would show one page and no way to the others. */
                    showTreeOnOpen = false;
                    control->setShowSidebar(true);
                    if (Sidebar* sb = control->getSidebar()) {
                        sb->setSelectedTab(0);
                    }
                }
            },
            -1, false);
}

void NotebookController::closeNotebook() {
    if (!notebookOpen) {
        return;
    }
    autosaveNow([this](bool) {
        runSync(SyncKind::Push, [this]() {
            releaseLock();
            notebookOpen = false;
            currentSection.clear();
            notebookModel = std::make_unique<NotebookModel>();
            bookDir.clear();
            setStatus("");
            fireChanged();
            /* Back to an empty document: leaving a book's page open after "Close
               Notebook" would let it be edited outside the book. */
            control->openFileWithoutSavingTheCurrentDocument(fs::path(), false, -1, [](bool) {});
        });
    });
}

void NotebookController::onQuit() {
    if (!notebookOpen) {
        return;
    }
    if (autosaveSource) {
        g_source_remove(autosaveSource);
        autosaveSource = 0;
    }
    if (pushSource) {
        g_source_remove(pushSource);
        pushSource = 0;
    }
    const std::string exe = findBookSync();
    if (!exe.empty()) {
        /* The page was saved by Control::close (never asked). Commit it now -- local
           and quick -- and leave the sending to a push in its own session that
           finishes after we are gone. Offline? The commit waits for the next time
           this book syncs from anywhere on this device. */
        const std::string dir = char_cast(bookDir.u8string().c_str());
        auto argvOf = [&](const char* verb) {
            std::vector<std::string> v = bookSyncArgv(exe);
            v.push_back(verb);
            v.push_back(dir);
            return v;
        };
        auto cArgv = [](const std::vector<std::string>& v) {
            std::vector<const gchar*> c;
            for (const auto& a: v) {
                c.push_back(a.c_str());
            }
            c.push_back(nullptr);
            return c;
        };
        const auto saveV = argvOf("save");
        auto saveArgv = cArgv(saveV);
        g_spawn_sync(nullptr, const_cast<gchar**>(saveArgv.data()), nullptr,
                     GSpawnFlags(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr, nullptr,
                     nullptr, nullptr, nullptr);
        const auto pushV = argvOf("push");
        auto pushArgv = cArgv(pushV);
#ifndef _WIN32
        GSpawnChildSetupFunc setup = detach;
#else
        GSpawnChildSetupFunc setup = nullptr;
#endif
        g_spawn_async(nullptr, const_cast<gchar**>(pushArgv.data()), nullptr,
                      GSpawnFlags(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), setup, nullptr, nullptr,
                      nullptr);
    }
    releaseLock();
}

/* ---- saving -------------------------------------------------------------- */

void NotebookController::undoRedoChanged() {
    if (!ownsCurrentDocument() || !control->getUndoRedoHandler()->isChanged()) {
        return;
    }
    lastEdit = g_get_monotonic_time();
    scheduleAutosave();
}

void NotebookController::undoRedoPageChanged(PageRef) {}

void NotebookController::scheduleAutosave() {
    if (autosaveSource) {
        g_source_remove(autosaveSource);
    }
    autosaveSource = g_timeout_add(
            AUTOSAVE_MS,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<NotebookController*>(data);
                self->autosaveSource = 0;
                if (self->userIsMidEdit()) {
                    /* Saving ends text editing and drops a selection (saveImpl calls
                       clearSelectionEndText); doing that in the middle of typing or
                       while you move something would be worse than a few more
                       seconds unsaved. Leaving the page saves regardless. */
                    self->scheduleAutosave();
                    return G_SOURCE_REMOVE;
                }
                self->autosaveNow(nullptr);
                return G_SOURCE_REMOVE;
            },
            this);
}

bool NotebookController::userIsMidEdit() const {
    MainWindow* win = control->getWindow();
    XournalView* view = win ? win->getXournal() : nullptr;
    return view && (view->getTextEditor() || view->getSelection());
}

void NotebookController::autosaveNow(std::function<void(bool)> then) {
    if (autosaveSource) {
        g_source_remove(autosaveSource);
        autosaveSource = 0;
    }
    if (!ownsCurrentDocument() || !control->getUndoRedoHandler()->isChanged()) {
        if (then) {
            then(true);
        }
        return;
    }
    if (inputBlocked) {
        /* A full sync may be rewriting this page right now; write after it. (Input
           is off while it runs, so the only edits waiting are older than it.) */
        g_timeout_add(
                300,
                +[](gpointer data) -> gboolean {
                    auto* call = static_cast<std::pair<NotebookController*, std::function<void(bool)>>*>(data);
                    call->first->autosaveNow(std::move(call->second));
                    delete call;
                    return G_SOURCE_REMOVE;
                },
                new std::pair<NotebookController*, std::function<void(bool)>>(this, std::move(then)));
        return;
    }
    saving = true;
    control->save([this, then = std::move(then)](bool ok) {
        saving = false;
        saveFailed = !ok;
        if (ok) {
            schedulePush();
        } else {
            setStatus(_("Could not save this page"));
        }
        if (then) {
            then(ok);
        }
    });
}

bool NotebookController::saveInsteadOfAsking(std::function<void(bool)> then) {
    if (saveFailed || !ownsCurrentDocument()) {
        return false;
    }
    autosaveNow(std::move(then));
    return true;
}

/* ---- syncing ------------------------------------------------------------- */

std::string NotebookController::findBookSync() {
#ifndef _WIN32
    if (gchar* p = g_find_program_in_path("book-sync")) {
        std::string s(p);
        g_free(p);
        return s;
    }
#endif
    /* X0urnal++ started from a launcher or a desktop entry may not have ~/bin or
       ~/.local/bin on its PATH, so those are looked in too. On Windows that is
       %USERPROFILE%\bin\book-sync: a script with no extension, which Windows does
       not call executable -- bookSyncArgv runs it. PATH is not asked there: it
       answers with a name Windows can run, the terminal's book-sync.cmd beside the
       script, and Python cannot run that. */
#ifdef _WIN32
    const GFileTest runnable = G_FILE_TEST_IS_REGULAR;
#else
    const GFileTest runnable = G_FILE_TEST_IS_EXECUTABLE;
#endif
    for (const char* sub: {"bin", ".local/bin"}) {
        const fs::path p = fs::u8path(g_get_home_dir()) / sub / "book-sync";
        if (g_file_test(char_cast(p.u8string().c_str()), runnable)) {
            return char_cast(p.u8string().c_str());
        }
    }
    return {};
}

std::vector<std::string> NotebookController::bookSyncArgv(const std::string& exe) {
#ifdef _WIN32
    /* Windows runs a Python script only through Python. pythonw, not python: a
       console program started from a window gets a console window of its own, one
       black flash per sync (book-sync starts its git with CREATE_NO_WINDOW for the
       same reason). */
    for (const char* py: {"pythonw", "python"}) {
        if (gchar* p = g_find_program_in_path(py)) {
            std::string s(p);
            g_free(p);
            return {s, exe};
        }
    }
#endif
    return {exe};
}

void NotebookController::schedulePush() {
    if (pushSource) {
        g_source_remove(pushSource);
    }
    pushSource = g_timeout_add(
            PUSH_MS,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<NotebookController*>(data);
                self->pushSource = 0;
                self->autosaveNow([self](bool) { self->runSync(SyncKind::Push, nullptr); });
                return G_SOURCE_REMOVE;
            },
            this);
}

void NotebookController::syncNow() {
    if (!notebookOpen) {
        return;
    }
    autosaveNow([this](bool) { runSync(SyncKind::Full, nullptr); });
}

void NotebookController::runSync(SyncKind kind, std::function<void()> then) {
    if (!notebookOpen) {
        if (then) {
            then();
        }
        return;
    }
    const std::string exe = findBookSync();
    if (exe.empty()) {
        setStatus(_("Saved on this device · book-sync is not installed"));
        if (then) {
            then();
        }
        return;
    }
    if (syncing) {
        /* One book-sync at a time per book (it also locks, but queueing here keeps
           the order of what was asked): run once more when this one ends, as a
           full sync if anyone asked for one. */
        pendingAny = true;
        pendingFull = pendingFull || kind == SyncKind::Full;
        if (then) {
            pendingThens.push_back(std::move(then));
        }
        return;
    }
    syncing = true;
    if (kind == SyncKind::Full) {
        setInputBlocked(true, _("Syncing notebook…"));
    } else {
        setStatus(_("Saved · sending…"));
    }
    std::vector<std::string> args = bookSyncArgv(exe);
    args.push_back(kind == SyncKind::Full ? "sync" : "push");
    args.push_back(char_cast(bookDir.u8string().c_str()));
    runBookSync(args,
                [this, kind, then = std::move(then)](const std::string& out) mutable {
                    syncFinished(kind, out, std::move(then));
                });
}

void NotebookController::runBookSync(const std::vector<std::string>& args,
                                     std::function<void(const std::string&)> done) {
    std::vector<const gchar*> argv;
    for (const auto& a: args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    GError* err = nullptr;
#ifndef _WIN32
    GSubprocess* proc =
            g_subprocess_newv(argv.data(), GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                              &err);
#else
    /* A `sync` merges a book that is held open only when this X0urnal++ started it
       (book-sync's open_elsewhere). On Linux it asks /proc who its parent is;
       Windows has no /proc, and GLib starts the child through gspawn-win64-helper,
       which is gone by then. So say it. */
    GSubprocessLauncher* launcher =
            g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    g_subprocess_launcher_setenv(launcher, "BOOK_SYNC_STARTED_BY", "X0urnal++", true);
    GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
    g_object_unref(launcher);
#endif
    if (!proc) {
        std::string msg = err ? err->message : "could not start book-sync";
        if (err) {
            g_error_free(err);
        }
        done(R"({"state": "error", "message": ")" + msg + R"("})");
        return;
    }
    g_subprocess_communicate_utf8_async(
            proc, nullptr, nullptr,
            +[](GObject* src, GAsyncResult* res, gpointer data) {
                auto* cb = static_cast<std::function<void(const std::string&)>*>(data);
                gchar* out = nullptr;
                GError* e = nullptr;
                g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, &e);
                std::string s = out ? out : "";
                g_free(out);
                if (e) {
                    g_error_free(e);
                }
                g_object_unref(src);
                (*cb)(s);
                delete cb;
            },
            new std::function<void(const std::string&)>(std::move(done)));
}

void NotebookController::syncFinished(SyncKind kind, const std::string& out, std::function<void()> then) {
    if (kind == SyncKind::Full) {
        setInputBlocked(false, nullptr);
    }
    syncing = false;

    SyncResult r = parseResult(out);
    if (r.state.empty()) {
        r.state = "error";
        r.message = out.empty() ? "book-sync gave no answer" : out;
    }
    const gint64 now = g_get_monotonic_time();
    if (kind == SyncKind::Full && r.state != "error") {
        lastFullSync = now;
        fullSyncDue = false;
    }

    /* What the merge changed on disk is reloaded: the tree if notebook.xml changed,
       the open page if its file did -- unless something is about to open another
       page anyway (`then`), which reads the new files by itself. */
    if (notebookOpen && contains(r.changed, "notebook.xml")) {
        loadTree(false);
    }
    if (notebookOpen && !then && !currentSection.empty()) {
        const NotebookNode* node = notebookModel->findById(currentSection);
        if (node && contains(r.changed, node->sectionPath)) {
            if (control->getUndoRedoHandler()->isChanged()) {
                /* Can't happen while input is off during a full sync; if it ever
                   does, keep what is on screen (it saves over the merge, which is
                   still in the book's history) rather than throw edits away. */
                g_warning("X0urnal++: page changed on disk with unsaved edits; keeping the edits");
            } else {
                const std::string cur = currentSection;
                currentSection.clear();
                openSectionNow(cur);
            }
        }
    }

    if (r.state == "synced") {
        setStatus(r.message.empty() || r.message == "Up to date" ? FS(_F("Synced {1}") % hhmm(r.time))
                                                                 : r.message + " · " + hhmm(r.time));
    } else if (r.state == "conflict") {
        setStatus(FS(_F("Merged · {1} conflict page(s) to check") % r.conflicts.size()));
    } else if (r.state == "offline") {
        setStatus(_("Saved on this device · offline, syncs later"));
    } else if (r.state == "behind") {
        fullSyncDue = true;
        setStatus(_("Newer changes on another device"));
    } else if (r.state == "local") {
        setStatus(_("Saved on this device · not set up to sync"));
    } else {
        setStatus(_("Sync problem: ") + r.message);
    }

    if (then) {
        then();
    }

    if (pendingAny) {
        const SyncKind next = pendingFull ? SyncKind::Full : SyncKind::Push;
        auto thens = std::move(pendingThens);
        pendingThens.clear();
        pendingAny = pendingFull = false;
        runSync(next, [thens = std::move(thens)]() {
            for (const auto& t: thens) {
                t();
            }
        });
    } else if (kind == SyncKind::Push && r.state == "behind" && now - lastEdit > IDLE_US) {
        /* Another device has newer changes and you are not writing: bring them in
           now, with input off for the moment it takes. Mid-writing, they wait for
           the next page switch, focus or Sync tap. */
        runSync(SyncKind::Full, nullptr);
    }
}

void NotebookController::setInputBlocked(bool blocked, const char* why) {
    if (blocked == inputBlocked) {
        return;
    }
    inputBlocked = blocked;
    /* Control::block disables the actions and the sidebar and shows the status bar,
       but the pen goes straight to the page widget, so that is switched off too:
       a full sync may rewrite the open page, and a stroke drawn meanwhile would be
       lost on reload. */
    MainWindow* win = control->getWindow();
    XournalView* view = win ? win->getXournal() : nullptr;
    if (blocked) {
        control->block(why ? why : "");
        if (view) {
            gtk_widget_set_sensitive(view->getWidget(), false);
        }
        setStatus(why ? why : "");
    } else {
        if (view) {
            gtk_widget_set_sensitive(view->getWidget(), true);
        }
        control->unblock();
    }
}

NotebookController::SyncResult NotebookController::parseResult(const std::string& json) {
    /* book-sync prints exactly one flat JSON object of strings and string arrays
       (book-sync/README.md); this reads that and nothing more, so X0urnal++ needs
       no JSON library. Unknown keys are skipped. */
    SyncResult r;
    size_t i = json.find('{');
    if (i == std::string::npos) {
        return r;
    }
    auto ws = [&]() {
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
    };
    auto str = [&](std::string& out) -> bool {
        ws();
        if (i >= json.size() || json[i] != '"') {
            return false;
        }
        ++i;
        out.clear();
        while (i < json.size() && json[i] != '"') {
            char c = json[i++];
            if (c == '\\' && i < json.size()) {
                const char e = json[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > json.size()) {
                            return false;
                        }
                        const auto cp = static_cast<gunichar>(std::stoul(json.substr(i, 4), nullptr, 16));
                        i += 4;
                        gchar buf[8] = {0};
                        out.append(buf, static_cast<size_t>(g_unichar_to_utf8(cp, buf)));
                        break;
                    }
                    default: out += e;  // \" \\ \/
                }
            } else {
                out += c;
            }
        }
        ++i;  // closing quote
        return true;
    };
    ++i;  // '{'
    while (i < json.size()) {
        std::string key;
        if (!str(key)) {
            break;
        }
        ws();
        if (i < json.size() && json[i] == ':') {
            ++i;
        }
        ws();
        if (i < json.size() && json[i] == '"') {
            std::string val;
            str(val);
            if (key == "state") {
                r.state = val;
            } else if (key == "time") {
                r.time = val;
            } else if (key == "message") {
                r.message = val;
            }
        } else if (i < json.size() && json[i] == '[') {
            ++i;
            std::vector<std::string> items;
            ws();
            while (i < json.size() && json[i] != ']') {
                std::string item;
                if (!str(item)) {
                    break;
                }
                items.push_back(item);
                ws();
                if (i < json.size() && json[i] == ',') {
                    ++i;
                }
                ws();
            }
            ++i;  // ']'
            if (key == "changed") {
                r.changed = std::move(items);
            } else if (key == "conflicts") {
                r.conflicts = std::move(items);
            }
        } else {
            while (i < json.size() && json[i] != ',' && json[i] != '}') {
                ++i;  // a number, true/false/null: not used
            }
        }
        ws();
        if (i < json.size() && json[i] == ',') {
            ++i;
        } else {
            break;
        }
    }
    return r;
}

void NotebookController::setStatus(std::string s) {
    status = std::move(s);
    if (onStatus) {
        onStatus();
    }
}

/* ---- the window ------------------------------------------------------------ */

void NotebookController::connectWindow() {
    if (listening) {
        return;
    }
    listening = true;
    control->getUndoRedoHandler()->addUndoRedoListener(this);
    if (GtkWindow* win = control->getGtkWindow()) {
        g_signal_connect(win, "notify::is-active", G_CALLBACK(+[](GObject* w, GParamSpec*, gpointer data) {
                             auto* self = static_cast<NotebookController*>(data);
                             if (gtk_window_is_active(GTK_WINDOW(w))) {
                                 self->onFocusIn();
                             } else {
                                 self->onFocusOut();
                             }
                         }),
                         this);
    }
}

void NotebookController::onFocusOut() {
    if (!notebookOpen) {
        return;
    }
    /* You went somewhere else -- maybe to another device. Save, and send what is
       waiting now instead of after the burst timer. */
    if (pushSource || control->getUndoRedoHandler()->isChanged()) {
        if (pushSource) {
            g_source_remove(pushSource);
            pushSource = 0;
        }
        autosaveNow([this](bool) { runSync(SyncKind::Push, nullptr); });
    }
}

void NotebookController::onFocusIn() {
    if (!notebookOpen || syncing) {
        return;
    }
    /* Back after a while: what was written elsewhere meanwhile comes in now. */
    if (fullSyncDue || g_get_monotonic_time() - lastFullSync > FULL_SYNC_EVERY_US) {
        autosaveNow([this](bool) { runSync(SyncKind::Full, nullptr); });
    }
}

/* ---- the one-window-per-book lock and the last page ---------------------- */

bool NotebookController::acquireLock(const fs::path& dir) {
    /* Two windows on one book would each save the same page files over the other
       -- the one thing syncing can't merge, because it never sees both. The lock
       lives in .git (never committed) or, for a book that isn't a clone yet,
       beside notebook.xml. */
    const fs::path git = dir / ".git";
    std::error_code ec;
    const fs::path lock = fs::is_directory(git, ec) ? git / "x0urnal.lock" : dir / ".x0urnal.lock";
#ifndef _WIN32
    const int fd = ::open(char_cast(lock.u8string().c_str()), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return true;  // can't lock (read-only?): don't refuse to open over it
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    releaseLock();
    lockFd = fd;
#else
    /* Windows: no flock, so an exclusive lock on byte 0 of the same file -- the
       byte book-sync's open_elsewhere tests with msvcrt.locking, so the two agree.
       Shared for reading and writing, or book-sync could not open the file to
       test it; no security attributes, so the handle is not inherited and a
       book-sync started from here never holds it. */
    HANDLE h = CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return true;  // can't lock (read-only?): don't refuse to open over it
    }
    OVERLAPPED at = {};  // offset 0
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        CloseHandle(h);
        return false;
    }
    releaseLock();
    lockHandle = h;
#endif
    return true;
}

void NotebookController::releaseLock() {
#ifndef _WIN32
    if (lockFd >= 0) {
        flock(lockFd, LOCK_UN);
        ::close(lockFd);
        lockFd = -1;
    }
#else
    if (lockHandle) {
        /* Unlocked first: closing the handle frees the byte too, but Windows only
           promises to do that "eventually". */
        OVERLAPPED at = {};
        UnlockFileEx(static_cast<HANDLE>(lockHandle), 0, 1, 0, &at);
        CloseHandle(static_cast<HANDLE>(lockHandle));
        lockHandle = nullptr;
    }
#endif
}

void NotebookController::rememberPage() const {
    /* Per device, never synced: where you were HERE. In .git so it isn't committed. */
    std::error_code ec;
    if (!fs::is_directory(bookDir / ".git", ec) || currentSection.empty()) {
        return;
    }
    const fs::path p = bookDir / ".git" / "x0urnal-last-page";
    g_file_set_contents(char_cast(p.u8string().c_str()), currentSection.c_str(), -1, nullptr);
}

std::string NotebookController::lastPage() const {
    const fs::path p = bookDir / ".git" / "x0urnal-last-page";
    gchar* contents = nullptr;
    if (!g_file_get_contents(char_cast(p.u8string().c_str()), &contents, nullptr, nullptr)) {
        return {};
    }
    std::string s(contents);
    g_free(contents);
    return s;
}

std::set<std::string> NotebookController::openChapters() const {
    /* .git/x0urnal-open-chapters: one chapter id per line, beside x0urnal-last-page. */
    std::set<std::string> out;
    if (bookDir.empty()) {
        return out;
    }
    const fs::path p = bookDir / ".git" / "x0urnal-open-chapters";
    gchar* contents = nullptr;
    if (!g_file_get_contents(char_cast(p.u8string().c_str()), &contents, nullptr, nullptr)) {
        return out;
    }
    std::string all(contents);
    g_free(contents);
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find('\n', start);
        if (end == std::string::npos) {
            end = all.size();
        }
        if (end > start) {
            out.insert(all.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

void NotebookController::rememberOpenChapters(const std::set<std::string>& ids) const {
    std::error_code ec;
    if (!notebookOpen || !fs::is_directory(bookDir / ".git", ec)) {
        return;
    }
    std::string s;
    for (const auto& id: ids) {
        s += id;
        s += '\n';
    }
    const fs::path p = bookDir / ".git" / "x0urnal-open-chapters";
    g_file_set_contents(char_cast(p.u8string().c_str()), s.c_str(), -1, nullptr);
}

/* ---- tree edits -------------------------------------------------------------- */

void NotebookController::writeIndex() {
    std::string error;
    if (!NotebookArchive::writeIndex(bookDir, *notebookModel, error)) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), _("Could not save the notebook's page list:\n") + error);
        return;
    }
    schedulePush();
}

NotebookNode* NotebookController::addChapter(const std::string& title, const std::string& parentId) {
    if (!notebookOpen) {
        return nullptr;
    }
    NotebookNode* parent = parentId.empty() ? nullptr : notebookModel->findById(parentId);
    /* A section cannot contain a chapter; fall back to its parent's level by
       adding at the root, which is the least surprising outcome. */
    if (parent && !parent->isChapter()) {
        parent = nullptr;
    }
    NotebookNode* node = notebookModel->add(NotebookNode::Kind::Chapter, title, parent);
    writeIndex();
    fireChanged();
    return node;
}

NotebookNode* NotebookController::addSection(const std::string& title, const std::string& parentId) {
    if (!notebookOpen) {
        return nullptr;
    }
    NotebookNode* parent = parentId.empty() ? nullptr : notebookModel->findById(parentId);
    if (parent && !parent->isChapter()) {
        parent = nullptr;
    }
    NotebookNode* node = notebookModel->add(NotebookNode::Kind::Section, title, parent);

    /* A new page needs a real file at once, or opening it would fail -- and the
       file must exist before notebook.xml names it, so a sync never ships a tree
       pointing at nothing. */
    std::string error;
    if (!NotebookArchive::writeBlankPage(pagePath(*node), title, error)) {
        XojMsgBox::showErrorToUser(control->getGtkWindow(), _("Could not create the page:\n") + error);
    }
    writeIndex();
    fireChanged();
    return node;
}

bool NotebookController::moveNode(const std::string& id, const std::string& newParentId, int index) {
    if (!notebookOpen) {
        return false;
    }
    NotebookNode* parent = newParentId.empty() ? nullptr : notebookModel->findById(newParentId);
    if (!newParentId.empty() && !parent) {
        return false;
    }
    if (!notebookModel->move(id, parent, index)) {
        return false;
    }
    writeIndex();
    fireChanged();
    return true;
}

bool NotebookController::moveBy(const std::string& id, int delta) {
    if (!notebookOpen) {
        return false;
    }
    NotebookNode* parent = notebookModel->parentOf(id);
    if (!parent) {
        return false;
    }
    int at = -1;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i]->id == id) {
            at = static_cast<int>(i);
        }
    }
    const int to = at + delta;
    if (at < 0 || to < 0 || to >= static_cast<int>(parent->children.size())) {
        return false;  // already first / last
    }
    /* move() inserts before the child at an index counted before the move: up = before
       the one above, down = before the one after the one below. */
    const std::string parentId = parent == &notebookModel->root() ? "" : parent->id;
    return moveNode(id, parentId, delta < 0 ? to : to + 1);
}

bool NotebookController::removeNode(const std::string& id) {
    if (!notebookOpen) {
        return false;
    }
    NotebookNode* node = notebookModel->findById(id);
    if (!node) {
        return false;
    }
    /* Nothing is deleted: a removed page's file moves to trash/ (the book's
       recycle bin, synced like the rest) and every version of it stays in the
       book's history. A removed chapter takes its pages with it. */
    std::vector<const NotebookNode*> pages;
    if (node->isChapter()) {
        std::vector<const NotebookNode*> stack{node};
        while (!stack.empty()) {
            const NotebookNode* n = stack.back();
            stack.pop_back();
            for (const auto& c: n->children) {
                (c->isChapter() ? stack : pages).push_back(c.get());
            }
        }
    } else {
        pages.push_back(node);
    }
    bool removedCurrent = false;
    std::error_code ec;
    fs::create_directories(bookDir / "trash", ec);
    for (const NotebookNode* p: pages) {
        const fs::path from = pagePath(*p);
        fs::rename(from, bookDir / "trash" / from.filename(), ec);
        removedCurrent = removedCurrent || p->id == currentSection;
    }
    const bool ok = notebookModel->remove(id);
    if (ok) {
        writeIndex();
        fireChanged();
        if (removedCurrent) {
            currentSection.clear();
            const auto rest = notebookModel->sections();
            if (!rest.empty()) {
                openSectionNow(rest.front()->id);
            } else {
                control->openFileWithoutSavingTheCurrentDocument(fs::path(), false, -1, [](bool) {});
            }
        }
    }
    return ok;
}

void NotebookController::renameNode(const std::string& id, const std::string& title) {
    if (NotebookNode* node = notebookModel->findById(id)) {
        node->title = title;
        writeIndex();
        fireChanged();
        control->updateWindowTitle();
    }
}

}  // namespace x0urnal
