#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#include <control/selection.h>
#include <control/dragndrop.h>
#include <control/font.h>

#include "icons.h"
#include "util.h"
#include "widget.h"

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
	X(_XEMBED, NULL)                        \
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
	X(STATUSBAR, "StatusBarEnable",  "statusBarEnable")   \
	X(BARSTATUS, "EnableStatusBar",  "enableStatusBar")   \
	X(OPACITY,   "Opacity",          "opacity")

#define STATUSBAR_HEIGHT(w) ((w)->fonth * 2)
#define STATUSBAR_MARGIN(w) ((w)->fonth / 2)

enum {
	XEMBED_EMBEDDED_NOTIFY,
	XEMBED_WINDOW_ACTIVATE,
	XEMBED_WINDOW_DEACTIVATE,
	XEMBED_REQUEST_FOCUS,
	XEMBED_FOCUS_IN,
	XEMBED_FOCUS_OUT,
	XEMBED_FOCUS_NEXT,
	XEMBED_FOCUS_PREV,
	XEMBED_GRAB_KEY,
	XEMBED_UNGRAB_KEY,
	XEMBED_MODALITY_ON,
	XEMBED_MODALITY_OFF,
	XEMBED_REGISTER_ACCELERATOR,
	XEMBED_UNREGISTER_ACCELERATOR,
	XEMBED_ACTIVATE_ACCELERATOR,
};

enum {
	XEMBED_FOCUS_CURRENT,
	XEMBED_FOCUS_FIRST,
	XEMBED_FOCUS_LAST,
};

enum {
	/* hardcoded object sizes in pixels */
	/* there's no ITEM_HEIGHT for it is computed at runtime from font height */
	THUMBSIZE       = 64,                   /* maximum thumbnail size */
	ICON_MARGIN     = (THUMBSIZE / 2),      /* margin around item icon */
	ITEM_WIDTH      = (THUMBSIZE * 2),      /* width of an item (icon + margins) */
	MARGIN          = 16,                   /* top margin above first row */

	/* draw up to NLINES lines of label; each one up to LABELWIDTH pixels long */
	NLINES          = 2,                    /* change it for more lines below icons */
	LABELWIDTH      = ITEM_WIDTH - 16,      /* save 8 pixels each side around label */

	/* times in milliseconds */
	DOUBLECLICK     = 250,                  /* time of a doubleclick, in milliseconds */
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
	Bool start, isset, error;
	int redraw;

	/* X11 stuff */
	Display *display;
	GC gc;
	Cursor busycursor;
	Window window, root, child;
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

	CtrlFontSet *fontset;

	struct {
		XrmClass class;
		XrmName name;
	} application, resources[NRESOURCES];
	const char **cliresources;

	char *gototext;
	char ksymbuf[64];               /* buffer where the keysym passed to xfilesctl is held */

	struct {
		Pixmap pix;
		Picture pict;
	} layers[LAYER_LAST];

	Pixmap namepix;                 /* temporary pixmap for the labels */
	Pixmap namepict;

	struct clipboard {
		unsigned char *buf;
		size_t size;
		FILE *stream;
		Bool filled;
	} plainclip, uriclip;           /* streams for X Selection content */

	/*
	 * Lock used for synchronizing the thumbnail and the main threads.
	 */
	pthread_mutex_t lock;

	/*
	 * Items to be displayed
	 */
	Item *items;
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
	Bool status_enable;
};

struct Options {
	const char *class;
	const char *name;
	int argc;
	char **argv;
};

static Atom atoms[NATOMS];

/* ellipsis has two dots rather than three; the third comes from the extension */
static char const *ELLIPSIS = "..";

static int
error_handler(Display *display, XErrorEvent *error)
{
	char msg[128], req[128], num[8];

	if (error->error_code == BadWindow)
		return 0;
	(void)XGetErrorText(display, error->error_code, msg, sizeof(msg));
	(void)snprintf(num, sizeof(num), "%d", error->request_code);
	(void)XGetErrorDatabaseText(
		display, "XRequest", num,
		"unknown request", req, sizeof(req)
	);
	errx(EXIT_FAILURE, "xlib: %s: %s", req, msg);
	return 0; /* unreachable */
}

