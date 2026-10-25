/*
 * X0urnal++ -- the notebook tree in the sidebar. See the header for the design.
 *
 * @license GNU GPLv2 or later
 */

#include "SidebarNotebookPage.h"

#include <algorithm>
#include <filesystem>

#include "control/Control.h"
#include "control/notebook/NotebookController.h"
#include "model/notebook/NotebookModel.h"
#include "util/gtk4_helper.h"
#include "util/i18n.h"
#include "util/StringUtils.h"  // char_cast

namespace fs = std::filesystem;

namespace {
/* Tree columns. The id is what everything is addressed by; the title is shown
   and is editable in place. */
enum { COL_TITLE = 0, COL_ID, COL_IS_CHAPTER, N_COLS };

/* A flat, finger-sized button that runs `fn` (popover menus: the book switcher, a row's menu). */
using Fn = std::function<void()>;
void runFn(GtkButton* b, gpointer) {
    if (auto* fn = static_cast<Fn*>(g_object_get_data(G_OBJECT(b), "x0urnal-fn"))) {
        (*fn)();
    }
}
GtkWidget* fnButton(const std::string& label, Fn fn) {
    GtkWidget* b = gtk_button_new_with_label(label.c_str());
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    if (GtkWidget* l = gtk_bin_get_child(GTK_BIN(b))) {
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    }
    gtk_widget_set_size_request(b, -1, 48);
    g_object_set_data_full(G_OBJECT(b), "x0urnal-fn", new Fn(std::move(fn)),
                           +[](gpointer p) { delete static_cast<Fn*>(p); });
    g_signal_connect(b, "clicked", G_CALLBACK(runFn), nullptr);
    return b;
}
GtkWidget* menuLabel(const char* text) {
    GtkWidget* l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_margin_start(l, 8);
    gtk_widget_set_margin_top(l, 6);
    gtk_widget_set_sensitive(l, false);
    return l;
}
/* Close a row menu. Its "closed" handler destroys it -- later, from idle, never inside one of
   its own buttons' handlers -- so closing and destroying happen in exactly one place each. */
void dropPopover(GtkWidget* pop) { gtk_popover_popdown(GTK_POPOVER(pop)); }
}  // namespace

