#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/xpm.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#include "ctrlfnt.h"
#include "ctrlsel.h"
#include "util.h"
#include "widget.h"
#include "winicon.data"         /* window icon, for the window manager */

#define ATOMS                                   \
	X(TEXT, NULL)                           \
	X(TEXT_URI_LIST, "text/uri-list")       \
	X(UTF8_STRING, NULL)                    \
	X(WM_PROTOCOLS, NULL)                   \
	X(WM_DELETE_WINDOW, NULL)               \
	X(_NET_WM_ICON, NULL)                   \
	X(_NET_WM_NAME, NULL)                   \
	X(_NET_WM_PID, NULL)                    \
	X(_NET_WM_WINDOW_TYPE, NULL)            \
	X(_NET_WM_WINDOW_TYPE_DND, NULL)        \
	X(_NET_WM_WINDOW_TYPE_NORMAL, NULL)     \
	X(_NET_WM_WINDOW_OPACITY, NULL)         \
	X(_CONTROL_STATUS, NULL)                \
	X(_CONTROL_CWD, NULL)                   \
	X(_CONTROL_GOTO, NULL)

#define RESOURCES                                             \
	/*            CLASS               NAME             */ \
	X(GEOMETRY,  "Geometry",         "geometry")          \
	X(FACE_NAME, "FaceName",         "faceName")          \
	X(FACE_SIZE, "FaceSize",         "faceSize")          \
	X(ICONS,     "FileIcons",        "fileIcons")         \
	X(NORMAL_BG, "Background",       "background")        \
	X(NORMAL_FG, "Foreground",       "foreground")        \
	X(SELECT_BG, "ActiveBackground", "activeBackground")  \
	X(SELECT_FG, "ActiveForeground", "activeForeground")  \
	X(STATUSBAR, "StatusBarEnable",  "statusBarEnable")     \
	X(BARSTATUS, "EnableStatusBar",  "enableStatusBar")     \
	X(OPACITY,   "Opacity",          "opacity")

#define EVENT_MASK      (StructureNotifyMask | PropertyChangeMask | KeyPressMask |\
                         PointerMotionMask | ButtonReleaseMask | ButtonPressMask)
#define WINDOW_MASK     (CWBackPixel | CWEventMask | CWColormap | CWBorderPixel)

#define DEF_COLOR_BG    (XRenderColor){ .red = 0x0000, .green = 0x0000, .blue = 0x0000, .alpha = 0xFFFF }
#define DEF_COLOR_FG    (XRenderColor){ .red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .alpha = 0xFFFF }
#define DEF_COLOR_SELBG (XRenderColor){ .red = 0x3400, .green = 0x6500, .blue = 0xA400, .alpha = 0xFFFF }
#define DEF_COLOR_SELFG (XRenderColor){ .red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .alpha = 0xFFFF }
#define DEF_SIZE        (XRectangle){ .x = 0, .y = 0, .width = 600 , .height = 460 }
#define DEF_OPACITY     0xFFFF
#define DEF_STATUSBAR   true

#define FREE(x)         do{free(x); x = NULL;}while(0)

#define UNKNOWN_STATUS  "<\?\?\?>"

/* ellipsis has two dots rather than three; the third comes from the extension */
#define ELLIPSIS        ".."

/* opacity of drag-and-drop mini window */
#define DND_OPACITY     0x7FFFFFFF

/* opacity of rectangular selection */
#define SEL_OPACITY     0xC000
#define RECT_OPACITY    0x4000

/* constants to check a .ppm file */
#define PPM_HEADER      "P6\n"
#define PPM_COLOR       "255\n"

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

#define STATUSBAR_HEIGHT(w) ((w)->fonth * 2)
#define STATUSBAR_MARGIN(w) ((w)->fonth / 2)

enum {
	/* size of border of rectangular selection */
	RECT_BORDER     = 1,

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
	KSYM_BUFSIZE    = 64,                   /* key symbol name buffer size */

	/* hardcoded object sizes in pixels */
	/* there's no ITEM_HEIGHT for it is computed at runtime from font height */
	THUMBSIZE       = 64,                   /* maximum thumbnail size */
	ICON_MARGIN     = (THUMBSIZE / 2),      /* margin around item icon */
	ITEM_WIDTH      = (THUMBSIZE * 2),      /* width of an item (icon + margins) */
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
	SCROLL_TIME     = 128,

	/* scrolling */
	SCROLL_STEP     = 32,                   /* pixels per scroll */
	SCROLLER_SIZE   = 32,                   /* size of the scroller */
	SCROLLER_MIN    = 16,                   /* min lines to scroll for the scroller to change */
	HANDLE_MAX_SIZE = (SCROLLER_SIZE - 4),  /* max size of the scroller handle */
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

enum Layer {
	LAYER_CANVAS,
	LAYER_ICONS,
	LAYER_SELALPHA,
	LAYER_RECTALPHA,
	LAYER_SCROLLER,
	LAYER_STATUSBAR,
	LAYER_LAST,
};

enum Atom {
#define X(atom, name) atom,
	ATOMS
	NATOMS
#undef  X
};

enum Resource {
#define X(res, class, name) res,
	RESOURCES
	NRESOURCES
#undef  X
};

struct Icon {
	const char *name;
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
	int start;
	int redraw;

	/* X11 stuff */
	Display *display;
	Atom atoms[NATOMS];
	GC gc;
	Cursor busycursor;
	Window window, root;
	struct {
		XRenderColor chans;
		Pixmap pix;
		Picture pict;
	} colors[SELECT_LAST][COLOR_LAST];
	XRenderPictFormat *format, *alpha_format;
	Visual *visual;
	Colormap colormap;
	int fd;
	int screen;
	unsigned int depth;
	unsigned short opacity;
	struct timespec time;

	CtrlFontSet *fontset;

	struct {
		XrmClass class;
		XrmName name;
	} application, resources[NRESOURCES];
	const char **cliresources;

	Atom lastprop;
	char *lasttext;

	struct {
		Pixmap pix;
		Picture pict;
	} layers[LAYER_LAST];

	Pixmap namepix;                 /* temporary pixmap for the labels */
	Pixmap namepict;

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
	CtrlSelContext *selctx, *dragctx, *dropctx;
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
	int w, h;                       /* icon area size */
	int winw, winh;                 /* window size */
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
	int nicons;

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
	 * Statusbar describing highlighted item.
	 */
	int status_enable;
};

struct Options {
	const char *class;
	const char *name;
	int argc;
	char **argv;
};

static char *
getitemstatus(Widget *widget, int index)
{
	if (index < 0 || index >= widget->nitems)
		return UNKNOWN_STATUS;
	if (widget->items[widget->highlight][ITEM_STATUS] == NULL)
		return UNKNOWN_STATUS;
	return widget->items[widget->highlight][ITEM_STATUS];
}

static void
resetlayer(Widget *widget, enum Layer layer, int width, int height)
{
	Bool isalpha = (layer == LAYER_SELALPHA || layer == LAYER_RECTALPHA);

	if (widget->layers[layer].pix != None)
		XFreePixmap(widget->display, widget->layers[layer].pix);
	if (widget->layers[layer].pict != None)
		XRenderFreePicture(widget->display, widget->layers[layer].pict);
	widget->layers[layer].pix = XCreatePixmap(
		widget->display,
		widget->window,
		width,
		height,
		isalpha ? 8 : widget->depth
	);
	widget->layers[layer].pict = XRenderCreatePicture(
		widget->display,
		widget->layers[layer].pix,
		isalpha ? widget->alpha_format : widget->format,
		0,
		NULL
	);
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[layer].pict,
		&(XRenderColor){ 0 },
		0, 0, widget->w, widget->h
	);
}