static char const *
getitemstatus(Widget *widget, int index)
{
	static char const *UNKNOWN_STATUS = "<\?\?\?>";
	if (index < 0 || index >= widget->nitems)
		return UNKNOWN_STATUS;
	if (widget->items[widget->highlight].status == NULL)
		return UNKNOWN_STATUS;
	return widget->items[widget->highlight].status;
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
	char const *status;
	char countstr[64];    /* enough for writing number of files */
	char scrollstr[8];    /* enough for writing the percentage */
	int scrollpct;

	if (!widget->status_enable)
		return;

	etlock(&widget->lock);
	widget->redraw = True;

	/* clear previous content */
	XRenderFillRectangle(
		widget->display,
		PictOpClear,
		widget->layers[LAYER_STATUSBAR].pict,
		&(XRenderColor){ 0 },
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
			widget->items[widget->highlight].name,
			strlen(widget->items[widget->highlight].name)
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
		PictOpClear,
		widget->layers[LAYER_STATUSBAR].pict,
		&(XRenderColor){ 0 },
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
	Bool changefont = False;

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
			changefont = True;
			break;
		case FACE_SIZE:
			d = strtod(value, &endp);
			if (value[0] != '\0' && *endp == '\0' && d > 0.0 && d <= 100.0) {
				fontsize = d;
				changefont = True;
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

static void
embed_resize(Widget *widget)
{
	if (widget->child == None)
		return;
	XMoveResizeWindow(
		widget->display,
		widget->child,
		0, 0,
		widget->winw,
		widget->winh
	);
	XSendEvent(
		widget->display, widget->child, False, StructureNotifyMask,
		&(XEvent){ .xconfigure = {
			.type = ConfigureNotify,
			.display = widget->display,
			.event = widget->child,
			.window = widget->child,
			.x = 0,
			.y = 0,
			.width = widget->winw,
			.height = widget->winh,
			.border_width = 0,
			.above = None,
			.override_redirect = False,
		}}
	);
	XSync(widget->display, False);
}

static int
calcsize(Widget *widget, int w, int h)
{
	int ncols, nrows, ret;
	int nrows_in_window;
	int nrows_in_dir;
	double d;

	ret = False;
	if (widget->winw == w && widget->winh == h)
		return False;
	widget->redraw = True;
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
	nrows_in_window = widget->h / widget->itemh;
	widget->ncols = max(widget->w / widget->itemw, 1);
	widget->nrows = max(nrows_in_window + (widget->h % widget->itemh ? 2 : 1), 1);
	nrows_in_dir = widget->nitems / widget->ncols;
	nrows_in_dir += widget->nitems % widget->ncols != 0;    /* extra unfilled row */
	widget->x0 = max((widget->w - widget->ncols * widget->itemw) / 2, 0);
	widget->nscreens = nrows_in_dir - nrows_in_window + 1;
	widget->nscreens = max(widget->nscreens, 1);
	d = (double)widget->nscreens / SCROLLER_MIN;
	d = (d < 1.0 ? 1.0 : d);
	widget->handlew = max(SCROLLER_SIZE / d - 2, 1);
	widget->handlew = min(widget->handlew, HANDLE_MAX_SIZE);
	if (widget->handlew == HANDLE_MAX_SIZE && nrows_in_dir > nrows_in_window)
		widget->handlew = HANDLE_MAX_SIZE - 1;
	if (ncols != widget->ncols || nrows != widget->nrows) {
		widget->pixw = widget->ncols * widget->itemw;
		widget->pixh = widget->nrows * widget->itemh;
		resetlayer(widget, LAYER_ICONS, widget->pixw, widget->pixh);
		resetlayer(widget, LAYER_SELALPHA, widget->pixw, widget->pixh);
		ret = True;
	}
	resetlayer(widget, LAYER_RECTALPHA, widget->w, widget->h);
	resetlayer(widget, LAYER_STATUSBAR, widget->winw, STATUSBAR_HEIGHT(widget));
	resetlayer(widget, LAYER_CANVAS, widget->winw, widget->winh);
	embed_resize(widget);
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
	return &widget->icons[widget->items[index].icon];
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
	text = widget->items[index].name;
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

			/* opacity for color layer above selected items */
			.alpha = 0xC000,
		},
		x, y,
		widget->itemw,
		widget->itemh - (NLINES + 1) * widget->fonth
	);
done:
	etunlock(&widget->lock);
	widget->redraw = True;
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
		0, 0, widget->winw, widget->winh
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
			.prop = atoms[_NET_WM_NAME],
			.type = atoms[UTF8_STRING],
		},
		{
			.prop = XA_WM_NAME,
			.type = XA_STRING,
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
}

static int
gethandlepos(Widget *widget)
{
	int retval;

	retval = HANDLE_MAX_SIZE - widget->handlew;
	retval *= ((double)widget->row + 1) / widget->nscreens;
	return retval;
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

static Bool
scroll(Widget *widget, int y)
{
	int prevhand, newhand;          /* position of the scroller handle */
	int prevrow, newrow;
	int prevdiff;

	if (y == 0)
		return False;
	if (y > 0 && widget->row + 1 >= widget->nscreens)
		return False;
	if (y < 0 && widget->row == 0 && widget->ydiff == 0)
		return False;
	widget->redraw = True;
	prevhand = gethandlepos(widget);
	prevdiff = widget->ydiff;
	newrow = prevrow = widget->row;
	widget->ydiff += y;
	if (widget->ydiff < 0) {
		newrow += widget->ydiff / widget->itemh - 1;
		widget->ydiff = widget->ydiff % widget->itemh + widget->itemh;
	} else {
		newrow += widget->ydiff / widget->itemh;
		widget->ydiff %= widget->itemh;
	}
	if (newrow > widget->nscreens) {
		widget->ydiff = 0;
		newrow = widget->nscreens - 1;
	} else if (newrow < 0) {
		widget->ydiff = 0;
		newrow = 0;
	}
	setrow(widget, newrow);
	newhand = gethandlepos(widget);
	if (prevhand != newhand) {
		drawscroller(widget, newhand);
	}
	if (prevrow != newrow) {
		drawstatusbar(widget);
		drawitems(widget);
		return True;
	}
	if (newrow == widget->nscreens-1)
		return False;
	return prevdiff != widget->ydiff;
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
	widget->seltime = 0;
}

static void
writeuri(FILE *stream, unsigned char *s)
{
#define UNRESERVED \
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
	"abcdefghijklmnopqrstuvwxyz" \
	"0123456789" "-._~" "/"

	(void)fprintf(stream, "file://");
	for (; *s != '\0'; s++) {
		if (strchr(UNRESERVED, *s) != NULL)
			(void)fprintf(stream, "%c", *s);
		else
			(void)fprintf(stream, "%%%02X", *s);
	}
	(void)fprintf(stream, "\r\n");
}

static ssize_t
fillclipboard(Widget *widget, unsigned char **bufp, Bool uriformat)
{
	struct Selection *sel;
	struct clipboard *clip;
	char const *delim = "\n";

	if (widget->sel == NULL)
		return -1;
	clip = uriformat ? &widget->uriclip : &widget->plainclip;
	if (clip->filled)
		goto done;
	if (fflush(clip->stream) == EOF)
		goto done;
	if (fseek(clip->stream, 0L, SEEK_SET) == -1)
		goto done;
	if (widget->sel != NULL && widget->sel->next == NULL) {
		/* only one item selected; do not add trailling newline */
		delim = "";
	}
	for (sel = widget->sel; sel != NULL; sel = sel->next) {
		char *name = widget->items[sel->index].fullname;

		if (!uriformat)
			(void)fprintf(clip->stream, "%s%s", name, delim);
		else
			writeuri(clip->stream, (unsigned char *)name);
	}
	clip->filled = True;
done:
	fflush(clip->stream);
	*bufp = clip->buf;
	return ferror(clip->stream) ? -1 : ftello(clip->stream);
}

static ssize_t
convertcallback(void *arg, Atom target, unsigned char **pbuf)
{
	Widget *widget = arg;

	if (widget->sel == NULL) {
		*pbuf = (unsigned char *)"";
		return 0;
	}
	if (target == XA_STRING || target == atoms[UTF8_STRING])
		return fillclipboard(widget, pbuf, False);
	if (target == atoms[TEXT_URI_LIST])
		return fillclipboard(widget, pbuf, True);
	return -1;
}

static void
sendprimary(Widget *widget, XEvent *event)
{
	int error;

	if (widget->seltime == 0)
		return;
	error = ctrlsel_answer(
		event, widget->seltime,
		(Atom[]){
			XA_STRING,
			atoms[UTF8_STRING],
			atoms[TEXT_URI_LIST]
		}, 3,
		convertcallback, widget
	);
	if (error == ENOMEM) {
		warnx("could not send selection (no memory)");
	}
}

static void
ownprimary(Widget *widget, Time time)
{
	widget->seltime = ctrlsel_own(
		widget->display, widget->window, time, XA_PRIMARY
	);
}

static void
resetclipboard(Widget *widget)
{
	widget->plainclip.filled = widget->uriclip.filled = False;
}

static void
cleanwidget(Widget *widget)
{
	struct Thumb *thumb;
	struct Selection *sel;

	if (!widget->isset)
		return;
	resetclipboard(widget);
	thumb = widget->thumbhead;
	while (thumb != NULL) {
		struct Thumb *tmp = thumb;
		thumb = thumb->next;
		XDestroyImage(tmp->img);
		free(tmp);
	}
	sel = widget->sel;
	while (sel != NULL) {
		struct Selection *tmp = sel;
		sel = sel->next;
		free(tmp);
	}
	widget->sel = NULL;
	widget->rectsel = NULL;
#define FREE(x) (free(x), x = NULL)
	FREE(widget->gototext);
	FREE(widget->thumbs);
	FREE(widget->linelen);
	FREE(widget->nlines);
	FREE(widget->issel);
#undef  FREE
	disownprimary(widget);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		atoms[_CONTROL_STATUS],
		atoms[UTF8_STRING],
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

	resetclipboard(widget);
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
		free(sel);
		widget->issel[index] = NULL;
	} else {
		return;
	}
	drawitem(widget, index);
}

static void
highlight(Widget *widget, int index)
{
	int prevhili;
	char const *status;

	if (widget->highlight == index || index < 0 || index >= widget->nitems)
		return;
	prevhili = widget->highlight;
	widget->highlight = index;
	if (index != 0)
		drawitem(widget, index);
	/* we still have to redraw the previous entry */
	drawitem(widget, prevhili);
	drawstatusbar(widget);
	if (widget->highlight > 0) {
		status = getitemstatus(widget, widget->highlight);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			atoms[_CONTROL_STATUS],
			atoms[UTF8_STRING],
			8,
			PropModeReplace,
			(unsigned char *)widget->items[widget->highlight].name,
			strlen(widget->items[widget->highlight].name)
		);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			atoms[_CONTROL_STATUS],
			atoms[UTF8_STRING],
			8,
			PropModeAppend,
			(unsigned char *)" - ",
			3
		);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			atoms[_CONTROL_STATUS],
			atoms[UTF8_STRING],
			8,
			PropModeAppend,
			(unsigned char *)status,
			strlen(status)
		);
	} else {
		(void)XChangeProperty(
			widget->display,
			widget->window,
			atoms[_CONTROL_STATUS],
			atoms[UTF8_STRING],
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
		selectitem(widget, i, True, 0);
	}
}

