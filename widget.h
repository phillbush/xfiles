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
	WIDGET_GOTO,
	WIDGET_KEYPRESS,
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

typedef struct Widget Widget;

Widget *widget_create(
	const char *class,
	const char *name,
	int argc,
	char *argv[],
	const char *resources[]
);

int widget_openicons(Widget *widget, char **xpms[], int nxpms);

int widget_set(
	Widget *widget,
	const char *title,
	char **items[],
	int itemicons[],
	size_t nitems,
	Scroll *scrl
);

void widget_map(Widget *widget);

WidgetEvent widget_poll(Widget *widget, int *selitems, int *nselitems, Scroll *scrl, char **sel);

void widget_thumb(Widget *widget, char *path, int index);

void widget_free(Widget *widget);

void widget_busy(Widget *widget);