static void
setfont(Widget *widget, const char *facename, double fontsize)
{
	CtrlFontSet *fontset;

	if (facename == NULL)
		facename = "xft:";
	fontset = ctrlfnt_open(
		widget->display,
		widget->screen,
		widget->visual,
		widget->colormap,
		facename,
		fontsize
	);
	if (fontset == NULL)
		return;
	if (widget->fontset != NULL)
		ctrlfnt_free(widget->fontset);
	widget->fontset = fontset;
	widget->fonth = ctrlfnt_height(widget->fontset);
	widget->itemh = THUMBSIZE + (NLINES + 1) * widget->fonth;
	widget->ellipsisw = ctrlfnt_width(widget->fontset, ELLIPSIS, strlen(ELLIPSIS));
	if (widget->namepix != None)
		XFreePixmap(widget->display, widget->namepix);
	if (widget->namepict != None)
		XRenderFreePicture(widget->display, widget->namepict);
	widget->namepix = XCreatePixmap(
		widget->display,
		widget->window,
		LABELWIDTH,
		widget->fonth,
		widget->depth
	);
	widget->namepict = XRenderCreatePicture(
		widget->display,
		widget->namepix,
		widget->format,
		0,
		NULL
	);
}

static void
setcolor(Widget *widget, int scheme, int colornum, const char *colorname)
{
	XColor color;

	if (scheme >= SELECT_LAST || colornum >= COLOR_LAST || colorname == NULL)
		return;
	if (!XParseColor(widget->display, widget->colormap, colorname, &color)) {
		warnx("%s: unknown color name", colorname);
		return;
	}
	widget->colors[scheme][colornum].chans = (XRenderColor){
		.red   = FLAG(color.flags, DoRed)   ? color.red   : 0x0000,
		.green = FLAG(color.flags, DoGreen) ? color.green : 0x0000,
		.blue  = FLAG(color.flags, DoBlue)  ? color.blue  : 0x0000,
		.alpha = 0xFFFF,
	};
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->colors[scheme][colornum].pict,
		&widget->colors[scheme][colornum].chans,
		0, 0, 1, 1
	);
}

static void
setopacity(Widget *widget, const char *value)
{
	char *endp;
	double d;

	if (value == NULL)
		return;
	d = strtod(value, &endp);
	if (endp == value || *endp != '\0' || d < 0.0 || d > 1.0) {
		warnx("%s: invalid opacity value", value);
		return;
	}
	widget->opacity = d * 0xFFFF;
}

static char *
getresource(XrmDatabase xdb, XrmClass appclass, XrmName appname, XrmClass resclass, XrmName resname)
{
	XrmQuark name[] = { appname, resname, NULLQUARK };
	XrmQuark class[] = { appclass, resclass, NULLQUARK };
	XrmRepresentation tmp;
	XrmValue xval;

	if (XrmQGetResource(xdb, name, class, &tmp, &xval))
		return xval.addr;
	return NULL;
}

static XrmDatabase
loadxdb(Widget *widget, const char *str)
{
	XrmDatabase xdb, tmp;
	int i;

	if ((xdb = XrmGetStringDatabase(str)) == NULL)
		return NULL;
	for (i = 0; widget->cliresources[i] != NULL; i++) {
		tmp = XrmGetStringDatabase(widget->cliresources[i]);
		XrmMergeDatabases(tmp, &xdb);
	}
	return xdb;
}

static void
drawstatusbar(Widget *widget)
{
	size_t statuslen, scrolllen;
	int countwid, statuswid, scrollwid, rightwid;
	char *status;
	char countstr[64];    /* enough for writing number of files */
	char scrollstr[8];    /* enough for writing the percentage */
	int scrollpct;

	if (!widget->status_enable)
		return;

	etlock(&widget->lock);
	widget->redraw = true;

	/* clear previous content */
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_STATUSBAR].pict,
		&widget->colors[SELECT_NOT][COLOR_BG].chans,
		0, 0, widget->winw, STATUSBAR_HEIGHT(widget)
	);

	/* draw item counter */
	(void)snprintf(
		countstr,
		LEN(countstr),
		"[%d/%d]",
		widget->highlight > 0 ? widget->highlight : 0,
		widget->nitems - 1      /* -1 because the first item ".." is not counted */
	);
	countwid = ctrlfnt_draw(
		widget->fontset,
		widget->layers[LAYER_STATUSBAR].pict,
		widget->colors[SELECT_NOT][COLOR_FG].pict,
		(XRectangle){
			.x = STATUSBAR_MARGIN(widget),
			.y = STATUSBAR_MARGIN(widget),
			.width = widget->w,
			.height = widget->fonth,
		},
		countstr,
		strlen(countstr)
	);
	countwid += STATUSBAR_MARGIN(widget);

	/* draw name of highlighted item */
	if (widget->highlight > 0) {
		ctrlfnt_draw(
			widget->fontset,
			widget->layers[LAYER_STATUSBAR].pict,
			widget->colors[SELECT_NOT][COLOR_FG].pict,
			(XRectangle){
				.x = STATUSBAR_MARGIN(widget) + countwid,
				.y = STATUSBAR_MARGIN(widget),
				.width = widget->w,
				.height = widget->fonth,
			},
			widget->items[widget->highlight][ITEM_NAME],
			strlen(widget->items[widget->highlight][ITEM_NAME])
		);
	}

	/* get percentage */
	scrollpct = 100 * ((double)(widget->row + 1) / widget->nscreens);
	scrollpct = min(scrollpct, 100);
	(void)snprintf(scrollstr, LEN(scrollstr), "[%d%%]", scrollpct);
	scrolllen = strlen(scrollstr);
	scrollwid = ctrlfnt_width(widget->fontset, scrollstr, scrolllen);
	rightwid = STATUSBAR_MARGIN(widget) * 2 + scrollwid;

	/* get metadata */
	if (widget->highlight > 0) {
		status = getitemstatus(widget, widget->highlight);
		statuslen = strlen(status);
		statuswid = ctrlfnt_width(widget->fontset, status, statuslen);
		rightwid += STATUSBAR_MARGIN(widget) + statuswid;
	}

	/* clear content below right side of statusbar */
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_STATUSBAR].pict,
		&widget->colors[SELECT_NOT][COLOR_BG].chans,
		widget->winw - rightwid, 0,
		rightwid,
		STATUSBAR_HEIGHT(widget)
	);

	/* draw percentage */
	ctrlfnt_draw(
		widget->fontset,
		widget->layers[LAYER_STATUSBAR].pict,
		widget->colors[SELECT_NOT][COLOR_FG].pict,
		(XRectangle){
			.x = widget->winw - scrollwid - STATUSBAR_MARGIN(widget),
			.y = STATUSBAR_MARGIN(widget),
			.width = scrollwid,
			.height = widget->fonth,
		},
		scrollstr,
		scrolllen
	);

	/* draw metadata for highlighted item */
	if (widget->highlight > 0) {
		ctrlfnt_draw(
			widget->fontset,
			widget->layers[LAYER_STATUSBAR].pict,
			widget->colors[SELECT_NOT][COLOR_FG].pict,
			(XRectangle){
				.x = widget->winw - rightwid + STATUSBAR_MARGIN(widget),
				.y = STATUSBAR_MARGIN(widget),
				.width = statuswid,
				.height = widget->fonth,
			},
			status,
			statuslen
		);
	}

	etunlock(&widget->lock);
}

static void
loadresources(Widget *widget, const char *str)
{
	XrmDatabase xdb;
	char *value;
	enum Resource resource;
	char *endp;
	char *fontname = NULL;
	double d;
	double fontsize = 0.0;
	int changefont = false;

	if (str == NULL)
		return;
	if ((xdb = loadxdb(widget, str)) == NULL)
		return;
	for (resource = 0; resource < NRESOURCES; resource++) {
		value = getresource(
			xdb,
			widget->application.class,
			widget->application.name,
			widget->resources[resource].class,
			widget->resources[resource].name
		);
		if (value == NULL)
			continue;
		switch (resource) {
		case FACE_NAME:
			fontname = value;
			changefont = true;
			break;
		case FACE_SIZE:
			d = strtod(value, &endp);
			if (value[0] != '\0' && *endp == '\0' && d > 0.0 && d <= 100.0) {
				fontsize = d;
				changefont = true;
			}
			break;
		case NORMAL_BG:
			setcolor(widget, SELECT_NOT, COLOR_BG, value);
			break;
		case NORMAL_FG:
			setcolor(widget, SELECT_NOT, COLOR_FG, value);
			break;
		case SELECT_BG:
			setcolor(widget, SELECT_YES, COLOR_BG, value);
			break;
		case SELECT_FG:
			setcolor(widget, SELECT_YES, COLOR_FG, value);
			break;
		case OPACITY:
			setopacity(widget, value);
			break;
		case STATUSBAR:
		case BARSTATUS:
			widget->status_enable =  strcasecmp(value, "on") == 0
			                     || strcasecmp(value, "true") == 0
			                     || strcmp(value, "1") == 0;
			break;
		default:
			break;
		}
	}
	if (changefont)
		setfont(widget, fontname, fontsize);
	XrmDestroyDatabase(xdb);
}

