#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#include "ctrlsel.h"
#include "util.h"
#include "widget.h"
#include "winicon.data"         /* window icon, for the window manager */

#define NCLIENTMSG_DATA 5               /* number of members on a the .data.l[] array of a XClientMessageEvent */

/* free and set to null */
#define FREE(x)         do{free(x); x = NULL;}while(0)

/* default theme configuration */
#define DEF_FONT        "monospace:size=9"
#define DEF_BG          "#0A0A0A"
#define DEF_FG          "#FFFFFF"
#define DEF_SELBG       "#121212"
#define DEF_SELFG       "#707880"
#define DEF_OPACITY     1.0

/* ellipsis has two dots rather than three; the third comes from the extension */
#define ELLIPSIS        ".."

/* what to display when item's status is unknown */
#define STATUS_UNKNOWN  "?"

/* opacity of drag-and-drop mini window */
#define DND_OPACITY     0x7FFFFFFF

/* constants to check a .ppm file */
#define PPM_HEADER      "P6\n"
#define PPM_COLOR       "255\n"

#define SCREEN(d)       (DefaultScreen((d)))
#define WIDTH(d)        (DisplayWidth((d), DefaultScreen((d))))
#define HEIGHT(d)       (DisplayHeight((d), DefaultScreen((d))))
#define ROOT(d)         (DefaultRootWindow((d)))
#define FLAG(f, b)      (((f) & (b)) == (b))
#define ATOI(c)         (((c) >= '0' && (c) <= '9') ? (c) - '0' : -1)

/* how much to scroll on PageUp/PageDown (half the window height) */
#define PAGE_STEP(w)    ((w)->h / 2)

/* window capacity (max number of rows that can fit on the window) */
#define WIN_ROWS(w)     ((w)->h / (w)->itemh)

/* number of WHOLE rows that the current directory has */
#define WHL_ROWS(w)     ((w)->nitems / (w)->ncols)

/* 0 if all rows are entirely filled; 1 if there's an extra, half-filled row */
#define MOD_ROWS(w)     (((w)->nitems % (w)->ncols) != 0 ? 1 : 0)

/* actual number of rows (either whole or not) that the current directory has */
#define ALL_ROWS(w)     (WHL_ROWS(w) + MOD_ROWS(w))

enum {
	/* distance the cursor must move to be considered a drag */
	DRAG_THRESHOLD  = 8,
	DRAG_SQUARE     = (DRAG_THRESHOLD * DRAG_THRESHOLD),

	/* buttons not defined by X.h */
	BUTTON8         = 8,
	BUTTON9         = 9,

	/* one byte was 8 bits in size last time I checked */
	BYTE            = 8,

	/* color depths */
	CLIP_DEPTH      = 1,                    /* 0/1 */
	PPM_DEPTH       = 3,                    /* RGB */
	DATA_DEPTH      = 4,                    /* BGRA */

	/* sizes of string buffers */
	TITLE_BUFSIZE   = 1028,                 /* title buffer size */
	STATUS_BUFSIZE  = 64,                   /* status buffer size */
	RES_BUFSIZE     = 512,                  /* resource buffer size */
	KSYM_BUFSIZE    = 64,                   /* key symbol name buffer size */

	/* hardcoded object sizes in pixels */
	/* there's no ITEM_HEIGHT for it is computed at runtime from font height */
	THUMBSIZE       = 64,                   /* maximum thumbnail size */
	ITEM_WIDTH      = (THUMBSIZE * 2),      /* width of an item (icon + margins) */
	MIN_WIDTH       = ITEM_WIDTH,           /* minimum window width */
	MIN_HEIGHT      = (THUMBSIZE * 3),      /* minimum window height */
	DEF_WIDTH       = 600,                  /* default window width */
	DEF_HEIGHT      = 460,                  /* default window height */
	STIPPLE_SIZE    = 2,                    /* size of stipple pattern */
	MARGIN          = 16,                   /* top margin above first row */

	/* constants for parsing .ppm files */
	PPM_HEADER_SIZE = (sizeof(PPM_HEADER) - 1),
	PPM_COLOR_SIZE  = (sizeof(PPM_COLOR) - 1),
	PPM_BUFSIZE     = 8,

	/* draw up to NLINES lines of label; each one up to LABELWIDTH pixels long */
	NLINES          = 2,                    /* change it for more lines below icons */
	LABELWIDTH      = ITEM_WIDTH - 16,      /* save 8 pixels each side around label */

	/* times in milliseconds */
	DOUBLECLICK     = 250,                  /* time of a doubleclick, in milliseconds */
	MOTION_TIME     = 32,                   /* update time rate for rectangular selection */

	/* scrolling */
	SCROLL_STEP     = 32,                   /* pixels per scroll */
	SCROLLER_SIZE   = 32,                   /* size of the scroller */
	SCROLLER_MIN    = 16,                   /* min lines to scroll for the scroller to change */
	HANDLE_MAX_SIZE = (SCROLLER_SIZE - 2),  /* max size of the scroller handle */
};

enum {
	SELECT_NOT,
	SELECT_YES,
	SELECT_LAST,
};

enum {
	TARGET_STRING,
	TARGET_UTF8,
	TARGET_URI,
	TARGET_LAST
};

enum {
	COLOR_BG,
	COLOR_FG,
	COLOR_LAST,
};

enum {
	/* selection targets */
	TEXT_URI_LIST,
	UTF8_STRING,
	COMPOUND_TEXT,
	DELETE,
	INCR,
	MULTIPLE,
	TARGETS,
	TEXT,
	TIMESTAMP,

	/* selection properties */
	ATOM_PAIR,

	/* window protocols */
	WM_PROTOCOLS,
	WM_DELETE_WINDOW,

	/* window properties */
	_NET_WM_ICON,
	_NET_WM_NAME,
	_NET_WM_PID,
	_NET_WM_WINDOW_TYPE,
	_NET_WM_WINDOW_TYPE_DND,
	_NET_WM_WINDOW_TYPE_NORMAL,
	_NET_WM_WINDOW_OPACITY,

	/* widget control properties */
	_CONTROL_CWD,
	_CONTROL_GOTO,

	/* others */
	_NULL,

	ATOM_LAST,
};

struct Icon {
	Pixmap pix, mask;
};

struct Thumb {
	struct Thumb *next;
	int w, h;
	XImage *img;
};

struct Selection {
	struct Selection *prev, *next;
	int index;
};

struct Widget {
	char *progname;
	int start;
	int redraw;

	/* X11 stuff */
	Display *display;
	Atom atoms[ATOM_LAST];
	GC stipgc, gc;
	Cursor busycursor;
	Window window;
	XftColor colors[SELECT_LAST][COLOR_LAST];
	XftFont *font;
	Visual *visual;
	Colormap colormap;
	unsigned int depth;

	Atom lastprop;
	char *lasttext;

	Pixmap pix;                     /* the pixmap of the icon grid */
	Pixmap namepix;                 /* temporary pixmap for the labels */
	Pixmap stipple;                 /* stipple for painting icons of selected items */
	Pixmap rectbord;                /* rectangular selection border */

	XftDraw *draw;                  /* used for drawing with alpha channel */

	/*
	 * Size of the rectbord pixmap.
	 */
	int rectw, recth;

	/*
	 * Lock used for synchronizing the thumbnail and the main threads.
	 */
	pthread_mutex_t lock;

	/*
	 * .items is an array of arrays of strings.
	 * For each index i, .items[i] contains (at least) three elements:
	 * - .items[i][ITEM_NAME]   -- The label displayed for the item.
	 * - .items[i][ITEM_PATH]   -- The path given in PRIMARY selection.
	 * - .items[i][ITEM_STATUS] -- The status string displayed on the
	 *                             titlebar when the item is selected.
	 */
	char ***items;                  /* see comment above */
	char *selbuf, *uribuf, *dndbuf; /* buffer for selections */
	size_t selbufsiz, uribufsiz;
	int nitems;                     /* number of items */
	int *linelen;                   /* for each item, the lengths of its largest label line */
	int *nlines;                    /* for each item, the number of label lines */

	/*
	 * Items can be selected with the mouse and the Control and Shift modifiers.
	 *
	 * We keep track of selections in a list of selections, which is
	 * essentially a doubly-linked list of indices.  It's kept as a
	 * doubly-linked list for easily adding and removing any
	 * element.
	 *
	 * We also maintain an array of pointers to selections, so we
	 * can easily access a selection in the list, and remove it for
	 * example, given the index of an item.
	 */
	struct CtrlSelTarget targets[TARGET_LAST];
	struct CtrlSelTarget dragtarget;
	struct CtrlSelTarget droptarget;
	struct CtrlSelContext *selctx, *dragctx;
	struct CtrlSelContext dropctx;
	struct Selection *sel;          /* list of selections */
	struct Selection *rectsel;      /* list of selections by rectsel */
	struct Selection **issel;       /* array of pointers to Selections */
	Time seltime;

	/*
	 * We keep track of thumbnails in a list of thumbnails, which is
	 * essentially a singly-linked list of XImages.  It's kept as a
	 * singly-linked list just so we can traverse them one-by-one at
	 * the end for freeing them.
	 *
	 * We also maintain an array of pointers to thumbnails, so we
	 * can easily access a thumbnail in the list, given the index
	 * of an item.
	 */
	struct Thumb *thumbhead;
	struct Thumb **thumbs;

	/*
	 * Geometry of the window and its contents.
	 *
	 * WARNING:
	 * - .nrows is the number of rows in the pixmap, which is
	 *   approximately the number of rows visible in the window
	 *   (that is, those that are not hidden by scrolling) plus 2.
	 * - .ncols is also the number of rows in the pixmap, which is
	 *   exactly the number of columns visible in the window.
	 *
	 * I call "screenful" what is being visible at a given time on
	 * the window.
	 */
	int w, h;                       /* window size */
	int pixw, pixh;                 /* pixmap size */
	int itemw, itemh;               /* size of a item (margin + icon + label) */
	int ydiff;                      /* how much the pixmap is scrolled up */
	int ncols, nrows;               /* number of columns and rows visible at a time */
	int nscreens;                   /* maximum number of screenfuls we can scroll */
	int row;                        /* index of first row visible in the current screenful */
	int fonth;                      /* font height */
	int x0;                         /* position of first column after the left margin */
	int ellipsisw;                  /* width of the ellipsis we draw on long labels */

	/*
	 * We use icons for items that do not have a thumbnail.
	 */
	struct Icon *icons;             /* array of icons set by the user */
	int nicons;                     /* number of icons set by the user */

	/*
	 * This array, with .nitem members, defines the index in the
	 * .icons array for the icon of each item.
	 */
	int *itemicons;

	/* Strings used to build the title bar. */
	const char *title;
	const char *class;

	/*
	 * Index of highlighted item (usually the last item clicked by
	 * the user); or -1 if none.
	 */
	int highlight;

	/*
	 * The scroller how this code calls the widget that replaces the
	 * scrollbar.  It is a little pop-up window that appears after a
	 * middle-click.  It can be either controlled as a scrollbar, by
	 * dragging its inner manipulable object (called the "handler"),
	 * or by moving the pointer up and down the scroller itself.
	 *
	 * The scroller is based on a Firefox's hidden feature called
	 * autoScroll.
	 *
	 * TIP: To enable this feature in Firefox, set the option
	 * "general.autoScroll" to True in about:config.
	 */
	Window scroller;                /* the scroller popup window */
	int handlew;                    /* size of scroller handle */

