/*
 * X0urnal++ -- the menu bar, made of popovers. See TouchMenubar.h for why.
 *
 * @license GNU GPLv2 or later
 */

#include "TouchMenubar.h"

namespace x0urnal {

namespace {

/* (Re)create one button per top-level item. Called at build time and whenever the top level
   changes (a plugin can add a menu after the window is up). */
void fill(GtkWidget* bar, GMenuModel* model) {
    GList* old = gtk_container_get_children(GTK_CONTAINER(bar));
    for (GList* l = old; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(old);

    const int n = g_menu_model_get_n_items(model);
    for (int i = 0; i < n; i++) {
        gchar* label = nullptr;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
        GMenuModel* sub = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
        if (label && sub) {
            GtkWidget* btn = gtk_menu_button_new();
            /* The label replaces the button's default arrow; "_File" keeps its mnemonic, so
               Alt+F still opens it from a keyboard. */
            gtk_button_set_label(GTK_BUTTON(btn), label);
            gtk_button_set_use_underline(GTK_BUTTON(btn), true);
            gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
            gtk_menu_button_set_use_popover(GTK_MENU_BUTTON(btn), true);
            gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn), sub);
            /* Opening a menu must not pull keyboard focus off the page (or a text box). */
            gtk_widget_set_focus_on_click(btn, false);
            gtk_style_context_add_class(gtk_widget_get_style_context(btn), "x0urnal-menubutton");
            gtk_box_pack_start(GTK_BOX(bar), btn, false, false, 0);
        }
        if (sub) {
            g_object_unref(sub);
        }
        g_free(label);
    }
    gtk_widget_show_all(bar);
}

}  // namespace

GtkWidget* buildTouchMenubar(GMenuModel* model) {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    /* "menubar" so the theme draws it like the menubar it replaces (xournalpp.css styles it). */
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "menubar");
    gtk_widget_set_name(bar, "x0urnalTouchMenubar");
    if (!model) {
        return bar;
    }
    g_object_set_data_full(G_OBJECT(bar), "x0urnal-menu-model", g_object_ref(model), g_object_unref);
    fill(bar, model);
    g_signal_connect_object(model, "items-changed",
                            G_CALLBACK(+[](GMenuModel* m, gint, gint, gint, gpointer b) { fill(GTK_WIDGET(b), m); }),
                            bar, G_CONNECT_DEFAULT);
    return bar;
}

}  // namespace x0urnal
