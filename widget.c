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
#include <X11/extensions/sync.h>

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

#define ALARMFLAGS      (XSyncCACounter | XSyncCAValue | XSyncCAValueType | XSyncCATestType | XSyncCADelta)
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
	Display *dpy;
	Atom atoms[ATOM_LAST];
	GC stipgc, gc;
	Cursor busycursor;
	Window win;
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
	 *
	 * We use the XSync extension for the scroller.
	 * See https://nrk.neocities.org/articles/x11-timeout-with-xsyncalarm.html
	 */
	Window scroller;                /* the scroller popup window */
	int handlew;                    /* size of scroller handle */
	XSyncAlarmAttributes syncattr;
	int syncevent;

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
createwin(Widget wid, const char *class, const char *name, const char *geom, int argc, char *argv[], unsigned long *icon, size_t iconsize)
{
	XftDraw *draw;
	Pixmap bg;
	unsigned int dw, dh;
	int x, y;
	int dx, dy;
	int flags, sizehints;
	pid_t pid;

	x = y = 0;
	wid->w = DEF_WIDTH;
	wid->h = DEF_HEIGHT;
	sizehints = 0;
	pid = getpid();
	if (geom != NULL) {
		flags = XParseGeometry(geom, &dx, &dy, &dw, &dh);
		dw = max(MIN_WIDTH, dw);
		dh = max(MIN_HEIGHT, dh);
		if (FLAG(flags, WidthValue)) {
			wid->w = dw;
			sizehints |= USSize;
		}
		if (FLAG(flags, HeightValue)) {
			wid->h = dh;
			sizehints |= USSize;
		}
		if (FLAG(flags, XValue | XNegative)) {
			x = WIDTH(wid->dpy) - wid->w - (dx > 0 ? dx : 0);
			sizehints |= USPosition;
		} else if (FLAG(flags, XValue)) {
			x = dx;
			sizehints |= USPosition;
		}
		if (FLAG(flags, YValue | YNegative)) {
			y = HEIGHT(wid->dpy) - wid->h - (dy > 0 ? dy : 0);
			sizehints |= USPosition;
		} else if (FLAG(flags, XValue)) {
			y = dy;
			sizehints |= USPosition;
		}
	}
	wid->win = XCreateWindow(
		wid->dpy, ROOT(wid->dpy),
		x, y, wid->w, wid->h, 0,
		wid->depth, InputOutput, wid->visual,
		CWBackPixel | CWEventMask | CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.colormap = wid->colormap,
			.background_pixel = 0,
			.event_mask = StructureNotifyMask | ExposureMask
			            | KeyPressMask | PointerMotionMask
			            | ButtonReleaseMask | ButtonPressMask
			            | PropertyChangeMask,
		}
	);
	if (wid->win == None)
		return RETURN_FAILURE;
	bg = XCreatePixmap(
		wid->dpy,
		wid->win,
		THUMBSIZE,
		THUMBSIZE,
		wid->depth
	);
	if (bg == None)
		return RETURN_FAILURE;
	draw = XftDrawCreate(wid->dpy, bg, wid->visual, wid->colormap);
	XftDrawRect(draw, &wid->colors[SELECT_NOT][COLOR_BG], 0, 0, THUMBSIZE, THUMBSIZE);
	XftDrawDestroy(draw);
	XSetWindowBackgroundPixmap(wid->dpy, wid->win, bg);
	XFreePixmap(wid->dpy, bg);
	wid->namepix = XCreatePixmap(
		wid->dpy,
		wid->win,
		LABELWIDTH,
		wid->fonth,
		wid->depth
	);
	if (wid->namepix == None)
		return RETURN_FAILURE;
	XmbSetWMProperties(
		wid->dpy, wid->win,
		class, class,
		argv, argc,
		&(XSizeHints){ .flags = sizehints, },
		NULL,
		&(XClassHint){ .res_class = (char *)class, .res_name = (char *)name, }
	);
	XSetWMProtocols(wid->dpy, wid->win, &wid->atoms[WM_DELETE_WINDOW], 1);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING], 8, PropModeReplace,
		(unsigned char *)class,
		strlen(class) + 1
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *)&wid->atoms[_NET_WM_WINDOW_TYPE_NORMAL],
		1
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_ICON],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)icon, iconsize
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_PID],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&pid,
		1
	);
	return RETURN_SUCCESS;
}

static int
ealloccolor(Widget wid, const char *s, XftColor *color, unsigned short alpha)
{
	XColor screen, exact;

	if (!XAllocNamedColor(wid->dpy, wid->colormap, s, &screen, &exact))
		return RETURN_FAILURE;
	color->pixel = screen.pixel;
	color->color.red = exact.red;
	color->color.green = exact.green;
	color->color.blue = exact.blue;
	color->color.alpha = alpha;
	return RETURN_SUCCESS;
}

static int
eallocfont(Display *dpy, const char *s, XftFont **font)
{
	if ((*font = XftFontOpenXlfd(dpy, DefaultScreen(dpy), s)) == NULL)
		if ((*font = XftFontOpenName(dpy, DefaultScreen(dpy), s)) == NULL)
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
textwidth(Widget wid, const char *text, int len)
{
	XGlyphInfo box;

	XftTextExtentsUtf8(wid->dpy, wid->font, (const FcChar8 *)text, len, &box);
	return box.width;
}

static int
inittheme(Widget wid, const char *class, const char *name)
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
	if ((xrm = XResourceManagerString(wid->dpy)) != NULL)
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
			} else if (ealloccolor(wid, s, &wid->colors[i][j], alpha) == RETURN_FAILURE) {
				/* resource found, but allocation failed; use default value */
				warnx("\"%s\": could not load color (falling back to \"%s\")", s, defvalue[i][j]);
				s = defvalue[i][j];
			} else {
				/* resource found and successfully allocated */
				continue;
			}
			if (ealloccolor(wid, s, &wid->colors[i][j], alpha) == RETURN_FAILURE) {
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
	} else if (eallocfont(wid->dpy, s, &wid->font) == RETURN_FAILURE) {
		/* resource found, but allocation failed; use default value */
		warnx("\"%s\": could not open font (falling back to \"%s\")", s, DEF_FONT);
		s = DEF_FONT;
	} else {
		goto done;
	}
	if (eallocfont(wid->dpy, s, &wid->font) == RETURN_FAILURE) {
		warnx("\"%s\": could not open font", s);
		fonterror = TRUE;
		goterror = TRUE;
	}
done:
	if (goterror)
		goto error;
	wid->fonth = wid->font->height;
	wid->itemw = ITEM_WIDTH;
	wid->itemh = THUMBSIZE + (NLINES + 1) * wid->fonth;
	wid->ellipsisw = textwidth(wid, ELLIPSIS, strlen(ELLIPSIS));
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RETURN_SUCCESS;
error:
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			if (colorerror[i][j])
				continue;
			XftColorFree(wid->dpy, wid->visual, wid->colormap, &wid->colors[i][j]);
		}
	}
	if (!fonterror)
		XftFontClose(wid->dpy, wid->font);
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RETURN_FAILURE;
}

static int
calcsize(Widget wid, int w, int h)
{
	int ncols, nrows, ret;
	double d;

	ret = FALSE;
	if (wid->w == w && wid->h == h)
		return FALSE;
	etlock(&wid->lock);
	ncols = wid->ncols;
	nrows = wid->nrows;
	if (w > 0 && h > 0) {
		wid->w = w;
		wid->h = h;
		wid->ydiff = 0;
	}
	wid->ncols = max(wid->w / wid->itemw, 1);
	wid->nrows = max(WIN_ROWS(wid) + (wid->h % wid->itemh ? 2 : 1), 1);
	wid->x0 = max((wid->w - wid->ncols * wid->itemw) / 2, 0);
	wid->nscreens = ALL_ROWS(wid) - WIN_ROWS(wid);
	wid->nscreens = max(wid->nscreens, 1);
	d = (double)wid->nscreens / SCROLLER_MIN;
	d = (d < 1.0 ? 1.0 : d);
	wid->handlew = max(SCROLLER_SIZE / d - 2, 1);
	wid->handlew = min(wid->handlew, HANDLE_MAX_SIZE);
	if (wid->handlew == HANDLE_MAX_SIZE && ALL_ROWS(wid) > WIN_ROWS(wid))
		wid->handlew = HANDLE_MAX_SIZE - 1;
	if (ncols != wid->ncols || nrows != wid->nrows) {
		if (wid->pix != None)
			XFreePixmap(wid->dpy, wid->pix);
		if (wid->draw != None)
			XftDrawDestroy(wid->draw);
		wid->pixw = wid->ncols * wid->itemw;
		wid->pixh = wid->nrows * wid->itemh;
		wid->pix = XCreatePixmap(wid->dpy, wid->win, wid->pixw, wid->pixh, wid->depth);
		wid->draw = XftDrawCreate(wid->dpy, wid->pix, wid->visual, wid->colormap);
		ret = TRUE;
	}
	if (wid->w > wid->rectw || wid->h > wid->recth) {
		wid->rectw = max(wid->rectw, wid->w);
		wid->recth = max(wid->recth, wid->h);
		if (wid->rectbord != None)
			XFreePixmap(wid->dpy, wid->rectbord);
		wid->rectbord = XCreatePixmap(wid->dpy, wid->win, wid->w, wid->h, CLIP_DEPTH);
	}
	etunlock(&wid->lock);
	return ret;
}