static int
calcsize(Widget *widget, int w, int h)
{
	int ncols, nrows, ret;
	double d;

	ret = false;
	if (widget->winw == w && widget->winh == h)
		return false;
	widget->redraw = true;
	etlock(&widget->lock);
	ncols = widget->ncols;
	nrows = widget->nrows;
	if (w > 0 && h > 0) {
		widget->winw = w;
		widget->winh = h;
		widget->ydiff = 0;
	}
	if (widget->status_enable)
		widget->h = max(widget->winh - STATUSBAR_HEIGHT(widget), widget->itemh);
	else
		widget->h = widget->winh;
	widget->w = widget->winw;
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
		widget->pixw = widget->ncols * widget->itemw;
		widget->pixh = widget->nrows * widget->itemh;
		resetlayer(widget, LAYER_ICONS, widget->pixw, widget->pixh);
		resetlayer(widget, LAYER_SELALPHA, widget->pixw, widget->pixh);
		ret = true;
	}
	resetlayer(widget, LAYER_RECTALPHA, widget->w, widget->h);
	resetlayer(widget, LAYER_STATUSBAR, widget->winw, STATUSBAR_HEIGHT(widget));
	resetlayer(widget, LAYER_CANVAS, widget->winw, widget->winh);
	etunlock(&widget->lock);
	return ret;
}

static int
isbreakable(char c)
{
	return c == '.' || c == '-' || c == '_';
}

static void
drawname(Widget *widget, Picture color, int x, const char *text, int len)
{
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->namepict,
		&(XRenderColor){ 0 },
		0, 0, LABELWIDTH, widget->fonth
	);
	ctrlfnt_draw(
		widget->fontset,
		widget->namepict,
		color,
		(XRectangle){
			.x = x,
			.y = 0,
			.width = LABELWIDTH,
			.height = widget->fonth,
		},
		text,
		len
	);
}

static void
setrow(Widget *widget, int row)
{
	etlock(&widget->lock);
	widget->row = row;
	etunlock(&widget->lock);
}

static struct Icon *
geticon(Widget *widget, int index)
{
	extern size_t deffiletype;
	int i, cmp;

	for (i = 0; widget->icons[i].name != NULL; i++) {
		cmp = strcmp(
			widget->icons[i].name,
			widget->items[index][ITEM_TYPE]
		);
		if (cmp == 0) {
			return &widget->icons[i];
		}
	}
	return &widget->icons[deffiletype];
}

static void
drawicon(Widget *widget, int index, int x, int y)
{
	struct Icon *icon;
	Pixmap pix, mask;
	int xorigin;

	icon = geticon(widget, index);
	pix = icon->pix;
	mask = icon->mask;
	if (widget->thumbs != NULL && widget->thumbs[index] != NULL) {
		/* draw thumbnail */
		XPutImage(
			widget->display,
			widget->layers[LAYER_ICONS].pix,
			widget->gc,
			widget->thumbs[index]->img,
			0, 0,
			x + (widget->itemw - widget->thumbs[index]->w) / 2,
			y + (THUMBSIZE - widget->thumbs[index]->h) / 2,
			widget->thumbs[index]->w,
			widget->thumbs[index]->h
		);
	} else {
		/* draw icon */
		xorigin = x + (widget->itemw - THUMBSIZE) / 2;
		XChangeGC(
			widget->display,
			widget->gc,
			GCClipXOrigin | GCClipYOrigin | GCClipMask,
			&(XGCValues){
				.clip_x_origin = xorigin,
				.clip_y_origin = y,
				.clip_mask = mask,
			}
		);
		XCopyArea(
			widget->display,
			pix, widget->layers[LAYER_ICONS].pix,
			widget->gc,
			0, 0,
			THUMBSIZE, THUMBSIZE,
			xorigin,
			y
		);
		XChangeGC(
			widget->display,
			widget->gc,
			GCClipMask,
			&(XGCValues){ .clip_mask = None }
		);
	}
}

static void
drawlabel(Widget *widget, int index, int x, int y)
{
	Picture color;
	int i, sel;
	int textx, maxw;
	int textw, w, textlen, len;
	int extensionw, extensionlen;
	char *text, *extension;

	if (widget->issel != NULL && widget->issel[index])
		sel = SELECT_YES;
	else
		sel = SELECT_NOT;
	color = widget->colors[sel][COLOR_FG].pict;
	text = widget->items[index][ITEM_NAME];
	widget->nlines[index] = 1;
	textx = x + widget->itemw / 2 - LABELWIDTH / 2;
	extension = NULL;
	textw = 0;
	maxw = 0;
	textlen = 0;
	widget->linelen[index] = 0;
	for (i = 0; i < widget->nlines[index]; i++) {
		while (isspace(text[textlen]))
			textlen++;
		text += textlen;
		textlen = strlen(text);
		textw = ctrlfnt_width(widget->fontset, text, textlen);
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
				w = ctrlfnt_width(widget->fontset, text, len);
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
		drawname(
			widget,
			color,
			max(LABELWIDTH / 2 - textw / 2, 0),
			text, textlen
		);
		textw = min(textw, LABELWIDTH);
		widget->linelen[index] = max(widget->linelen[index], textw);
		XCopyArea(
			widget->display,
			widget->namepix, widget->layers[LAYER_ICONS].pix,
			widget->gc,
			0, 0,
			LABELWIDTH, widget->fonth,
			textx, y + widget->itemh - (NLINES - i + 0.5) * widget->fonth
		);
	}
	if (textw >= LABELWIDTH)
		extension = strrchr(text, '.');
	if (extension != NULL && extension[1] != '\0') {
		extensionlen = strlen(extension);
		extensionw = ctrlfnt_width(widget->fontset, extension, extensionlen);
		/* draw ellipsis */
		drawname(
			widget,
			color,
			0,
			ELLIPSIS, strlen(ELLIPSIS)
		);
		XCopyArea(
			widget->display,
			widget->namepix, widget->layers[LAYER_ICONS].pix,
			widget->gc,
			0, 0,
			widget->ellipsisw, widget->fonth,
			textx + textw - extensionw - widget->ellipsisw,
			y + widget->itemh - (NLINES + 1 - widget->nlines[index] + 0.5) * widget->fonth
		);

		/* draw extension */
		drawname(widget, color, 0, extension, extensionlen);
		XCopyArea(
			widget->display,
			widget->namepix, widget->layers[LAYER_ICONS].pix,
			widget->gc,
			0, 0,
			extensionw, widget->fonth,
			textx + textw - extensionw,
			y + widget->itemh - (NLINES + 1 - widget->nlines[index] + 0.5) * widget->fonth
		);
	}
	if (index == widget->highlight) {
		XRenderFillRectangle(
			widget->display,
			PictOpOver,
			widget->layers[LAYER_ICONS].pict,
			&widget->colors[sel][COLOR_FG].chans,
			x + widget->itemw / 2 - maxw / 2 - 1,
			y + widget->itemh - (NLINES + 0.5) * widget->fonth - 1 + i * widget->fonth + 1,
			maxw + 2, 1
		);
	}
	if (widget->issel != NULL && widget->issel[index]) {
		XRenderFillRectangle(
			widget->display,
			PictOpOverReverse,
			widget->layers[LAYER_ICONS].pict,
			&widget->colors[sel][COLOR_BG].chans,
			x + widget->itemw / 2 - maxw / 2 - 1,
			y + widget->itemh - (NLINES + 0.5) * widget->fonth - 1,
			maxw + 2, i * widget->fonth + 2
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
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_ICONS].pict,
		&(XRenderColor){ 0 },
		x, y,
		widget->itemw, widget->itemh
	);
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_SELALPHA].pict,
		&(XRenderColor){ 0 },
		x, y,
		widget->itemw, widget->itemh
	);
	drawicon(widget, index, x, y);
	drawlabel(widget, index, x, y);
	XRenderFillRectangle(
		widget->display,
		widget->issel[index] ? PictOpSrc : PictOpClear,
		widget->layers[LAYER_SELALPHA].pict,
		&(XRenderColor){
			.red   = 0xFFFF,
			.green = 0xFFFF,
			.blue  = 0xFFFF,
			.alpha = SEL_OPACITY,
		},
		x, y,
		widget->itemw,
		widget->itemh - (NLINES + 1) * widget->fonth
	);
