/*
 * X0urnal++ -- the notebook tree in the sidebar.
 *
 * Modelled on SidebarIndexPage (gui/sidebar/indextree), which is already a
 * GtkTreeView whose selection navigates the document -- so this follows a
 * pattern the project already has rather than inventing one.
 *
 * The tab hides itself when no notebook is open: Sidebar::updateVisibleTabs
 * already hides any tab whose hasData() is false, so ordinary documents never
 * see it and no extra code is needed for that.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>
#include <set>
#include <string>

#include <gtk/gtk.h>

#include "gui/sidebar/AbstractSidebarPage.h"

class Control;

namespace x0urnal {
class NotebookNode;
}

class SidebarNotebookPage: public AbstractSidebarPage {
public:
    explicit SidebarNotebookPage(Control* control);
    ~SidebarNotebookPage() override;

    void enableSidebar() override;
    void disableSidebar() override;
    void layout() override;

    std::string getName() override;
    std::string getIconName() override;
    bool hasData() override;
    GtkWidget* getWidget() override;

    void selectPageNr(size_t page, size_t pdfPage) override;

    /* DocumentListener has no pure virtuals, so only the one that matters is
       overridden: the tree's content comes from the notebook model, not from
       the open document. */
    void documentChanged(DocumentChangeType type) override;

    /// Rebuild the tree from the controller's model.
    void rebuild();

private:
    static void onRowActivated(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn* col, SidebarNotebookPage* self);
    static void onHold(GtkGestureLongPress* g, gdouble x, gdouble y, SidebarNotebookPage* self);
    static void onBookMenu(GtkToggleButton* b, SidebarNotebookPage* self);

    /// Press and hold a row: rename, move up / down, move into a chapter.
    void showRowMenu(GtkTreePath* path, const std::string& id);
    /// Start renaming the row at `path` in place.
    void startRename(GtkTreePath* path);
    /// The book switcher's label: the open book's title.
    void updateBookButton();
    static void onEditingDone(GtkCellRenderer* r, SidebarNotebookPage* self);
    static void onAddSection(GtkButton* btn, SidebarNotebookPage* self);
    static void onAddChapter(GtkButton* btn, SidebarNotebookPage* self);
    static void onRemove(GtkButton* btn, SidebarNotebookPage* self);
    static void onSync(GtkButton* btn, SidebarNotebookPage* self);
    static void onTitleEdited(GtkCellRendererText* r, gchar* path, gchar* text, SidebarNotebookPage* self);
    static void onRowExpanded(GtkTreeView* view, GtkTreeIter* iter, GtkTreePath* path, SidebarNotebookPage* self);
    static void onRowCollapsed(GtkTreeView* view, GtkTreeIter* iter, GtkTreePath* path, SidebarNotebookPage* self);

    /// Open the remembered chapters under `parent` (nullptr = the top), and theirs in turn.
    void restoreOpen(GtkTreeIter* parent, const std::set<std::string>& open);
    std::string idAt(GtkTreeIter* iter) const;

    /// The id of the selected node, or empty.
    std::string selectedId() const;

    void appendNode(GtkTreeStore* store, GtkTreeIter* parent, const x0urnal::NotebookNode& node);

    GtkWidget* root = nullptr;
    GtkWidget* statusLabel = nullptr;  ///< the sync status line above the tree
    GtkWidget* bookButton = nullptr;   ///< every book on this device, one tap to switch
    GtkWidget* bookList = nullptr;     ///< the switcher's popover content, refilled when it opens
    GtkWidget* treeView = nullptr;
    GtkTreeStore* store = nullptr;
    GtkCellRenderer* titleRenderer = nullptr;  ///< editable only while a press-and-hold rename is open
    GtkTreeViewColumn* titleColumn = nullptr;
    GtkGesture* holdGesture = nullptr;         ///< press and hold a row = rename it
    gulong selectHandler = 0;

    /// Set while rebuild() repopulates, so selection churn does not open pages.
    bool rebuilding = false;
};