SidebarNotebookPage::SidebarNotebookPage(Control* control): AbstractSidebarPage(control) {
    this->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    this->treeView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(this->store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(this->treeView), false);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(this->treeView), true);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(this->treeView), COL_TITLE);

    GtkTreeViewColumn* column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_expand(column, true);
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    /* Rename in place: a notebook whose chapters cannot be renamed is a filing
       cabinet with no labels. Press and hold a row to rename it. The cell is editable
       only while that rename is open: GTK starts editing an editable cell on a tap of the row that is
       already selected, so a second tap on a chapter -- to close it -- opened a rename instead. */
    g_object_set(G_OBJECT(renderer), "editable", false, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    /* Rows a finger or the pen can hit: 8 px above and below each title, and an expander arrow
       twice GTK's 14 px. */
    g_object_set(G_OBJECT(renderer), "ypad", 8, nullptr);
    {
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "treeview.view { -GtkTreeView-expander-size: 28; }", -1, nullptr);
        gtk_style_context_add_provider(gtk_widget_get_style_context(this->treeView), GTK_STYLE_PROVIDER(css),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
    g_signal_connect(renderer, "edited", G_CALLBACK(&SidebarNotebookPage::onTitleEdited), this);
    g_signal_connect(renderer, "editing-canceled", G_CALLBACK(&SidebarNotebookPage::onEditingDone), this);
    this->titleRenderer = renderer;
    this->titleColumn = column;
    gtk_tree_view_column_pack_start(column, renderer, true);
    gtk_tree_view_column_set_attributes(column, renderer, "text", COL_TITLE, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(this->treeView), column);

    /* One tap does the thing: a page opens, a chapter opens or closes -- its whole row,
       not only the arrow, which is a small target for a finger. Activation, not cursor-changed: the
       cursor does not change when you tap the row it is already on, so a chapter could open once
       and never close, and a rebuild moving the cursor would open pages nobody tapped. */
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(this->treeView), true);
    this->holdGesture = gtk_gesture_long_press_new(this->treeView);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(this->holdGesture), GTK_PHASE_CAPTURE);
    g_signal_connect(this->holdGesture, "pressed", G_CALLBACK(&SidebarNotebookPage::onHold), this);
    this->selectHandler =
            g_signal_connect(this->treeView, "row-activated", G_CALLBACK(&SidebarNotebookPage::onRowActivated), this);
    /* Remember how the tree is folded: every open and close by hand is written down,
       per book and per device, and rebuild() folds it back the same way. */
    g_signal_connect(this->treeView, "row-expanded", G_CALLBACK(&SidebarNotebookPage::onRowExpanded), this);
    g_signal_connect(this->treeView, "row-collapsed", G_CALLBACK(&SidebarNotebookPage::onRowCollapsed), this);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), this->treeView);
    gtk_widget_set_vexpand(scroll, true);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    auto addButton = [&](const char* label, const char* tip, GCallback cb) {
        GtkWidget* b = gtk_button_new_with_label(label);
        gtk_widget_set_tooltip_text(b, tip);
        g_signal_connect(b, "clicked", cb, this);
        gtk_box_append(GTK_BOX(buttons), b);
    };
    addButton(_("+ Page"), _("Add a page to this chapter"), G_CALLBACK(&SidebarNotebookPage::onAddSection));
    addButton(_("+ Chapter"), _("Add a chapter"), G_CALLBACK(&SidebarNotebookPage::onAddChapter));
    addButton(_("Remove"), _("Remove the selected page or chapter (it goes to the book's trash)"),
              G_CALLBACK(&SidebarNotebookPage::onRemove));
    /* Sync now: what OneNote's Sync Now is for -- you are about to switch devices,
       or you want what the other one just wrote. Big enough for a finger. */
    addButton(_("Sync"), _("Save, then bring this book up to date with your other devices"),
              G_CALLBACK(&SidebarNotebookPage::onSync));

    /* One line of sync status above the tree: Synced 14:02, offline,
       conflict pages to check. OneNote shows the same thing in its sync bar. */
    this->statusLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(this->statusLabel), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(this->statusLabel), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(this->statusLabel, 6);

    /* Every book on this device, one tap away. A popover, not a combo box: GTK3 drops a
       combo's popup menu on touch release (TouchMenubar.h, gtk#5115). */
    this->bookButton = gtk_menu_button_new();
    gtk_widget_set_tooltip_text(this->bookButton, _("Your notebooks: tap one to switch to it"));
    gtk_widget_set_size_request(this->bookButton, -1, 44);
    GtkWidget* bookPop = gtk_popover_new();
    this->bookList = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(this->bookList, 4);
    gtk_widget_set_margin_end(this->bookList, 4);
    gtk_widget_set_margin_top(this->bookList, 4);
    gtk_widget_set_margin_bottom(this->bookList, 4);
    gtk_popover_set_child(GTK_POPOVER(bookPop), this->bookList);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(this->bookButton), bookPop);
    g_signal_connect(this->bookButton, "toggled", G_CALLBACK(&SidebarNotebookPage::onBookMenu), this);

    this->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(this->root), this->bookButton);
    gtk_box_append(GTK_BOX(this->root), this->statusLabel);
    gtk_box_append(GTK_BOX(this->root), scroll);
    gtk_box_append(GTK_BOX(this->root), buttons);
    g_object_ref(this->root);
    gtk_widget_show_all(this->root);

    /* Rebuild whenever the model changes, wherever the change came from. */
    control->getNotebookController()->setOnChanged([this]() { this->rebuild(); });
    control->getNotebookController()->setOnStatus([this]() {
        const std::string& s = this->control->getNotebookController()->statusText();
        gtk_label_set_text(GTK_LABEL(this->statusLabel), s.c_str());
        gtk_widget_set_tooltip_text(this->statusLabel, s.c_str());
    });

    registerListener(control);
    rebuild();
}

SidebarNotebookPage::~SidebarNotebookPage() {
    if (this->holdGesture) {
        g_object_unref(this->holdGesture);
    }
    if (this->root) {
        g_object_unref(this->root);
    }
    if (this->store) {
        g_object_unref(this->store);
    }
}

void SidebarNotebookPage::enableSidebar() {}
void SidebarNotebookPage::disableSidebar() {}
void SidebarNotebookPage::layout() {}

std::string SidebarNotebookPage::getName() { return _("Notebook"); }

/* An icon the theme already ships, so no new asset is needed. */
std::string SidebarNotebookPage::getIconName() { return "xopp-sidebar-index"; }

/* Sidebar::updateVisibleTabs hides any tab whose hasData() is false, so this
   one line is the whole "hide it for ordinary documents" behaviour. */
bool SidebarNotebookPage::hasData() { return control->getNotebookController()->isOpen(); }

GtkWidget* SidebarNotebookPage::getWidget() { return this->root; }

void SidebarNotebookPage::selectPageNr(size_t, size_t) {}