done:
	etunlock(&widget->lock);
	widget->redraw = true;
}

static void
drawitems(Widget *widget)
{
	int i, n;

	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_ICONS].pict,
		&(XRenderColor){ 0 },
		0, 0, widget->pixw, widget->pixh
	);
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_SELALPHA].pict,
		&(XRenderColor){ 0 },
		0, 0, widget->pixw, widget->pixh
	);
	n = lastvisible(widget);
	for (i = widget->row * widget->ncols; i <= n; i++) {
		drawitem(widget, i);
	}
}

static void
commitdraw(Widget *widget)
{
	XRenderColor color = {
		.red   = widget->colors[SELECT_NOT][COLOR_BG].chans.red,
		.green = widget->colors[SELECT_NOT][COLOR_BG].chans.green,
		.blue  = widget->colors[SELECT_NOT][COLOR_BG].chans.blue,
		.alpha = widget->opacity,
	};

	etlock(&widget->lock);
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_CANVAS].pict,
		&(XRenderColor){ 0 },
		0, 0, widget->w, widget->h
	);
	XRenderComposite(
		widget->display,
		PictOpOver,
		widget->layers[LAYER_ICONS].pict,
		None,
		widget->layers[LAYER_CANVAS].pict,
		0, widget->ydiff - MARGIN,
		0, 0,
		widget->x0, 0,
		widget->w, widget->h
	);
	XRenderComposite(
		widget->display,
		PictOpAtop,
		widget->colors[SELECT_YES][COLOR_BG].pict,
		widget->layers[LAYER_SELALPHA].pict,
		widget->layers[LAYER_CANVAS].pict,
		0, 0,
		0, widget->ydiff - MARGIN,
		widget->x0, 0,
		widget->w, widget->h
	);
	XRenderComposite(
		widget->display,
		PictOpOver,
		widget->colors[SELECT_YES][COLOR_BG].pict,
		widget->layers[LAYER_RECTALPHA].pict,
		widget->layers[LAYER_CANVAS].pict,
		0, 0,
		0, 0,
		0, 0,
		widget->w, widget->h
	);
	if (widget->status_enable) {
		XRenderComposite(
			widget->display,
			PictOpOver,
			widget->layers[LAYER_STATUSBAR].pict,
			None,
			widget->layers[LAYER_CANVAS].pict,
			0, 0,
			0, 0,
			0, widget->h,
			widget->winw, STATUSBAR_HEIGHT(widget)
		);
	}
	XRenderFillRectangle(
		widget->display,
		PictOpOverReverse,
		widget->layers[LAYER_CANVAS].pict,
		&color,
		0, 0,
		widget->winw, widget->winh
	);
	XSetWindowBackgroundPixmap(
		widget->display,
		widget->window,
		widget->layers[LAYER_CANVAS].pix
	);
	XClearWindow(widget->display, widget->window);
	XFlush(widget->display);
	etunlock(&widget->lock);
}

static void
settitle(Widget *widget)
{
	struct {
		Atom prop, type;
	} titleprops[] = {
		/*
		 * There are two properties for the window title (aka
		 * window name): the newer UTF8-aware _NET_WM_NAME;
		 * and the older WM_NAME.
		 */
		{
			.prop = widget->atoms[_NET_WM_NAME],
			.type = widget->atoms[UTF8_STRING],
		},
		{
			.prop = XA_WM_NAME,
			.type = widget->atoms[TEXT],
		}
	};
	const char *titlesegmnt[] = {
		/* title segments */
		widget->title,
		" - ",
		widget->class
	};
	size_t i, j;

	for (i = 0; i < LEN(titleprops); i++) {
		for (j = 0; j < LEN(titlesegmnt); j++) {
			XChangeProperty(
				widget->display,
				widget->window,
				titleprops[i].prop,
				titleprops[i].type,
				8,
				j == 0 ? PropModeReplace : PropModeAppend,
				(unsigned char *)titlesegmnt[j],
				strlen(titlesegmnt[j])
			);
		}
	}
	XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_CONTROL_CWD],
		widget->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)widget->title,
		strlen(widget->title)
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
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_SCROLLER].pict,
		&widget->colors[SELECT_NOT][COLOR_FG].chans,
		0, 0,
		SCROLLER_SIZE,
		SCROLLER_SIZE
	);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_SCROLLER].pict,
		&widget->colors[SELECT_NOT][COLOR_BG].chans,
		1, 1,
		SCROLLER_SIZE - 2,
		SCROLLER_SIZE - 2
	);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_SCROLLER].pict,
		&widget->colors[SELECT_NOT][COLOR_FG].chans,
		2, y + 2,
		HANDLE_MAX_SIZE,
		widget->handlew
	);
	XSetWindowBackgroundPixmap(
		widget->display,
		widget->scroller,
		widget->layers[LAYER_SCROLLER].pix
	);
	XClearWindow(widget->display, widget->scroller);
}

static int
scroll(Widget *widget, int y)
{
	int prevhand, newhand;          /* position of the scroller handle */
	int prevrow, newrow;

	if (y == 0)
		return false;
	if (ALL_ROWS(widget) + 1 < widget->nrows)
		return false;
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
	if (prevhand != newhand) {
		drawscroller(widget, newhand);
	}
	if (prevrow != newrow) {
		drawstatusbar(widget);
		return true;
	}
	return false;
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
		if (c < '0' || c > '9')
			break;
		n = c - '0';
		size *= 10;
		size += n;
	}
	if (i == 0 || (i == 3 && c >= '0' && c <= '9'))
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
getitemundercursor(Widget *widget, int x, int y)
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
	widget->selctx = NULL;
}

static void
disowndnd(Widget *widget)
{
	if (widget->dragctx == NULL)
		return;
	ctrlsel_dnddisown(widget->dragctx);
	widget->dragctx = NULL;
}

static void
ownprimary(Widget *widget, Time time)
{
	struct Selection *sel;
	size_t i, j;

	if (widget->sel == NULL)
		return;
	disownprimary(widget);
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
	widget->selctx = ctrlsel_setowner(
		widget->display,
		widget->window,
		XA_PRIMARY,
		time,
		0,
		widget->targets,
		TARGET_LAST
	);
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
	(void)XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_CONTROL_STATUS],
		widget->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)"",
		0
	);
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
	char *status;

	if (widget->highlight == index)
		return;
	prevhili = widget->highlight;
	widget->highlight = index;
	if (redraw)
		drawitem(widget, index);
	/* we still have to redraw the previous one */
	drawitem(widget, prevhili);
	drawstatusbar(widget);
	if (widget->highlight > 0) {
		status = getitemstatus(widget, widget->highlight);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			widget->atoms[_CONTROL_STATUS],
			widget->atoms[UTF8_STRING],
			8,
			PropModeReplace,
			(unsigned char *)widget->items[widget->highlight][ITEM_NAME],
			strlen(widget->items[widget->highlight][ITEM_NAME])
		);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			widget->atoms[_CONTROL_STATUS],
			widget->atoms[UTF8_STRING],
			8,
			PropModeAppend,
			(unsigned char *)" - ",
			3
		);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			widget->atoms[_CONTROL_STATUS],
			widget->atoms[UTF8_STRING],
			8,
			PropModeAppend,
			(unsigned char *)status,
			strlen(status)
		);
	} else {
		(void)XChangeProperty(
			widget->display,
			widget->window,
			widget->atoms[_CONTROL_STATUS],
			widget->atoms[UTF8_STRING],
			8,
			PropModeReplace,
			(unsigned char *)"",
			0
		);
	}
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
		selectitem(widget, i, true, 0);
	}
}