	/*
	 * We can be either in the main loop (see pollwidget), in the
	 * rectangular selection sub-loop (see rectmotion), or in the
	 * scroller widget sub-loop (see scrollmotion).  Some routines
	 * depend on the loop we're in, so we mark its current state
	 * with this enum.
	 *
	 * NOTE: whenever implementing a new sub-loop, always set its
	 * state at beginning and reset it to STATE_NORMAL at the end
	 * of the function.
	 */
	enum {
		STATE_NORMAL,
		STATE_SELECTING,
		STATE_SCROLLING,
		STATE_DRAGGING,
	} state;
};

static char *atomnames[ATOM_LAST] = {
	[TEXT_URI_LIST]              = "text/uri-list",
	[ATOM_PAIR]                  = "ATOM_PAIR",
	[COMPOUND_TEXT]              = "COMPOUND_TEXT",
	[UTF8_STRING]                = "UTF8_STRING",
	[WM_PROTOCOLS]               = "WM_PROTOCOLS",
	[WM_DELETE_WINDOW]           = "WM_DELETE_WINDOW",
	[DELETE]                     = "DELETE",
	[INCR]                       = "INCR",
	[MULTIPLE]                   = "MULTIPLE",
	[TARGETS]                    = "TARGETS",
	[TEXT]                       = "TEXT",
	[TIMESTAMP]                  = "TIMESTAMP",
	[_NET_WM_ICON]               = "_NET_WM_ICON",
	[_NET_WM_NAME]               = "_NET_WM_NAME",
	[_NET_WM_PID]                = "_NET_WM_PID",
	[_NET_WM_WINDOW_TYPE]        = "_NET_WM_WINDOW_TYPE",
	[_NET_WM_WINDOW_TYPE_DND]    = "_NET_WM_WINDOW_TYPE_DND",
	[_NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	[_NET_WM_WINDOW_OPACITY]     = "_NET_WM_WINDOW_OPACITY",
	[_CONTROL_CWD]               = "_CONTROL_CWD",
	[_CONTROL_GOTO]              = "_CONTROL_GOTO",
	[_NULL]                      = "NULL",
};

static int
createwin(Widget *widget, const char *class, const char *name, const char *geom, int argc, char *argv[], unsigned long *icon, size_t iconsize)
{
	XftDraw *draw;
	Pixmap bg;
	unsigned int dw, dh;
	int x, y;
	int dx, dy;
	int flags, sizehints;
	pid_t pid;

	x = y = 0;
	widget->w = DEF_WIDTH;
	widget->h = DEF_HEIGHT;
	sizehints = 0;
	pid = getpid();
	if (geom != NULL) {
		flags = XParseGeometry(geom, &dx, &dy, &dw, &dh);
		dw = max(MIN_WIDTH, dw);
		dh = max(MIN_HEIGHT, dh);
		if (FLAG(flags, WidthValue)) {
			widget->w = dw;
			sizehints |= USSize;
		}
		if (FLAG(flags, HeightValue)) {
			widget->h = dh;
			sizehints |= USSize;
		}
		if (FLAG(flags, XValue | XNegative)) {
			x = WIDTH(widget->display) - widget->w - (dx > 0 ? dx : 0);
			sizehints |= USPosition;
		} else if (FLAG(flags, XValue)) {
			x = dx;
			sizehints |= USPosition;
		}
		if (FLAG(flags, YValue | YNegative)) {
			y = HEIGHT(widget->display) - widget->h - (dy > 0 ? dy : 0);
			sizehints |= USPosition;
		} else if (FLAG(flags, XValue)) {
			y = dy;
			sizehints |= USPosition;
		}
	}
	widget->window = XCreateWindow(
		widget->display, ROOT(widget->display),
		x, y, widget->w, widget->h, 0,
		widget->depth, InputOutput, widget->visual,
		CWBackPixel | CWEventMask | CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.colormap = widget->colormap,
			.background_pixel = 0,
			.event_mask = StructureNotifyMask | ExposureMask
			            | KeyPressMask | PointerMotionMask
			            | ButtonReleaseMask | ButtonPressMask
			            | PropertyChangeMask,
		}
	);
	if (widget->window == None)
		return RETURN_FAILURE;
	bg = XCreatePixmap(
		widget->display,
		widget->window,
		THUMBSIZE,
		THUMBSIZE,
		widget->depth
	);
	if (bg == None)
		return RETURN_FAILURE;
	draw = XftDrawCreate(widget->display, bg, widget->visual, widget->colormap);
	XftDrawRect(draw, &widget->colors[SELECT_NOT][COLOR_BG], 0, 0, THUMBSIZE, THUMBSIZE);
	XftDrawDestroy(draw);
	XSetWindowBackgroundPixmap(widget->display, widget->window, bg);
	XFreePixmap(widget->display, bg);
	widget->namepix = XCreatePixmap(
		widget->display,
		widget->window,
		LABELWIDTH,
		widget->fonth,
		widget->depth
	);
	if (widget->namepix == None)
		return RETURN_FAILURE;
	XmbSetWMProperties(
		widget->display, widget->window,
		class, class,
		argv, argc,
		&(XSizeHints){ .flags = sizehints, },
		NULL,
		&(XClassHint){ .res_class = (char *)class, .res_name = (char *)name, }
	);
	XSetWMProtocols(widget->display, widget->window, &widget->atoms[WM_DELETE_WINDOW], 1);
	XChangeProperty(
		widget->display, widget->window,
		widget->atoms[_NET_WM_NAME],
		widget->atoms[UTF8_STRING], 8, PropModeReplace,
		(unsigned char *)class,
		strlen(class) + 1
	);
	XChangeProperty(
		widget->display, widget->window,
		widget->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *)&widget->atoms[_NET_WM_WINDOW_TYPE_NORMAL],
		1
	);
	XChangeProperty(
		widget->display, widget->window,
		widget->atoms[_NET_WM_ICON],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)icon, iconsize
	);
	XChangeProperty(
		widget->display, widget->window,
		widget->atoms[_NET_WM_PID],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&pid,
		1
	);
	return RETURN_SUCCESS;
}

static int
ealloccolor(Widget *widget, const char *s, XftColor *color, unsigned short alpha)
{
	XColor screen, exact;

	if (!XAllocNamedColor(widget->display, widget->colormap, s, &screen, &exact))
		return RETURN_FAILURE;
	color->pixel = screen.pixel;
	color->color.red = exact.red;
	color->color.green = exact.green;
	color->color.blue = exact.blue;
	color->color.alpha = alpha;
	return RETURN_SUCCESS;
}

static int
eallocfont(Display *display, const char *s, XftFont **font)
{
	if ((*font = XftFontOpenXlfd(display, DefaultScreen(display), s)) == NULL)
		if ((*font = XftFontOpenName(display, DefaultScreen(display), s)) == NULL)
			return RETURN_FAILURE;
	return RETURN_SUCCESS;
}

static char *
getresource(XrmDatabase xdb, const char *class, const char *name, const char *resource)
{
	XrmValue xval;
	char *type;
	char classbuf[RES_BUFSIZE], namebuf[RES_BUFSIZE];

	if (xdb == NULL)
		return NULL;
	(void)snprintf(classbuf, RES_BUFSIZE, "%s.%s", class, resource);
	(void)snprintf(namebuf, RES_BUFSIZE, "%s.%s", name, resource);
	if (XrmGetResource(xdb, namebuf, classbuf, &type, &xval) == True)
		return xval.addr;
	return NULL;
}

static int
textwidth(Widget *widget, const char *text, int len)
{
	XGlyphInfo box;

	XftTextExtentsUtf8(widget->display, widget->font, (const FcChar8 *)text, len, &box);
	return box.width;
}

static int
inittheme(Widget *widget, const char *class, const char *name)
{
	XrmDatabase xdb;
	int i, j, goterror;;
	char *xrm, *s, *endp;
	char *resources[SELECT_LAST][COLOR_LAST] = {
		[SELECT_NOT][COLOR_BG] = "background",
		[SELECT_NOT][COLOR_FG] = "foreground",
		[SELECT_YES][COLOR_BG] = "selbackground",
		[SELECT_YES][COLOR_FG] = "selforeground",
	};
	char *defvalue[SELECT_LAST][COLOR_LAST] = {
		[SELECT_NOT][COLOR_BG] = DEF_BG,
		[SELECT_NOT][COLOR_FG] = DEF_FG,
		[SELECT_YES][COLOR_BG] = DEF_SELBG,
		[SELECT_YES][COLOR_FG] = DEF_SELFG,
	};
	int colorerror[SELECT_LAST][COLOR_LAST] = { { FALSE, FALSE }, { FALSE, FALSE } };
	int fonterror = FALSE;
	unsigned short alpha;
	double opacity;

	xdb = NULL;
	goterror = FALSE;
	if ((xrm = XResourceManagerString(widget->display)) != NULL)
		xdb = XrmGetStringDatabase(xrm);
	alpha = DEF_OPACITY * 0xFFFF;
	if ((s = getresource(xdb, class, name, "opacity")) != NULL) {
		opacity = strtod(s, &endp);
		if (endp == s || *endp != '\0' || opacity < 0.0 || opacity > 1.0)
			opacity = DEF_OPACITY;
		alpha = opacity * 0xFFFF;
	}
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			s = getresource(xdb, class, name, resources[i][j]);
			if (s == NULL) {
				/* could not found resource; use default value */
				s = defvalue[i][j];
			} else if (ealloccolor(widget, s, &widget->colors[i][j], alpha) == RETURN_FAILURE) {
				/* resource found, but allocation failed; use default value */
				warnx("\"%s\": could not load color (falling back to \"%s\")", s, defvalue[i][j]);
				s = defvalue[i][j];
			} else {
				/* resource found and successfully allocated */
				continue;
			}
			if (ealloccolor(widget, s, &widget->colors[i][j], alpha) == RETURN_FAILURE) {
				warnx("\"%s\": could not load color", s);
				colorerror[i][j] = TRUE;
				goterror = TRUE;
			}
			alpha = 0xFFFF;
		}
	}
	s = getresource(xdb, class, name, "faceName");
	if (s == NULL) {
		/* could not found resource; use default value */
		s = DEF_FONT;
	} else if (eallocfont(widget->display, s, &widget->font) == RETURN_FAILURE) {
		/* resource found, but allocation failed; use default value */
		warnx("\"%s\": could not open font (falling back to \"%s\")", s, DEF_FONT);
		s = DEF_FONT;
	} else {
		goto done;
	}
	if (eallocfont(widget->display, s, &widget->font) == RETURN_FAILURE) {
		warnx("\"%s\": could not open font", s);
		fonterror = TRUE;
		goterror = TRUE;
	}
done:
	if (goterror)
		goto error;
	widget->fonth = widget->font->height;
	widget->itemw = ITEM_WIDTH;
	widget->itemh = THUMBSIZE + (NLINES + 1) * widget->fonth;
	widget->ellipsisw = textwidth(widget, ELLIPSIS, strlen(ELLIPSIS));
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RETURN_SUCCESS;
error:
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			if (colorerror[i][j])
				continue;
			XftColorFree(widget->display, widget->visual, widget->colormap, &widget->colors[i][j]);
		}
	}
	if (!fonterror)
		XftFontClose(widget->display, widget->font);
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RETURN_FAILURE;
}