void SidebarNotebookPage::documentChanged(DocumentChangeType) {}

void SidebarNotebookPage::appendNode(GtkTreeStore* treeStore, GtkTreeIter* parent, const x0urnal::NotebookNode& node) {
    for (const auto& child: node.children) {
        GtkTreeIter iter;
        gtk_tree_store_append(treeStore, &iter, parent);
        gtk_tree_store_set(treeStore, &iter, COL_TITLE, child->title.c_str(), COL_ID, child->id.c_str(),
                           COL_IS_CHAPTER, child->isChapter(), -1);
        appendNode(treeStore, &iter, *child);
    }
}

void SidebarNotebookPage::rebuild() {
    /* Repopulating moves the cursor and selects rows; nothing opens on that (pages open on
       row-activated), and the flag keeps the fold handlers from writing down the folding
       rebuild() itself does. */
    this->rebuilding = true;
    gtk_tree_store_clear(this->store);
    appendNode(this->store, nullptr, control->getNotebookController()->model().root());
    /* Folded as you left it, not all open. Nothing remembered for this book: every chapter
       closed. */
    restoreOpen(nullptr, control->getNotebookController()->openChapters());
    this->rebuilding = false;
    updateBookButton();

    /* NOT calling Sidebar::updateVisibleTabs() here: it is private, and it does
       not need calling. Sidebar::documentChanged already runs it on
       DOCUMENT_CHANGE_COMPLETE, which is exactly what fires when a section is
       opened or the notebook is closed -- so the tab appears and disappears on
       its own without widening upstream's API. */
}

std::string SidebarNotebookPage::selectedId() const {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(this->treeView));
    GtkTreeIter iter;
    GtkTreeModel* model = nullptr;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) {
        return {};
    }
    gchar* id = nullptr;
    gtk_tree_model_get(model, &iter, COL_ID, &id, -1);
    std::string out = id ? id : "";
    g_free(id);
    return out;
}

void SidebarNotebookPage::onRowActivated(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn*,
                                         SidebarNotebookPage* self) {
    GtkTreeIter iter;
    if (self->rebuilding || !gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store), &iter, path)) {
        return;
    }
    gboolean isChapter = false;
    gtk_tree_model_get(GTK_TREE_MODEL(self->store), &iter, COL_IS_CHAPTER, &isChapter, -1);
    if (isChapter) {
        /* row-expanded / row-collapsed remember it. Only this chapter: never its siblings. */
        if (gtk_tree_view_row_expanded(view, path)) {
            gtk_tree_view_collapse_row(view, path);
        } else {
            gtk_tree_view_expand_row(view, path, false);
        }
        return;
    }
    self->control->getNotebookController()->openSection(self->idAt(&iter));
}

void SidebarNotebookPage::onAddSection(GtkButton*, SidebarNotebookPage* self) {
    /* The chapter the new page lands in opens, so the page you just made is in sight. */
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(self->treeView));
    GtkTreeIter iter, chapter;
    GtkTreeModel* model = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        gboolean isChapter = false;
        gtk_tree_model_get(model, &iter, COL_IS_CHAPTER, &isChapter, -1);
        bool found = true;
        if (isChapter) {
            chapter = iter;
        } else {
            found = gtk_tree_model_iter_parent(model, &chapter, &iter);
        }
        if (found) {
            auto* nc = self->control->getNotebookController();
            auto open = nc->openChapters();
            open.insert(self->idAt(&chapter));
            nc->rememberOpenChapters(open);
        }
    }
    self->control->getNotebookController()->addSection(_("New page"), self->selectedId());
}

std::string SidebarNotebookPage::idAt(GtkTreeIter* iter) const {
    gchar* id = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(this->store), iter, COL_ID, &id, -1);
    std::string out = id ? id : "";
    g_free(id);
    return out;
}

void SidebarNotebookPage::restoreOpen(GtkTreeIter* parent, const std::set<std::string>& open) {
    GtkTreeModel* model = GTK_TREE_MODEL(this->store);
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, parent)) {
        return;
    }
    do {
        if (gtk_tree_model_iter_has_child(model, &child) && open.count(idAt(&child))) {
            GtkTreePath* path = gtk_tree_model_get_path(model, &child);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(this->treeView), path, false);
            gtk_tree_path_free(path);
            restoreOpen(&child, open);  // a group's own chapters, as they were left inside it
        }
    } while (gtk_tree_model_iter_next(model, &child));
}