static void
unselectitems(Widget *widget)
{
	while (widget->sel) {
		selectitem(widget, widget->sel->index, false, 0);
	}
}

static int
mouse1click(Widget *widget, XButtonPressedEvent *ev)
{
	int prevhili, index;

	index = getitemundercursor(widget, ev->x, ev->y);
	if (index > 0 && widget->issel[index] != NULL)
		return index;
	if (!(ev->state & (ControlMask | ShiftMask)))
		unselectitems(widget);
	if (index < 0) {
		return index;
	}
	/*
	 * If index != 0, there's no need to ask highlight() to redraw the item,
	 * as selectitem() or selectitems() will already redraw it.
	 */
	prevhili = widget->highlight;
	highlight(widget, index, (index == 0));
	if (prevhili != -1 && ev->state & ShiftMask)
		selectitems(widget, widget->highlight, prevhili);
	else
		selectitem(widget, widget->highlight, ((ev->state & ControlMask) ? widget->issel[widget->highlight] == NULL : true), false);
	ownprimary(widget, ev->time);
	return index;
}

static int
mouse3click(Widget *widget, int x, int y)
{
	int index;

	index = getitemundercursor(widget, x, y);
	if (index != -1) {
		if (widget->issel[index] == NULL) {
			highlight(widget, index, false);
			unselectitems(widget);
			selectitem(widget, index, true, false);
		} else {
			highlight(widget, index, true);
		}
	}
	return index;
}

static void
rectclear(Widget *widget)
{
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_RECTALPHA].pict,
		&(XRenderColor){ 0 },
		0, 0, widget->w, widget->h
	);
}

static void
rectdraw(Widget *widget, int row, int ydiff, int x0, int y0, int x, int y)
{
	int w, h;

	rectclear(widget);
	if (row < widget->row) {
		y0 -= min(widget->row - row, widget->nrows) * widget->itemh;
	} else if (row > widget->row) {
		y0 += min(row - widget->row, widget->nrows) * widget->itemh;
	}
	y0 += ydiff - widget->ydiff;
	w = (x0 > x) ? x0 - x : x - x0;
	h = (y0 > y) ? y0 - y : y - y0;
	x = min(x0, x);
	y = min(y0, y);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_RECTALPHA].pict,
		&(XRenderColor){
			.red   = 0xFFFF,
			.green = 0xFFFF,
			.blue  = 0xFFFF,
			.alpha = 0xFFFF,
		},
		x, y,
		w + 1, h + 1
	);
	XRenderFillRectangle(
		widget->display,
		PictOpSrc,
		widget->layers[LAYER_RECTALPHA].pict,
		&(XRenderColor){
			.red   = 0xFFFF,
			.green = 0xFFFF,
			.blue  = 0xFFFF,
			.alpha = RECT_OPACITY,
		},
		x + 1,
		y + 1,
		max(w - 1, 0),
		max(h - 1, 0)
	);
}

static void
pixelstocolrow(Widget *widget, int visrow, int x, int y, int *col, int *row)
{
	int i, w, h;

	/* get column and row given position in pixels */
	w = x / widget->itemw;
	h = y / widget->itemh;
	visrow *= widget->ncols;
	i = visrow + h * widget->ncols + w;
	*col = i % widget->ncols;
	*row = i / widget->ncols;
}