static int
isbreakable(char c)
{
	return c == '.' || c == '-' || c == '_';
}

static void
drawtext(Widget wid, Drawable pix, XftColor *color, int x, const char *text, int len)
{
	XftDraw *draw;
	int w;

	w = textwidth(wid, text, len);
	draw = XftDrawCreate(wid->dpy, pix, wid->visual, wid->colormap);
	XftDrawRect(draw, &wid->colors[SELECT_NOT][COLOR_BG], 0, 0, LABELWIDTH, wid->fonth);
	XftDrawRect(draw, &color[COLOR_BG], x, 0, w, wid->fonth);
	XftDrawStringUtf8(draw, &color[COLOR_FG], wid->font, x, wid->font->ascent, (const FcChar8 *)text, len);
	XftDrawDestroy(draw);
}

static void
setrow(Widget wid, int row)
{
	etlock(&wid->lock);
	wid->row = row;
	etunlock(&wid->lock);
}

static void
drawicon(Widget wid, int index, int x, int y)
{
	XGCValues val;
	Pixmap pix, mask;
	int icon;

	icon = wid->itemicons[index];
	pix = wid->icons[icon].pix;
	mask = wid->icons[icon].mask;
	if (wid->thumbs != NULL && wid->thumbs[index] != NULL) {
		/* draw thumbnail */
		XPutImage(
			wid->dpy,
			wid->pix,
			wid->gc,
			wid->thumbs[index]->img,
			0, 0,
			x + (wid->itemw - wid->thumbs[index]->w) / 2,
			y + (THUMBSIZE - wid->thumbs[index]->h) / 2,
			wid->thumbs[index]->w,
			wid->thumbs[index]->h
		);
		if (wid->issel[index]) {
			XSetFillStyle(wid->dpy, wid->gc, FillStippled);
			XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_YES][COLOR_BG].pixel);
			XFillRectangle(
				wid->dpy,
				wid->pix,
				wid->gc,
				x + (wid->itemw - wid->thumbs[index]->w) / 2,
				y + (THUMBSIZE - wid->thumbs[index]->h) / 2,
				wid->thumbs[index]->w,
				wid->thumbs[index]->h
			);
			XSetFillStyle(wid->dpy, wid->gc, FillSolid);
		}
	} else {
		/* draw icon */
		val.clip_x_origin = x + (wid->itemw - THUMBSIZE) / 2;
		val.clip_y_origin = y;
		val.clip_mask = mask;
		XChangeGC(wid->dpy, wid->gc, GCClipXOrigin | GCClipYOrigin | GCClipMask, &val);
		XCopyArea(
			wid->dpy,
			pix, wid->pix,
			wid->gc,
			0, 0,
			THUMBSIZE, THUMBSIZE,
			val.clip_x_origin,
			y
		);
		if (wid->issel[index]) {
			XSetFillStyle(wid->dpy, wid->gc, FillStippled);
			XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_YES][COLOR_BG].pixel);
			XFillRectangle(
				wid->dpy,
				wid->pix,
				wid->gc,
				val.clip_x_origin, y,
				THUMBSIZE, THUMBSIZE
			);
			XSetFillStyle(wid->dpy, wid->gc, FillSolid);
		}
		val.clip_mask = None;
		XChangeGC(wid->dpy, wid->gc, GCClipMask, &val);
	}
}

static void
drawlabel(Widget wid, int index, int x, int y)
{
	XftColor *color;
	int i;
	int textx, maxw;
	int textw, w, textlen, len;
	int extensionw, extensionlen;
	char *text, *extension;

	color = wid->colors[(wid->issel != NULL && wid->issel[index]) ? SELECT_YES : SELECT_NOT];
	text = wid->items[index][ITEM_NAME];
	wid->nlines[index] = 1;
	textx = x + wid->itemw / 2 - LABELWIDTH / 2;
	extension = NULL;
	maxw = 0;
	textlen = 0;
	wid->linelen[index] = 0;
	for (i = 0; i < wid->nlines[index]; i++) {
		while (isspace(text[textlen]))
			textlen++;
		text += textlen;
		textlen = strlen(text);
		textw = textwidth(wid, text, textlen);
		if (wid->nlines[index] < NLINES && textw >= LABELWIDTH) {
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
				w = textwidth(wid, text, len);
				if (text[len] == '\0') {
					break;
				}
			}
			if (textw > 0) {
				wid->nlines[index] = min(wid->nlines[index] + 1, NLINES);
			} else {
				textlen = len;
				textw = w;
			}
		}
		textw = min(LABELWIDTH, textw);
		maxw = max(textw, maxw);
		drawtext(
			wid,
			wid->namepix, color,
			max(LABELWIDTH / 2 - textw / 2, 0),
			text, textlen
		);
		textw = min(textw, LABELWIDTH);
		wid->linelen[index] = max(wid->linelen[index], textw);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			LABELWIDTH, wid->fonth,
			textx, y + wid->itemh - (NLINES - i + 0.5) * wid->fonth
		);
	}
	if (index == wid->highlight) {
		XSetForeground(wid->dpy, wid->gc, color[COLOR_FG].pixel);
		XDrawRectangle(
			wid->dpy,
			wid->pix,
			wid->gc,
			x + wid->itemw / 2 - maxw / 2 - 1,
			y + wid->itemh - (NLINES + 0.5) * wid->fonth - 1,
			maxw + 1, i * wid->fonth + 1
		);
	}
	if (textw >= LABELWIDTH &&
	    (extension = strrchr(text, '.')) != NULL &&
	    extension[1] != '\0') {
		extensionlen = strlen(extension);
		extensionw = textwidth(wid, extension, extensionlen);
	}
	if (extension != NULL) {
		/* draw ellipsis */
		drawtext(
			wid,
			wid->namepix, color,
			0,
			ELLIPSIS, strlen(ELLIPSIS)
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			wid->ellipsisw, wid->fonth,
			textx + textw - extensionw - wid->ellipsisw,
			y + wid->itemh - (NLINES + 1 - wid->nlines[index] + 0.5) * wid->fonth
		);

		/* draw extension */
		drawtext(
			wid,
			wid->namepix, color,
			0, extension, extensionlen
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			extensionw, wid->fonth,
			textx + textw - extensionw,
			y + wid->itemh - (NLINES + 1 - wid->nlines[index] + 0.5) * wid->fonth
		);
	}
}

static int
firstvisible(Widget wid)
{
	/* gets index of last visible item */
	return wid->row * wid->ncols;
}

static int
lastvisible(Widget wid)
{
	/* gets index of last visible item */
	return min(wid->nitems, firstvisible(wid) + wid->nrows * wid->ncols) - 1;
}

static void
drawitem(Widget wid, int index)
{
	int i, x, y, min, max;

	etlock(&wid->lock);
	min = firstvisible(wid);
	max = lastvisible(wid);
	if (index < min || index > max)
		goto done;
	i = index - min;
	x = i % wid->ncols;
	y = (i / wid->ncols) % wid->nrows;
	x *= wid->itemw;
	y *= wid->itemh;
	XftDrawRect(wid->draw, &wid->colors[SELECT_NOT][COLOR_BG], x, y, wid->itemw, wid->itemh);
	drawicon(wid, index, x, y);
	drawlabel(wid, index, x, y);
done:
	etunlock(&wid->lock);
	wid->redraw = TRUE;
}

static void
drawitems(Widget wid)
{
	int i, n;

	XftDrawRect(wid->draw, &wid->colors[SELECT_NOT][COLOR_BG], 0, 0, wid->w, wid->nrows * wid->itemh);
	n = lastvisible(wid);
	for (i = wid->row * wid->ncols; i <= n; i++) {
		drawitem(wid, i);
	}
}

