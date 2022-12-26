typedef enum {
	WIDGET_CLOSE,
	WIDGET_OPEN,
	WIDGET_PREV,
	WIDGET_NEXT,
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

Widget initwidget(const char *appclass, const char *appname, const char *geom, const char *state[], size_t nstate, int argc, char *argv[], unsigned long *icon, size_t iconsize, int hasthumb);
void initicons(const char *paths[], size_t npaths);
WidgetEvent pollwidget(Widget, int *);
void mapwidget(Widget wid);
void setwidget(Widget wid, const char *doc, char ***items, int *foundicons, size_t nitems);
void mapwidget(Widget wid);
void closewidget(Widget wid);
void openicons(Widget wid, char **paths, int nicons);
void setthumbnail(Widget wid, char *path, int item);
void widgetcursor(Widget wid, int cursor);