void SidebarNotebookPage::onRowExpanded(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*, SidebarNotebookPage* self) {
    if (self->rebuilding) {
        return;
    }
    auto* nc = self->control->getNotebookController();
    auto open = nc->openChapters();
    open.insert(self->idAt(iter));
    nc->rememberOpenChapters(open);
    /* GTK forgets how the rows inside were folded when their parent closes: open the ones that
       were open, the way OneNote's section groups come back. */
    self->rebuilding = true;
    self->restoreOpen(iter, open);
    self->rebuilding = false;
}

void SidebarNotebookPage::onRowCollapsed(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*, SidebarNotebookPage* self) {
    if (self->rebuilding) {
        return;
    }
    auto* nc = self->control->getNotebookController();
    auto open = nc->openChapters();
    open.erase(self->idAt(iter));  // its own chapters stay remembered for when it opens again
    nc->rememberOpenChapters(open);
}

void SidebarNotebookPage::onAddChapter(GtkButton*, SidebarNotebookPage* self) {
    self->control->getNotebookController()->addChapter(_("New chapter"), self->selectedId());
}

void SidebarNotebookPage::onRemove(GtkButton*, SidebarNotebookPage* self) {
    const std::string id = self->selectedId();
    if (id.empty()) {
        return;
    }
    /* Removing pages of notes still deserves a question, even though nothing is
       destroyed: the page moves to the book's trash/ and its history stays. */
    GtkWidget* dlg = gtk_message_dialog_new(self->control->getGtkWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                            GTK_BUTTONS_OK_CANCEL, "%s",
                                            _("Move this to the notebook's trash, with everything inside it?"));
    const int response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (response == GTK_RESPONSE_OK) {
        self->control->getNotebookController()->removeNode(id);
    }
}

void SidebarNotebookPage::onSync(GtkButton*, SidebarNotebookPage* self) {
    self->control->getNotebookController()->syncNow();
}

void SidebarNotebookPage::onHold(GtkGestureLongPress* g, gdouble x, gdouble y, SidebarNotebookPage* self) {
    /* Held, not tapped: this touch opens the row's menu, so the tree view must not also take it as
       a tap (which would open the page or fold the chapter when the finger lifts). */
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    gint bx = 0, by = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(self->treeView), static_cast<gint>(x),
                                                      static_cast<gint>(y), &bx, &by);
    GtkTreePath* path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(self->treeView), bx, by, &path, nullptr, nullptr, nullptr)) {
        return;
    }
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store), &iter, path)) {
        self->showRowMenu(path, self->idAt(&iter));
    }
    gtk_tree_path_free(path);
}

void SidebarNotebookPage::startRename(GtkTreePath* path) {
    g_object_set(G_OBJECT(this->titleRenderer), "editable", true, nullptr);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(this->treeView), path, this->titleColumn, true);
}

void SidebarNotebookPage::showRowMenu(GtkTreePath* path, const std::string& id) {
    /* Rearranging chapters and pages. A menu under the finger rather than drag and drop:
       a drag on a tablet starts too easily, and GTK3's tree drag cannot be told "not into a page".
       Moves go through the controller, which writes notebook.xml and sends it like any edit. */
    auto* nc = this->control->getNotebookController();
    x0urnal::NotebookNode* node = nc->model().findById(id);
    if (!node) {
        return;
    }
    GdkRectangle rect;
    gtk_tree_view_get_cell_area(GTK_TREE_VIEW(this->treeView), path, this->titleColumn, &rect);
    gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(this->treeView), rect.x, rect.y, &rect.x, &rect.y);
    GtkWidget* pop = gtk_popover_new();
    gtk_popover_set_relative_to(GTK_POPOVER(pop), this->treeView);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_size_request(box, 240, -1);

    gchar* pathStr = gtk_tree_path_to_string(path);
    const std::string where = pathStr ? pathStr : "";
    g_free(pathStr);
    gtk_box_append(GTK_BOX(box), fnButton(_("Rename"), [this, pop, where]() {
        dropPopover(pop);
        if (GtkTreePath* p = gtk_tree_path_new_from_string(where.c_str())) {
            startRename(p);
            gtk_tree_path_free(p);
        }
    }));
    gtk_box_append(GTK_BOX(box), fnButton(_("Move up"), [nc, pop, id]() {
        dropPopover(pop);
        nc->moveBy(id, -1);
    }));
    gtk_box_append(GTK_BOX(box), fnButton(_("Move down"), [nc, pop, id]() {
        dropPopover(pop);
        nc->moveBy(id, +1);
    }));

    /* Move into: every chapter it may go into (not its own parent, not itself or anything
       under it), indented as in the tree, and the top level. */
    x0urnal::NotebookNode* parent = nc->model().parentOf(id);
    const std::string parentId = parent && parent != &nc->model().root() ? parent->id : "";
    std::vector<std::pair<std::string, std::string>> targets;  // (id, label)
    std::function<void(const x0urnal::NotebookNode&, int)> walk = [&](const x0urnal::NotebookNode& n, int depth) {
        for (const auto& c: n.children) {
            if (!c->isChapter() || c->id == id) {
                continue;  // a page can't hold anything; a chapter never goes into itself
            }
            if (c->id != parentId) {
                targets.emplace_back(c->id, std::string(static_cast<size_t>(depth) * 3, ' ') + c->title);
            }
            walk(*c, depth + 1);
        }
    };
    walk(nc->model().root(), 0);
    if (!parentId.empty() && node->isChapter()) {
        targets.emplace(targets.begin(), "", _("(top level)"));
    }
    if (!targets.empty()) {
        gtk_box_append(GTK_BOX(box), menuLabel(_("Move into")));
        GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        for (const auto& [tid, label]: targets) {
            gtk_box_append(GTK_BOX(list), fnButton(label, [nc, pop, id, tid = tid, this]() {
                dropPopover(pop);
                if (!tid.empty()) {  // the chapter it went into opens, so it stays in sight
                    auto open = nc->openChapters();
                    open.insert(tid);
                    nc->rememberOpenChapters(open);
                }
                nc->moveNode(id, tid, -1);
            }));
        }
        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
        gtk_box_append(GTK_BOX(box), scroll);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_show_all(box);
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) {
        g_idle_add(+[](gpointer w) -> gboolean {
            gtk_widget_destroy(GTK_WIDGET(w));
            return G_SOURCE_REMOVE;
        }, p);
    }), nullptr);
    gtk_popover_popup(GTK_POPOVER(pop));
}