static int
calcsize(Widget *widget, int w, int h)
{
	int ncols, nrows, ret;
	double d;

	ret = FALSE;
	if (widget->w == w && widget->h == h)
		return FALSE;
	etlock(&widget->lock);
	ncols = widget->ncols;
	nrows = widget->nrows;
	if (w > 0 && h > 0) {
		widget->w = w;
		widget->h = h;
		widget->ydiff = 0;
	}
	widget->ncols = max(widget->w / widget->itemw, 1);
	widget->nrows = max(WIN_ROWS(widget) + (widget->h % widget->itemh ? 2 : 1), 1);
	widget->x0 = max((widget->w - widget->ncols * widget->itemw) / 2, 0);
	widget->nscreens = ALL_ROWS(widget) - WIN_ROWS(widget);
	widget->nscreens = max(widget->nscreens, 1);
	d = (double)widget->nscreens / SCROLLER_MIN;
	d = (d < 1.0 ? 1.0 : d);
	widget->handlew = max(SCROLLER_SIZE / d - 2, 1);
	widget->handlew = min(widget->handlew, HANDLE_MAX_SIZE);
	if (widget->handlew == HANDLE_MAX_SIZE && ALL_ROWS(widget) > WIN_ROWS(widget))
		widget->handlew = HANDLE_MAX_SIZE - 1;
	if (ncols != widget->ncols || nrows != widget->nrows) {
		if (widget->pix != None)
			XFreePixmap(widget->display, widget->pix);
		if (widget->draw != None)
			XftDrawDestroy(widget->draw);
		widget->pixw = widget->ncols * widget->itemw;
		widget->pixh = widget->nrows * widget->itemh;
		widget->pix = XCreatePixmap(widget->display, widget->window, widget->pixw, widget->pixh, widget->depth);
		widget->draw = XftDrawCreate(widget->display, widget->pix, widget->visual, widget->colormap);
		ret = TRUE;
	}
	if (widget->w > widget->rectw || widget->h > widget->recth) {
		widget->rectw = max(widget->rectw, widget->w);
		widget->recth = max(widget->recth, widget->h);
		if (widget->rectbord != None)
			XFreePixmap(widget->display, widget->rectbord);
		widget->rectbord = XCreatePixmap(widget->display, widget->window, widget->w, widget->h, CLIP_DEPTH);
	}
	etunlock(&widget->lock);
	return ret;
}

static int
isbreakable(char c)
{
	return c == '.' || c == '-' || c == '_';
}

static void
drawtext(Widget *widget, Drawable pix, XftColor *color, int x, const char *text, int len)
{
	XftDraw *draw;
	int w;

	w = textwidth(widget, text, len);
	draw = XftDrawCreate(widget->display, pix, widget->visual, widget->colormap);
	XftDrawRect(draw, &widget->colors[SELECT_NOT][COLOR_BG], 0, 0, LABELWIDTH, widget->fonth);
	XftDrawRect(draw, &color[COLOR_BG], x, 0, w, widget->fonth);
	XftDrawStringUtf8(draw, &color[COLOR_FG], widget->font, x, widget->font->ascent, (const FcChar8 *)text, len);
	XftDrawDestroy(draw);
}

static void
setrow(Widget *widget, int row)
{
	etlock(&widget->lock);
	widget->row = row;
	etunlock(&widget->lock);
}

static void
drawicon(Widget *widget, int index, int x, int y)
{
	XGCValues val;
	Pixmap pix, mask;
	int icon;

	icon = widget->itemicons[index];
	pix = widget->icons[icon].pix;
	mask = widget->icons[icon].mask;
	if (widget->thumbs != NULL && widget->thumbs[index] != NULL) {
		/* draw thumbnail */
		XPutImage(
			widget->display,
			widget->pix,
			widget->gc,
			widget->thumbs[index]->img,
			0, 0,
			x + (widget->itemw - widget->thumbs[index]->w) / 2,
			y + (THUMBSIZE - widget->thumbs[index]->h) / 2,
			widget->thumbs[index]->w,
			widget->thumbs[index]->h
		);
		if (widget->issel[index]) {
			XSetFillStyle(widget->display, widget->gc, FillStippled);
			XSetForeground(widget->display, widget->gc, widget->colors[SELECT_YES][COLOR_BG].pixel);
			XFillRectangle(
				widget->display,
				widget->pix,
				widget->gc,
				x + (widget->itemw - widget->thumbs[index]->w) / 2,
				y + (THUMBSIZE - widget->thumbs[index]->h) / 2,
				widget->thumbs[index]->w,
				widget->thumbs[index]->h
			);
			XSetFillStyle(widget->display, widget->gc, FillSolid);
		}
	} else {
		/* draw icon */
		val.clip_x_origin = x + (widget->itemw - THUMBSIZE) / 2;
		val.clip_y_origin = y;
		val.clip_mask = mask;
		XChangeGC(widget->display, widget->gc, GCClipXOrigin | GCClipYOrigin | GCClipMask, &val);
		XCopyArea(
			widget->display,
			pix, widget->pix,
			widget->gc,
			0, 0,
			THUMBSIZE, THUMBSIZE,
			val.clip_x_origin,
			y
		);
		if (widget->issel[index]) {
			XSetFillStyle(widget->display, widget->gc, FillStippled);
			XSetForeground(widget->display, widget->gc, widget->colors[SELECT_YES][COLOR_BG].pixel);
			XFillRectangle(
				widget->display,
				widget->pix,
				widget->gc,
				val.clip_x_origin, y,
				THUMBSIZE, THUMBSIZE
			);
			XSetFillStyle(widget->display, widget->gc, FillSolid);
		}
		val.clip_mask = None;
		XChangeGC(widget->display, widget->gc, GCClipMask, &val);
	}
}

static void
drawlabel(Widget *widget, int index, int x, int y)
{
	XftColor *color;
	int i;
	int textx, maxw;
	int textw, w, textlen, len;
	int extensionw, extensionlen;
	char *text, *extension;

	color = widget->colors[(widget->issel != NULL && widget->issel[index]) ? SELECT_YES : SELECT_NOT];
	text = widget->items[index][ITEM_NAME];
	widget->nlines[index] = 1;
	textx = x + widget->itemw / 2 - LABELWIDTH / 2;
	extension = NULL;
	maxw = 0;
	textlen = 0;
	widget->linelen[index] = 0;
	for (i = 0; i < widget->nlines[index]; i++) {
		while (isspace(text[textlen]))
			textlen++;
		text += textlen;
		textlen = strlen(text);
		textw = textwidth(widget, text, textlen);
		if (widget->nlines[index] < NLINES && textw >= LABELWIDTH) {
			textlen = len = 0;
			w = 0;
			while (w < LABELWIDTH) {
				textlen = len;
				textw = w;
				while (isspace(text[len]))
					len++;
				while (isbreakable(text[len]))
					len++;
				while (text[len] != '\0' && !isspace(text[len]) && !isbreakable(text[len]))
					len++;
				w = textwidth(widget, text, len);
				if (text[len] == '\0') {
					break;
				}
			}
			if (textw > 0) {
				widget->nlines[index] = min(widget->nlines[index] + 1, NLINES);
			} else {
				textlen = len;
				textw = w;
			}
		}
		textw = min(LABELWIDTH, textw);
		maxw = max(textw, maxw);
		drawtext(
			widget,
			widget->namepix, color,
			max(LABELWIDTH / 2 - textw / 2, 0),
			text, textlen
		);
		textw = min(textw, LABELWIDTH);
		widget->linelen[index] = max(widget->linelen[index], textw);
		XCopyArea(
			widget->display,
			widget->namepix, widget->pix,
			widget->gc,
			0, 0,
			LABELWIDTH, widget->fonth,
			textx, y + widget->itemh - (NLINES - i + 0.5) * widget->fonth
		);
	}
	if (index == widget->highlight) {
		XSetForeground(widget->display, widget->gc, color[COLOR_FG].pixel);
		XDrawRectangle(
			widget->display,
			widget->pix,
			widget->gc,
			x + widget->itemw / 2 - maxw / 2 - 1,
			y + widget->itemh - (NLINES + 0.5) * widget->fonth - 1,
			maxw + 1, i * widget->fonth + 1
		);
	}
	if (textw >= LABELWIDTH &&
	    (extension = strrchr(text, '.')) != NULL &&
	    extension[1] != '\0') {
		extensionlen = strlen(extension);
		extensionw = textwidth(widget, extension, extensionlen);
	}
	if (extension != NULL) {
		/* draw ellipsis */
		drawtext(
			widget,
			widget->namepix, color,
			0,
			ELLIPSIS, strlen(ELLIPSIS)
		);
		XCopyArea(
			widget->display,
			widget->namepix, widget->pix,
			widget->gc,
			0, 0,
			widget->ellipsisw, widget->fonth,
			textx + textw - extensionw - widget->ellipsisw,
			y + widget->itemh - (NLINES + 1 - widget->nlines[index] + 0.5) * widget->fonth
		);

		/* draw extension */
		drawtext(
			widget,
			widget->namepix, color,
			0, extension, extensionlen
		);
		XCopyArea(
			widget->display,
			widget->namepix, widget->pix,
			widget->gc,
			0, 0,
			extensionw, widget->fonth,
			textx + textw - extensionw,
			y + widget->itemh - (NLINES + 1 - widget->nlines[index] + 0.5) * widget->fonth
		);
	}
}

static int
firstvisible(Widget *widget)
{
	/* gets index of last visible item */
	return widget->row * widget->ncols;
}

static int
lastvisible(Widget *widget)
{
	/* gets index of last visible item */
	return min(widget->nitems, firstvisible(widget) + widget->nrows * widget->ncols) - 1;
}

static void
drawitem(Widget *widget, int index)
{
	int i, x, y, min, max;

	etlock(&widget->lock);
	min = firstvisible(widget);
	max = lastvisible(widget);
	if (index < min || index > max)
		goto done;
	i = index - min;
	x = i % widget->ncols;
	y = (i / widget->ncols) % widget->nrows;
	x *= widget->itemw;
	y *= widget->itemh;
	XftDrawRect(widget->draw, &widget->colors[SELECT_NOT][COLOR_BG], x, y, widget->itemw, widget->itemh);
	drawicon(widget, index, x, y);
	drawlabel(widget, index, x, y);
done:
	etunlock(&widget->lock);
	widget->redraw = TRUE;
}

static void
drawitems(Widget *widget)
{
	int i, n;

	XftDrawRect(widget->draw, &widget->colors[SELECT_NOT][COLOR_BG], 0, 0, widget->w, widget->nrows * widget->itemh);
	n = lastvisible(widget);
	for (i = widget->row * widget->ncols; i <= n; i++) {
		drawitem(widget, i);
	}
}

