typedef struct Scroll {
	/* scroll position */
	int row, ydiff;
	int highlight;
} Scroll;

typedef struct Item {
	unsigned char mode;     /* entry mode */
	struct timespec mtime;	/* modification time */
	size_t size;		/* file size in bytes */
	char *name;             /* item display name */
	char *fullname;         /* item full name */
	char *status;           /* item statusbar info */
	size_t icon;            /* index for the icon array */
} Item;

typedef enum {
	WIDGET_NONE,
	WIDGET_INTERNAL,
	WIDGET_CONTEXT,
	WIDGET_CLOSE,
	WIDGET_OPEN,
	WIDGET_PREV,
	WIDGET_NEXT,
	WIDGET_GOTO,
	WIDGET_SORTBY,
	WIDGET_KEYPRESS,
	WIDGET_DROPASK,
	WIDGET_DROPCOPY,
	WIDGET_DROPMOVE,
	WIDGET_DROPLINK,
	WIDGET_ERROR,
} WidgetEvent;

typedef struct Widget Widget;

Widget *widget_create(
	const char *class,
	const char *name,
	int argc,
	char *argv[],
	const char *resources[]
);

int widget_set(
	Widget *widget,
	const char *cwd,
	const char *title,
	Item *items,
	size_t nitems,
	Scroll *scrl
);

/* get value of icons resource into allocated string */
char *widget_geticons(Widget *widget);

WidgetEvent widget_wait(Widget *widget);

int widget_fd(Widget *widget);

const char *widget_get_sortby(Widget *widget);

void widget_map(Widget *widget);

WidgetEvent widget_poll(Widget *widget, int *selitems, int *nselitems, Scroll *scrl, char **sel);

void widget_thumb(Widget *widget, char *path, int index);

void widget_free(Widget *widget);

void widget_busy(Widget *widget);