static int
rectselect(Widget *widget, int srcrow, int srcydiff, int x0, int y0, int x1, int y1)
{
	int sel, row, col, tmp, i;
	int changed = false;
	int col0, col1, row0, row1;

	/* normalize source and destination points to geometry of icon area */
	x0 -= widget->x0;
	x1 -= widget->x0;
	y0 += srcydiff - MARGIN;
	y1 += widget->ydiff - MARGIN;
	x0 = min(widget->itemw * widget->ncols - 1, max(0, x0));
	x1 = min(widget->itemw * widget->ncols - 1, max(0, x1));
	y0 = min(widget->itemh * widget->nrows - 1, max(0, y0));
	y1 = min(widget->itemh * widget->nrows - 1, max(0, y1));

	/* convert position in pixels into column and row */
	pixelstocolrow(widget, srcrow, x0, y0, &col0, &row0);
	pixelstocolrow(widget, widget->row, x1, y1, &col1, &row1);

	/* make x0,y0 the top-left and x1,y1 the bottom-right values */
	if (x0 > x1) {
		tmp = x0, x0 = x1, x1 = tmp;
		tmp = col0, col0 = col1, col1 = tmp;
	}
	if (y0 > y1) {
		tmp = y0, y0 = y1, y1 = tmp;
		tmp = row0, row0 = row1, row1 = tmp;
	}

	/* make position relative to window be relative to item bounding box */
	x0 %= widget->itemw;
	x1 %= widget->itemw;
	y0 %= widget->itemh;
	y1 %= widget->itemh;

	/* select (unselect) items inside (outside) rectangle */
	for (i = firstvisible(widget); i <= lastvisible(widget); i++) {
		sel = true;
		row = i / widget->ncols;
		col = i % widget->ncols;
		if (col < col0 || col > col1 || row < row0 || row > row1) {
			/* item is out of selection */
			sel = false;
		} else if ((col == col0 && x0 > ITEM_WIDTH - ICON_MARGIN) ||
		           (col == col1 && x1 < ICON_MARGIN)) {
			/* item is on a column at edge of selection */
			sel = false;
		} else if ((row == row0 && y0 > THUMBSIZE) ||
		           (row == row1 && y1 < 0)) {
			/* item is on a row at edge of selection */
			sel = false;
		}
		if (!sel && (widget->issel[i] == NULL || widget->issel[i]->index > 0))
			continue;
		if (sel && widget->issel[i] != NULL && widget->issel[i]->index > 0)
			selectitem(widget, i, false, false);
		selectitem(widget, i, sel, true);
		changed = true;
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
		drawstatusbar(widget);
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
	struct Icon *icon;
	Window win;
	GC gc;
	unsigned long opacity;
	Pixmap pix, mask;
	int xroot, yroot, w, h;

	if (index <= 0)
		return None;
	if (!querypointer(widget, widget->root, &xroot, &yroot, NULL))
		return None;
	w = h = THUMBSIZE;
	if (widget->thumbs[index] != NULL) {
		w = widget->thumbs[index]->w;
		h = widget->thumbs[index]->h;
	}
	win = XCreateWindow(
		widget->display,
		widget->root,
		xroot, yroot, w, h, 0,
		widget->depth, InputOutput, widget->visual,
		CWBackPixel | CWOverrideRedirect| CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.background_pixel = 0,
			.border_pixel = 0,
			.colormap = widget->colormap,
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
		icon = geticon(widget, index);
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
		XCopyArea(widget->display, icon->mask, mask, gc, 0, 0, w, h, 0, 0);
		XShapeCombineMask(widget->display, win, ShapeBounding, 0, 0, mask, ShapeSet);
		XFreePixmap(widget->display, mask);
		XFreeGC(widget->display, gc);
		XSetWindowBackgroundPixmap(widget->display, win, icon->pix);
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
gettextprop(Widget *widget, Window window, Atom prop, Bool delete)
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
		window,
		prop,
		0L,
		0x1FFFFFFF,
		delete,
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

static int
getgeometry(Widget *widget, XRectangle *rect)
{
	unsigned int width, height;
	int x, y, flags, retval;
	XrmDatabase xdb;
	char *str, *geometry;

	*rect = DEF_SIZE;
	retval = 0;
	if ((str = XResourceManagerString(widget->display)) == NULL)
		return 0;
	if ((xdb = loadxdb(widget, str)) == NULL)
		return 0;
	geometry = getresource(
		xdb,
		widget->application.class,
		widget->application.name,
		widget->resources[GEOMETRY].class,
		widget->resources[GEOMETRY].name
	);
	if (geometry == NULL)
		goto done;
	flags = XParseGeometry(geometry, &x, &y, &width, &height);
	if (FLAG(flags, WidthValue) && width > THUMBSIZE) {
		rect->width = width;
		retval |= USSize;
	}
	if (FLAG(flags, HeightValue) && height > THUMBSIZE) {
		rect->height = height;
		retval |= USSize;
	}
	if (FLAG(flags, XValue)) {
		if (FLAG(flags, XNegative)) {
			x += DisplayWidth(widget->display, widget->screen);
			x -= rect->width;
		}
		rect->x = x;
		retval |= USPosition;
	}
	if (FLAG(flags, YValue)) {
		if (FLAG(flags, YNegative)) {
			y += DisplayHeight(widget->display, widget->screen);
			y -= rect->height;
		}
		rect->y = y;
		retval |= USPosition;
	}
done:
	XrmDestroyDatabase(xdb);
	return retval;
}

static int
scrollerpos(Widget *widget)
{
	int pos;

	(void)querypointer(widget, widget->scroller, NULL, &pos, NULL);
	if (pos > SCROLLER_SIZE)
		return pos - SCROLLER_SIZE;
	if (pos < 0)
		return pos;
	return 0;
}

static Window
createwindow(Widget *widget, Window parent, XRectangle rect, long eventmask)
{
	return XCreateWindow(
		widget->display,
		parent,
		rect.x,
		rect.y,
		rect.width,
		rect.height,
		0,
		widget->depth,
		InputOutput,
		widget->visual,
		WINDOW_MASK,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.background_pixel = 0,
			.colormap = widget->colormap,
			.event_mask = eventmask,
		}
	);
}

/*
 * event filters
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
		selectitem(widget, widget->highlight, widget->issel[widget->highlight] == NULL, false);
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
		redrawall = true;
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
			redrawall = true;
		} else if (widget->row == index / widget->ncols) {
			widget->ydiff = 0;
			widget->redraw = true;
		}
draw:
		previtem = widget->highlight;
		highlight(widget, index, true);
		if (xev->state & ShiftMask)
			selectitems(widget, index, previtem);
		else if (xev->state & ControlMask)
			selectitem(widget, index, true, 0);
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
	char *str;

	if (widget->selctx != NULL) {
		switch (ctrlsel_send(widget->selctx, ev)) {
		case CTRLSEL_LOST:
			unselectitems(widget);
			disownprimary(widget);
			goto done;
		case CTRLSEL_INTERNAL:
			goto done;
		default:
			break;
		}
	}
	if (widget->dragctx != NULL) {
		switch (ctrlsel_dndsend(widget->dragctx, ev)) {
		case CTRLSEL_SENT:
			widget->dragctx = NULL;
			goto done;
		case CTRLSEL_LOST:
			disowndnd(widget);
			goto done;
		case CTRLSEL_INTERNAL:
			goto done;
		default:
			break;
		}
	}
	switch (ctrlsel_dndreceive(widget->dropctx, ev)) {
	case CTRLSEL_RECEIVED:
		FREE(widget->droptarget.buffer);
		goto done;
	case CTRLSEL_INTERNAL:
		goto done;
	default:
		break;
	}
	widget->redraw = false;
	switch (ev->type) {
	case ClientMessage:
		if (ev->xclient.message_type == widget->atoms[WM_PROTOCOLS] &&
		    (Atom)ev->xclient.data.l[0] == widget->atoms[WM_DELETE_WINDOW])
			return WIDGET_CLOSE;
		return WIDGET_NONE;
	case ConfigureNotify:
		if (calcsize(widget, ev->xconfigure.width, ev->xconfigure.height)) {
			if (widget->row >= widget->nscreens)
				setrow(widget, widget->nscreens - 1);
			drawitems(widget);
		}
		drawstatusbar(widget);
		break;
	case PropertyNotify:
		if (ev->xproperty.state != PropertyNewValue)
			return WIDGET_NONE;
		if (ev->xproperty.window == widget->root &&
		    ev->xproperty.atom == XA_RESOURCE_MANAGER) {
			str = gettextprop(
				widget,
				widget->root,
				XA_RESOURCE_MANAGER,
				False
			);
			if (str == NULL)
				return WIDGET_NONE;
			loadresources(widget, str);
			FREE(str);
			drawitems(widget);
			widget->redraw = true;
		} else if (ev->xproperty.window == widget->window &&
		           ev->xproperty.atom == widget->atoms[_CONTROL_GOTO]) {
			FREE(widget->lasttext);
			widget->lastprop = widget->atoms[_CONTROL_GOTO];
			widget->lasttext = gettextprop(
				widget,
				widget->window,
				widget->atoms[_CONTROL_GOTO],
				True
			);
		}
		break;
	default:
		return WIDGET_NONE;
	}
done:
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
 * event loops
 */

static int
nextevent(Widget *widget, XEvent *ev, int timeout)
{
	struct pollfd pfd = {
		.fd = widget->fd,
		.events = POLLIN,
	};
	struct timespec ts;
	int elapsed;

	for (;;) {
		if (XPending(widget->display) > 0)
			goto done;
		if (timeout > 0 && clock_gettime(CLOCK_MONOTONIC, &ts) != -1) {
			elapsed = (ts.tv_sec - widget->time.tv_sec) * 1000;
			elapsed += ts.tv_nsec / 1000000;
			elapsed -= widget->time.tv_nsec / 1000000;
			if (elapsed < timeout) {
				timeout -= elapsed;
			} else {
				timeout = 0;
			}
		}
		switch (poll(&pfd, 1, timeout)) {
		case -1:
			if (errno == EINTR)
				continue;
			goto done;
		case 0:
			return false;
		default:
			goto done;
		}
	}
done:
	(void)XNextEvent(widget->display, ev);
	return true;
}

static WidgetEvent
scrollmode(Widget *widget, int x, int y)
{
	XEvent ev;
	int grabpos, pos, left;
	struct timespec ts;

	grabpos = widget->handlew / 2;             /* we grab the handle in its middle */
	drawscroller(widget, gethandlepos(widget));
	XMoveWindow(widget->display, widget->scroller, x - SCROLLER_SIZE / 2 - 1, y - SCROLLER_SIZE / 2 - 1);
	XMapRaised(widget->display, widget->scroller);
	left = false;
	for (;;) {
		if (!nextevent(widget, &ev, SCROLL_TIME)) {
			if ((pos = scrollerpos(widget)) != 0) {
				if (scroll(widget, pos))
					drawitems(widget);
				commitdraw(widget);
			}
			if (clock_gettime(CLOCK_MONOTONIC, &ts) != -1) {
				widget->time = ts;
			}
			continue;
		}
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
				left = true;
			}
			break;
		case ButtonRelease:
			if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
				if (scroll(widget, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP)))
					drawitems(widget);
				widget->redraw = true;
			} else if (left) {
				goto done;
			}
			break;
		case ButtonPress:
			if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
				break;
			if (ev.xbutton.button != Button1)
				goto done;
			if (ev.xbutton.window == widget->window)
				goto done;
			if (ev.xbutton.window == widget->scroller) {
				left = true;
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
	XUnmapWindow(widget->display, widget->scroller);
	return WIDGET_NONE;
}

static WidgetEvent
selmode(Widget *widget, Time lasttime, int shift, int clickx, int clicky)
{
	XEvent ev;
	int rectrow, rectydiff, ownsel, pos;

	rectrow = widget->row;
	rectydiff = widget->ydiff;
	ownsel = false;
	if (!shift)
		unselectitems(widget);
	for (;;) {
		if (!nextevent(widget, &ev, SCROLL_TIME)) {
			(void)querypointer(
				widget,
				widget->window,
				&ev.xmotion.x,
				&ev.xmotion.y,
				NULL
			);
			pos = ev.xmotion.y;
			if (pos > widget->h)
				pos -= widget->h;
			else if (pos >= 0)
				continue;
			if (pos > 0)
				pos += SCROLL_STEP;
			pos /= SCROLL_STEP;
			if (pos != 0) {
				if (scroll(widget, pos))
					drawitems(widget);
				widget->redraw = true;
				goto motion;
			}
			continue;
		}
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
			lasttime = ev.xmotion.time;
motion:
			rectdraw(widget, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y);
			if (rectselect(widget, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y))
				ownsel = true;
			commitdraw(widget);
			break;
		}
		endevent(widget);
	}
done:
	rectclear(widget);
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
		&widget->dragctx
	);
	if (dragwin != None)
		XDestroyWindow(widget->display, dragwin);
	if (state == CTRLSEL_ERROR) {
		warnx("could not perform drag-and-drop");
	} else if (state == CTRLSEL_DROPSELF) {
		querypointer(widget, widget->window, &x, &y, &mask);
		clicki = getitemundercursor(widget, x, y);
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
	} else if (state == CTRLSEL_DROPOTHER) {
		return WIDGET_INTERNAL;
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
		switch (ctrlsel_dndreceive(widget->dropctx, &ev)) {
		case CTRLSEL_RECEIVED:
			if (widget->droptarget.buffer == NULL)
				return WIDGET_INTERNAL;
			*text = (char *)widget->droptarget.buffer;
			*nitems = 1;
			querypointer(widget, widget->window, &x, &y, NULL);
			selitems[0] = getitemundercursor(widget, x, y);
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
				widget->redraw = true;
			} else if (ev.xbutton.button == Button2) {
				state = scrollmode(widget, ev.xmotion.x, ev.xmotion.y);
				if (state != WIDGET_NONE)
					return state;
			} else if (ev.xbutton.button == Button3) {
				if (mouse3click(widget, ev.xbutton.x, ev.xbutton.y) > 0)
					*nitems = fillselitems(widget, selitems, -1);
				widget->redraw = true;
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
			if (clicki == 0)
				break;
			if (clicki == -1)
				state = selmode(widget, ev.xmotion.time, ev.xmotion.state & (ShiftMask | ControlMask), clickx, clicky);
			else
				state = dragmode(widget, ev.xmotion.time, clicki, selitems, nitems);
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
 * widget initializers
 */

static int
initxconn(Widget *widget, struct Options *options)
{
	static char *atomnames[] = {
#define X(atom, name) [atom] = name ? name : #atom,
		ATOMS
#undef  X
	};
	static struct {
		const char *class, *name;
	} resourceids[] = {
#define X(res, s1, s2) [res] = { .class = s1, .name = s2, },
		RESOURCES
#undef  X
	};
	int i;

	(void)options;
	ctrlfnt_init();
	if (!XInitThreads()) {
		warnx("could not initialize support for threads");
		return RETURN_FAILURE;
	}
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale()) {
		warnx("could not set locale");
	}
	if ((widget->display = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		return RETURN_FAILURE;
	}
	widget->screen = DefaultScreen(widget->display);
	widget->root = RootWindow(widget->display, widget->screen);
	widget->fd = XConnectionNumber(widget->display);
	if (fcntl(widget->fd, F_SETFD, FD_CLOEXEC) == RETURN_FAILURE) {
		warnx("could not set connection to X server to close on exec");
		return RETURN_FAILURE;
	}
	if (!XInternAtoms(widget->display, atomnames,
	                  NATOMS, False, widget->atoms)) {
		warnx("could not intern X atoms");
		return RETURN_FAILURE;
	}
	XrmInitialize();
	widget->application.class = XrmPermStringToQuark(options->class);
	widget->application.name = XrmPermStringToQuark(options->name);
	for (i = 0; i < NRESOURCES; i++) {
		widget->resources[i].class = XrmPermStringToQuark(resourceids[i].class);
		widget->resources[i].name = XrmPermStringToQuark(resourceids[i].name);
	}
	return RETURN_SUCCESS;
}

static int
initvisual(Widget *widget, struct Options *options)
{
	XVisualInfo vinfo;
	Colormap colormap;
	int success;

	(void)options;
	success = XMatchVisualInfo(
		widget->display,
		widget->screen,
		32,             /* preferred depth */
		TrueColor,
		&vinfo
	);
	colormap = success ? XCreateColormap(
		widget->display,
		widget->root,
		vinfo.visual,
		AllocNone
	) : None;
	if (success && colormap != None) {
		widget->colormap = colormap;
		widget->visual = vinfo.visual;
		widget->depth = vinfo.depth;
	} else {
		widget->colormap = DefaultColormap(
			widget->display,
			widget->screen
		);
		widget->visual = DefaultVisual(
			widget->display,
			widget->screen
		);
		widget->depth = DefaultDepth(
			widget->display,
			widget->screen
		);
	}
	widget->format = XRenderFindVisualFormat(
		widget->display,
		widget->visual
	);
	if (widget->format == NULL)
		goto error;
	widget->alpha_format = XRenderFindStandardFormat(
		widget->display,
		PictStandardA8
	);
	if (widget->alpha_format == NULL)
		goto error;
	return RETURN_SUCCESS;
error:
	warnx("could not find XRender visual format");
	return RETURN_FAILURE;
}

static int
initwindow(Widget *widget, struct Options *options)
{
	XRectangle geometry;
	pid_t pid = getpid();
	int sizehints;
	char buf[16]; /* 16: enough for digits in 32-bit number + final '\0' */

	sizehints = getgeometry(widget, &geometry);
	widget->winw = geometry.width,
	widget->winh = geometry.height,
	widget->window = createwindow(
		widget,
		widget->root,
		geometry,
		EVENT_MASK
	);
	if (widget->window == None) {
		warnx("could not create window");
		return RETURN_FAILURE;
	}
	widget->scroller = createwindow(
		widget,
		widget->window,
		(XRectangle){.width = SCROLLER_SIZE, .height = SCROLLER_SIZE},
		ButtonReleaseMask | ButtonPressMask | PointerMotionMask
	);
	if (widget->scroller == None) {
		warnx("could not create window");
		return RETURN_FAILURE;
	}
	widget->gc = XCreateGC(widget->display, widget->window, 0, NULL);
	if (widget->gc == NULL) {
		warnx("could not create graphics context");
		return RETURN_FAILURE;
	}
	(void)XSetWMProtocols(
		widget->display,
		widget->window,
		&widget->atoms[WM_DELETE_WINDOW],
		1
	);
	(void)XmbSetWMProperties(
		widget->display,
		widget->window,
		options->class,
		options->class,
		options->argv,
		options->argc,
		&(XSizeHints){ .flags = sizehints },
		NULL,
		&(XClassHint){
			.res_class = (char *)options->class,
			.res_name = (char *)options->name,
		}
	);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_NET_WM_NAME],
		widget->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)options->class,
		strlen(options->class)
	);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM,
		32,
		PropModeReplace,
		(unsigned char *)&widget->atoms[_NET_WM_WINDOW_TYPE_NORMAL],
		1
	);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_NET_WM_ICON],
		XA_CARDINAL,
		32,
		PropModeReplace,
		(unsigned char *)winicon_data,
		winicon_size
	);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		widget->atoms[_NET_WM_PID],
		XA_CARDINAL,
		32,
		PropModeReplace,
		(unsigned char *)&pid,
		1
	);
	(void)snprintf(buf, LEN(buf), "%lu", (unsigned long)widget->window);
	if (setenv("WINDOWID", buf, true) == RETURN_FAILURE)
		warn("setenv");
	return RETURN_SUCCESS;
}