static void
commitdraw(Widget *widget)
{
	etlock(&widget->lock);
	XClearWindow(widget->display, widget->window);
	XCopyArea(
		widget->display,
		widget->pix, widget->window,
		widget->gc,
		0, widget->ydiff - MARGIN,
		widget->pixw, widget->pixh,
		widget->x0, 0
	);
	if (widget->state != STATE_SELECTING)
		goto done;
	XChangeGC(
		widget->display,
		widget->gc,
		GCClipXOrigin | GCClipYOrigin | GCClipMask,
		&(XGCValues) {
			.clip_x_origin = 0,
			.clip_y_origin = 0,
			.clip_mask = widget->rectbord,
		}
	);
	XSetForeground(widget->display, widget->gc, widget->colors[SELECT_NOT][COLOR_FG].pixel);
	XFillRectangle(widget->display, widget->window, widget->gc, 0, 0, widget->w, widget->h);
	XChangeGC(
		widget->display,
		widget->gc,
		GCClipMask,
		&(XGCValues) {
			.clip_mask = None,
		}
	);
	XFlush(widget->display);
done:
	etunlock(&widget->lock);
}

static void
settitle(Widget *widget)
{
	char title[TITLE_BUFSIZE];
	char nitems[STATUS_BUFSIZE];
	char *selitem, *status;
	int scrollpct;                  /* scroll percentage */

	if (widget->row == 0 && widget->nscreens > 1)
		scrollpct = 0;
	else
		scrollpct = 100 * ((double)(widget->row + 1) / widget->nscreens);
	(void)snprintf(nitems, STATUS_BUFSIZE, "%d items", widget->nitems - 1);
	selitem = "";
	status = nitems;
	selitem = (widget->highlight > 0 ? widget->items[widget->highlight][ITEM_NAME] : "");
	if (widget->highlight <= 0)
		status = nitems;
	else if (widget->items[widget->highlight][ITEM_STATUS] == NULL)
		status = STATUS_UNKNOWN;
	else
		status = widget->items[widget->highlight][ITEM_STATUS];
	if (widget->title != NULL) {
		(void)snprintf(
			title, TITLE_BUFSIZE,
			"%s%s%s (%s) - %s (%d%%)",
			widget->title,
			(strcmp(widget->title, "/") != 0 ? "/" : ""),
			selitem,
			status,
			widget->class,
			scrollpct
		);
	}
	XmbSetWMProperties(widget->display, widget->window, title, title, NULL, 0, NULL, NULL, NULL);
	XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_NET_WM_NAME],
		widget->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)title,
		strlen(title)
	);
	XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_CONTROL_CWD],
		widget->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)widget->title,
		strlen(title)
	);
}

static int
gethandlepos(Widget *widget)
{
	int row;

	if (widget->ydiff >= widget->itemh)
		row = widget->nscreens;
	else
		row = widget->row;
	return (HANDLE_MAX_SIZE - widget->handlew) * ((double)row / widget->nscreens);
}

static void
drawscroller(Widget *widget, int y)
{
	XftDraw *draw;
	XftColor color;
	Pixmap pix;

	if ((pix = XCreatePixmap(widget->display, widget->scroller, SCROLLER_SIZE, SCROLLER_SIZE, widget->depth)) == None)
		return;
	draw = XftDrawCreate(widget->display, pix, widget->visual, widget->colormap);
	color = widget->colors[SELECT_NOT][COLOR_BG];
	color.color.alpha = 0xFFFF;
	XftDrawRect(draw, &color, 0, 0, SCROLLER_SIZE, SCROLLER_SIZE);
	XftDrawRect(draw, &widget->colors[SELECT_NOT][COLOR_FG], 1, y + 1, HANDLE_MAX_SIZE, widget->handlew);
	XSetWindowBackgroundPixmap(widget->display, widget->scroller, pix);
	XClearWindow(widget->display, widget->scroller);
	XFreePixmap(widget->display, pix);
	XftDrawDestroy(draw);
}

static int
scroll(Widget *widget, int y)
{
	int prevhand, newhand;          /* position of the scroller handle */
	int prevrow, newrow;

	if (y == 0)
		return FALSE;
	if (ALL_ROWS(widget) + 1 < widget->nrows)
		return FALSE;
	prevhand = gethandlepos(widget);
	newrow = prevrow = widget->row;
	widget->ydiff += y;
	newrow += widget->ydiff / widget->itemh;
	widget->ydiff %= widget->itemh;
	if (widget->ydiff < 0) {
		widget->ydiff += widget->itemh;
		newrow--;
	}
	if (y > 0) {
		if (newrow >= widget->nscreens) {
			widget->ydiff = widget->itemh;
			newrow = widget->nscreens - 1;
		}
	} else if (y < 0) {
		if (newrow < 0) {
			widget->ydiff = 0;
			newrow = 0;
		}
	}
	setrow(widget, newrow);
	newhand = gethandlepos(widget);
	if (widget->state == STATE_SCROLLING && prevhand != newhand) {
		drawscroller(widget, newhand);
	}
	if (prevrow != newrow) {
		settitle(widget);
		return TRUE;
	}
	return FALSE;
}

static int
readsize(FILE *fp)
{
	int size, c, n, i;

	size = 0;
	for (i = 0; i < 3; i++) {
		c = fgetc(fp);
		if (c == EOF)
			return -1;
		n = ATOI(c);
		if (n == -1)
			break;
		size *= 10;
		size += n;
	}
	if (i == 0 || (i == 3 && ATOI(c) != -1))
		return -1;
	return size;
}

static int
getitem(Widget *widget, int row, int ydiff, int *x, int *y)
{
	int i, w, h;

	*y -= MARGIN;
	*y += ydiff;
	*x -= widget->x0;
	if (*x < 0 || *x >= widget->ncols * widget->itemw)
		return -1;
	if (*y < 0 || *y >= widget->h + ydiff)
		return -1;
	w = *x / widget->itemw;
	h = *y / widget->itemh;
	row *= widget->ncols;
	i = row + h * widget->ncols + w;
	if (i < row)
		return -1;
	*x -= w * widget->itemw;
	*y -= h * widget->itemh;
	return i;
}

static int
getpointerclick(Widget *widget, int x, int y)
{
	int iconx, textx, texty, i;

	if ((i = getitem(widget, widget->row, widget->ydiff, &x, &y)) < 0)
		return -1;
	if (i < 0 || i >= widget->nitems)
		return -1;
	iconx = (widget->itemw - THUMBSIZE) / 2;
	if (x >= iconx && x < iconx + THUMBSIZE && y >= 0 && y < THUMBSIZE + widget->fonth / 2)
		return i;
	if (widget->linelen == NULL)
		return -1;
	textx = (widget->itemw - widget->linelen[i]) / 2;
	texty = widget->itemh - (NLINES + 0.5) * widget->fonth;
	if (x >= textx && x < textx + widget->linelen[i] &&
	    y >= texty && y < texty + widget->nlines[i] * widget->fonth) {
		return i;
	}
	return -1;
}

static void
disownprimary(Widget *widget)
{
	if (widget->selctx == NULL)
		return;
	ctrlsel_disown(widget->selctx);
	FREE(widget->selctx);
}

static void
disowndnd(Widget *widget)
{
	if (widget->dragctx == NULL)
		return;
	ctrlsel_dnddisown(widget->dragctx);
	FREE(widget->dragctx);
}

static void
ownprimary(Widget *widget, Time time)
{
	struct Selection *sel;
	size_t i, j;
	int success;

	if (widget->sel == NULL)
		return;
	disownprimary(widget);
	if ((widget->selctx = malloc(sizeof(*widget->selctx))) == NULL) {
		warn("malloc");
		return;
	}
	i = j = 0;
	for (sel = widget->sel; sel != NULL; sel = sel->next) {
		if (sel->next != NULL)
			i += snprintf(widget->selbuf + i, widget->selbufsiz - i, "%s\n", widget->items[sel->index][ITEM_PATH]);
		j += snprintf(widget->uribuf + j, widget->uribufsiz - j, "file://%s\r\n", widget->items[sel->index][ITEM_PATH]);
	}
	ctrlsel_filltarget(
		XA_STRING, XA_STRING,
		8, (unsigned char *)widget->selbuf, i,
		&widget->targets[TARGET_STRING]
	);
	ctrlsel_filltarget(
		widget->atoms[UTF8_STRING], widget->atoms[UTF8_STRING],
		8, (unsigned char *)widget->selbuf, i,
		&widget->targets[TARGET_UTF8]
	);
	ctrlsel_filltarget(
		widget->atoms[TEXT_URI_LIST], widget->atoms[TEXT_URI_LIST],
		8, (unsigned char *)widget->uribuf, j,
		&widget->targets[TARGET_URI]
	);
	success = ctrlsel_setowner(
		widget->display, widget->window,
		XA_PRIMARY, time, 0,
		widget->targets, TARGET_LAST,
		widget->selctx
	);
	if (!success) {
		FREE(widget->selctx);
	}
}

static void
cleanwidget(Widget *widget)
{
	struct Thumb *thumb;
	struct Selection *sel;
	void *tmp;

	thumb = widget->thumbhead;
	while (thumb != NULL) {
		tmp = thumb;
		thumb = thumb->next;
		XDestroyImage(((struct Thumb *)tmp)->img);
		FREE(tmp);
	}
	sel = widget->sel;
	while (sel != NULL) {
		tmp = sel;
		sel = sel->next;
		FREE(tmp);
	}
	widget->sel = NULL;
	widget->rectsel = NULL;
	FREE(widget->thumbs);
	FREE(widget->linelen);
	FREE(widget->nlines);
	FREE(widget->issel);
	FREE(widget->selbuf);
	widget->selbufsiz = 0;
	FREE(widget->uribuf);
	widget->uribufsiz = 0;
	FREE(widget->dndbuf);
	disownprimary(widget);
	disowndnd(widget);
}

static void
selectitem(Widget *widget, int index, int select, int rectsel)
{
	struct Selection *sel;
	struct Selection **header;

	if (widget->issel == NULL || index <= 0 || index >= widget->nitems)
		return;
	/*
	 * We have two lists of selections: the global list (widget->sel),
	 * and the list used by rectangular selection (widget->rectsel).
	 */
	header = rectsel ? &widget->rectsel : &widget->sel;
	if (select && widget->issel[index] == NULL) {
		if ((sel = malloc(sizeof(*sel))) == NULL)
			return;
		*sel = (struct Selection){
			.next = *header,
			.prev = NULL,
			.index = (rectsel ? -1 : 1) * index,
		};
		if (*header != NULL)
			(*header)->prev = sel;
		*header = sel;
		widget->issel[index] = sel;
	} else if (!select && widget->issel[index] != NULL) {
		sel = widget->issel[index];
		if (sel->next != NULL)
			sel->next->prev = sel->prev;
		if (sel->prev != NULL)
			sel->prev->next = sel->next;
		if (*header == sel)
			*header = sel->next;
		FREE(sel);
		widget->issel[index] = NULL;
	} else {
		return;
	}
	drawitem(widget, index);
}

static void
highlight(Widget *widget, int index, int redraw)
{
	int prevhili;

	if (widget->highlight == index)
		return;
	prevhili = widget->highlight;
	widget->highlight = index;
	if (redraw)
		drawitem(widget, index);
	/* we still have to redraw the previous one */
	drawitem(widget, prevhili);
	settitle(widget);
}

static void
selectitems(Widget *widget, int a, int b)
{
	int i, min, max;

	if (a < 0 || b < 0 || a >= widget->nitems || b >= widget->nitems)
		return;
	if (a < b) {
		min = a;
		max = b;
	} else {
		min = b;
		max = a;
	}
	for (i = min; i <= max; i++) {
		selectitem(widget, i, TRUE, 0);
	}
}

