typedef struct Scroll {
	/* scroll position */
	int row, ydiff;
} Scroll;

typedef enum {
	WIDGET_NONE,
	WIDGET_INTERNAL,
	WIDGET_CONTEXT,
	WIDGET_CLOSE,
	WIDGET_OPEN,
	WIDGET_PREV,
	WIDGET_NEXT,
	WIDGET_REFRESH,
	WIDGET_PARENT,
	WIDGET_TOGGLE_HIDE,
	WIDGET_DROPASK,
	WIDGET_DROPCOPY,
	WIDGET_DROPMOVE,
	WIDGET_DROPLINK,
	WIDGET_ERROR,
} WidgetEvent;

enum {
	CURSOR_NORMAL,
	CURSOR_WATCH,
	CURSOR_DRAG,
	CURSOR_NODROP,
	CURSOR_LAST,
};

enum {
	/* item elements */
	ITEM_NAME,   /* indexes the label displayed for the item */
	ITEM_PATH,   /* indexes the path given in PRIMARY selection */
	ITEM_STATUS, /* indexes the status displayed on titlebar when item is selected */
	ITEM_LAST,
};

typedef struct Widget *Widget;

/*
 * Create and initialize a widget, and returns a pointer to it.
 *
 * If an error has occurred, a line describing the error is written to
 * stderr and it returns NULL.  If an error occurred, this function must
 * not be called again.
 *
 * The parameters are as follows:
 *
 * - class:
 *   String which the .res_class member of the widget's XClassHint(3)
 *   property is set to.  It should not be null; and should not be freed
 *   or changed during the entire life of the widget.
 *
 * - name:
 *   String which the .res_name member of the widget's XClassHint(3)
 *   property is set to.  It can be null, in which case, the name of
 *   the program (passed through `argv`) is used.  If not null, it
 *   should not be freed or changed during the entire life of the
 *   widget.
 *
 * - geom:
 *   String to be parsed by XParseGeometry(3) specifying the initial
 *   geometry for the widget window.  It can be freed after calling
 *   initwidget().
 *
 * - argc:
 *   Integer passed to XSetCommand(3) specifying the number of arguments
 *   the program was called with.
 *
 * - argv:
 *   Array of strings passed to XSetCommand(3) specifying the arguments
 *   the program was called with.  It can be freed after calling
 *   initwidget().
 */
Widget initwidget(
	const char *class,
	const char *name,
	const char *geom,
	int argc,
	char *argv[]
);

/* Return the id of the widget's window as an unsigned long.
 *
 * - wid:
 *   Widget previously created with initwidget().
 */
unsigned long widgetwinid(Widget wid);

/*
 * Try to open a sequence of .xpm icons for displaying the items on the
 * widget.  This function can only be called once for each widget, after
 * the widget has been created.
 *
 * If an icon from the xpms[] array could not be loaded, it writes a
 * line describing the problem to stderr and returns -1.  The caller
 * should not use the widget again and should close it immediately.
 *
 * If all icons could be loaded successfully, it outputs nothing and
 * returns 0.
 *
 * A loaded icon can be referenced by giving its index to setwidget().
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 *
 * - xpms:
 *   Array of array of strings for the xpm files included at compile time.
 *
 * - nxpms:
 *   Integer for the number of string arrays in xpms[].
 */
int widopenicons(Widget wid, char **xpms[], int nxpms);

/*
 * Set the current state of a widget and its current list of items.
 *
 * It returns 0 if successful; or -1 if an error occurred.  If an error
 * has uccurred, the widget must not be used again, and must be closed.
 *
 * This function can be called at any time for each widget, after it has
 * been created.  Whenever it is called, the list of items displayed on
 * the widget is reset.
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 *
 * - title:
 *   String naming the currently visible set of icons (such as the name
 *   of the current directory, whe using widget.c to display directory
 *   entries).  It should not be null, and should not be freed or
 *   changed until the next call of setwidget().
 *
 * - items:
 *   Array of arrays of strings.  Each element of the outer array
 *   represents the data of an item.  Each inner array contains
 *   ITEM_LAST (3) items, and are indexed by the ITEM_* enums (see
 *   above).  Neither it or its members should not be null, and should
 *   not be freed or changed until the next call of setwidget().
 *
 * - itemicons:
 *   Array of integers, each member is the index of an icon open by
 *   openicons().  If, for an item, the icon index is less than zero, or
 *   if the corresponding icon could not have been open by openicons(),
 *   a default icon (the X logo) is displayed for the item instead.  It
 *   should not be null, and should not be freed until the next call of
 *   setwidget().
 *
 * - nitems:
 *   Size variable setting the number of members of the "items" and
 *   "itemicons" parameter arrays.
 *
 * - scrl
 *   Pointer which, if non-zero, pointers to a scroll state to place the
 *   icon list.
 */