static void
commitdraw(Widget wid)
{
	etlock(&wid->lock);
	XClearWindow(wid->dpy, wid->win);
	XCopyArea(
		wid->dpy,
		wid->pix, wid->win,
		wid->gc,
		0, wid->ydiff - MARGIN,
		wid->pixw, wid->pixh,
		wid->x0, 0
	);
	if (wid->state != STATE_SELECTING)
		goto done;
	XChangeGC(
		wid->dpy,
		wid->gc,
		GCClipXOrigin | GCClipYOrigin | GCClipMask,
		&(XGCValues) {
			.clip_x_origin = 0,
			.clip_y_origin = 0,
			.clip_mask = wid->rectbord,
		}
	);
	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_FG].pixel);
	XFillRectangle(wid->dpy, wid->win, wid->gc, 0, 0, wid->w, wid->h);
	XChangeGC(
		wid->dpy,
		wid->gc,
		GCClipMask,
		&(XGCValues) {
			.clip_mask = None,
		}
	);
	XFlush(wid->dpy);
done:
	etunlock(&wid->lock);
}

static void
settitle(Widget wid)
{
	char title[TITLE_BUFSIZE];
	char nitems[STATUS_BUFSIZE];
	char *selitem, *status;
	int scrollpct;                  /* scroll percentage */

	if (wid->row == 0 && wid->nscreens > 1)
		scrollpct = 0;
	else
		scrollpct = 100 * ((double)(wid->row + 1) / wid->nscreens);
	(void)snprintf(nitems, STATUS_BUFSIZE, "%d items", wid->nitems - 1);
	selitem = "";
	status = nitems;
	selitem = (wid->highlight > 0 ? wid->items[wid->highlight][ITEM_NAME] : "");
	if (wid->highlight <= 0)
		status = nitems;
	else if (wid->items[wid->highlight][ITEM_STATUS] == NULL)
		status = STATUS_UNKNOWN;
	else
		status = wid->items[wid->highlight][ITEM_STATUS];
	if (wid->title != NULL) {
		(void)snprintf(
			title, TITLE_BUFSIZE,
			"%s%s%s (%s) - %s (%d%%)",
			wid->title,
			(strcmp(wid->title, "/") != 0 ? "/" : ""),
			selitem,
			status,
			wid->class,
			scrollpct
		);
	}
	XmbSetWMProperties(wid->dpy, wid->win, title, title, NULL, 0, NULL, NULL, NULL);
	XChangeProperty(
		wid->dpy,
		wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)title,
		strlen(title)
	);
	XChangeProperty(
		wid->dpy,
		wid->win,
		wid->atoms[_CONTROL_CWD],
		wid->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)wid->title,
		strlen(title)
	);
}

static int
gethandlepos(Widget wid)
{
	int row;

	if (wid->ydiff >= wid->itemh)
		row = wid->nscreens;
	else
		row = wid->row;
	return (HANDLE_MAX_SIZE - wid->handlew) * ((double)row / wid->nscreens);
}

static void
drawscroller(Widget wid, int y)
{
	XftDraw *draw;
	XftColor color;
	Pixmap pix;

	if ((pix = XCreatePixmap(wid->dpy, wid->scroller, SCROLLER_SIZE, SCROLLER_SIZE, wid->depth)) == None)
		return;
	draw = XftDrawCreate(wid->dpy, pix, wid->visual, wid->colormap);
	color = wid->colors[SELECT_NOT][COLOR_BG];
	color.color.alpha = 0xFFFF;
	XftDrawRect(draw, &color, 0, 0, SCROLLER_SIZE, SCROLLER_SIZE);
	XftDrawRect(draw, &wid->colors[SELECT_NOT][COLOR_FG], 1, y + 1, HANDLE_MAX_SIZE, wid->handlew);
	XSetWindowBackgroundPixmap(wid->dpy, wid->scroller, pix);
	XClearWindow(wid->dpy, wid->scroller);
	XFreePixmap(wid->dpy, pix);
	XftDrawDestroy(draw);
}