static void
unselectitems(Widget *widget)
{
	while (widget->sel) {
		selectitem(widget, widget->sel->index, FALSE, 0);
	}
}

static int
mouse1click(Widget *widget, XButtonPressedEvent *ev)
{
	int prevhili, index;

	index = getpointerclick(widget, ev->x, ev->y);
	if (index > 0 && widget->issel[index] != NULL)
		return index;
	if (!(ev->state & (ControlMask | ShiftMask)))
		unselectitems(widget);
	if (index < 0)
		return index;
	/*
	 * If index != 0, there's no need to ask highlight() to redraw the item,
	 * as selectitem() or selectitems() will already redraw it.
	 */
	prevhili = widget->highlight;
	highlight(widget, index, (index == 0));
	if (prevhili != -1 && ev->state & ShiftMask)
		selectitems(widget, widget->highlight, prevhili);
	else
		selectitem(widget, widget->highlight, ((ev->state & ControlMask) ? widget->issel[widget->highlight] == NULL : TRUE), FALSE);
	ownprimary(widget, ev->time);
	return index;
}

static void
mouse3click(Widget *widget, int x, int y)
{
	int index;

	index = getpointerclick(widget, x, y);
	if (index != -1) {
		if (widget->issel[index] == NULL) {
			highlight(widget, index, FALSE);
			unselectitems(widget);
			selectitem(widget, index, TRUE, FALSE);
		} else {
			highlight(widget, index, TRUE);
		}
	}
}

static void
rectdraw(Widget *widget, int row, int ydiff, int x0, int y0, int x, int y)
{
	int w, h;

	XSetForeground(widget->display, widget->stipgc, 0);
	XFillRectangle(
		widget->display,
		widget->rectbord,
		widget->stipgc,
		0, 0,
		widget->w, widget->h
	);
	if (widget->state != STATE_SELECTING)
		return;
	if (row < widget->row) {
		y0 -= min(widget->row - row, widget->nrows) * widget->itemh;
	} else if (row > widget->row) {
		y0 += min(row - widget->row, widget->nrows) * widget->itemh;
	}
	y0 += ydiff - widget->ydiff;
	w = (x0 > x) ? x0 - x : x - x0;
	h = (y0 > y) ? y0 - y : y - y0;
	XSetForeground(widget->display, widget->stipgc, 1);
	XDrawRectangle(
		widget->display,
		widget->rectbord,
		widget->stipgc,
		min(x0, x),
		min(y0, y),
		w, h
	);
}

static int
rectselect(Widget *widget, int srcrow, int srcydiff, int srcx, int srcy, int dstx, int dsty)
{
	int row, col, tmp, i;
	int changed;
	int sel, x, y;

	/* x,y positions of the vertices of the rectangular selection */
	int minx, maxx, miny;

	/*
	 * Indices of visible items.
	 */
	int vismin, vismax;

	/*
	 * Indices of items at the top left of the rectangular selection
	 * and at bottom right of the rectangular selection.
	 */
	int indexmin, indexmax, srci, dsti;

	/*
	 * First and last columns and rows of the items at the
	 * rectangular selection.
	 */
	int colmin, colmax;
	int rowmin;
	int rowsrc;

	miny = min(srcy, dsty);
	minx = min(srcx, dstx);
	maxx = max(srcx, dstx);
	if ((dstx > srcx && srcy > dsty) || (dstx < srcx && srcy < dsty)) {
		tmp = dstx;
		dstx = srcx;
		srcx = tmp;
	}
	if (dstx < widget->x0)              dstx = widget->x0;
	if (srcx < widget->x0)              srcx = widget->x0;
	if (dstx >= widget->x0 + widget->pixw) dstx = widget->x0 + widget->pixw - 1;
	if (srcx >= widget->x0 + widget->pixw) srcx = widget->x0 + widget->pixw - 1;
	if (dsty < MARGIN)               dsty = MARGIN;
	if (srcy < MARGIN)               srcy = MARGIN;
	if (dsty >= widget->h)              dsty = widget->h - 1;
	if (srcy >= widget->h)              srcy = widget->h - 1;
	if ((srci = getitem(widget, srcrow, srcydiff, &srcx, &srcy)) < 0)
		return FALSE;
	if ((dsti = getitem(widget, widget->row, widget->ydiff, &dstx, &dsty)) < 0)
		return FALSE;
	vismin = firstvisible(widget);
	vismax = lastvisible(widget);
	indexmin = min(srci, dsti);
	indexmax = max(srci, dsti);
	colmin = indexmin % widget->ncols;
	colmax = indexmax % widget->ncols;
	indexmin = min(indexmin, widget->nitems - 1);
	indexmax = min(indexmax, widget->nitems - 1);
	rowmin = indexmin / widget->ncols;
	rowsrc = srci / widget->ncols;
	changed = FALSE;
	for (i = vismin; i <= vismax; i++) {
		sel = TRUE;
		row = i / widget->ncols;
		col = i % widget->ncols;
		x = widget->x0 + col * widget->itemw + (widget->itemw - THUMBSIZE) / 2;
		y = (row - widget->row + 1) * widget->itemh -
		    (NLINES - widget->nlines[i] + 0.5) * widget->fonth +
		    MARGIN - widget->ydiff;
		if (i < indexmin || i > indexmax) {
			sel = FALSE;
		} else if ((col == colmin || col == colmax) && (minx > x + THUMBSIZE || maxx < x)) {
			sel = FALSE;
		} else if (col < colmin || col > colmax) {
			sel = FALSE;
		} else if (row == rowmin && row != rowsrc && row >= widget->row && miny > y) {
			sel = FALSE;
		}
		if (!sel && (widget->issel[i] == NULL || widget->issel[i]->index > 0))
			continue;
		if (sel && widget->issel[i] != NULL && widget->issel[i]->index > 0)
			selectitem(widget, i, FALSE, FALSE);
		selectitem(widget, i, sel, TRUE);
		changed = TRUE;
	}
	return changed;
}

static void
commitrectsel(Widget *widget)
{
	struct Selection *sel, *next;

	/*
	 * We keep the items selected by rectangular selection on a
	 * temporary list.  Move them to the regular list.
	 */
	while (widget->rectsel != NULL) {
		next = widget->rectsel->next;
		sel = widget->rectsel;
		sel->next = widget->sel;
		sel->prev = NULL;
		if (sel->index < 0)
			sel->index *= -1;
		if (widget->sel != NULL)
			widget->sel->prev = sel;
		widget->sel = sel;
		widget->rectsel = next;
	}
}

static void
endevent(Widget *widget)
{
	if (widget->redraw) {
		commitdraw(widget);
	}
}

static int
querypointer(Widget *widget, Window win, int *retx, int *rety, unsigned int *retmask)
{
	Window root, child;
	unsigned int mask;
	int rootx, rooty;
	int x, y;
	int retval;

	retval = XQueryPointer(
		widget->display,
		win,
		&root, &child,
		&rootx, &rooty,
		&x, &y,
		&mask
	);
	if (retmask != NULL)
		*retmask = mask;
	if (retx != NULL)
		*retx = x;
	if (rety != NULL)
		*rety = y;
	return retval;
}

static void
scrollerset(Widget *widget, int pos)
{
	int prevrow, newrow, maxpos;

	maxpos = HANDLE_MAX_SIZE - widget->handlew;
	if (maxpos <= 0) {
		/* all files are visible, there's nothing to scroll */
		return;
	}
	pos = max(pos, 0);
	pos = min(pos, maxpos);
	newrow = pos * widget->nscreens / maxpos;
	newrow = max(newrow, 0);
	newrow = min(newrow, widget->nscreens);
	if (newrow == widget->nscreens) {
		widget->ydiff = widget->itemh;
		newrow = widget->nscreens - 1;
	} else {
		widget->ydiff = 0;
	}
	prevrow = widget->row;
	setrow(widget, newrow);
	drawscroller(widget, pos);
	if (prevrow != newrow) {
		settitle(widget);
		drawitems(widget);
	}
}

static int
checkheader(FILE *fp, char *header, size_t size)
{
	char buf[PPM_BUFSIZE];

	if (fread(buf, 1, size, fp) != size)
		return RETURN_FAILURE;
	if (memcmp(buf, header, size) != 0)
		return RETURN_FAILURE;
	return RETURN_SUCCESS;
}

static int
pixmapfromdata(Widget *widget, char **data, Pixmap *pix, Pixmap *mask)
{
	XpmAttributes xa = {
		.valuemask = XpmVisual | XpmColormap | XpmDepth,
		.visual = widget->visual,
		.colormap = widget->colormap,
		.depth = widget->depth,
	};

	if (XpmCreatePixmapFromData(widget->display, widget->window, data, pix, mask, &xa) != XpmSuccess) {
		*pix = None;
		*mask = None;
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
}

static int
fillselitems(Widget *widget, int *selitems, int clicked)
{
	struct Selection *sel;
	int nitems;

	nitems = 0;
	if (clicked != -1)
		selitems[nitems++] = clicked;
	for (sel = widget->sel; sel != NULL; sel = sel->next) {
		if (sel->index == clicked)
			continue;
		selitems[nitems++] = sel->index;
	}
	return nitems;
}

static Window
createdragwin(Widget *widget, int index)
{
	Window win;
	GC gc;
	unsigned long opacity;
	int icon, pix, mask;
	int xroot, yroot, w, h;

	if (index <= 0)
		return None;
	if (!querypointer(widget, ROOT(widget->display), &xroot, &yroot, NULL))
		return None;
	w = h = THUMBSIZE;
	if (widget->thumbs[index] != NULL) {
		w = widget->thumbs[index]->w;
		h = widget->thumbs[index]->h;
	}
	win = XCreateWindow(
		widget->display, ROOT(widget->display),
		xroot, yroot, w, h, 0,
		widget->depth, InputOutput, widget->visual,
		CWBackPixel | CWOverrideRedirect| CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.colormap = widget->colormap,
			.background_pixel = widget->colors[SELECT_NOT][COLOR_BG].pixel,
			.override_redirect = True
		}
	);
	if (win == None)
		return None;
	opacity = DND_OPACITY;
	XChangeProperty(
		widget->display, win,
		widget->atoms[_NET_WM_WINDOW_OPACITY],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&opacity,
		1
	);
	XChangeProperty(
		widget->display, win,
		widget->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *)&widget->atoms[_NET_WM_WINDOW_TYPE_DND],
		1
	);
	if (widget->thumbs[index] == NULL) {
		icon = widget->itemicons[index];
		pix = widget->icons[icon].pix;
		if ((mask = XCreatePixmap(widget->display, win, w, h, CLIP_DEPTH)) == None) {
			XDestroyWindow(widget->display, win);
			return None;
		}
		if ((gc = XCreateGC(widget->display, mask, 0, NULL)) == None) {
			XFreePixmap(widget->display, mask);
			XDestroyWindow(widget->display, win);
			return None;
		}
		XSetForeground(widget->display, gc, 0);
		XFillRectangle(widget->display, mask, gc, 0, 0, w, h);
		XCopyArea(widget->display, widget->icons[icon].mask, mask, gc, 0, 0, w, h, 0, 0);
		XShapeCombineMask(widget->display, win, ShapeBounding, 0, 0, mask, ShapeSet);
		XFreePixmap(widget->display, mask);
		XFreeGC(widget->display, gc);
		XSetWindowBackgroundPixmap(widget->display, win, pix);
	} else {
		if ((pix = XCreatePixmap(widget->display, win, w, h, widget->depth)) == None) {
			XDestroyWindow(widget->display, win);
			return None;
		}
		XPutImage(
			widget->display,
			pix,
			widget->gc,
			widget->thumbs[index]->img,
			0, 0, 0, 0, w, h
		);
		XSetWindowBackgroundPixmap(widget->display, win, pix);
		XFreePixmap(widget->display, pix);
	}
	XMapRaised(widget->display, win);
	XFlush(widget->display);
	return win;
}