static int
inittheme(Widget *widget, struct Options *options)
{
	int i, j;

	(void)options;
	resetlayer(widget, LAYER_SCROLLER, SCROLLER_SIZE, SCROLLER_SIZE);
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			widget->colors[i][j].pix = XCreatePixmap(
				widget->display,
				widget->window,
				1, 1,
				widget->depth
			);
			if (widget->colors[i][j].pix == None) {
				goto error;
			}
			widget->colors[i][j].pict = XRenderCreatePicture(
				widget->display,
				widget->colors[i][j].pix,
				widget->format,
				CPRepeat,
				&(XRenderPictureAttributes){
					.repeat = RepeatNormal,
				}
			);
			if (widget->colors[i][j].pict == None) {
				goto error;
			}
			XRenderFillRectangle(
				widget->display,
				PictOpSrc,
				widget->colors[i][j].pict,
				&widget->colors[i][j].chans,
				0, 0, 1, 1
			);
		}
	}
	loadresources(widget, XResourceManagerString(widget->display));
	if (widget->fontset == NULL)
		setfont(widget, NULL, 0.0);
	if (widget->fontset == NULL) {
		warnx("could not load any font");
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
error:
	warnx("could not create XRender picture");
	return RETURN_FAILURE;
}

static int
initicons(Widget *widget, struct Options *options)
{
	extern size_t ndeffileicons;
	extern char *deffiletypes[];
	extern struct {int type; char **xpm;} deffileicons[];
	int success, retval, i;

	(void)options;
	widget->nicons = ndeffileicons;
	if ((widget->icons = calloc(widget->nicons, sizeof(*widget->icons))) == NULL) {
		warn("calloc");
		return RETURN_FAILURE;
	}
	retval = RETURN_SUCCESS;
	for (i = 0; i < widget->nicons; i++) {
		widget->icons[i].name = deffiletypes[deffileicons[i].type];
		widget->icons[i].pix  = None;
		widget->icons[i].mask = None;
		success = pixmapfromdata(
			widget,
			deffileicons[i].xpm,
			&widget->icons[i].pix,
			&widget->icons[i].mask
		) != RETURN_FAILURE;
		if (!success) {
			warnx("%s: could not open pixmap", widget->icons[i].name);
			retval = RETURN_FAILURE;
		}
	}
	return retval;
}

