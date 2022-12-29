typedef enum {
	WIDGET_CONTINUE,
	WIDGET_CLOSE,
	WIDGET_OPEN,
	WIDGET_ERROR,
} WidgetEvent;

enum {
	CURSOR_NORMAL,
	CURSOR_WATCH,
	CURSOR_LAST,
};

enum {
	/* item elements */
	ITEM_NAME,
	ITEM_PATH,
	ITEM_STATUS,
	ITEM_LAST,
};

typedef struct Widget *Widget;;

/*
 * Create and initialize a widget.  A widget is an icon container used
 * to display a scrollable grid of icons.
 *
 * The `class` and `name` parameters are the values which .res_class and
 * .res_name members of the widget's XClassHint(3) property are set to.
 *
 * All pointer parameters can be freed after calling initwidget(),
 * because this function copies them if necessary into the widget
 * itself.
 *
 * The `geom` parameter is a string specifying the initial geometry for
 * the widget window; the string is parsed with XParseGeometry(3).
 *
 * The `argc` and `argv` argument are used to set the `WM_COMMAND`
 * property of the widget's window.  initwidget() does not change
 * argv, nor the strings it points to.
 *
 * This function can only be called once for each widget.
 */
Widget initwidget(const char *class, const char *name, const char *geom, int argc, char *argv[]);

/*
 * Opens .xpm icons to be used for displaying the items on the icon
 * grid.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * The `path` parameter is an array of `nicons` strings.  Each one
 * containing the path to a icon.
 *
 * When called, this function tries to open each icon using libXpm
 * functions.
 *
 * This function can only be called once for each widget, after they
 * have been created.
 */
void openicons(Widget wid, char *paths[], int nicons);

/*
 * Set the current state and icons of the widget `wid`.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * The `title` parameter is the name of the currently visible set of
 * icons (such as the name of the current directory, when using widget.c
 * to display directory entries).
 *
 * The `items` parameter is an array of arrays of strings.  The outer
 * array, `items` must have `nitems` arrays, each one representing the
 * data of an item.  For each item `i` (0<=i<nitems), `items[i]` is an
 * array of, at least three strings:
 * - .items[i][ITEM_NAME]   -- The label displayed for the item.
 * - .items[i][ITEM_PATH]   -- The path given in PRIMARY selection.
 * - .items[i][ITEM_STATUS] -- The status string displayed on the
 *                             titlebar when the item is selected.
 *
 * The `itemicons` parameter is an array of `nitems` integers, each
 * member is the index of an icon open by `openicons()`.  If, for an
 * item, the icon index is less than zero, or if the corresponding icon
 * could not be open by `openicons()`, a default icon (the X logo) is
 * displayed for the item instead.
 *
 * All the parameters `title`, `items` and `itemicons` and the memory
 * pointed to by them, either directly or indirectly, should exist
 * and be acessible and not be deallocated until the next call of
 * setwidget(), after which they can be freed.
 *
 * This function can be called at any time for each item, after it has
 * been created.  Whenever it is called, the list of items is reset.
 */
void setwidget(Widget wid, const char *title, char **items[], int itemicons[], size_t nitems);

/*
 * Realize the widget by mapping its window in the screen.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * This function can be called at any time after a widget was created
 * with `initwidget()` and set with `setwidget()`.  Note, however that
 * it is only necessary to call this function once; because, once the
 * widget is mapped, it does not need to be mapped again.
 */
void mapwidget(Widget wid);

/*
 * Reads events for the widget, such as user interaction, and process
 * them.  It return a `WidgetEvent` value depending on the result of the
 * event processing:
 *
 * - WIDGET_CONTINUE: This value is never returned, and is only used
 *   internally by widget.c
 *
 * - WIDGET_CLOSE: The widget was closed.
 *
 * - WIDGET_OPEN: An item was open by the user by double-clicking it.
 *   The index of the open item is returned at *index.
 *
 * - WIDGET_ERROR: An error occurred after processing the widget.  If
 *   this is returned, pollwidget() should not be called again; and the
 *   widget must be closed.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * The `index` parameter is a pointer to a index returned when a item is
 * open.
 *
 * This function can be called at any time after a widget was created
 * with `initwidget()` and set with `setwidget()`.
 */
WidgetEvent pollwidget(Widget, int *index);

/*
 * Set the thumbnail (aka miniature) of a given item.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * The `path` parameter is the path to a .ppm image file.
 * The image should be at most 64x64 in size.
 *
 * The `index` parameter is the index of a item, as set by
 * `setwidget()`.
 *
 * This function can be called at any time after a widget was created
 * with `initwidget()` and set with `setwidget()`.
 */
void setthumbnail(Widget wid, char *path, int index);

/*
 * Set the cursor for a widget.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * The `cursor` parameter is either `CURSOR_NORMAL` (for a normal
 * cursor), or `CURSOR_WATCH` (for a hourglass cursor, used while
 * processing something).
 *
 * This function can be called at any time after a widget was created
 * with `initwidget()` and set with `setwidget()`.
 */
void widgetcursor(Widget wid, int cursor);

/*
 * Close the widget and free all memory used by it.
 *
 * The `wid` parameter is a widget created with `initwidget()`.
 *
 * This function must only be called once for each widget.
 */
void closewidget(Widget wid);