static char *
gettextprop(Widget *widget, Atom prop)
{
	char *text;
	unsigned char *p;
	unsigned long len;
	unsigned long dl;               /* dummy variable */
	int format, status;
	Atom type;

	text = NULL;
	status = XGetWindowProperty(
		widget->display,
		widget->window,
		prop,
		0L,
		0x1FFFFFFF,
		True,
		AnyPropertyType,
		&type, &format,
		&len, &dl,
		&p
	);
	if (status != Success || len == 0 || p == NULL) {
		goto done;
	}
	if ((text = emalloc(len + 1)) == NULL) {
		goto done;
	}
	memcpy(text, p, len);
	text[len] = '\0';
done:
	XFree(p);
	return text;
}

static void
xinitvisual(Widget *widget)
{
	XVisualInfo tpl = {
		.screen = SCREEN(widget->display),
		.depth = 32,
		.class = TrueColor
	};
	XVisualInfo *infos;
	XRenderPictFormat *fmt;
	long masks = VisualScreenMask | VisualDepthMask | VisualClassMask;
	int nitems;
	int i;

	widget->visual = NULL;
	if ((infos = XGetVisualInfo(widget->display, masks, &tpl, &nitems)) != NULL) {
		for (i = 0; i < nitems; i++) {
			fmt = XRenderFindVisualFormat(widget->display, infos[i].visual);
			if (fmt->type == PictTypeDirect && fmt->direct.alphaMask) {
				widget->depth = infos[i].depth;
				widget->visual = infos[i].visual;
				widget->colormap = XCreateColormap(widget->display, ROOT(widget->display), widget->visual, AllocNone);
				break;
			}
		}
		XFree(infos);
	}
	if (widget->visual == NULL) {
		widget->depth = DefaultDepth(widget->display, SCREEN(widget->display));
		widget->visual = DefaultVisual(widget->display, SCREEN(widget->display));
		widget->colormap = DefaultColormap(widget->display, SCREEN(widget->display));
	}
}

/*
 * The following routines check an event, and process then if needed.
 * They return WIDGET_NONE if the event is not processed.
 */

static WidgetEvent
keypress(Widget *widget, XKeyEvent *xev, int *selitems, int *nitems, char **text)
{
	KeySym ksym;
	unsigned int state;
	int row[2];
	int redrawall, previtem, index, shift, newrow, n, i;
	char *kstr;

	if (!XkbLookupKeySym(widget->display, xev->keycode, xev->state, &state, &ksym))
		return WIDGET_NONE;
	switch (ksym) {
	case XK_KP_Enter:       ksym = XK_Return;       break;
	case XK_KP_Space:       ksym = XK_space;        break;
	case XK_KP_Home:        ksym = XK_Home;         break;
	case XK_KP_End:         ksym = XK_End;          break;
	case XK_KP_Left:        ksym = XK_Left;         break;
	case XK_KP_Right:       ksym = XK_Right;        break;
	case XK_KP_Up:          ksym = XK_Up;           break;
	case XK_KP_Down:        ksym = XK_Down;         break;
	case XK_KP_Prior:       ksym = XK_Prior;        break;
	case XK_KP_Next:        ksym = XK_Next;         break;
	default:                                        break;
	}
	switch (ksym) {
	case XK_Escape:
		if (widget->sel == NULL)
			break;
		unselectitems(widget);
		break;
	case XK_Return:
		if (widget->highlight == -1)
			break;
		*nitems = fillselitems(widget, selitems, widget->highlight);
		return WIDGET_OPEN;
	case XK_Menu:
		*nitems = fillselitems(widget, selitems, -1);
		return WIDGET_CONTEXT;
	case XK_space:
		if (widget->highlight == -1)
			break;
		selectitem(widget, widget->highlight, widget->issel[widget->highlight] == NULL, FALSE);
		break;
	case XK_Prior:
	case XK_Next:
		if (scroll(widget, (ksym == XK_Prior ? -1 : 1) * PAGE_STEP(widget)))
			drawitems(widget);
		break;
	case XK_Home:
	case XK_End:
	case XK_Up:
	case XK_Down:
	case XK_Left:
	case XK_Right:
hjkl:
		redrawall = TRUE;
		if (ksym == XK_Home) {
			index = 0;
			widget->ydiff = 0;
			setrow(widget, 0);
			goto draw;
		}
		if (ksym == XK_End) {
			index = widget->nitems - 1;
			widget->ydiff = 0;
			setrow(widget, widget->nscreens);
			goto draw;
		}
		if (widget->highlight == -1) {
			widget->highlight = 0;
			setrow(widget, 0);
		}
		if (ksym == XK_Up || ksym == XK_k) {
			n = -widget->ncols;
		} else if (ksym == XK_Down || ksym == XK_j) {
			n = widget->highlight < (WHL_ROWS(widget)) * widget->ncols
			  ? widget->nitems - widget->highlight - 1
			  : 0;
			n = min(widget->ncols, n);
		} else if (ksym == XK_Left || ksym == XK_h) {
			n = -1;
		} else {
			n = 1;
		}
		if ((index = widget->highlight + n) < 0 || index >= widget->nitems)
			break;
		row[0] = widget->highlight / widget->ncols;
		row[1] = index / widget->ncols;
		newrow = widget->row;
		for (i = 0; i < 2; i++) {
			/*
			 * Try to make both previously highlighted item
			 * and new highlighted item visible.
			 */
			if (row[i] < newrow) {
				newrow = row[i];
			} else if (row[i] >= newrow + WIN_ROWS(widget)) {
				newrow = row[i] - WIN_ROWS(widget) + 1;
			}
		}
		if (widget->row != newrow) {
			widget->ydiff = 0;
			setrow(widget, newrow);
			redrawall = TRUE;
		} else if (widget->row == index / widget->ncols) {
			widget->ydiff = 0;
			widget->redraw = TRUE;
		}
draw:
		previtem = widget->highlight;
		highlight(widget, index, TRUE);
		if (xev->state & ShiftMask)
			selectitems(widget, index, previtem);
		else if (xev->state & ControlMask)
			selectitem(widget, index, TRUE, 0);
		if (redrawall)
			drawitems(widget);
		break;
	default:
		/*
		 * Check if the key symbol is a printable ASCII symbol.
		 * If they are, fallthrough if they are modified by
		 * Control.
		 *
		 * We're using some implementational knowledge here.
		 *
		 * The range of ASCII printable characters goes from
		 * 0x20 (space) to 0x7E (tilde).  Keysyms for ASCII
		 * symbols are thankfully encoded the same as their
		 * ASCII counterparts!
		 *
		 * That is, XK_A == 'A' == 0x41.
		 */
		if (ksym < XK_space || ksym > XK_asciitilde)
			break;
		if (!FLAG(xev->state, ControlMask)) {
			if (ksym == XK_h || ksym == XK_j || ksym == XK_k || ksym == XK_l)
				goto hjkl;
			break;
		}
		/* FALLTHROUGH */
	case XK_F1: case XK_F2: case XK_F3: case XK_F4: case XK_F5: case XK_F6:
	case XK_F7: case XK_F8: case XK_F9: case XK_F10: case XK_F11: case XK_F12:
	case XK_F13: case XK_Delete: case XK_BackSpace: case XK_Tab:
		/*
		 * User pressed either Control + ASCII Key, an F-key,
		 * Delete, Backspace or Tab.  We inform the calling
		 * module that such key is pressed for it to process
		 * it however wants.
		 */
		kstr = XKeysymToString(ksym);
		if ((*text = malloc(KSYM_BUFSIZE)) == NULL)
			break;
		shift = FLAG(xev->state, ShiftMask);
		(void)snprintf(*text, KSYM_BUFSIZE, "^%s%s", shift ? "S-" : "", kstr);
		*nitems = fillselitems(widget, selitems, -1);
		return WIDGET_KEYPRESS;
	}
	return WIDGET_NONE;
}

static WidgetEvent
processevent(Widget *widget, XEvent *ev)
{
	if (widget->selctx != NULL) {
		switch (ctrlsel_send(widget->selctx, ev)) {
		case CTRLSEL_LOST:
			unselectitems(widget);
			disownprimary(widget);
			return WIDGET_INTERNAL;
		case CTRLSEL_INTERNAL:
			return WIDGET_INTERNAL;
		default:
			break;
		}
	}
	if (widget->dragctx != NULL) {
		switch (ctrlsel_dndsend(widget->dragctx, ev)) {
		case CTRLSEL_SENT:
			disowndnd(widget);
			return WIDGET_REFRESH;
		case CTRLSEL_LOST:
			disowndnd(widget);
			return WIDGET_INTERNAL;
		case CTRLSEL_INTERNAL:
			return WIDGET_INTERNAL;
		default:
			break;
		}
	}
	switch (ctrlsel_dndreceive(&widget->dropctx, ev)) {
	case CTRLSEL_RECEIVED:
		FREE(widget->droptarget.buffer);
		return WIDGET_INTERNAL;
	case CTRLSEL_INTERNAL:
		return WIDGET_INTERNAL;
	default:
		break;
	}
	widget->redraw = FALSE;
	switch (ev->type) {
	case ClientMessage:
		if (ev->xclient.message_type == widget->atoms[WM_PROTOCOLS] &&
		    (Atom)ev->xclient.data.l[0] == widget->atoms[WM_DELETE_WINDOW])
			return WIDGET_CLOSE;
		return WIDGET_NONE;
	case Expose:
		if (ev->xexpose.count == 0)
			commitdraw(widget);
		break;
	case ConfigureNotify:
		if (calcsize(widget, ev->xconfigure.width, ev->xconfigure.height)) {
			if (widget->row >= widget->nscreens)
				setrow(widget, widget->nscreens - 1);
			drawitems(widget);
		}
		break;
	case PropertyNotify:
		if (ev->xproperty.window != widget->window)
			break;
		if (ev->xproperty.state != PropertyNewValue)
			break;
		if (ev->xproperty.atom != widget->atoms[_CONTROL_GOTO])
			break;
		FREE(widget->lasttext);
		widget->lastprop = widget->atoms[_CONTROL_GOTO];
		widget->lasttext = gettextprop(widget, widget->atoms[_CONTROL_GOTO]);
		break;
	default:
		return WIDGET_NONE;
	}
	endevent(widget);
	return WIDGET_INTERNAL;
}