static void
unselectitems(Widget *widget)
{
	while (widget->sel) {
		selectitem(widget, widget->sel->index, False, 0);
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
	if (index < 0)
		return index;
	prevhili = widget->highlight;
	highlight(widget, index);
	if (prevhili != -1 && ev->state & ShiftMask)
		selectitems(widget, widget->highlight, prevhili);
	else
		selectitem(widget, widget->highlight, ((ev->state & ControlMask) ? widget->issel[widget->highlight] == NULL : True), False);
	ownprimary(widget, ev->time);
	return index;
}

static int
mouse3click(Widget *widget, int x, int y)
{
	int index;

	index = getitemundercursor(widget, x, y);
	if (index != -1) {
		highlight(widget, index);
		if (widget->issel[index] == NULL) {
			unselectitems(widget);
			selectitem(widget, index, True, False);
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

			/* opacity for color layer above rectangular selection */
			.alpha = 0x4000,
		},
		x + 1,
		y + 1,
		max(w - 1, 0),
		max(h - 1, 0)
	);
	widget->redraw = True;
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

static Bool
rectselect(Widget *widget, int srcrow, int srcydiff, int x0, int y0, int x1, int y1)
{
	Bool changed = False;
	Bool sel;
	int row, col, tmp, i;
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
		sel = True;
		row = i / widget->ncols;
		col = i % widget->ncols;
		if (col < col0 || col > col1 || row < row0 || row > row1) {
			/* item is out of selection */
			sel = False;
		} else if ((col == col0 && x0 > ITEM_WIDTH - ICON_MARGIN) ||
		           (col == col1 && x1 < ICON_MARGIN)) {
			/* item is on a column at edge of selection */
			sel = False;
		} else if ((row == row0 && y0 > THUMBSIZE) ||
		           (row == row1 && y1 < 0)) {
			/* item is on a row at edge of selection */
			sel = False;
		}
		if (!sel && (widget->issel[i] == NULL || widget->issel[i]->index > 0))
			continue;
		if (sel && widget->issel[i] != NULL && widget->issel[i]->index > 0)
			selectitem(widget, i, False, False);
		selectitem(widget, i, sel, True);
		changed = True;
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
	if (newrow >= widget->nscreens) {
		widget->ydiff = 0;
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
checkheader(FILE *fp, unsigned char const *header, size_t size)
{
	char buf[8];    /* enough for a .PPM header field */

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
fillselitems(Widget *widget, int *selitems)
{
	int i, nitems;

	nitems = 0;
	if (widget->issel == NULL)
		return 0;
	for (i = 0; i < widget->nitems; i++)
		if (widget->issel[i] != NULL)
			selitems[nitems++] = i;
	return nitems;
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
	static XRectangle DEF_SIZE = {
		.x = 0, .y = 0,
		.width = 600 , .height = 460
	};

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

static Window
createwindow(Widget *widget, Window parent, XRectangle rect, long eventmask, Bool override)
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
		CWOverrideRedirect | CWEventMask | CWColormap |
		CWBackPixel | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.background_pixel = 0,
			.colormap = widget->colormap,
			.event_mask = eventmask,
			.override_redirect = override,
		}
	);
}

static Window
create_dragwin(Widget *widget, int index)
{
	Pixmap pix, iconbg, iconmask;
	Window dndicon;
	struct Icon *icon;
	unsigned int width, height;

	if (index < 1)
		return None;
	pix = None;
	dndicon = None;
	if (widget->thumbs[index] != NULL) {
		width = widget->thumbs[index]->w;
		height = widget->thumbs[index]->h;
		pix = XCreatePixmap(
			widget->display, widget->window,
			width, height, widget->depth
		);
		if (pix == None)
			goto error;
		(void)XPutImage(
			widget->display, pix,
			widget->gc,
			widget->thumbs[index]->img,
			0, 0, 0, 0, width, height
		);
		iconbg = pix;
		iconmask = None;
	} else if ((icon = geticon(widget, index)) != NULL) {
		width = THUMBSIZE;
		height = THUMBSIZE;
		iconbg = icon->pix;
		iconmask = icon->mask;
	} else {
		goto error;
	}
	dndicon = createwindow(
		widget, widget->root,
		(XRectangle){0, 0, width, height}, 0, True
	);
	(void)XSetWindowBackgroundPixmap(widget->display, dndicon, iconbg);
	if (dndicon == None)
		goto error;
	if (iconmask == None)
		goto error;
	XShapeCombineMask(
		widget->display, dndicon, ShapeBounding,
		0, 0, iconmask, ShapeSet
	);
error:
	if (pix != None)
		XFreePixmap(widget->display, pix);
	return dndicon;
}

static void
embed_message(Widget *widget, Time time, long msg, long d0, long d1, long d2)
{
	XSendEvent(
		widget->display, widget->child, False, NoEventMask,
		&(XEvent){ .xclient = {
			.type = ClientMessage,
			.display = widget->display,
			.window = widget->child,
			.message_type = atoms[_XEMBED],
			.format = 32,
			.data.l[0] = time,
			.data.l[1] = msg,
			.data.l[2] = d0,
			.data.l[3] = d1,
			.data.l[4] = d2,
		}}
	);
}

static void
embed_focus(Widget *widget, Time time)
{
	Window focused;

	if (widget->child == None)
		return;
	(void)XGetInputFocus(widget->display, &focused, (int[]){0});
	if (focused == widget->child)
		return;
	if (focused != widget->window)
		return;
	(void)XSetInputFocus(
		widget->display,
		widget->child,
		RevertToParent,
		CurrentTime
	);
	embed_message(widget, time, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
	embed_message(widget, time, XEMBED_FOCUS_IN, XEMBED_FOCUS_CURRENT, 0, 0);
}

static void
embed_unfocus(Widget *widget, Time time)
{
	if(widget->child == None)
		return;
	embed_message(widget, time, XEMBED_FOCUS_OUT, 0, 0, 0);
}

static void
embed_set(Widget *widget, Time time, Window window)
{
	if (window == None)
		return;
	if (widget->child != None)
		return; /* we already have an embedded window */
	if (window == widget->window)
		return; /* we should not embed our own window */
	if (window == widget->scroller)
		return;
	/*
	 * We do not check whether the embedded client speaks the XEmbed
	 * protocol, because some applications (like st), does not set
	 * the _XEMBED_INFO property to inform they are XEmbed-aware.
	 * We just unconditionally embed any child window we get that we
	 * have not created.
	 */
	widget->child = window;
	XWithdrawWindow(widget->display, widget->child, widget->screen);
	XReparentWindow(widget->display, widget->child, widget->window, 0, 0);
	XSetWindowBorderWidth(widget->display, widget->child, 0);
	XMapRaised(widget->display, widget->child);
	XSync(widget->display, False);
	embed_message(widget, time, XEMBED_EMBEDDED_NOTIFY, 0, widget->window, 0);
	embed_focus(widget, time);
}

static void
embed_close(Widget *widget, Time time)
{
	XSendEvent(
		widget->display, widget->child, False, NoEventMask,
		&(XEvent) { .xclient = {
			.type = ClientMessage,
			.display = widget->display,
			.window = widget->child,
			.message_type = atoms[WM_PROTOCOLS],
			.format = 32,
			.data.l[0] = atoms[WM_DELETE_WINDOW],
			.data.l[1] = time,
		}}
	);
}

static Bool
is_timed_out(struct timespec *start, Time timeout)
{
	struct timespec stop;
	Time elapsed;

	if (timeout == 0)
		return False;
	if (clock_gettime(CLOCK_MONOTONIC, &stop) == -1)
		return False;
	elapsed = (stop.tv_sec - start->tv_sec) * 1000;
	elapsed += stop.tv_nsec / 1000000;
	elapsed -= start->tv_nsec / 1000000;
	if (elapsed < timeout)
		return False;
	*start = stop;
	return True;
}

/*
 * event filters
 */

enum events {
	/*
	 * The .type field in XEvent(3) structures start from 2 because
	 * 0 and 1 are reserved by the X11 protocol for errors and
	 * replies.  XNextEvent(3) and related functions never return
	 * an error or reply (or so it is expected).  So we use those
	 * two values for our own purposes.
	 */
	CloseNotify     = 0,
	TimeoutNotify   = 1,
};

static WidgetEvent
keypress(Widget *widget, XKeyEvent *xev, int *selitems, int *nitems, char **text)
{
	KeySym ksym;
	unsigned int state;
	int row[2];
	int redrawall, previtem, index, newrow, i;
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
		selitems[0] = widget->highlight;
		*nitems = 1;
		return WIDGET_OPEN;
	case XK_Menu:
		*nitems = fillselitems(widget, selitems);
		return WIDGET_CONTEXT;
	case XK_space:
		if (widget->highlight != -1) {
			selectitem(
				widget,
				widget->highlight,
				widget->issel[widget->highlight] == NULL,
				False
			);
		}
		highlight(widget, widget->highlight + 1);
		break;
	case XK_Prior:
	case XK_Next:
		/* scroll half page up or down */
		if (scroll(widget, (ksym == XK_Prior ? -1 : 1) * widget->h/2)) {
			index = widget->row * widget->ncols;
			if (widget->ydiff != 0 && index + 1 < widget->nitems)
				index += widget->ncols;
			highlight(widget, index);
		}
		break;
	case XK_Home:
	case XK_End:
	case XK_Up:
	case XK_Down:
	case XK_Left:
	case XK_Right:
hjkl:
		redrawall = True;
		if (ksym == XK_Home) {
			index = 0;
			widget->ydiff = 0;
			setrow(widget, 0);
			goto draw;
		}
		if (ksym == XK_End) {
			index = widget->nitems - 1;
			widget->ydiff = 0;
			setrow(widget, widget->nscreens - 1);
			goto draw;
		}
		if (widget->highlight == -1) {
			widget->highlight = 0;
			setrow(widget, 0);
		}
		if (ksym == XK_Up || ksym == XK_k) {
			index = widget->highlight - widget->ncols;
		} else if (ksym == XK_Down || ksym == XK_j) {
			if (widget->highlight / widget->ncols >= widget->nitems / widget->ncols)
				break;
			index = min(widget->nitems - 1, widget->highlight + widget->ncols);
		} else if (ksym == XK_Left || ksym == XK_h) {
			index = widget->highlight - 1;
		} else {
			index = widget->highlight + 1;
		}
		if (index == widget->highlight || index < 0 || index >= widget->nitems)
			break;
		row[0] = widget->highlight / widget->ncols;
		row[1] = index / widget->ncols;
		newrow = widget->row;
		for (i = 0; i < 2; i++) {
			int nrows_in_window = widget->h / widget->itemh;

			/*
			 * Try to make both previously highlighted item
			 * and new highlighted item visible.
			 */
			if (row[i] < newrow) {
				newrow = row[i];
			} else if (row[i] >= newrow + nrows_in_window) {
				newrow = row[i] - nrows_in_window + 1;
			}
		}
		if (widget->row != newrow) {
			widget->ydiff = 0;
			setrow(widget, newrow);
			redrawall = True;
		} else if (widget->row == index / widget->ncols) {
			widget->ydiff = 0;
			widget->redraw = True;
		}
draw:
		previtem = widget->highlight;
		highlight(widget, index);
		if (xev->state & ShiftMask)
			selectitems(widget, index, previtem);
		else if (xev->state & ControlMask)
			selectitem(widget, index, True, 0);
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
		(void)snprintf(widget->ksymbuf, sizeof(widget->ksymbuf), "^%s", kstr);
		*text = widget->ksymbuf;
		*nitems = 0;
		if (widget->sel != NULL)
			*nitems = fillselitems(widget, selitems);
		else if ((xev->state & ShiftMask) && widget->highlight > 0)
			selitems[(*nitems)++] = widget->highlight;
		return WIDGET_KEYPRESS;
	}
	return WIDGET_NONE;
}

static void
compress_motion(Display *display, XEvent *event)
{
	XEvent next;

	if (event->type != MotionNotify)
		return;
	while (XPending(display)) {
		XPeekEvent(display, &next);
		if (next.type != MotionNotify)
			break;
		if (next.xmotion.window != event->xmotion.window)
			break;
		if (next.xmotion.subwindow != event->xmotion.subwindow)
			break;
		XNextEvent(display, event);
	}
}

static Bool
filter_event(Widget *widget, XEvent *ev)
{
	XWindowAttributes wattr;
	int newrow;

	widget->redraw = False;
	switch (ev->type) {
	case MotionNotify:
		compress_motion(widget->display, ev);
		return False;
	case CreateNotify:
		if (ev->xcreatewindow.parent != widget->window)
			break;
		if (ev->xcreatewindow.override_redirect)
			break;
		embed_set(widget, CurrentTime, ev->xcreatewindow.window);
		break;
	case ReparentNotify:
		if (ev->xreparent.parent != widget->window)
			break;
		if (ev->xreparent.override_redirect)
			break;
		embed_set(widget, CurrentTime, ev->xreparent.window);
		break;
	case MapRequest:
		if (ev->xmaprequest.parent != widget->window)
			break;
		if (!XGetWindowAttributes(widget->display, ev->xmaprequest.window, &wattr))
			break;
		if (!wattr.override_redirect)
			break;
		embed_set(widget, CurrentTime, ev->xmaprequest.window);
		break;
	case FocusIn:
		if (ev->xfocus.window != widget->window)
			break;
		embed_focus(widget, CurrentTime);
		break;
	case FocusOut:
		if (ev->xfocus.window != widget->window)
			break;
		if (ev->xfocus.detail == NotifyInferior)
			break;
		embed_unfocus(widget, CurrentTime);
		break;
	case MapNotify:
		if (ev->xmap.window != widget->child)
			break;
		embed_resize(widget);
		embed_focus(widget, CurrentTime);
		break;
	case UnmapNotify:
		if (ev->xunmap.window == widget->child)
			widget->child = None;
		break;
	case DestroyNotify:
		if (ev->xdestroywindow.window == widget->child)
			widget->child = None;
		if (ev->xdestroywindow.window != widget->window)
			break;
		widget->error = True;
		goto close;
	case ClientMessage:
		if (ev->xclient.window != widget->window)
			return False;
		if (ev->xclient.message_type != atoms[WM_PROTOCOLS])
			return False;
		if ((Atom)ev->xclient.data.l[0] != atoms[WM_DELETE_WINDOW])
			return False;
close:
		embed_close(widget, ev->xclient.data.l[1]);
		ev->type = CloseNotify;
		return False;
	case ConfigureNotify:
		if (ev->xconfigure.window != widget->window)
			break;
		if (calcsize(widget, ev->xconfigure.width, ev->xconfigure.height)) {
			if (widget->highlight < 0) {
				setrow(widget, 0);
			} else {
				newrow = widget->highlight / widget->ncols;
				newrow = min(newrow, widget->nscreens - 1);
				setrow(widget, newrow);
			}
			drawitems(widget);
		}
		drawstatusbar(widget);
		break;
	case PropertyNotify:
		if (ev->xproperty.state != PropertyNewValue)
			return False;
		if (ev->xproperty.window == widget->root &&
		    ev->xproperty.atom == XA_RESOURCE_MANAGER) {
			char *str;
			str = gettextprop(
				widget,
				widget->root,
				XA_RESOURCE_MANAGER,
				False
			);
			if (str == NULL)
				break;
			loadresources(widget, str);
			free(str);
			drawitems(widget);
			widget->redraw = True;
			break;
		}
		if (ev->xproperty.window == widget->window &&
		    ev->xproperty.atom == atoms[_CONTROL_GOTO]) {
			free(widget->gototext);
			widget->gototext = gettextprop(
				widget,
				widget->window,
				atoms[_CONTROL_GOTO],
				True
			);
		}
		return False;
	case SelectionRequest:
		if (ev->xselectionrequest.owner != widget->window)
			break;
		if (ev->xselectionrequest.selection == XA_PRIMARY)
			sendprimary(widget, ev);
		break;
	case SelectionClear:
		if (ev->xselectionclear.window != widget->window)
			break;
		if (ev->xselectionclear.selection == XA_PRIMARY)
			disownprimary(widget);
		break;
	default:
		return False;
	}
	endevent(widget);
	return True;
}

static int
dnd_event_handler(XEvent *event, void *arg)
{
	Widget *widget = arg;
	int index, x, y;
	static Time lasttime = 0;

	if (filter_event(widget, event))
		return 0;
	if (event->type != MotionNotify)
		return 0;
	if (event->xmotion.window != widget->window)
		return 0;
	x = event->xmotion.x;
	y = event->xmotion.y;
	index = getitemundercursor(widget, x, y);
	if (index < 0)
		index = 0;
	highlight(widget, index);
	if (lasttime + SCROLL_TIME <= event->xmotion.time) {
		if (y < SCROLL_STEP)
			scroll(widget, -SCROLL_STEP);
		else if (y >= widget->h - SCROLL_STEP)
			scroll(widget, +SCROLL_STEP);
		lasttime = event->xmotion.time;
	}
	endevent(widget);
	return 0;
}

/*
 * event loops
 */

static int
nextevent(Widget *widget, XEvent *ev, Time timeout)
{
	static struct timespec lasttime = { 0 };
	struct pollfd pfd = {
		.fd = widget->fd,
		.events = POLLIN,
	};

	endevent(widget); /* end of previous event */
	for (;;) {
		if (is_timed_out(&lasttime, timeout))
			return TimeoutNotify;
		if (XPending(widget->display) == 0)
			if (poll(&pfd, 1, timeout?timeout:-1) <= 0)
				continue;
		(void)XNextEvent(widget->display, ev);
		if (!filter_event(widget, ev))
			return ev->type;
	}
}

static WidgetEvent
scrollmode(Widget *widget, Time lasttime, int clickx, int clicky)
{
	XEvent ev;
	int grabpos, pos;
	Bool went_out = False;

	drawscroller(widget, gethandlepos(widget));
	XMoveWindow(
		widget->display, widget->scroller,
		clickx - SCROLLER_SIZE / 2 - 1,
		clicky - SCROLLER_SIZE / 2 - 1
	);
	XMapRaised(widget->display, widget->scroller);
	/* grab on scroller; so motion position is relative to it */
	if (XGrabPointer(
		widget->display, widget->scroller, False,
		ButtonReleaseMask | ButtonPressMask | PointerMotionMask,
		GrabModeAsync, GrabModeAsync, None, None, lasttime
	) != GrabSuccess)
		goto done;
	pos = 0;
	for (;;) switch (nextevent(widget, &ev, SCROLL_TIME)) {
	case TimeoutNotify:
		if (pos < 0)
			scroll(widget, pos);
		else if (pos > SCROLLER_SIZE)
			scroll(widget, pos - SCROLLER_SIZE);
		continue;
	case FocusIn:
	case FocusOut:
		goto done;
	case MotionNotify:
		if (ev.xmotion.y < 0)
			pos = ev.xmotion.y;
		else if (ev.xmotion.y > SCROLLER_SIZE)
			pos = ev.xmotion.y - SCROLLER_SIZE;
		else
			continue;
		went_out = True;
		continue;
	case ButtonRelease:
		if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
			scroll(widget, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP));
		if (ev.xbutton.button == Button1 || ev.xbutton.button == Button3)
			goto done;
		if (ev.xbutton.button == Button2 && went_out)
			goto done;
		continue;
	case ButtonPress:
		if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
			continue;
		if (ev.xbutton.button != Button1)
			goto done;
		/* return if press is outside scroller */
		if (ev.xbutton.x < 0 || ev.xbutton.x >= SCROLLER_SIZE)
			goto done;
		if (ev.xbutton.y < 0 || ev.xbutton.y >= SCROLLER_SIZE)
			goto done;
		went_out = False;
		grabpos = gethandlepos(widget);
		if (ev.xbutton.y < grabpos || ev.xbutton.y > grabpos + widget->handlew)
			grabpos = widget->handlew / 2;
		else
			grabpos = ev.xbutton.y - grabpos;
		scrollerset(widget, ev.xbutton.y - grabpos);
		while (nextevent(widget, &ev, SCROLL_TIME) != ButtonRelease)
			if (ev.type == MotionNotify)
				scrollerset(widget, ev.xmotion.y - grabpos);
		continue;
	}
done:
	XUngrabPointer(widget->display, lasttime);
	XUnmapWindow(widget->display, widget->scroller);
	return WIDGET_NONE;
}

static WidgetEvent
selmode(Widget *widget, int shift, int clickx, int clicky)
{
	XEvent ev;
	int rectrow, rectydiff, ownsel;
	int x = clickx;
	int y = clicky;

	highlight(widget, 0);
	rectrow = widget->row;
	rectydiff = widget->ydiff;
	ownsel = False;
	if (!shift)
		unselectitems(widget);
	for (;;) switch (nextevent(widget, &ev, SCROLL_TIME)) {
	case CloseNotify:
		return WIDGET_CLOSE;
	case TimeoutNotify:
		if (y >= 0 && y < widget->h)
			continue;
		if (scroll(widget, (y > 0 ? +1 : -1) * SCROLLER_SIZE))
			goto motion;
		continue;
	case ButtonPress:
	case ButtonRelease:
		goto done;
	case MotionNotify:
		x = ev.xmotion.x;
		y = ev.xmotion.y;
motion:
		rectdraw(widget, rectrow, rectydiff, clickx, clicky, x, y);
		if (rectselect(widget, rectrow, rectydiff, clickx, clicky, x, y))
			ownsel = True;
		continue;
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
dragmode(Widget *widget, Time timestamp, int index, int *selitems, int *nitems)
{
	struct ctrldnd_drop drop;
	unsigned char *plainbuf, *uribuf;
	ssize_t plainsize, urisize;
	Window dragwin;

	if (index < 1 || widget->sel == NULL)
		return WIDGET_NONE;
	plainsize = fillclipboard(widget, &plainbuf, False);
	urisize = fillclipboard(widget, &uribuf, True);
	if (plainsize < 0 || urisize < 0)
		return WIDGET_NONE;
	dragwin = create_dragwin(widget, index),
	drop = ctrldnd_drag(
		widget->display, widget->screen, timestamp, dragwin,
		(struct ctrldnd_data[]){
			{
				.data   = uribuf,
				.size   = urisize,
				.target = atoms[TEXT_URI_LIST],
			},
			{
				.data   = plainbuf,
				.size   = plainsize,
				.target = atoms[UTF8_STRING],
			},
			{
				.data   = plainbuf,
				.size   = plainsize,
				.target = XA_STRING,
			},
		},
		3, CTRLDND_ANYACTION, SCROLL_TIME,
		dnd_event_handler, widget
	);
	if (dragwin != None)
		XDestroyWindow(widget->display, dragwin);
	if (drop.window != widget->window)
		return WIDGET_NONE;
	/* user dropped items on widget's own window */
	index = getitemundercursor(widget, drop.x, drop.y);
	highlight(widget, index);
	if (index < 1 || index >= widget->nitems)
		return WIDGET_NONE;
	if (widget->issel[index] != NULL)
		return WIDGET_NONE;     /* dont drop item on itself */
	/*
	 * First item is the one where user has dropped.
	 * Following items are the ones being dropped.
	 */
	selitems[0] = index;
	*nitems = fillselitems(widget, &selitems[1]);
	(*nitems)++;
	switch (drop.action) {
	case CTRLDND_MOVE: return WIDGET_DROPMOVE;
	case CTRLDND_COPY: return WIDGET_DROPCOPY;
	case CTRLDND_LINK: return WIDGET_DROPLINK;
	default:           return WIDGET_DROPASK;
	}
}

static WidgetEvent
mainmode(Widget *widget, int *selitems, int *nitems, char **text)
{
	XEvent ev;
	Time lasttime = 0;
	int clickx = 0;
	int clicky = 0;
	int clicki = -1;
	WidgetEvent event;
	struct ctrldnd_drop drop;

	for (;;) switch (nextevent(widget, &ev, 0)) {
	case CloseNotify:
		return WIDGET_CLOSE;
	case PropertyNotify:
		if (widget->gototext != NULL) {
			*text = widget->gototext;
			return WIDGET_GOTO;
		}
		continue;
	case KeyPress:
		if (ev.xkey.window != widget->window)
			continue;
		event = keypress(widget, &ev.xkey, selitems, nitems, text);
		if (event != WIDGET_NONE)
			return event;
		continue;
	case ButtonPress:
		clickx = ev.xbutton.x;
		clicky = ev.xbutton.y;
		if (ev.xbutton.window != widget->window)
			continue;
		if (ev.xbutton.button == Button1) {
			clicki = mouse1click(widget, &ev.xbutton);
		} else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
			scroll(widget, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP));
			widget->redraw = True;
		} else if (ev.xbutton.button == Button2) {
			event = scrollmode(widget, ev.xmotion.time, ev.xmotion.x, ev.xmotion.y);
			if (event != WIDGET_NONE)
				return event;
		} else if (ev.xbutton.button == Button3) {
			if (mouse3click(widget, ev.xbutton.x, ev.xbutton.y) > 0)
				*nitems = fillselitems(widget, selitems);
			widget->redraw = True;
			XUngrabPointer(widget->display, ev.xbutton.time);
			XFlush(widget->display);
			return WIDGET_CONTEXT;
		}
		continue;
	case ButtonRelease:
		if (ev.xbutton.window != widget->window)
			continue;
		if (ev.xbutton.button == 8)
			return WIDGET_PREV;
		if (ev.xbutton.button == 9)
			return WIDGET_NEXT;
		if (ev.xbutton.button != Button1)
			continue;
		if (clicki < 0 ||
		    (ev.xbutton.state & (ControlMask | ShiftMask)) ||
		    ev.xbutton.time - lasttime > DOUBLECLICK) {
			lasttime = ev.xbutton.time;
			continue;
		}
		highlight(widget, getitemundercursor(widget, ev.xbutton.x, ev.xbutton.y));
		if (widget->highlight >= 0) {
			selitems[0] = widget->highlight;
			*nitems = 1;
		} else {
			*nitems = fillselitems(widget, selitems);
		}
		return WIDGET_OPEN;
	case MotionNotify:
		if (ev.xmotion.window != widget->window)
			continue;
		if (!(ev.xmotion.state & Button1Mask))
			continue;
		/* 64: square of distance cursor must move to be considered a dnd */
		if (diff(ev.xmotion.x, clickx) * diff(ev.xmotion.y, clicky) < 64)
			continue;
		if (clicki == 0)
			continue;
		if (clicki == -1)
			event = selmode(widget, ev.xmotion.state & (ShiftMask | ControlMask), clickx, clicky);
		else
			event = dragmode(widget, ev.xmotion.time, clicki, selitems, nitems);
		if (event != WIDGET_NONE)
			return event;
		continue;
	case ClientMessage:
		drop = ctrldnd_getdrop(
			&ev, &atoms[UTF8_STRING], 1,
			CTRLDND_ANYACTION, SCROLL_TIME,
			dnd_event_handler, widget
		);
		if (drop.window == widget->window && drop.content.size > 0) {
			*text = (char *)drop.content.data;
			*nitems = 1;
			selitems[0] = getitemundercursor(widget, drop.x, drop.y);
			switch (drop.action) {
			case CTRLDND_MOVE: return WIDGET_DROPMOVE;
			case CTRLDND_COPY: return WIDGET_DROPCOPY;
			case CTRLDND_LINK: return WIDGET_DROPLINK;
			default:           return WIDGET_DROPASK;
			}
		}
		XFree(drop.content.data);
		continue;
	}
	return WIDGET_CLOSE;
}

/*
 * widget initializers
 */

static int
initxconn(Widget *widget, struct Options *options)
{
	static char *atom_names[] = {
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
	if (!XInternAtoms(widget->display, atom_names, NATOMS, False, atoms)) {
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
	(void)XSetErrorHandler(error_handler);
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
		SubstructureRedirectMask | SubstructureNotifyMask |
		StructureNotifyMask | PropertyChangeMask | FocusChangeMask |
		KeyPressMask | KeyReleaseMask |
		PointerMotionMask | ButtonReleaseMask | ButtonPressMask,
		False
	);
	if (widget->window == None) {
		warnx("could not create window");
		return RETURN_FAILURE;
	}
	widget->scroller = createwindow(
		widget,
		widget->window,
		(XRectangle){.width = SCROLLER_SIZE, .height = SCROLLER_SIZE},
		ButtonReleaseMask | ButtonPressMask | PointerMotionMask,
		True
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
		&atoms[WM_DELETE_WINDOW],
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
		atoms[_NET_WM_NAME],
		atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)options->class,
		strlen(options->class)
	);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM,
		32,
		PropModeReplace,
		(unsigned char *)&atoms[_NET_WM_WINDOW_TYPE_NORMAL],
		1
	);
	(void)XChangeProperty(
		widget->display,
		widget->window,
		atoms[_NET_WM_PID],
		XA_CARDINAL,
		32,
		PropModeReplace,
		(unsigned char *)&pid,
		1
	);
	(void)snprintf(buf, LEN(buf), "%lu", (unsigned long)widget->window);
	if (setenv("WINDOWID", buf, True) == RETURN_FAILURE)
		warn("setenv");
	for (size_t i = 0; i < nwin_icons; i++) {
		(void)XChangeProperty(
			widget->display,
			widget->window,
			atoms[_NET_WM_ICON],
			XA_CARDINAL,
			32,
			(i == 0) ? PropModeReplace : PropModeAppend,
			(unsigned char *)(unsigned long []){
				win_icons[i].size,
				win_icons[i].size,
			},
			2
		);
		(void)XChangeProperty(
			widget->display,
			widget->window,
			atoms[_NET_WM_ICON],
			XA_CARDINAL,
			32,
			PropModeAppend,
			(unsigned char *)win_icons[i].data,
			win_icons[i].size * win_icons[i].size
		);
	}
	ctrldnd_announce(widget->display, widget->window);
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
	int success, retval, i;

	(void)options;
	widget->nicons = nicon_types;
	if ((widget->icons = calloc(widget->nicons, sizeof(*widget->icons))) == NULL) {
		warn("calloc");
		return RETURN_FAILURE;
	}
	retval = RETURN_SUCCESS;
	for (i = 0; i < widget->nicons; i++) {
		widget->icons[i].pix  = None;
		widget->icons[i].mask = None;
		success = pixmapfromdata(
			widget,
			icon_types[i].xpm,
			&widget->icons[i].pix,
			&widget->icons[i].mask
		) != RETURN_FAILURE;
		if (!success) {
			warnx("%s: could not open pixmap", icon_types[i].name);
			retval = RETURN_FAILURE;
		}
	}
	return retval;
}

static int
initstreams(Widget *widget, struct Options *options)
{
	struct clipboard *clips[] = {
		&widget->plainclip,
		&widget->uriclip,
	};

	(void)options;
	for (size_t i = 0; i < LEN(clips); i++) {
		clips[i]->buf = NULL;
		clips[i]->size = 0;
		clips[i]->filled = False;
		clips[i]->stream = open_memstream(
			(char **)&clips[i]->buf,
			&clips[i]->size
		);
		if (clips[i]->stream == NULL) {
			warn("open_memstream");
			return RETURN_FAILURE;
		}
	}
	return RETURN_SUCCESS;
}

static int
initmisc(Widget *widget, struct Options *options)
{
	(void)options;

	/*
	 * The progress/half-busy cursor (rendered as the regular
	 * pointing cursor with a spinning wheel or hourglass) is used
	 * to indicate that some computation is in progress, but the
	 * program still responds to user input.
	 *
	 * There is no default name for such cursor; and each theme may
	 * use a different name for it.  We try all these possibilities
	 * in turn.
	 *
	 * If all fail, we fallback to XC_watch, from default X11 cursor
	 * set.  It is mostly used to show that a program is busy and is
	 * unresponsive to user input.
	 */
	for (int i = 0; i < 3; i++) {
		static char *names[] = {
			"progress", "half-busy", "left_ptr_watch"
		};

		widget->busycursor = XcursorLibraryLoadCursor(
			widget->display, names[i]
		);
		if (widget->busycursor != None)
			break;
	}
	if (widget->busycursor == None)
		widget->busycursor = XCreateFontCursor(widget->display, XC_watch);

	/*
	 * Detect property changes on root to watch X Resources.
	 */
	(void)XSelectInput(widget->display, widget->root, PropertyChangeMask);

	/*
	 * No need to check for errors here:
	 * If cursor is None, XDefineCursor(3) fallback to default cursor.
	 * If not watching resources, we'll just not reload theme on the fly.
	 */
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
	free(widget->icons);

	if (widget->plainclip.stream != NULL)
		fclose(widget->plainclip.stream);
	if (widget->uriclip.stream != NULL)
		fclose(widget->uriclip.stream);
	if (widget->plainclip.buf != NULL)
		free(widget->plainclip.buf);
	if (widget->uriclip.buf != NULL)
		free(widget->uriclip.buf);
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
	free(widget);
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
		initstreams,
		initmisc,
	};

	if ((widget = malloc(sizeof(*widget))) == NULL) {
		warn("malloc");
		return NULL;
	}

#define COLOR(r,g,b) ((XRenderColor){ \
	.red = 0x##r##FF, .green = 0x##g##FF, .blue = 0x##b##FF, .alpha = 0xFFFF \
	})
	*widget = (Widget){
		.class = class,
		.colors[SELECT_NOT][COLOR_BG].chans = COLOR(00,00,00),
		.colors[SELECT_NOT][COLOR_FG].chans = COLOR(FF,FF,FF),
		.colors[SELECT_YES][COLOR_BG].chans = COLOR(34,65,A4),
		.colors[SELECT_YES][COLOR_FG].chans = COLOR(FF,FF,FF),
		.status_enable = True,
		.opacity = 0xFFFF,
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
widget_set(Widget *widget, const char *cwd, const char *title, Item items[], size_t nitems, Scroll *scrl)
{
	widget->isset = True;
	XUndefineCursor(widget->display, widget->window);
	cleanwidget(widget);
	widget->items = items;
	widget->nitems = nitems;
	if (scrl == NULL) {
		widget->highlight = -1;
		widget->ydiff = 0;
		widget->row = 0;
	} else {
		if (scrl->highlight < widget->nitems)
			widget->highlight = scrl->highlight;
		else
			widget->highlight = widget->nitems - 1;
		widget->ydiff = scrl->ydiff;
		widget->row = scrl->row;
	}
	widget->title = title;
	(void)calcsize(widget, -1, -1);
	if (scrl != NULL && widget->row >= widget->nscreens) {
		widget->ydiff = 0;
		setrow(widget, widget->nscreens - 1);
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
	if ((widget->thumbs = calloc(widget->nitems, sizeof(*widget->thumbs))) == NULL) {
		warn("calloc");
		goto error;
	}
	widget->thumbhead = NULL;
	settitle(widget);
	drawitems(widget);
	drawstatusbar(widget);
	commitdraw(widget);
	XChangeProperty(
		widget->display,
		widget->window,
		atoms[_CONTROL_CWD],
		atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)cwd,
		strlen(cwd)
	);
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
widget_geticons(Widget *widget)
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
	WidgetEvent retval;

	*text = NULL;
	*nitems = 0;
	retval = widget_wait(widget);
	if (widget->gototext != NULL) {
		*text = widget->gototext;
		return WIDGET_GOTO;
	}
	if (retval == WIDGET_CLOSE || retval == WIDGET_ERROR)
		return retval;
	widget->start = True;
	retval = mainmode(widget, selitems, nitems, text);
	endevent(widget);
	scrl->ydiff = widget->ydiff;
	scrl->row = widget->row;
	scrl->highlight = widget->highlight;
	return retval;
}

WidgetEvent
widget_wait(Widget *widget)
{
	XEvent ev;

	while (widget->start && XPending(widget->display) > 0) {
		(void)XNextEvent(widget->display, &ev);
		(void)filter_event(widget, &ev);
		if (widget->error)
			return WIDGET_ERROR;
		if (ev.type == CloseNotify)
			return WIDGET_CLOSE;
	}
	return WIDGET_NONE;
}

int
widget_fd(Widget *widget)
{
	return widget->fd;
}

void
widget_thumb(Widget *widget, char *path, int item)
{
	enum color_depths {
		CLIP_DEPTH = 1,		/* 0/1 */
		PPM_DEPTH = 3,		/* RGB */
		DATA_DEPTH = 4,		/* BGRA */
	};
	FILE *fp;
	size_t size, i;
	int w, h;
	char buf[DATA_DEPTH];
	unsigned char *data;
	static unsigned char const PPM_HEADER[] = {'P', '6', '\n'};
	static unsigned char const PPM_COLOR[] = {'2', '5', '5', '\n'};

	if (item < 0 || item >= widget->nitems || widget->thumbs == NULL)
		return;
	data = NULL;
	widget->thumbs[item] = NULL;
	if ((fp = fopen(path, "rb")) == NULL) {
		warn("%s", path);
		goto error;
	}
	if (checkheader(fp, PPM_HEADER, sizeof(PPM_HEADER)) == -1) {
		warnx("%s: not a ppm file", path);
		goto error;
	}
	if ((w = readsize(fp)) <= 0 || (h = readsize(fp)) <= 0) {
		warnx("%s: ppm file with invalid header", path);
		goto error;
	}
	if (w > THUMBSIZE || h > THUMBSIZE) {
		warnx("%s: ppm file too large: %dx%d", path, w, h);
		goto error;
	}
	if (checkheader(fp, PPM_COLOR, sizeof(PPM_COLOR)) == -1) {
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
		DATA_DEPTH * CHAR_BIT,
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
	free(data);
	free(widget->thumbs[item]);
	widget->thumbs[item] = NULL;
}

void
widget_busy(Widget *widget)
{
	XDefineCursor(widget->display, widget->window, widget->busycursor);
	XFlush(widget->display);
}