static int
initselection(Widget *widget, struct Options *options)
{
	(void)options;
	ctrlsel_filltarget(
		widget->atoms[TEXT_URI_LIST],
		widget->atoms[TEXT_URI_LIST],
		0, NULL, 0,
		&widget->droptarget
	);
	widget->dropctx = ctrlsel_dndwatch(
		widget->display,
		widget->window,
		CTRLSEL_COPY | CTRLSEL_MOVE | CTRLSEL_LINK | CTRLSEL_ASK,
		&widget->droptarget,
		1
	);
	if (widget->dropctx == NULL) {
		warnx("could not watch drag-and-drop selection");
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
}

static int
initmisc(Widget *widget, struct Options *options)
{
	/*
	 * No need to check for errors here.
	 *
	 * - If cursor loading function returns None, ignore;
	 *   XDefineCursor(3) just uses the default cursor when passed
	 *   None.
	 *
	 * - If selecting PropertyNotify on root window fail, ignore;
	 *   we should not be able to reload theme on the fly, but it
	 *   is not important for the widget to work correctly.
	 */
	(void)options;
	widget->busycursor = XCreateFontCursor(widget->display, XC_watch);
	(void)XSelectInput(widget->display, widget->root, PropertyChangeMask);
	return RETURN_SUCCESS;
}

/*
 * public routines
 */

void
widget_free(Widget *widget)
{
	int i, j;

	if (widget == NULL)
		return;
	cleanwidget(widget);
	if (widget->dropctx != NULL)
		ctrlsel_dndclose(widget->dropctx);
	for (i = 0; i < widget->nicons; i++) {
		if (widget->icons[i].pix != None) {
			XFreePixmap(widget->display, widget->icons[i].pix);
		}
		if (widget->icons[i].mask != None) {
			XFreePixmap(widget->display, widget->icons[i].mask);
		}
	}
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			if (widget->colors[i][j].pict != None) {
				XRenderFreePicture(
					widget->display,
					widget->colors[i][j].pict
				);
			}
			if (widget->colors[i][j].pix != None) {
				XFreePixmap(
					widget->display,
					widget->colors[i][j].pix
				);
			}
		}
	}
	for (i = 0; i < LAYER_LAST; i++) {
		if (widget->layers[i].pict != None) {
			XRenderFreePicture(
				widget->display,
				widget->layers[i].pict
			);
		}
		if (widget->layers[i].pix != None) {
			XFreePixmap(
				widget->display,
				widget->layers[i].pix
			);
		}
	}
	FREE(widget->icons);
	if (widget->busycursor != None)
		XFreeCursor(widget->display, widget->busycursor);
	if (widget->fontset != NULL)
		ctrlfnt_free(widget->fontset);
	if (widget->namepix != None)
		XFreePixmap(widget->display, widget->namepix);
	if (widget->namepict != None)
		XRenderFreePicture(widget->display, widget->namepict);
	if (widget->scroller != None)
		XDestroyWindow(widget->display, widget->scroller);
	if (widget->window != None)
		XDestroyWindow(widget->display, widget->window);
	if (widget->colormap != None)
		XFreeColormap(widget->display, widget->colormap);
	if (widget->gc != NULL)
		XFreeGC(widget->display, widget->gc);
	if (widget->display != NULL)
		XCloseDisplay(widget->display);
	FREE(widget);
	ctrlfnt_term();
}

Widget *
widget_create(const char *class, const char *name, int argc, char *argv[], const char *resources[])
{
	Widget *widget;
	size_t i;
	struct Options options = {
		.class = class,
		.name = name,
		.argc = argc,
		.argv = argv,
	};
	int (*initsteps[])(Widget *, struct Options *) = {
		initxconn,
		initvisual,
		initwindow,
		inittheme,
		initicons,
		initselection,
		initmisc,
	};

	if ((widget = malloc(sizeof(*widget))) == NULL) {
		warn("malloc");
		return NULL;
	}
	*widget = (Widget){
		.class = class,
		.colors[SELECT_NOT][COLOR_BG].chans = DEF_COLOR_BG,
		.colors[SELECT_NOT][COLOR_FG].chans = DEF_COLOR_FG,
		.colors[SELECT_YES][COLOR_BG].chans = DEF_COLOR_SELBG,
		.colors[SELECT_YES][COLOR_FG].chans = DEF_COLOR_SELFG,
		.status_enable = DEF_STATUSBAR,
		.opacity = DEF_OPACITY,
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.highlight = -1,
		.itemw = ITEM_WIDTH,
		.cliresources = resources,
	};
	for (i = 0; i < LEN(initsteps); i++) {
		if ((*initsteps[i])(widget, &options) == RETURN_FAILURE) {
			widget_free(widget);
			return NULL;
		}
	}
	return widget;
}

int
widget_set(Widget *widget, const char *title, char **items[], size_t nitems, Scroll *scrl)
{
	size_t i;

	XUndefineCursor(widget->display, widget->window);
	cleanwidget(widget);
	widget->items = items;
	widget->nitems = nitems;
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
	if (widget->selbufsiz > 0 && (widget->selbuf = malloc(widget->selbufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	widget->uribufsiz = widget->selbufsiz + (nitems * 8);         /* 8 for "file://\r" */
	if (widget->uribufsiz > 0 && (widget->uribuf = malloc(widget->uribufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	if (widget->uribufsiz > 0 && (widget->dndbuf = malloc(widget->uribufsiz)) == NULL) {
		warn("malloc");
		goto error;
	}
	widget->thumbhead = NULL;
	settitle(widget);
	drawitems(widget);
	drawstatusbar(widget);
	commitdraw(widget);
	return RETURN_SUCCESS;
error:
	cleanwidget(widget);
	return RETURN_FAILURE;
}

void
widget_map(Widget *widget)
{
	XMapWindow(widget->display, widget->window);
}

char *
widget_gettypes(Widget *widget)
{
	XrmDatabase xdb;
	char *str, *value, *p;

	if ((str = XResourceManagerString(widget->display)) == NULL)
		return NULL;
	if ((xdb = loadxdb(widget, str)) == NULL)
		return NULL;
	value = getresource(
		xdb,
		widget->application.class,
		widget->application.name,
		widget->resources[ICONS].class,
		widget->resources[ICONS].name
	);
	if (value == NULL)
		p = NULL;
	else
		p = strdup(value);
	XrmDestroyDatabase(xdb);
	return p;
}

WidgetEvent
widget_poll(Widget *widget, int *selitems, int *nitems, Scroll *scrl, char **text)
{
	XEvent ev;
	int retval;

	*text = NULL;
	*nitems = 0;
	widget->droptarget.buffer = NULL;
	while (widget->start && XPending(widget->display) > 0) {
		(void)XNextEvent(widget->display, &ev);
		if (processevent(widget, &ev) == WIDGET_CLOSE) {
			endevent(widget);
			return WIDGET_CLOSE;
		}
	}
	widget->start = true;
	if ((retval = checklastprop(widget, text)) == WIDGET_NONE)
		retval = mainmode(widget, selitems, nitems, text);
	endevent(widget);
	scrl->ydiff = widget->ydiff;
	scrl->row = widget->row;
	return retval;
}

void
widget_thumb(Widget *widget, char *path, int item)
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