void SidebarNotebookPage::updateBookButton() {
    auto* nc = this->control->getNotebookController();
    std::string title = nc->isOpen() ? nc->model().title() : std::string(_("Notebooks"));
    if (title.empty() && nc->isOpen()) {
        title = char_cast(nc->currentBook().stem().u8string().c_str());
    }
    std::replace(title.begin(), title.end(), '_', ' ');  // as the switcher's list shows it
    /* Its own label, shown: gtk4_helper's gtk_menu_button_set_label swaps in a new label that is
       never shown, which left a sliver of a button with nothing in it. */
    GtkWidget* label = gtk_label_new((title + "  ▾").c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_button_set_child(GTK_BUTTON(this->bookButton), label);
    gtk_widget_show(label);
}

void SidebarNotebookPage::onBookMenu(GtkToggleButton* b, SidebarNotebookPage* self) {
    if (!gtk_toggle_button_get_active(b)) {
        return;
    }
    /* Refilled every time it opens: a book cloned since (book-sync clone) shows up. */
    GList* kids = gtk_container_get_children(GTK_CONTAINER(self->bookList));
    for (GList* k = kids; k; k = k->next) {
        gtk_widget_destroy(GTK_WIDGET(k->data));
    }
    g_list_free(kids);
    auto* nc = self->control->getNotebookController();
    GtkWidget* pop = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(b)));
    const auto books = x0urnal::NotebookController::books();
    if (books.empty()) {
        gtk_box_append(GTK_BOX(self->bookList), menuLabel(_("No notebooks in Documents/notebooks yet")));
    }
    std::error_code ec;
    for (const auto& book: books) {
        std::string name = char_cast(book.stem().u8string().c_str());
        std::replace(name.begin(), name.end(), '_', ' ');
        const bool current = nc->isOpen() && fs::equivalent(book, nc->currentBook(), ec);
        gtk_box_append(GTK_BOX(self->bookList), fnButton((current ? "• " : "   ") + name, [nc, pop, book, current]() {
            gtk_popover_popdown(GTK_POPOVER(pop));
            if (!current) {
                nc->openNotebook(book);
            }
        }));
    }
    gtk_widget_show_all(self->bookList);
}

void SidebarNotebookPage::onEditingDone(GtkCellRenderer* r, SidebarNotebookPage*) {
    g_object_set(G_OBJECT(r), "editable", false, nullptr);
}

void SidebarNotebookPage::onTitleEdited(GtkCellRendererText* r, gchar* path, gchar* text, SidebarNotebookPage* self) {
    g_object_set(G_OBJECT(r), "editable", false, nullptr);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(self->store), &iter, path)) {
        return;
    }
    gchar* id = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(self->store), &iter, COL_ID, &id, -1);
    if (id) {
        self->control->getNotebookController()->renameNode(id, text ? text : "");
        g_free(id);
    }
}