int setwidget(
	Widget wid,
	const char *title,
	char **items[],
	int itemicons[],
	size_t nitems,
	Scroll *scrl
);

/*
 * Realize the widget by mapping its window in the screen.
 *
 * This function must only be called after a widget has been created
 * with initwidget() and set with setwidget(); but can be called at
 * any time thereafter.  Note, however, that it is only necessary to
 * call this function once; because, once the widget has been mapped,
 * it does not need to be mapped again.
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 */
void mapwidget(Widget wid);

/*
 * Process events directed to the widget, such as user interaction.
 * It return a `WidgetEvent` value depending on the result of the event
 * processing:
 *
 * - WIDGET_NONE:
 *   No event occurred.  This value is never returned, and is only used
 *   internally by widget.c.
 *
 * - WIDGET_INTERNAL:
 *   An internal event occurred.  This value is never returned, and is
 *   only used internally by widget.c.
 *
 * - WIDGET_CONTEXT:
 *   The Button 3 was pressed.
 *
 * - WIDGET_CLOSE:
 *   The widget was closed.
 *
 * - WIDGET_OPEN:
 *   An item was open by the user by double-clicking it.  The index of
 *   the open item is returned at *index.
 *
 * - WIDGET_ERROR:
 *   An error occurred while processing the widget.  If this is
 *   returned, pollwidget() should not be called again; and the
 *   widget must be closed.
 *
 * This function can be called at any time, usually in a loop, after a
 * widget was created with initwidget() and set with setwidget().
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 *
 * - selitems:
 *   Pointer to an array of integers indexing the items selected by the
 *   user.  This array must have been allocated by the caller and must
 *   contains at least n elements, where n is the value of the nitems
 *   parameter passed to the previous call to setwidget().  This array
 *   is filled by this function when it returns WIDGET_OPEN or
 *   WIDGET_CONTEXT.
 *
 * - nselitems
 *   Pointer to an integer counting the number of members of the
 *   selitems array filled by this function when it returns WIDGET_OPEN
 *   or WIDGET_CONTEXT.
 *
 * - scrl
 *   Pointer to a scroll state to fill with last scroll information.
 *
 * - sel
 *   Pointer to string. When files are dropped into the window, *sel is
 *   set to a string containing the dropped contents.  The content is a
 *   sequence of "file://PATH\r\n" strings.  The string must be freed by
 *   the caller.
 */
WidgetEvent pollwidget(Widget wid, int *selitems, int *nselitems, Scroll *scrl, char **sel);

/*
 * Set the thumbnail (aka miniature) of a given item.
 *
 * This function must only be called after a widget has been created
 * with initwidget() and set with setwidget(); but can be called at
 * any time thereafter.
 *
 * If the .ppm file could not be open, this function writes a line
 * describing the error on stderr, but the calling function is not
 * notified that the error occurred (as it is not fatal); in such case,
 * the icon originally set for the item when setwidget() was called is
 * used instead.
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 *
 * - path:
 *   String containing the path to a .ppm image file.  The image should
 *   be at most 64x64 in size.
 *
 * - index:
 *   Integer indexing the item whose miniature is to be set.  It must be
 *   greater than or equal to zero, and less than the value of the
 *   nitems parameter passed to the previous call to setwidget().
 */
void setthumbnail(Widget wid, char *path, int index);

/*
 * Set the cursor for the widget.  The only options available are
 * CURSOR_NORMAL (for the regular cursor) and CURSOR_WATCH (for a
 * hourglass cursor, generally used while processing something).
 *
 * This function must only be called after a widget has been created
 * with initwidget() and set with setwidget(); but can be called at
 * any time thereafter.
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 *
 * - cursor:
 *   Either CURSOR_NORMAL or CURSOR_WATCH.
 */
void widgetcursor(Widget wid, int cursor);

/*
 * Close the widget and free all memory used by it.
 *
 * This function must only be called once for each widget.
 *
 * The parameters are as follows:
 *
 * - wid:
 *   Widget previously created with initwidget().
 */
void closewidget(Widget wid);
