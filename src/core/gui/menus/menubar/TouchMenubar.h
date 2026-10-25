/*
 * X0urnal++ -- the menu bar, made of popovers, so a finger or a pen can open it.
 *
 * GTK3 builds a menubar's menus as xdg_popup windows that take a grab, and on Wayland compositors
 * other than mutter a touch-opened GtkMenu is closed again by the very touch that opened it
 * (gtk#5115: the compositor doesn't cancel the touch point when the popup grabs, and GTK then sees
 * the release land outside the popup). A mouse click works, so on a tablet with only touch and a
 * pen the menus cannot be opened at all.
 *
 * A GTK3 GtkPopover is different: it is drawn INSIDE the window, so there is no second surface, no
 * grab and nothing to lose the touch. So this builds the same menu model (ui/mainmenubar.xml plus
 * the submenus X0urnal++ fills in at run time) as a row of GtkMenuButtons, each opening its menu
 * as a popover -- same names, same items, same place -- and the GtkApplicationWindow's own menubar
 * is kept hidden (MainWindow::setMenubarVisible). Every platform gets it: the popovers work with a
 * mouse exactly as before.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <gio/gio.h>
#include <gtk/gtk.h>

namespace x0urnal {

/**
 * A horizontal box of menu buttons, one per top-level item of `model` ("_File", "_Edit", ...),
 * each opening that item's submenu as a popover. Rebuilt by itself if the top level changes.
 * The box holds its own reference to the model.
 */
GtkWidget* buildTouchMenubar(GMenuModel* model);

}  // namespace x0urnal