static int
scroll(Widget wid, int y)
{
	int prevhand, newhand;          /* position of the scroller handle */
	int prevrow, newrow;

	if (y == 0)
		return FALSE;
	if (ALL_ROWS(wid) + 1 < wid->nrows)
		return FALSE;
	prevhand = gethandlepos(wid);
	newrow = prevrow = wid->row;
	wid->ydiff += y;
	newrow += wid->ydiff / wid->itemh;
	wid->ydiff %= wid->itemh;
	if (wid->ydiff < 0) {
		wid->ydiff += wid->itemh;
		newrow--;
	}
	if (y > 0) {
		if (newrow >= wid->nscreens) {
			wid->ydiff = wid->itemh;
			newrow = wid->nscreens - 1;
		}
	} else if (y < 0) {
		if (newrow < 0) {
			wid->ydiff = 0;
			newrow = 0;
		}
	}
	setrow(wid, newrow);
	newhand = gethandlepos(wid);
	if (wid->state == STATE_SCROLLING && prevhand != newhand) {
		drawscroller(wid, newhand);
	}
	if (prevrow != newrow) {
		settitle(wid);
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
getitem(Widget wid, int row, int ydiff, int *x, int *y)
{
	int i, w, h;

	*y -= MARGIN;
	*y += ydiff;
	*x -= wid->x0;
	if (*x < 0 || *x >= wid->ncols * wid->itemw)
		return -1;
	if (*y < 0 || *y >= wid->h + ydiff)
		return -1;
	w = *x / wid->itemw;
	h = *y / wid->itemh;
	row *= wid->ncols;
	i = row + h * wid->ncols + w;
	if (i < row)
		return -1;
	*x -= w * wid->itemw;
	*y -= h * wid->itemh;
	return i;
}

static int
getpointerclick(Widget wid, int x, int y)
{
	int iconx, textx, texty, i;

	if ((i = getitem(wid, wid->row, wid->ydiff, &x, &y)) < 0)
		return -1;
	if (i < 0 || i >= wid->nitems)
		return -1;
	iconx = (wid->itemw - THUMBSIZE) / 2;
	if (x >= iconx && x < iconx + THUMBSIZE && y >= 0 && y < THUMBSIZE + wid->fonth / 2)
		return i;
	if (wid->linelen == NULL)
		return -1;
	textx = (wid->itemw - wid->linelen[i]) / 2;
	texty = wid->itemh - (NLINES + 0.5) * wid->fonth;
	if (x >= textx && x < textx + wid->linelen[i] &&
	    y >= texty && y < texty + wid->nlines[i] * wid->fonth) {
		return i;
	}
	return -1;
}

static void
disownprimary(Widget wid)
{
	if (wid->selctx == NULL)
		return;
	ctrlsel_disown(wid->selctx);
	FREE(wid->selctx);
}

static void
disowndnd(Widget wid)
{
	if (wid->dragctx == NULL)
		return;
	ctrlsel_dnddisown(wid->dragctx);
	FREE(wid->dragctx);
}

static void
ownprimary(Widget wid, Time time)
{
	struct Selection *sel;
	size_t i, j;
	int success;

	if (wid->sel == NULL)
		return;
	disownprimary(wid);
	if ((wid->selctx = malloc(sizeof(*wid->selctx))) == NULL) {
		warn("malloc");
		return;
	}
	i = j = 0;
	for (sel = wid->sel; sel != NULL; sel = sel->next) {
		if (sel->next != NULL)
			i += snprintf(wid->selbuf + i, wid->selbufsiz - i, "%s\n", wid->items[sel->index][ITEM_PATH]);
		j += snprintf(wid->uribuf + j, wid->uribufsiz - j, "file://%s\r\n", wid->items[sel->index][ITEM_PATH]);
	}
	ctrlsel_filltarget(
		XA_STRING, XA_STRING,
		8, (unsigned char *)wid->selbuf, i,
		&wid->targets[TARGET_STRING]
	);
	ctrlsel_filltarget(
		wid->atoms[UTF8_STRING], wid->atoms[UTF8_STRING],
		8, (unsigned char *)wid->selbuf, i,
		&wid->targets[TARGET_UTF8]
	);
	ctrlsel_filltarget(
		wid->atoms[TEXT_URI_LIST], wid->atoms[TEXT_URI_LIST],
		8, (unsigned char *)wid->uribuf, j,
		&wid->targets[TARGET_URI]
	);
	success = ctrlsel_setowner(
		wid->dpy, wid->win,
		XA_PRIMARY, time, 0,
		wid->targets, TARGET_LAST,
		wid->selctx
	);
	if (!success) {
		FREE(wid->selctx);
	}
}

static void
cleanwidget(Widget wid)
{
	struct Thumb *thumb;
	struct Selection *sel;
	void *tmp;

	thumb = wid->thumbhead;
	while (thumb != NULL) {
		tmp = thumb;
		thumb = thumb->next;
		XDestroyImage(((struct Thumb *)tmp)->img);
		FREE(tmp);
	}
	sel = wid->sel;
	while (sel != NULL) {
		tmp = sel;
		sel = sel->next;
		FREE(tmp);
	}
	wid->sel = NULL;
	wid->rectsel = NULL;
	FREE(wid->thumbs);
	FREE(wid->linelen);
	FREE(wid->nlines);
	FREE(wid->issel);
	FREE(wid->selbuf);
	wid->selbufsiz = 0;
	FREE(wid->uribuf);
	wid->uribufsiz = 0;
	FREE(wid->dndbuf);
	disownprimary(wid);
	disowndnd(wid);
}

static void
selectitem(Widget wid, int index, int select, int rectsel)
{
	struct Selection *sel;
	struct Selection **header;

	if (wid->issel == NULL || index <= 0 || index >= wid->nitems)
		return;
	/*
	 * We have two lists of selections: the global list (wid->sel),
	 * and the list used by rectangular selection (wid->rectsel).
	 */
	header = rectsel ? &wid->rectsel : &wid->sel;
	if (select && wid->issel[index] == NULL) {
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
		wid->issel[index] = sel;
	} else if (!select && wid->issel[index] != NULL) {
		sel = wid->issel[index];
		if (sel->next != NULL)
			sel->next->prev = sel->prev;
		if (sel->prev != NULL)
			sel->prev->next = sel->next;
		if (*header == sel)
			*header = sel->next;
		FREE(sel);
		wid->issel[index] = NULL;
	} else {
		return;
	}
	drawitem(wid, index);
}

static void
highlight(Widget wid, int index, int redraw)
{
	int prevhili;

	if (wid->highlight == index)
		return;
	prevhili = wid->highlight;
	wid->highlight = index;
	if (redraw)
		drawitem(wid, index);
	/* we still have to redraw the previous one */
	drawitem(wid, prevhili);
	settitle(wid);
}

static void
selectitems(Widget wid, int a, int b)
{
	int i, min, max;

	if (a < 0 || b < 0 || a >= wid->nitems || b >= wid->nitems)
		return;
	if (a < b) {
		min = a;
		max = b;
	} else {
		min = b;
		max = a;
	}
	for (i = min; i <= max; i++) {
		selectitem(wid, i, TRUE, 0);
	}
}

static void
unselectitems(Widget wid)
{
	while (wid->sel) {
		selectitem(wid, wid->sel->index, FALSE, 0);
	}
}

static int
mouse1click(Widget wid, XButtonPressedEvent *ev)
{
	int prevhili, index;

	index = getpointerclick(wid, ev->x, ev->y);
	if (index > 0 && wid->issel[index] != NULL)
		return index;
	if (!(ev->state & (ControlMask | ShiftMask)))
		unselectitems(wid);
	if (index < 0)
		return index;
	/*
	 * If index != 0, there's no need to ask highlight() to redraw the item,
	 * as selectitem() or selectitems() will already redraw it.
	 */
	prevhili = wid->highlight;
	highlight(wid, index, (index == 0));
	if (prevhili != -1 && ev->state & ShiftMask)
		selectitems(wid, wid->highlight, prevhili);
	else
		selectitem(wid, wid->highlight, ((ev->state & ControlMask) ? wid->issel[wid->highlight] == NULL : TRUE), FALSE);
	ownprimary(wid, ev->time);
	return index;
}

static void
mouse3click(Widget wid, int x, int y)
{
	int index;

	index = getpointerclick(wid, x, y);
	if (index != -1) {
		if (wid->issel[index] == NULL) {
			highlight(wid, index, FALSE);
			unselectitems(wid);
			selectitem(wid, index, TRUE, FALSE);
		} else {
			highlight(wid, index, TRUE);
		}
	}
}

static void
rectdraw(Widget wid, int row, int ydiff, int x0, int y0, int x, int y)
{
	int w, h;

	XSetForeground(wid->dpy, wid->stipgc, 0);
	XFillRectangle(
		wid->dpy,
		wid->rectbord,
		wid->stipgc,
		0, 0,
		wid->w, wid->h
	);
	if (wid->state != STATE_SELECTING)
		return;
	if (row < wid->row) {
		y0 -= min(wid->row - row, wid->nrows) * wid->itemh;
	} else if (row > wid->row) {
		y0 += min(row - wid->row, wid->nrows) * wid->itemh;
	}
	y0 += ydiff - wid->ydiff;
	w = (x0 > x) ? x0 - x : x - x0;
	h = (y0 > y) ? y0 - y : y - y0;
	XSetForeground(wid->dpy, wid->stipgc, 1);
	XDrawRectangle(
		wid->dpy,
		wid->rectbord,
		wid->stipgc,
		min(x0, x),
		min(y0, y),
		w, h
	);
}

static int
rectselect(Widget wid, int srcrow, int srcydiff, int srcx, int srcy, int dstx, int dsty)
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
	if (dstx < wid->x0)              dstx = wid->x0;
	if (srcx < wid->x0)              srcx = wid->x0;
	if (dstx >= wid->x0 + wid->pixw) dstx = wid->x0 + wid->pixw - 1;
	if (srcx >= wid->x0 + wid->pixw) srcx = wid->x0 + wid->pixw - 1;
	if (dsty < MARGIN)               dsty = MARGIN;
	if (srcy < MARGIN)               srcy = MARGIN;
	if (dsty >= wid->h)              dsty = wid->h - 1;
	if (srcy >= wid->h)              srcy = wid->h - 1;
	if ((srci = getitem(wid, srcrow, srcydiff, &srcx, &srcy)) < 0)
		return FALSE;
	if ((dsti = getitem(wid, wid->row, wid->ydiff, &dstx, &dsty)) < 0)
		return FALSE;
	vismin = firstvisible(wid);
	vismax = lastvisible(wid);
	indexmin = min(srci, dsti);
	indexmax = max(srci, dsti);
	colmin = indexmin % wid->ncols;
	colmax = indexmax % wid->ncols;
	indexmin = min(indexmin, wid->nitems - 1);
	indexmax = min(indexmax, wid->nitems - 1);
	rowmin = indexmin / wid->ncols;
	rowsrc = srci / wid->ncols;
	changed = FALSE;
	for (i = vismin; i <= vismax; i++) {
		sel = TRUE;
		row = i / wid->ncols;
		col = i % wid->ncols;
		x = wid->x0 + col * wid->itemw + (wid->itemw - THUMBSIZE) / 2;
		y = (row - wid->row + 1) * wid->itemh -
		    (NLINES - wid->nlines[i] + 0.5) * wid->fonth +
		    MARGIN - wid->ydiff;
		if (i < indexmin || i > indexmax) {
			sel = FALSE;
		} else if ((col == colmin || col == colmax) && (minx > x + THUMBSIZE || maxx < x)) {
			sel = FALSE;
		} else if (col < colmin || col > colmax) {
			sel = FALSE;
		} else if (row == rowmin && row != rowsrc && row >= wid->row && miny > y) {
			sel = FALSE;
		}
		if (!sel && (wid->issel[i] == NULL || wid->issel[i]->index > 0))
			continue;
		if (sel && wid->issel[i] != NULL && wid->issel[i]->index > 0)
			selectitem(wid, i, FALSE, FALSE);
		selectitem(wid, i, sel, TRUE);
		changed = TRUE;
	}
	return changed;
}

static void
commitrectsel(Widget wid)
{
	struct Selection *sel, *next;

	/*
	 * We keep the items selected by rectangular selection on a
	 * temporary list.  Move them to the regular list.
	 */
	while (wid->rectsel != NULL) {
		next = wid->rectsel->next;
		sel = wid->rectsel;
		sel->next = wid->sel;
		sel->prev = NULL;
		if (sel->index < 0)
			sel->index *= -1;
		if (wid->sel != NULL)
			wid->sel->prev = sel;
		wid->sel = sel;
		wid->rectsel = next;
	}
}

static void
endevent(Widget wid)
{
	if (wid->redraw) {
		commitdraw(wid);
	}
}

static int
querypointer(Widget wid, Window win, int *retx, int *rety, unsigned int *retmask)
{
	Window root, child;
	unsigned int mask;
	int rootx, rooty;
	int x, y;
	int retval;

	retval = XQueryPointer(
		wid->dpy,
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

static int
scrollerpos(Widget wid)
{
	int x, y;

	if (querypointer(wid, wid->scroller, &x, &y, NULL) == True) {
		if (y > SCROLLER_SIZE)
			return y - SCROLLER_SIZE;
		if (y < 0)
			return y;
		return 0;
	}
	return 0;
}

static void
scrollerset(Widget wid, int pos)
{
	int prevrow, newrow, maxpos;

	maxpos = HANDLE_MAX_SIZE - wid->handlew;
	if (maxpos <= 0) {
		/* all files are visible, there's nothing to scroll */
		return;
	}
	pos = max(pos, 0);
	pos = min(pos, maxpos);
	newrow = pos * wid->nscreens / maxpos;
	newrow = max(newrow, 0);
	newrow = min(newrow, wid->nscreens);
	if (newrow == wid->nscreens) {
		wid->ydiff = wid->itemh;
		newrow = wid->nscreens - 1;
	} else {
		wid->ydiff = 0;
	}
	prevrow = wid->row;
	setrow(wid, newrow);
	drawscroller(wid, pos);
	if (prevrow != newrow) {
		settitle(wid);
		drawitems(wid);
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
pixmapfromdata(Widget wid, char **data, Pixmap *pix, Pixmap *mask)
{
	XpmAttributes xa = {
		.valuemask = XpmVisual | XpmColormap | XpmDepth,
		.visual = wid->visual,
		.colormap = wid->colormap,
		.depth = wid->depth,
	};

	if (XpmCreatePixmapFromData(wid->dpy, wid->win, data, pix, mask, &xa) != XpmSuccess) {
		*pix = None;
		*mask = None;
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
}

static int
fillselitems(Widget wid, int *selitems, int clicked)
{
	struct Selection *sel;
	int nitems;

	nitems = 0;
	if (clicked != -1)
		selitems[nitems++] = clicked;
	for (sel = wid->sel; sel != NULL; sel = sel->next) {
		if (sel->index == clicked)
			continue;
		selitems[nitems++] = sel->index;
	}
	return nitems;
}

static Window
createdragwin(Widget wid, int index)
{
	Window win;
	GC gc;
	unsigned long opacity;
	int icon, pix, mask;
	int xroot, yroot, w, h;

	if (index <= 0)
		return None;
	if (!querypointer(wid, ROOT(wid->dpy), &xroot, &yroot, NULL))
		return None;
	w = h = THUMBSIZE;
	if (wid->thumbs[index] != NULL) {
		w = wid->thumbs[index]->w;
		h = wid->thumbs[index]->h;
	}
	win = XCreateWindow(
		wid->dpy, ROOT(wid->dpy),
		xroot, yroot, w, h, 0,
		wid->depth, InputOutput, wid->visual,
		CWBackPixel | CWOverrideRedirect| CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.colormap = wid->colormap,
			.background_pixel = wid->colors[SELECT_NOT][COLOR_BG].pixel,
			.override_redirect = True
		}
	);
	if (win == None)
		return None;
	opacity = DND_OPACITY;
	XChangeProperty(
		wid->dpy, win,
		wid->atoms[_NET_WM_WINDOW_OPACITY],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&opacity,
		1
	);
	XChangeProperty(
		wid->dpy, win,
		wid->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *)&wid->atoms[_NET_WM_WINDOW_TYPE_DND],
		1
	);
	if (wid->thumbs[index] == NULL) {
		icon = wid->itemicons[index];
		pix = wid->icons[icon].pix;
		if ((mask = XCreatePixmap(wid->dpy, win, w, h, CLIP_DEPTH)) == None) {
			XDestroyWindow(wid->dpy, win);
			return None;
		}
		if ((gc = XCreateGC(wid->dpy, mask, 0, NULL)) == None) {
			XFreePixmap(wid->dpy, mask);
			XDestroyWindow(wid->dpy, win);
			return None;
		}
		XSetForeground(wid->dpy, gc, 0);
		XFillRectangle(wid->dpy, mask, gc, 0, 0, w, h);
		XCopyArea(wid->dpy, wid->icons[icon].mask, mask, gc, 0, 0, w, h, 0, 0);
		XShapeCombineMask(wid->dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
		XFreePixmap(wid->dpy, mask);
		XFreeGC(wid->dpy, gc);
		XSetWindowBackgroundPixmap(wid->dpy, win, pix);
	} else {
		if ((pix = XCreatePixmap(wid->dpy, win, w, h, wid->depth)) == None) {
			XDestroyWindow(wid->dpy, win);
			return None;
		}
		XPutImage(
			wid->dpy,
			pix,
			wid->gc,
			wid->thumbs[index]->img,
			0, 0, 0, 0, w, h
		);
		XSetWindowBackgroundPixmap(wid->dpy, win, pix);
		XFreePixmap(wid->dpy, pix);
	}
	XMapRaised(wid->dpy, win);
	XFlush(wid->dpy);
	return win;
}

static char *
gettextprop(Widget wid, Atom prop)
{
	char *text;
	unsigned char *p;
	unsigned long len;
	unsigned long dl;               /* dummy variable */
	int format, status;
	Atom type;

	text = NULL;
	status = XGetWindowProperty(
		wid->dpy,
		wid->win,
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
xinitvisual(Widget wid)
{
	XVisualInfo tpl = {
		.screen = SCREEN(wid->dpy),
		.depth = 32,
		.class = TrueColor
	};
	XVisualInfo *infos;
	XRenderPictFormat *fmt;
	long masks = VisualScreenMask | VisualDepthMask | VisualClassMask;
	int nitems;
	int i;

	wid->visual = NULL;
	if ((infos = XGetVisualInfo(wid->dpy, masks, &tpl, &nitems)) != NULL) {
		for (i = 0; i < nitems; i++) {
			fmt = XRenderFindVisualFormat(wid->dpy, infos[i].visual);
			if (fmt->type == PictTypeDirect && fmt->direct.alphaMask) {
				wid->depth = infos[i].depth;
				wid->visual = infos[i].visual;
				wid->colormap = XCreateColormap(wid->dpy, ROOT(wid->dpy), wid->visual, AllocNone);
				break;
			}
		}
		XFree(infos);
	}
	if (wid->visual == NULL) {
		wid->depth = DefaultDepth(wid->dpy, SCREEN(wid->dpy));
		wid->visual = DefaultVisual(wid->dpy, SCREEN(wid->dpy));
		wid->colormap = DefaultColormap(wid->dpy, SCREEN(wid->dpy));
	}
}

/*
 * The following routines check an event, and process then if needed.
 * They return WIDGET_NONE if the event is not processed.
 */

static WidgetEvent
keypress(Widget wid, XKeyEvent *xev, int *selitems, int *nitems, char **text)
{
	KeySym ksym;
	unsigned int state;
	int row[2];
	int redrawall, previtem, index, shift, newrow, n, i;
	char *kstr;

	if (!XkbLookupKeySym(wid->dpy, xev->keycode, xev->state, &state, &ksym))
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
		if (wid->sel == NULL)
			break;
		unselectitems(wid);
		break;
	case XK_Return:
		if (wid->highlight == -1)
			break;
		*nitems = fillselitems(wid, selitems, wid->highlight);
		return WIDGET_OPEN;
	case XK_Menu:
		*nitems = fillselitems(wid, selitems, -1);
		return WIDGET_CONTEXT;
	case XK_space:
		if (wid->highlight == -1)
			break;
		selectitem(wid, wid->highlight, wid->issel[wid->highlight] == NULL, FALSE);
		break;
	case XK_Prior:
	case XK_Next:
		if (scroll(wid, (ksym == XK_Prior ? -1 : 1) * PAGE_STEP(wid)))
			drawitems(wid);
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
			wid->ydiff = 0;
			setrow(wid, 0);
			goto draw;
		}
		if (ksym == XK_End) {
			index = wid->nitems - 1;
			wid->ydiff = 0;
			setrow(wid, wid->nscreens);
			goto draw;
		}
		if (wid->highlight == -1) {
			wid->highlight = 0;
			setrow(wid, 0);
		}
		if (ksym == XK_Up || ksym == XK_k) {
			n = -wid->ncols;
		} else if (ksym == XK_Down || ksym == XK_j) {
			n = wid->highlight < (WHL_ROWS(wid)) * wid->ncols
			  ? wid->nitems - wid->highlight - 1
			  : 0;
			n = min(wid->ncols, n);
		} else if (ksym == XK_Left || ksym == XK_h) {
			n = -1;
		} else {
			n = 1;
		}
		if ((index = wid->highlight + n) < 0 || index >= wid->nitems)
			break;
		row[0] = wid->highlight / wid->ncols;
		row[1] = index / wid->ncols;
		newrow = wid->row;
		for (i = 0; i < 2; i++) {
			/*
			 * Try to make both previously highlighted item
			 * and new highlighted item visible.
			 */
			if (row[i] < newrow) {
				newrow = row[i];
			} else if (row[i] >= newrow + WIN_ROWS(wid)) {
				newrow = row[i] - WIN_ROWS(wid) + 1;
			}
		}
		if (wid->row != newrow) {
			wid->ydiff = 0;
			setrow(wid, newrow);
			redrawall = TRUE;
		} else if (wid->row == index / wid->ncols) {
			wid->ydiff = 0;
			wid->redraw = TRUE;
		}
draw:
		previtem = wid->highlight;
		highlight(wid, index, TRUE);
		if (xev->state & ShiftMask)
			selectitems(wid, index, previtem);
		else if (xev->state & ControlMask)
			selectitem(wid, index, TRUE, 0);
		if (redrawall)
			drawitems(wid);
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
		*nitems = fillselitems(wid, selitems, -1);
		return WIDGET_KEYPRESS;
	}
	return WIDGET_NONE;
}

static WidgetEvent
processevent(Widget wid, XEvent *ev)
{
	if (wid->selctx != NULL) {
		switch (ctrlsel_send(wid->selctx, ev)) {
		case CTRLSEL_LOST:
			unselectitems(wid);
			disownprimary(wid);
			return WIDGET_INTERNAL;
		case CTRLSEL_INTERNAL:
			return WIDGET_INTERNAL;
		default:
			break;
		}
	}
	if (wid->dragctx != NULL) {
		switch (ctrlsel_dndsend(wid->dragctx, ev)) {
		case CTRLSEL_SENT:
			disowndnd(wid);
			return WIDGET_REFRESH;
		case CTRLSEL_LOST:
			disowndnd(wid);
			return WIDGET_INTERNAL;
		case CTRLSEL_INTERNAL:
			return WIDGET_INTERNAL;
		default:
			break;
		}
	}
	switch (ctrlsel_dndreceive(&wid->dropctx, ev)) {
	case CTRLSEL_RECEIVED:
		FREE(wid->droptarget.buffer);
		return WIDGET_INTERNAL;
	case CTRLSEL_INTERNAL:
		return WIDGET_INTERNAL;
	default:
		break;
	}
	wid->redraw = FALSE;
	switch (ev->type) {
	case ClientMessage:
		if (ev->xclient.message_type == wid->atoms[WM_PROTOCOLS] &&
		    (Atom)ev->xclient.data.l[0] == wid->atoms[WM_DELETE_WINDOW])
			return WIDGET_CLOSE;
		return WIDGET_NONE;
	case Expose:
		if (ev->xexpose.count == 0)
			commitdraw(wid);
		break;
	case ConfigureNotify:
		if (calcsize(wid, ev->xconfigure.width, ev->xconfigure.height)) {
			if (wid->row >= wid->nscreens)
				setrow(wid, wid->nscreens - 1);
			drawitems(wid);
		}
		break;
	case PropertyNotify:
		if (ev->xproperty.window != wid->win)
			break;
		if (ev->xproperty.state != PropertyNewValue)
			break;
		if (ev->xproperty.atom != wid->atoms[_CONTROL_GOTO])
			break;
		FREE(wid->lasttext);
		wid->lastprop = wid->atoms[_CONTROL_GOTO];
		wid->lasttext = gettextprop(wid, wid->atoms[_CONTROL_GOTO]);
		break;
	default:
		return WIDGET_NONE;
	}
	endevent(wid);
	return WIDGET_INTERNAL;
}

static WidgetEvent
checklastprop(Widget wid, char **text)
{
	Atom prop;
	char *str;

	if (wid->lastprop != None) {
		prop = wid->lastprop;
		str = wid->lasttext;
		wid->lastprop = None;
		wid->lasttext = NULL;
		if (prop == wid->atoms[_CONTROL_GOTO]) {
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
scrollmode(Widget wid, int x, int y)
{
	XSyncAlarm alarm;
	XEvent ev;
	int grabpos, pos, left;

	wid->state = STATE_SCROLLING;
	grabpos = wid->handlew / 2;             /* we grab the handle in its middle */
	drawscroller(wid, gethandlepos(wid));
	XMoveWindow(wid->dpy, wid->scroller, x - SCROLLER_SIZE / 2 - 1, y - SCROLLER_SIZE / 2 - 1);
	XMapRaised(wid->dpy, wid->scroller);
	alarm = XSyncCreateAlarm(wid->dpy, ALARMFLAGS, &wid->syncattr);
	left = FALSE;
	while (!XNextEvent(wid->dpy, &ev)) {
		switch (processevent(wid, &ev)) {
		case WIDGET_CLOSE:
			XSyncDestroyAlarm(wid->dpy, alarm);
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		if (ev.type == wid->syncevent + XSyncAlarmNotify) {
			if ((pos = scrollerpos(wid)) != 0) {
				if (scroll(wid, pos))
					drawitems(wid);
				commitdraw(wid);
			}
			XSyncChangeAlarm(wid->dpy, alarm, ALARMFLAGS, &wid->syncattr);
			continue;
		}
		switch (ev.type) {
		case MotionNotify:
			if (ev.xmotion.window == wid->scroller && (ev.xmotion.state & Button1Mask)) {
				scrollerset(wid, ev.xmotion.y - grabpos);
			} else if (ev.xmotion.window == wid->win &&
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
			if (ev.xbutton.window == wid->win)
				goto done;
			if (ev.xbutton.window == wid->scroller) {
				left = TRUE;
				pos = gethandlepos(wid);
				if (ev.xmotion.y < pos || ev.xmotion.y > pos + wid->handlew) {
					/* grab handle in the middle */
					grabpos = wid->handlew / 2;
					scrollerset(wid, ev.xmotion.y - grabpos);
				} else {
					/* grab handle in position under pointer */
					grabpos = ev.xmotion.y - pos;
				}
			}
			break;
		}
		endevent(wid);
	}
done:
	wid->state = STATE_NORMAL;
	XSyncDestroyAlarm(wid->dpy, alarm);
	XUnmapWindow(wid->dpy, wid->scroller);
	return WIDGET_NONE;
}

static WidgetEvent
selmode(Widget wid, Time lasttime, int shift, int clickx, int clicky)
{
	XEvent ev;
	XSyncAlarm alarm;
	int rectrow, rectydiff, ownsel, x, y;

	wid->state = STATE_SELECTING;
	rectrow = wid->row;
	rectydiff = wid->ydiff;
	alarm = XSyncCreateAlarm(wid->dpy, ALARMFLAGS, &wid->syncattr);
	ownsel = FALSE;
	if (!shift)
		unselectitems(wid);
	while (!XNextEvent(wid->dpy, &ev)) {
		switch (processevent(wid, &ev)) {
		case WIDGET_CLOSE:
			XSyncDestroyAlarm(wid->dpy, alarm);
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		if (ev.type == wid->syncevent + XSyncAlarmNotify) {
			if (querypointer(wid, wid->win, &x, &y, NULL)) {
				if (y > wid->h)
					y -= wid->h;
				else if (y > 0)
					y = 0;
				if (scroll(wid, y)) {
					drawitems(wid);
				}
				if (y != 0) {
					commitdraw(wid);
				}
			}
			XSyncChangeAlarm(wid->dpy, alarm, ALARMFLAGS, &wid->syncattr);
			continue;
		}
		switch (ev.type) {
		case ButtonPress:
		case ButtonRelease:
			goto done;
		case MotionNotify:
			if (ev.xmotion.time - lasttime < MOTION_TIME)
				break;
			rectdraw(wid, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y);
			if (rectselect(wid, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y))
				ownsel = TRUE;
			commitdraw(wid);
			lasttime = ev.xmotion.time;
			break;
		}
		endevent(wid);
	}
done:
	wid->state = STATE_NORMAL;
	rectdraw(wid, 0, 0, 0, 0, 0, 0);
	commitrectsel(wid);
	commitdraw(wid);
	XSyncDestroyAlarm(wid->dpy, alarm);
	if (ownsel)
		ownprimary(wid, ev.xbutton.time);
	return WIDGET_NONE;
}

static WidgetEvent
dragmode(Widget wid, Time lasttime, int clicki, int *selitems, int *nitems)
{
	struct Selection *sel;
	Window dragwin;
	unsigned int mask;
	int state, i, x, y;

	if (wid->sel == NULL)
		return WIDGET_NONE;
	disowndnd(wid);
	if ((wid->dragctx = malloc(sizeof(*wid->dragctx))) == NULL) {
		warn("malloc");
		return WIDGET_NONE;
	}
	dragwin = createdragwin(wid, clicki);
	i = 0;
	for (sel = wid->sel; sel != NULL; sel = sel->next)
		i += snprintf(wid->dndbuf + i, wid->uribufsiz - i, "file://%s\r\n", wid->items[sel->index][ITEM_PATH]);
	ctrlsel_filltarget(
		wid->atoms[TEXT_URI_LIST],
		wid->atoms[TEXT_URI_LIST],
		8, (unsigned char *)wid->dndbuf, i,
		&wid->dragtarget
	);
	state = ctrlsel_dndown(
		wid->dpy,
		wid->win,
		dragwin,
		lasttime,
		&wid->dragtarget,
		1,
		wid->dragctx
	);
	if (dragwin != None)
		XDestroyWindow(wid->dpy, dragwin);
	if (state != CTRLSEL_DROPOTHER)
		FREE(wid->dragctx);
	if (state == CTRLSEL_ERROR) {
		warnx("could not perform drag-and-drop");
	} else if (state == CTRLSEL_DROPSELF) {
		querypointer(wid, wid->win, &x, &y, &mask);
		clicki = getpointerclick(wid, x, y);
		if (clicki < 1)
			return WIDGET_NONE;
		*nitems = fillselitems(wid, selitems, clicki);
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
mainmode(Widget wid, int *selitems, int *nitems, char **text)
{
	XEvent ev;
	Time lasttime = 0;
	int clickx = 0;
	int clicky = 0;
	int clicki = -1;
	int state;
	int x, y;

	while (!XNextEvent(wid->dpy, &ev)) {
		switch (ctrlsel_dndreceive(&wid->dropctx, &ev)) {
		case CTRLSEL_RECEIVED:
			if (wid->droptarget.buffer == NULL)
				return WIDGET_INTERNAL;
			*text = (char *)wid->droptarget.buffer;
			*nitems = 1;
			querypointer(wid, wid->win, &x, &y, NULL);
			selitems[0] = getpointerclick(wid, x, y);
			if (wid->droptarget.action == CTRLSEL_COPY)
				return WIDGET_DROPCOPY;
			if (wid->droptarget.action == CTRLSEL_MOVE)
				return WIDGET_DROPMOVE;
			if (wid->droptarget.action == CTRLSEL_LINK)
				return WIDGET_DROPLINK;
			return WIDGET_DROPASK;
		case CTRLSEL_INTERNAL:
			return WIDGET_INTERNAL;
		default:
			break;
		}
		switch ((state = processevent(wid, &ev))) {
		case WIDGET_CLOSE:
		case WIDGET_REFRESH:
			return state;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		if ((state = checklastprop(wid, text)) != WIDGET_NONE)
			return state;
		switch (ev.type) {
		case KeyPress:
			state = keypress(wid, &ev.xkey, selitems, nitems, text);
			if (state != WIDGET_NONE)
				return state;
			break;
		case ButtonPress:
			clickx = ev.xbutton.x;
			clicky = ev.xbutton.y;
			if (ev.xbutton.window != wid->win)
				break;
			if (ev.xbutton.button == Button1) {
				clicki = mouse1click(wid, &ev.xbutton);
			} else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
				if (scroll(wid, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP)))
					drawitems(wid);
				commitdraw(wid);
			} else if (ev.xbutton.button == Button2) {
				state = scrollmode(wid, ev.xmotion.x, ev.xmotion.y);
				if (state != WIDGET_NONE)
					return state;
			} else if (ev.xbutton.button == Button3) {
				mouse3click(wid, ev.xbutton.x, ev.xbutton.y);
				*nitems = fillselitems(wid, selitems, -1);
				commitdraw(wid);
				XUngrabPointer(wid->dpy, ev.xbutton.time);
				XFlush(wid->dpy);
				return WIDGET_CONTEXT;
			}
			break;
		case ButtonRelease:
			if (ev.xbutton.window != wid->win)
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
			*nitems = fillselitems(wid, selitems, wid->highlight);
			return WIDGET_OPEN;
		case MotionNotify:
			if (ev.xmotion.window != wid->win)
				break;
			if (ev.xmotion.state != Button1Mask &&
			    ev.xmotion.state != (Button1Mask|ShiftMask) &&
			    ev.xmotion.state != (Button1Mask|ControlMask))
				break;
			if (diff(ev.xmotion.x, clickx) * diff(ev.xmotion.y, clicky) < DRAG_SQUARE)
				break;
			if (clicki == -1) {
				state = selmode(wid, ev.xmotion.time, ev.xmotion.state & (ShiftMask | ControlMask), clickx, clicky);
			} else if (clicki > 0) {
				state = dragmode(wid, ev.xmotion.time, clicki, selitems, nitems);
			}
			if (state != WIDGET_NONE)
				return state;
			break;
		default:
			break;
		}
		endevent(wid);
	}
	return WIDGET_CLOSE;
}

/*
 * Check widget.h for description on the interface of the following
 * public functions.  Some of them rely on the existence of objects
 * in the given addresses, during Widget's lifetime.
 */

Widget
initwidget(const char *class, const char *name, const char *geom, int argc, char *argv[])
{
	XSyncSystemCounter *counters;
	Widget wid;
	int success, ncounters, tmp, i;
	char *progname, *s;

	wid = NULL;
	progname = "";
	if (argc > 0 && argv[0] != NULL) {
		progname = argv[0];
		if ((s = strrchr(argv[0], '/')) != NULL) {
			progname = s + 1;
		}
	}
	if (name == NULL)
		name = progname;
	if ((wid = malloc(sizeof(*wid))) == NULL) {
		warn("malloc");
		return NULL;
	}
	*wid = (struct Widget){
		.progname = progname,
		.dpy = NULL,
		.start = FALSE,
		.redraw = FALSE,
		.win = None,
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
		.syncattr = (XSyncAlarmAttributes){
			.trigger.counter        = None,
			.trigger.value_type     = XSyncRelative,
			.trigger.test_type      = XSyncPositiveComparison,
		},
	};
	if (!XInitThreads()) {
		warnx("XInitThreads");
		goto error;
	}
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale()) {
		warnx("could not set locale");
		goto error;
	}
	if ((wid->dpy = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		goto error;
	}
	xinitvisual(wid);
	if (!XSyncQueryExtension(wid->dpy, &wid->syncevent, &tmp)) {
		warnx("could not query XSync extension");
		goto error;
	}
	if (!XSyncInitialize(wid->dpy, &tmp, &tmp)) {
		warnx("could not initialize XSync extension");
		goto error;
	}
	XInternAtoms(wid->dpy, atomnames, ATOM_LAST, False, wid->atoms);
	if (fcntl(XConnectionNumber(wid->dpy), F_SETFD, FD_CLOEXEC) == -1) {
		warn("fcntl");
		goto error;
	}
	if (inittheme(wid, class, name) == -1) {
		warnx("could not set theme");
		goto error;
	}
	if (createwin(wid, class, name, geom, argc, argv, winicon_data, winicon_size) == -1) {
		warnx("could not create window");
		goto error;
	}
	if ((wid->gc = XCreateGC(wid->dpy, wid->win, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None) {
		warnx("could not create graphics context");
		goto error;
	}
	wid->scroller = XCreateWindow(
		wid->dpy, wid->win,
		0, 0, SCROLLER_SIZE, SCROLLER_SIZE, 1,
		wid->depth, InputOutput, wid->visual,
		CWBackPixel | CWBorderPixel | CWEventMask | CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.colormap = wid->colormap,
			.background_pixel = 0,
			.border_pixel = wid->colors[SELECT_NOT][COLOR_FG].pixel,
			.event_mask = ButtonPressMask | PointerMotionMask,
		}
	);
	if (wid->scroller == None) {
		warnx("could not create window");
		goto error;
	}
	if ((wid->stipple = XCreatePixmap(wid->dpy, wid->win, STIPPLE_SIZE, STIPPLE_SIZE, CLIP_DEPTH)) == None) {
		warnx("could not create pixmap");
		goto error;
	}
	if ((wid->stipgc = XCreateGC(wid->dpy, wid->stipple, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None) {
		warnx("could not create graphics context");
		goto error;
	}
	ctrlsel_filltarget(
		wid->atoms[TEXT_URI_LIST],
		wid->atoms[TEXT_URI_LIST],
		0, NULL, 0,
		&wid->droptarget
	);
	success = ctrlsel_dndwatch(
		wid->dpy,
		wid->win,
		CTRLSEL_COPY | CTRLSEL_MOVE | CTRLSEL_LINK | CTRLSEL_ASK,
		&wid->droptarget,
		1,
		&wid->dropctx
	);
	if (!success) {
		ctrlsel_dndclose(&wid->dropctx);
		goto error;
	}
	XSetForeground(wid->dpy, wid->stipgc, 0);
	XFillRectangle(wid->dpy, wid->stipple, wid->stipgc, 0, 0, STIPPLE_SIZE, STIPPLE_SIZE);
	XSetForeground(wid->dpy, wid->stipgc, 1);
	XFillRectangle(wid->dpy, wid->stipple, wid->stipgc, 0, 0, 1, 1);
	XSetStipple(wid->dpy, wid->gc, wid->stipple);
	wid->busycursor = XCreateFontCursor(wid->dpy, XC_watch);
	if ((counters = XSyncListSystemCounters(wid->dpy, &ncounters)) != NULL) {
		for (i = 0; i < ncounters; i++) {
			if (strcmp(counters[i].name, "SERVERTIME") == 0) {
				wid->syncattr.trigger.counter = counters[i].counter;
				break;
			}
		}
		XSyncFreeSystemCounterList(counters);
	}
	if (wid->syncattr.trigger.counter == None) {
		warnx("could not use XSync extension");
		goto error;
	}
	XSyncIntToValue(&wid->syncattr.trigger.wait_value, 128);
	XSyncIntToValue(&wid->syncattr.delta, 0);
	return wid;
error:
	if (wid->stipple != None)
		XFreePixmap(wid->dpy, wid->stipple);
	if (wid->scroller != None)
		XDestroyWindow(wid->dpy, wid->scroller);
	if (wid->win != None)
		XDestroyWindow(wid->dpy, wid->win);
	if (wid->dpy != NULL)
		XCloseDisplay(wid->dpy);
	FREE(wid);
	return NULL;
}

int
setwidget(Widget wid, const char *title, char **items[], int itemicons[], size_t nitems, Scroll *scrl)
{
	size_t i;

	XUndefineCursor(wid->dpy, wid->win);
	cleanwidget(wid);
	wid->items = items;
	wid->nitems = nitems;
	wid->itemicons = itemicons;
	if (scrl == NULL) {
		wid->ydiff = 0;
		wid->row = 0;
	} else {
		wid->ydiff = scrl->ydiff;
		wid->row = scrl->row;
	}
	wid->title = title;
	wid->highlight = -1;
	(void)calcsize(wid, -1, -1);
	if (scrl != NULL && wid->row >= wid->nscreens) {
		wid->ydiff = 0;
		setrow(wid, wid->nscreens);
	}
	if ((wid->issel = calloc(wid->nitems, sizeof(*wid->issel))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((wid->linelen = calloc(wid->nitems, sizeof(*wid->linelen))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((wid->nlines = calloc(wid->nitems, sizeof(*wid->nlines))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((wid->thumbs = malloc(nitems * sizeof(*wid->thumbs))) == NULL) {
		warn("malloc");
		goto error;
	}
	wid->selbufsiz = 0;
	for (i = 0; i < nitems; i++) {
		wid->thumbs[i] = NULL;
		wid->selbufsiz += strlen(items[i][ITEM_PATH]) + 1; /* +1 for '\n' */
	}
	if ((wid->selbuf = malloc(wid->selbufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	wid->uribufsiz = wid->selbufsiz + (nitems * 8);         /* 8 for "file://\r" */
	if ((wid->uribuf = malloc(wid->uribufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	if ((wid->dndbuf = malloc(wid->uribufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	wid->thumbhead = NULL;
	settitle(wid);
	drawitems(wid);
	commitdraw(wid);
	return RETURN_SUCCESS;
error:
	cleanwidget(wid);
	return RETURN_FAILURE;
}

void
mapwidget(Widget wid)
{
	XMapWindow(wid->dpy, wid->win);
}

WidgetEvent
pollwidget(Widget wid, int *selitems, int *nitems, Scroll *scrl, char **text)
{
	XEvent ev;
	int retval;

	*text = NULL;
	wid->droptarget.buffer = NULL;
	while (wid->start && XPending(wid->dpy) > 0) {
		(void)XNextEvent(wid->dpy, &ev);
		if (processevent(wid, &ev) == WIDGET_CLOSE) {
			endevent(wid);
			return WIDGET_CLOSE;
		}
	}
	wid->start = TRUE;
	if ((retval = checklastprop(wid, text)) == WIDGET_NONE)
		retval = mainmode(wid, selitems, nitems, text);
	endevent(wid);
	scrl->ydiff = wid->ydiff;
	scrl->row = wid->row;
	return retval;
}

void
closewidget(Widget wid)
{
	int i, j;

	ctrlsel_dndclose(&wid->dropctx);
	cleanwidget(wid);
	for (i = 0; i < wid->nicons; i++) {
		if (wid->icons[i].pix != None) {
			XFreePixmap(wid->dpy, wid->icons[i].pix);
		}
		if (wid->icons[i].mask != None) {
			XFreePixmap(wid->dpy, wid->icons[i].mask);
		}
	}
	FREE(wid->icons);
	if (wid->draw != NULL)
		XftDrawDestroy(wid->draw);
	if (wid->pix != None)
		XFreePixmap(wid->dpy, wid->pix);
	if (wid->rectbord != None)
		XFreePixmap(wid->dpy, wid->rectbord);
	if (wid->namepix != None)
		XFreePixmap(wid->dpy, wid->namepix);
	if (wid->stipple != None)
		XFreePixmap(wid->dpy, wid->stipple);
	for (i = 0; i < SELECT_LAST; i++)
		for (j = 0; j < COLOR_LAST; j++)
			XftColorFree(wid->dpy, wid->visual, wid->colormap, &wid->colors[i][j]);
	XftFontClose(wid->dpy, wid->font);
	XDestroyWindow(wid->dpy, wid->scroller);
	XDestroyWindow(wid->dpy, wid->win);
	XFreeGC(wid->dpy, wid->stipgc);
	XFreeGC(wid->dpy, wid->gc);
	XCloseDisplay(wid->dpy);
	FREE(wid);
}

int
widopenicons(Widget wid, char **xpms[], int nxpms)
{
	int retval, i;

	wid->nicons = nxpms;
	if ((wid->icons = calloc(wid->nicons, sizeof(*wid->icons))) == NULL) {
		warn("calloc");
		return RETURN_FAILURE;
	}
	retval = RETURN_SUCCESS;
	for (i = 0; i < wid->nicons; i++) {
		if (pixmapfromdata(wid, xpms[i], &wid->icons[i].pix, &wid->icons[i].mask) == RETURN_FAILURE) {
			warnx("could not open %d-th default icon pixmap", i);
			retval = RETURN_FAILURE;
		}
	}
	return retval;
}

void
setthumbnail(Widget wid, char *path, int item)
{
	FILE *fp;
	size_t size, i;
	int w, h;
	char buf[DATA_DEPTH];
	unsigned char *data;

	if (item < 0 || item >= wid->nitems || wid->thumbs == NULL)
		return;
	data = NULL;
	wid->thumbs[item] = NULL;
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
	if ((wid->thumbs[item] = malloc(sizeof(*wid->thumbs[item]))) == NULL) {
		warn("malloc");
		goto error;
	}
	*wid->thumbs[item] = (struct Thumb){
		.w = w,
		.h = h,
		.img = NULL,
		.next = wid->thumbhead,
	};
	wid->thumbs[item]->img = XCreateImage(
		wid->dpy,
		wid->visual,
		wid->depth,
		ZPixmap,
		0, (char *)data,
		w, h,
		DATA_DEPTH * BYTE,
		0
	);
	if (wid->thumbs[item]->img == NULL) {
		warnx("%s: could not allocate XImage", path);
		goto error;
	}
	XInitImage(wid->thumbs[item]->img);
	wid->thumbhead = wid->thumbs[item];
	if (item >= wid->row * wid->ncols && item < wid->row * wid->ncols + wid->nrows * wid->ncols) {
		drawitem(wid, item);
		commitdraw(wid);
	}
	return;
error:
	if (fp != NULL)
		fclose(fp);
	FREE(data);
	FREE(wid->thumbs[item]);
}

void
widget_busy(Widget wid)
{
	XDefineCursor(wid->dpy, wid->win, wid->busycursor);
	XFlush(wid->dpy);
}

unsigned long
widgetwinid(Widget wid)
{
	return (unsigned long)wid->win;
}