static WidgetEvent
checklastprop(Widget *widget, char **text)
{
	Atom prop;
	char *str;

	if (widget->lastprop != None) {
		prop = widget->lastprop;
		str = widget->lasttext;
		widget->lastprop = None;
		widget->lasttext = NULL;
		if (prop == widget->atoms[_CONTROL_GOTO]) {
			*text = str;
			return WIDGET_GOTO;
		} else {
			FREE(str);
		}
	}
	return WIDGET_NONE;
}

/*
 * The following are the event loops we use.
 *
 * mainmode() is the main event loop; while the others are modes that
 * the widget can get in before returning to main mode.
 *
 * All event loops call processevent to handle events that are common
 * to all modes.
 */

static WidgetEvent
scrollmode(Widget *widget, int x, int y)
{
	XEvent ev;
	int grabpos, pos, left;

	widget->state = STATE_SCROLLING;
	grabpos = widget->handlew / 2;             /* we grab the handle in its middle */
	drawscroller(widget, gethandlepos(widget));
	XMoveWindow(widget->display, widget->scroller, x - SCROLLER_SIZE / 2 - 1, y - SCROLLER_SIZE / 2 - 1);
	XMapRaised(widget->display, widget->scroller);
	left = FALSE;
	while (!XNextEvent(widget->display, &ev)) {
		switch (processevent(widget, &ev)) {
		case WIDGET_CLOSE:
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		switch (ev.type) {
		case MotionNotify:
			if (ev.xmotion.window == widget->scroller && (ev.xmotion.state & Button1Mask)) {
				scrollerset(widget, ev.xmotion.y - grabpos);
			} else if (ev.xmotion.window == widget->window &&
			    (diff(ev.xmotion.x, x) > SCROLLER_SIZE / 2 || diff(ev.xmotion.y, y) > SCROLLER_SIZE / 2)) {
				left = TRUE;
			}
			break;
		case ButtonRelease:
			if (left)
				goto done;
			break;
		case ButtonPress:
			if (ev.xbutton.button != Button1)
				goto done;
			if (ev.xbutton.window == widget->window)
				goto done;
			if (ev.xbutton.window == widget->scroller) {
				left = TRUE;
				pos = gethandlepos(widget);
				if (ev.xmotion.y < pos || ev.xmotion.y > pos + widget->handlew) {
					/* grab handle in the middle */
					grabpos = widget->handlew / 2;
					scrollerset(widget, ev.xmotion.y - grabpos);
				} else {
					/* grab handle in position under pointer */
					grabpos = ev.xmotion.y - pos;
				}
			}
			break;
		}
		endevent(widget);
	}
done:
	widget->state = STATE_NORMAL;
	XUnmapWindow(widget->display, widget->scroller);
	return WIDGET_NONE;
}

static WidgetEvent
selmode(Widget *widget, Time lasttime, int shift, int clickx, int clicky)
{
	XEvent ev;
	int rectrow, rectydiff, ownsel;

	widget->state = STATE_SELECTING;
	rectrow = widget->row;
	rectydiff = widget->ydiff;
	ownsel = FALSE;
	if (!shift)
		unselectitems(widget);
	while (!XNextEvent(widget->display, &ev)) {
		switch (processevent(widget, &ev)) {
		case WIDGET_CLOSE:
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		switch (ev.type) {
		case ButtonPress:
		case ButtonRelease:
			goto done;
		case MotionNotify:
			if (ev.xmotion.time - lasttime < MOTION_TIME)
				break;
			rectdraw(widget, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y);
			if (rectselect(widget, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y))
				ownsel = TRUE;
			commitdraw(widget);
			lasttime = ev.xmotion.time;
			break;
		}
		endevent(widget);
	}
done:
	widget->state = STATE_NORMAL;
	rectdraw(widget, 0, 0, 0, 0, 0, 0);
	commitrectsel(widget);
	commitdraw(widget);
	if (ownsel)
		ownprimary(widget, ev.xbutton.time);
	return WIDGET_NONE;
}

static WidgetEvent
dragmode(Widget *widget, Time lasttime, int clicki, int *selitems, int *nitems)
{
	struct Selection *sel;
	Window dragwin;
	unsigned int mask;
	int state, i, x, y;

	if (widget->sel == NULL)
		return WIDGET_NONE;
	disowndnd(widget);
	if ((widget->dragctx = malloc(sizeof(*widget->dragctx))) == NULL) {
		warn("malloc");
		return WIDGET_NONE;
	}
	dragwin = createdragwin(widget, clicki);
	i = 0;
	for (sel = widget->sel; sel != NULL; sel = sel->next)
		i += snprintf(widget->dndbuf + i, widget->uribufsiz - i, "file://%s\r\n", widget->items[sel->index][ITEM_PATH]);
	ctrlsel_filltarget(
		widget->atoms[TEXT_URI_LIST],
		widget->atoms[TEXT_URI_LIST],
		8, (unsigned char *)widget->dndbuf, i,
		&widget->dragtarget
	);
	state = ctrlsel_dndown(
		widget->display,
		widget->window,
		dragwin,
		lasttime,
		&widget->dragtarget,
		1,
		widget->dragctx
	);
	if (dragwin != None)
		XDestroyWindow(widget->display, dragwin);
	if (state != CTRLSEL_DROPOTHER)
		FREE(widget->dragctx);
	if (state == CTRLSEL_ERROR) {
		warnx("could not perform drag-and-drop");
	} else if (state == CTRLSEL_DROPSELF) {
		querypointer(widget, widget->window, &x, &y, &mask);
		clicki = getpointerclick(widget, x, y);
		if (clicki < 1)
			return WIDGET_NONE;
		*nitems = fillselitems(widget, selitems, clicki);
		if (FLAG(mask, ControlMask|ShiftMask))
			return WIDGET_DROPLINK;
		if (FLAG(mask, ShiftMask))
			return WIDGET_DROPMOVE;
		if (FLAG(mask, ControlMask))
			return WIDGET_DROPCOPY;
		return WIDGET_DROPASK;
	}
	return WIDGET_NONE;
}

static WidgetEvent
mainmode(Widget *widget, int *selitems, int *nitems, char **text)
{
	XEvent ev;
	Time lasttime = 0;
	int clickx = 0;
	int clicky = 0;
	int clicki = -1;
	int state;
	int x, y;

	while (!XNextEvent(widget->display, &ev)) {
		switch (ctrlsel_dndreceive(&widget->dropctx, &ev)) {
		case CTRLSEL_RECEIVED:
			if (widget->droptarget.buffer == NULL)
				return WIDGET_INTERNAL;
			*text = (char *)widget->droptarget.buffer;
			*nitems = 1;
			querypointer(widget, widget->window, &x, &y, NULL);
			selitems[0] = getpointerclick(widget, x, y);
			if (widget->droptarget.action == CTRLSEL_COPY)
				return WIDGET_DROPCOPY;
			if (widget->droptarget.action == CTRLSEL_MOVE)
				return WIDGET_DROPMOVE;
			if (widget->droptarget.action == CTRLSEL_LINK)
				return WIDGET_DROPLINK;
			return WIDGET_DROPASK;
		case CTRLSEL_INTERNAL:
			return WIDGET_INTERNAL;
		default:
			break;
		}
		switch ((state = processevent(widget, &ev))) {
		case WIDGET_CLOSE:
		case WIDGET_REFRESH:
			return state;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		if ((state = checklastprop(widget, text)) != WIDGET_NONE)
			return state;
		switch (ev.type) {
		case KeyPress:
			state = keypress(widget, &ev.xkey, selitems, nitems, text);
			if (state != WIDGET_NONE)
				return state;
			break;
		case ButtonPress:
			clickx = ev.xbutton.x;
			clicky = ev.xbutton.y;
			if (ev.xbutton.window != widget->window)
				break;
			if (ev.xbutton.button == Button1) {
				clicki = mouse1click(widget, &ev.xbutton);
			} else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
				if (scroll(widget, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP)))
					drawitems(widget);
				commitdraw(widget);
			} else if (ev.xbutton.button == Button2) {
				state = scrollmode(widget, ev.xmotion.x, ev.xmotion.y);
				if (state != WIDGET_NONE)
					return state;
			} else if (ev.xbutton.button == Button3) {
				mouse3click(widget, ev.xbutton.x, ev.xbutton.y);
				*nitems = fillselitems(widget, selitems, -1);
				commitdraw(widget);
				XUngrabPointer(widget->display, ev.xbutton.time);
				XFlush(widget->display);
				return WIDGET_CONTEXT;
			}
			break;
		case ButtonRelease:
			if (ev.xbutton.window != widget->window)
				break;
			if (ev.xbutton.button == BUTTON8)
				return WIDGET_PREV;
			if (ev.xbutton.button == BUTTON9)
				return WIDGET_NEXT;
			if (ev.xbutton.button != Button1)
				break;
			if (clicki < 0 ||
			    (ev.xbutton.state & (ControlMask | ShiftMask)) ||
			    ev.xbutton.time - lasttime > DOUBLECLICK) {
				lasttime = ev.xbutton.time;
				break;
			}
			*nitems = fillselitems(widget, selitems, widget->highlight);
			return WIDGET_OPEN;
		case MotionNotify:
			if (ev.xmotion.window != widget->window)
				break;
			if (ev.xmotion.state != Button1Mask &&
			    ev.xmotion.state != (Button1Mask|ShiftMask) &&
			    ev.xmotion.state != (Button1Mask|ControlMask))
				break;
			if (diff(ev.xmotion.x, clickx) * diff(ev.xmotion.y, clicky) < DRAG_SQUARE)
				break;
			if (clicki == -1) {
				state = selmode(widget, ev.xmotion.time, ev.xmotion.state & (ShiftMask | ControlMask), clickx, clicky);
			} else if (clicki > 0) {
				state = dragmode(widget, ev.xmotion.time, clicki, selitems, nitems);
			}
			if (state != WIDGET_NONE)
				return state;
			break;
		default:
			break;
		}
		endevent(widget);
	}
	return WIDGET_CLOSE;
}

/*
 * Check widget.h for description on the interface of the following
 * public functions.  Some of them rely on the existence of objects
 * in the given addresses, during Widget's lifetime.
 */

Widget *
initwidget(const char *class, const char *name, const char *geom, int argc, char *argv[])
{
	Widget *widget;
	int success;
	char *progname, *s;

	widget = NULL;
	progname = "";
	if (argc > 0 && argv[0] != NULL) {
		progname = argv[0];
		if ((s = strrchr(argv[0], '/')) != NULL) {
			progname = s + 1;
		}
	}
	if (name == NULL)
		name = progname;
	if ((widget = malloc(sizeof(*widget))) == NULL) {
		warn("malloc");
		return NULL;
	}
	*widget = (struct Widget){
		.progname = progname,
		.display = NULL,
		.start = FALSE,
		.redraw = FALSE,
		.window = None,
		.scroller = None,
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.draw = NULL,
		.thumbs = NULL,
		.thumbhead = NULL,
		.linelen = NULL,
		.nlines = NULL,
		.icons = NULL,
		.highlight = -1,
		.title = "",
		.class = class,
		.sel = NULL,
		.rectsel = NULL,
		.issel = NULL,
		.seltime = 0,
		.pix = None,
		.rectbord = None,
		.rectw = 0,
		.recth = 0,
		.state = STATE_NORMAL,
		.namepix = None,
		.stipple = None,
		.lastprop = None,
		.lasttext = NULL,
		.selctx = NULL,
		.dragctx = NULL,
		.uribuf = NULL,
		.dndbuf = NULL,
		.selbuf = NULL,
	};
	if (!XInitThreads()) {
		warnx("XInitThreads");
		goto error;
	}
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale()) {
		warnx("could not set locale");
		goto error;
	}
	if ((widget->display = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		goto error;
	}
	xinitvisual(widget);
	XInternAtoms(widget->display, atomnames, ATOM_LAST, False, widget->atoms);
	if (fcntl(XConnectionNumber(widget->display), F_SETFD, FD_CLOEXEC) == -1) {
		warn("fcntl");
		goto error;
	}
	if (inittheme(widget, class, name) == -1) {
		warnx("could not set theme");
		goto error;
	}
	if (createwin(widget, class, name, geom, argc, argv, winicon_data, winicon_size) == -1) {
		warnx("could not create window");
		goto error;
	}
	if ((widget->gc = XCreateGC(widget->display, widget->window, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None) {
		warnx("could not create graphics context");
		goto error;
	}
	widget->scroller = XCreateWindow(
		widget->display, widget->window,
		0, 0, SCROLLER_SIZE, SCROLLER_SIZE, 1,
		widget->depth, InputOutput, widget->visual,
		CWBackPixel | CWBorderPixel | CWEventMask | CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.colormap = widget->colormap,
			.background_pixel = 0,
			.border_pixel = widget->colors[SELECT_NOT][COLOR_FG].pixel,
			.event_mask = ButtonPressMask | PointerMotionMask,
		}
	);
	if (widget->scroller == None) {
		warnx("could not create window");
		goto error;
	}
	if ((widget->stipple = XCreatePixmap(widget->display, widget->window, STIPPLE_SIZE, STIPPLE_SIZE, CLIP_DEPTH)) == None) {
		warnx("could not create pixmap");
		goto error;
	}
	if ((widget->stipgc = XCreateGC(widget->display, widget->stipple, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None) {
		warnx("could not create graphics context");
		goto error;
	}
	ctrlsel_filltarget(
		widget->atoms[TEXT_URI_LIST],
		widget->atoms[TEXT_URI_LIST],
		0, NULL, 0,
		&widget->droptarget
	);
	success = ctrlsel_dndwatch(
		widget->display,
		widget->window,
		CTRLSEL_COPY | CTRLSEL_MOVE | CTRLSEL_LINK | CTRLSEL_ASK,
		&widget->droptarget,
		1,
		&widget->dropctx
	);
	if (!success) {
		ctrlsel_dndclose(&widget->dropctx);
		goto error;
	}
	XSetForeground(widget->display, widget->stipgc, 0);
	XFillRectangle(widget->display, widget->stipple, widget->stipgc, 0, 0, STIPPLE_SIZE, STIPPLE_SIZE);
	XSetForeground(widget->display, widget->stipgc, 1);
	XFillRectangle(widget->display, widget->stipple, widget->stipgc, 0, 0, 1, 1);
	XSetStipple(widget->display, widget->gc, widget->stipple);
	widget->busycursor = XCreateFontCursor(widget->display, XC_watch);
	return widget;
error:
	if (widget->stipple != None)
		XFreePixmap(widget->display, widget->stipple);
	if (widget->scroller != None)
		XDestroyWindow(widget->display, widget->scroller);
	if (widget->window != None)
		XDestroyWindow(widget->display, widget->window);
	if (widget->display != NULL)
		XCloseDisplay(widget->display);
	FREE(widget);
	return NULL;
}

int
setwidget(Widget *widget, const char *title, char **items[], int itemicons[], size_t nitems, Scroll *scrl)
{
	size_t i;

	XUndefineCursor(widget->display, widget->window);
	cleanwidget(widget);
	widget->items = items;
	widget->nitems = nitems;
	widget->itemicons = itemicons;
	if (scrl == NULL) {
		widget->ydiff = 0;
		widget->row = 0;
	} else {
		widget->ydiff = scrl->ydiff;
		widget->row = scrl->row;
	}
	widget->title = title;
	widget->highlight = -1;
	(void)calcsize(widget, -1, -1);
	if (scrl != NULL && widget->row >= widget->nscreens) {
		widget->ydiff = 0;
		setrow(widget, widget->nscreens);
	}
	if ((widget->issel = calloc(widget->nitems, sizeof(*widget->issel))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((widget->linelen = calloc(widget->nitems, sizeof(*widget->linelen))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((widget->nlines = calloc(widget->nitems, sizeof(*widget->nlines))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((widget->thumbs = malloc(nitems * sizeof(*widget->thumbs))) == NULL) {
		warn("malloc");
		goto error;
	}
	widget->selbufsiz = 0;
	for (i = 0; i < nitems; i++) {
		widget->thumbs[i] = NULL;
		widget->selbufsiz += strlen(items[i][ITEM_PATH]) + 1; /* +1 for '\n' */
	}
	if ((widget->selbuf = malloc(widget->selbufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	widget->uribufsiz = widget->selbufsiz + (nitems * 8);         /* 8 for "file://\r" */
	if ((widget->uribuf = malloc(widget->uribufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	if ((widget->dndbuf = malloc(widget->uribufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	widget->thumbhead = NULL;
	settitle(widget);
	drawitems(widget);
	commitdraw(widget);
	return RETURN_SUCCESS;
error:
	cleanwidget(widget);
	return RETURN_FAILURE;
}

void
mapwidget(Widget *widget)
{
	XMapWindow(widget->display, widget->window);
}

WidgetEvent
pollwidget(Widget *widget, int *selitems, int *nitems, Scroll *scrl, char **text)
{
	XEvent ev;
	int retval;

	*text = NULL;
	widget->droptarget.buffer = NULL;
	while (widget->start && XPending(widget->display) > 0) {
		(void)XNextEvent(widget->display, &ev);
		if (processevent(widget, &ev) == WIDGET_CLOSE) {
			endevent(widget);
			return WIDGET_CLOSE;
		}
	}
	widget->start = TRUE;
	if ((retval = checklastprop(widget, text)) == WIDGET_NONE)
		retval = mainmode(widget, selitems, nitems, text);
	endevent(widget);
	scrl->ydiff = widget->ydiff;
	scrl->row = widget->row;
	return retval;
}

void
closewidget(Widget *widget)
{
	int i, j;

	ctrlsel_dndclose(&widget->dropctx);
	cleanwidget(widget);
	for (i = 0; i < widget->nicons; i++) {
		if (widget->icons[i].pix != None) {
			XFreePixmap(widget->display, widget->icons[i].pix);
		}
		if (widget->icons[i].mask != None) {
			XFreePixmap(widget->display, widget->icons[i].mask);
		}
	}
	FREE(widget->icons);
	if (widget->draw != NULL)
		XftDrawDestroy(widget->draw);
	if (widget->pix != None)
		XFreePixmap(widget->display, widget->pix);
	if (widget->rectbord != None)
		XFreePixmap(widget->display, widget->rectbord);
	if (widget->namepix != None)
		XFreePixmap(widget->display, widget->namepix);
	if (widget->stipple != None)
		XFreePixmap(widget->display, widget->stipple);
	for (i = 0; i < SELECT_LAST; i++)
		for (j = 0; j < COLOR_LAST; j++)
			XftColorFree(widget->display, widget->visual, widget->colormap, &widget->colors[i][j]);
	XftFontClose(widget->display, widget->font);
	XDestroyWindow(widget->display, widget->scroller);
	XDestroyWindow(widget->display, widget->window);
	XFreeGC(widget->display, widget->stipgc);
	XFreeGC(widget->display, widget->gc);
	XCloseDisplay(widget->display);
	FREE(widget);
}

int
widopenicons(Widget *widget, char **xpms[], int nxpms)
{
	int retval, i;

	widget->nicons = nxpms;
	if ((widget->icons = calloc(widget->nicons, sizeof(*widget->icons))) == NULL) {
		warn("calloc");
		return RETURN_FAILURE;
	}
	retval = RETURN_SUCCESS;
	for (i = 0; i < widget->nicons; i++) {
		if (pixmapfromdata(widget, xpms[i], &widget->icons[i].pix, &widget->icons[i].mask) == RETURN_FAILURE) {
			warnx("could not open %d-th default icon pixmap", i);
			retval = RETURN_FAILURE;
		}
	}
	return retval;
}

void
setthumbnail(Widget *widget, char *path, int item)
{
	FILE *fp;
	size_t size, i;
	int w, h;
	char buf[DATA_DEPTH];
	unsigned char *data;

	if (item < 0 || item >= widget->nitems || widget->thumbs == NULL)
		return;
	data = NULL;
	widget->thumbs[item] = NULL;
	if ((fp = fopen(path, "rb")) == NULL) {
		warn("%s", path);
		goto error;
	}
	if (checkheader(fp, PPM_HEADER, PPM_HEADER_SIZE) == -1) {
		warnx("%s: not a ppm file", path);
		goto error;
	}
	w = readsize(fp);
	h = readsize(fp);
	if (w <= 0 || w > THUMBSIZE || h <= 0 || h > THUMBSIZE) {
		warnx("%s: ppm file with invalid header", path);
		goto error;
	}
	if (checkheader(fp, PPM_COLOR, PPM_COLOR_SIZE) == -1) {
		warnx("%s: ppm file with invalid header", path);
		goto error;
	}
	size = w * h;
	if ((data = malloc(size * DATA_DEPTH)) == NULL) {
		warn("malloc");
		goto error;
	}
	for (i = 0; i < size; i++) {
		if (fread(buf, 1, PPM_DEPTH, fp) != PPM_DEPTH) {
			warn("%s", path);
			goto error;
		}
		data[i * DATA_DEPTH + 0] = buf[2];   /* B */
		data[i * DATA_DEPTH + 1] = buf[1];   /* G */
		data[i * DATA_DEPTH + 2] = buf[0];   /* R */
		data[i * DATA_DEPTH + 3] = 0xFF;     /* A */
	}
	fclose(fp);
	fp = NULL;
	if ((widget->thumbs[item] = malloc(sizeof(*widget->thumbs[item]))) == NULL) {
		warn("malloc");
		goto error;
	}
	*widget->thumbs[item] = (struct Thumb){
		.w = w,
		.h = h,
		.img = NULL,
		.next = widget->thumbhead,
	};
	widget->thumbs[item]->img = XCreateImage(
		widget->display,
		widget->visual,
		widget->depth,
		ZPixmap,
		0, (char *)data,
		w, h,
		DATA_DEPTH * BYTE,
		0
	);
	if (widget->thumbs[item]->img == NULL) {
		warnx("%s: could not allocate XImage", path);
		goto error;
	}
	XInitImage(widget->thumbs[item]->img);
	widget->thumbhead = widget->thumbs[item];
	if (item >= widget->row * widget->ncols && item < widget->row * widget->ncols + widget->nrows * widget->ncols) {
		drawitem(widget, item);
		commitdraw(widget);
	}
	return;
error:
	if (fp != NULL)
		fclose(fp);
	FREE(data);
	FREE(widget->thumbs[item]);
}

void
widget_busy(Widget *widget)
{
	XDefineCursor(widget->display, widget->window, widget->busycursor);
	XFlush(widget->display);
}

unsigned long
widgetwinid(Widget *widget)
{
	return (unsigned long)widget->window;
}
