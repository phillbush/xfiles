#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/cursorfont.h>
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/sync.h>

#include "util.h"
#include "widget.h"
#include "fileicon.xpm"

/* ellipsis has two dots rather than three; the third comes from the extension */
#define ELLIPSIS        ".."
#define DEF_FONT        "monospace:size=9"
#define DEF_BG          "#0A0A0A"
#define DEF_FG          "#FFFFFF"
#define DEF_SELBG       "#121212"
#define DEF_SELFG       "#707880"
#define PPM_HEADER      "P6\n"
#define PPM_COLOR       "255\n"
#define ALARMFLAGS      (XSyncCACounter | XSyncCAValue | XSyncCAValueType | XSyncCATestType | XSyncCADelta)

#define VISUAL(d)       (DefaultVisual((d), DefaultScreen((d))))
#define COLORMAP(d)     (DefaultColormap((d), DefaultScreen((d))))
#define DEPTH(d)        (DefaultDepth((d), DefaultScreen((d))))
#define WIDTH(d)        (DisplayWidth((d), DefaultScreen((d))))
#define HEIGHT(d)       (DisplayHeight((d), DefaultScreen((d))))
#define ROOT(d)         (DefaultRootWindow((d)))
#define FLAG(f, b)      (((f) & (b)) == (b))
#define ATOI(c)         (((c) >= '0' && (c) <= '9') ? (c) - '0' : -1)

enum {
	NTARGETS        = 7,
	STIPPLE_SIZE    = 2,
	CLIP_DEPTH      = 1,
	TITLE_SIZE      = 1028,
	STATUS_SIZE     = 64,
	THUMBSIZE       = 64,
	MIN_WIDTH       = (THUMBSIZE * 2),
	MIN_HEIGHT      = (THUMBSIZE * 3),
	DEF_WIDTH       = 600,
	DEF_HEIGHT      = 460,
	PPM_DEPTH       = 3,
	DATA_DEPTH      = 4,
	DATA_SIZE       = (THUMBSIZE * THUMBSIZE * DATA_DEPTH),
	PPM_HEADER_SIZE = (sizeof(PPM_HEADER) - 1),
	PPM_COLOR_SIZE  = (sizeof(PPM_COLOR) - 1),
	PPM_BUFSIZE     = 8,
	BUF_SIZE        = 512,
	MARGIN          = 16,
	MAXICONS        = 256,
	NLINES          = 2,
	BYTE            = 8,
	DOUBLECLICK     = 250,
	NAMEWIDTH       = ((int)(THUMBSIZE * 1.75)),

	/* update time rate for rectangular selection */
	RECTTIME        = 32,

	SCROLL_STEP     = 5,            /* pixels per scroll */
	SCROLLER_SIZE   = 32,           /* size of the scroller */
	HANDLE_MAX_SIZE = (SCROLLER_SIZE - 2),
	SCROLLER_MIN    = 16,           /* min lines to scroll for the scroller to change */
};

enum {
	COLOR_BG,
	COLOR_FG,
	COLOR_LAST,
};

enum {
	ATOM_PAIR,
	COMPOUND_TEXT,
	DELETE,
	MULTIPLE,
	TARGETS,
	TEXT,
	TIMESTAMP,
	UTF8_STRING,
	WM_DELETE_WINDOW,
	_NET_WM_ICON,
	_NET_WM_NAME,
	_NET_WM_PID,
	_NET_WM_WINDOW_TYPE,
	_NET_WM_WINDOW_TYPE_NORMAL,
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
	int start;

	Display *dpy;
	Atom atoms[ATOM_LAST];
	Cursor cursors[CURSOR_LAST];
	GC stipgc, gc;
	Window win;
	XftColor normal[COLOR_LAST];
	XftColor select[COLOR_LAST];
	XftFont *font;
	Pixmap pix;
	Pixmap namepix;
	Pixmap stipple;
	Pixmap rectbord;        /* rectangular selection border */
	int ellipsisw;
	int fonth;
	int hasthumb;
	const char **states;

	pthread_mutex_t rowlock;
	struct Thumb **thumbs;
	struct Thumb *thumbhead;
	int (*linelens)[2];       /* length of first and second text lines */
	int nitems;
	int nstates;
	char ***items;

	struct Selection *sel;  /* list of selections */
	struct Selection **issel;/* array of pointers to Selections */
	Time seltime;

	int w, h;               /* window width and height */
	int pixw, pixh;
	int itemw, itemh;
	int ydiff;              /* how much the icons are scrolled up */
	int ncols, nrows;       /* number of columns and rows visible at a time */
	int maxrow;             /* maximum value .row can scroll */
	int row;                /* index of first visible row */
	int x0;                 /* position of first column */

	struct Icon deficon;
	struct Icon *icons;
	int nicons;
	int *foundicons;

	int clickx, clicky;

	const char *title;
	const char *class;

	int *lastitemp;

	XSyncAlarmAttributes syncattr;
	int syncevent;
	Window scroller;
	int handlew;            /* size of scroller handle */

	enum {
		STATE_NORMAL,
		STATE_SELECTING,
		STATE_SCROLLING,
	} state;
};

static char *atomnames[ATOM_LAST] = {
	[ATOM_PAIR]                  = "ATOM_PAIR",
	[COMPOUND_TEXT]              = "COMPOUND_TEXT",
	[UTF8_STRING]                = "UTF8_STRING",
	[WM_DELETE_WINDOW]           = "WM_DELETE_WINDOW",
	[DELETE]                     = "DELETE",
	[MULTIPLE]                   = "MULTIPLE",
	[TARGETS]                    = "TARGETS",
	[TEXT]                       = "TEXT",
	[TIMESTAMP]                  = "TIMESTAMP",
	[_NET_WM_ICON]               = "_NET_WM_ICON",
	[_NET_WM_NAME]               = "_NET_WM_NAME",
	[_NET_WM_PID]                = "_NET_WM_PID",
	[_NET_WM_WINDOW_TYPE]        = "_NET_WM_WINDOW_TYPE",
	[_NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	[_NULL]                      = "NULL",
};

static int
createwin(Widget wid, const char *class, const char *name, const char *geom, int argc, char *argv[], unsigned long *icon, size_t iconsize)
{
	int x, y;
	int dx, dy, dw, dh;
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
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWEventMask,
		&(XSetWindowAttributes){
			.background_pixel = wid->normal[COLOR_BG].pixel,
			.event_mask = StructureNotifyMask | ExposureMask
			            | KeyPressMask | PointerMotionMask
			            | ButtonReleaseMask | ButtonPressMask,
		}
	);
	wid->namepix = XCreatePixmap(
		wid->dpy,
		wid->win,
		NAMEWIDTH,
		wid->fonth,
		DEPTH(wid->dpy)
	);
	if (wid->win == None)
		return RET_ERROR;
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
		class,
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
	return RET_OK;
}

/* get color from color string */
static int
ealloccolor(Display *dpy, const char *s, XftColor *color)
{
	if(!XftColorAllocName(dpy, VISUAL(dpy), COLORMAP(dpy), s, color))
		return RET_ERROR;
	return RET_OK;
}

static void
getresource(XrmDatabase xdb, const char *class, const char *name, const char *resource, char **val)
{
	XrmValue xval;
	char *type;
	char classbuf[BUF_SIZE], namebuf[BUF_SIZE];

	(void)snprintf(classbuf, BUF_SIZE, "%s.%s", class, resource);
	(void)snprintf(namebuf, BUF_SIZE, "%s.%s", name, resource);
	if (XrmGetResource(xdb, namebuf, classbuf, &type, &xval) == True) {
		*val = xval.addr;
	}
}

static int
textwidth(Widget wid, const char *text, int len)
{
	XGlyphInfo box;

	XftTextExtentsUtf8(wid->dpy, wid->font, text, len, &box);
	return box.width;
}

static int
inittheme(Widget wid, const char *class, const char *name)
{
	XrmDatabase xdb;
	char *bg, *fg, *selbg, *selfg, *font;
	char *xrm;

	bg = DEF_BG;
	fg = DEF_FG;
	selbg = DEF_SELBG;
	selfg = DEF_SELFG;
	font = DEF_FONT;
	xdb = NULL;
	if ((xrm = XResourceManagerString(wid->dpy)) != NULL && (xdb = XrmGetStringDatabase(xrm)) != NULL) {
		getresource(xdb, class, name, "background", &bg);
		getresource(xdb, class, name, "foreground", &fg);
		getresource(xdb, class, name, "selbackground", &selbg);
		getresource(xdb, class, name, "selforeground", &selfg);
		getresource(xdb, class, name, "faceName", &font);
	}
	if (ealloccolor(wid->dpy, bg, &wid->normal[COLOR_BG]) == -1)
		goto error_0;
	if (ealloccolor(wid->dpy, fg, &wid->normal[COLOR_FG]) == -1)
		goto error_1;
	if (ealloccolor(wid->dpy, selbg, &wid->select[COLOR_BG]) == -1)
		goto error_2;
	if (ealloccolor(wid->dpy, selfg, &wid->select[COLOR_FG]) == -1)
		goto error_3;
	if ((wid->font = XftFontOpenXlfd(wid->dpy, DefaultScreen(wid->dpy), font)) == NULL)
		if ((wid->font = XftFontOpenName(wid->dpy, DefaultScreen(wid->dpy), font)) == NULL)
			goto error_4;
	wid->fonth = wid->font->height;
	wid->itemw = THUMBSIZE * 2;
	wid->itemh = THUMBSIZE + 3 * wid->fonth;
	wid->ellipsisw = textwidth(wid, ELLIPSIS, strlen(ELLIPSIS));
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RET_OK;
	XftFontClose(wid->dpy, wid->font);
error_4:
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_FG]);
error_3:
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_BG]);
error_2:
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_FG]);
error_1:
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_BG]);
error_0:
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RET_ERROR;
}

static int
calcsize(Widget wid, int w, int h)
{
	int ncols, nrows, ret;
	double d;

	ret = 0;
	if (wid->w == w && wid->h == h)
		return 0;
	etlock(&wid->rowlock);
	ncols = wid->ncols;
	nrows = wid->nrows;
	if (w >= 0 && h >= 0) {
		wid->w = w;
		wid->h = h;
	}
	wid->ncols = max(wid->w / wid->itemw, 1);
	wid->nrows = max(wid->h / wid->itemh + (wid->h % wid->itemh ? 2 : 1), 1);
	wid->ydiff = 0;
	wid->x0 = max((wid->w - wid->ncols * wid->itemw) / 2, 0);
	wid->maxrow = wid->nitems / wid->ncols - wid->h / wid->itemh + (wid->nitems % wid->ncols != 0 ? 1 : 0);
	wid->maxrow = max(wid->maxrow, 1);
	d = (double)wid->maxrow / SCROLLER_MIN;
	d = (d < 1.0 ? 1.0 : d);
	wid->handlew = max(SCROLLER_SIZE / d - 2, 1);
	wid->handlew = min(wid->handlew, HANDLE_MAX_SIZE);
	if (wid->handlew == HANDLE_MAX_SIZE && wid->maxrow > 1)
		wid->handlew = HANDLE_MAX_SIZE - 1;
	if (ncols != wid->ncols || nrows != wid->nrows) {
		if (wid->pix != None)
			XFreePixmap(wid->dpy, wid->pix);
		if (wid->rectbord != None)
			XFreePixmap(wid->dpy, wid->rectbord);
		wid->pixw = wid->ncols * wid->itemw;
		wid->pixh = wid->nrows * wid->itemh;
		wid->pix = XCreatePixmap(wid->dpy, wid->win, wid->pixw, wid->pixh, DEPTH(wid->dpy));
		wid->rectbord = XCreatePixmap(wid->dpy, ROOT(wid->dpy), wid->w, wid->h, CLIP_DEPTH);
		ret = 1;
	}
	etunlock(&wid->rowlock);
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
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->namepix, wid->gc, 0, 0, NAMEWIDTH, wid->fonth);
	XSetForeground(wid->dpy, wid->gc, color[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->namepix, wid->gc, x, 0, w, wid->fonth);
	draw = XftDrawCreate(wid->dpy, pix, VISUAL(wid->dpy), COLORMAP(wid->dpy));
	XftDrawStringUtf8(draw, &color[COLOR_FG], wid->font, x, wid->font->ascent, text, len);
	XftDrawDestroy(draw);
}

static void
setrow(Widget wid, int row)
{
	etlock(&wid->rowlock);
	wid->row = row;
	etunlock(&wid->rowlock);
}

static void
drawicon(Widget wid, int index, int x, int y)
{
	XGCValues val;
	Pixmap pix, mask;

	if (wid->foundicons[index] >= 0 && wid->foundicons[index] < wid->nicons &&
	    wid->icons[wid->foundicons[index]].pix != None && wid->icons[wid->foundicons[index]].mask != None) {
		pix = wid->icons[wid->foundicons[index]].pix;
		mask = wid->icons[wid->foundicons[index]].mask;
	} else {
		pix = wid->deficon.pix;
		mask = wid->deficon.mask;
	}
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
			XSetForeground(wid->dpy, wid->gc, wid->select[COLOR_BG].pixel);
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
			XSetForeground(wid->dpy, wid->gc, wid->select[COLOR_BG].pixel);
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
	int i, nlines;
	int textx, maxw;
	int textw, w, textlen, len;
	int extensionw, extensionlen;
	char *text, *extension;

	if (wid->issel != NULL && wid->issel[index])
		color = wid->select;
	else
		color = wid->normal;
	text = wid->items[index][ITEM_NAME];
	textlen = strlen(text);
	textw = textwidth(wid, text, textlen);
	nlines = 1;
	textx = x + wid->itemw / 2 - NAMEWIDTH / 2;
	extension = NULL;
	if (textw >= NAMEWIDTH) {
		textlen = len = 0;
		w = 0;
		while (w < NAMEWIDTH) {
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
			nlines++;
		} else {
			textlen = len;
			textw = w;
		}
	}
	maxw = 0;
	for (i = 0; i < nlines; i++) {
		textw = min(NAMEWIDTH, textw);
		maxw = max(textw, maxw);
		drawtext(
			wid,
			wid->namepix, color,
			max(NAMEWIDTH / 2 - textw / 2, 0),
			text, textlen
		);
		if (wid->linelens != NULL)
			wid->linelens[index][i] = min(NAMEWIDTH, textw);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			NAMEWIDTH, wid->fonth,
			textx, y + wid->itemh - (2.5 - i) * wid->fonth
		);
		if (i + 1 < nlines) {
			while (isspace(text[textlen]))
				textlen++;
			text += textlen;
			textlen = strlen(text);
			textw = textwidth(wid, text, textlen);
		}
	}
	if (wid->lastitemp != NULL && index == *wid->lastitemp) {
		XSetForeground(wid->dpy, wid->gc, color[COLOR_FG].pixel);
		XDrawRectangle(
			wid->dpy,
			wid->pix,
			wid->gc,
			x + wid->itemw / 2 - maxw / 2 - 1,
			y + wid->itemh - 2.5 * wid->fonth - 1,
			maxw + 1, i * wid->fonth + 1
		);
	}
	if (textw >= NAMEWIDTH &&
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
			y + wid->itemh - (3.5 - nlines) * wid->fonth
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
			y + wid->itemh - (3.5 - nlines) * wid->fonth
		);
	}
}

static void
drawitem(Widget wid, int index)
{
	int i, x, y;

	i = index - wid->row * wid->ncols;
	x = i % wid->ncols;
	y = (i / wid->ncols) % wid->nrows;
	x *= wid->itemw;
	y *= wid->itemh;
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->pix, wid->gc, x, y, wid->itemw, wid->itemh);
	drawicon(wid, index, x, y);
	drawlabel(wid, index, x, y);
}

static void
drawitems(Widget wid)
{
	int i, n;

	etlock(&wid->rowlock);
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->pix, wid->gc, 0, 0, wid->w, wid->nrows * wid->itemh);
	n = min(wid->nitems, wid->row * wid->ncols + wid->nrows * wid->ncols);
	for (i = wid->row * wid->ncols; i < n; i++) {
		drawitem(wid, i);
	}
	etunlock(&wid->rowlock);
}

static void
commitdraw(Widget wid)
{
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
		return;
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
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_FG].pixel);
	XFillRectangle(wid->dpy, wid->win, wid->gc, 0, 0, wid->w, wid->h);
	XChangeGC(
		wid->dpy,
		wid->gc,
		GCClipMask,
		&(XGCValues) {
			.clip_mask = None,
		}
	);
}

static void
settitle(Widget wid)
{
	char title[TITLE_SIZE];
	char nitems[STATUS_SIZE];
	char *selitem, *status;
	int scrollpct;                  /* scroll percentage */
	int lastitem;

	if (wid->row == 0 && wid->maxrow > 1)
		scrollpct = 0;
	else
		scrollpct = 100 * ((double)(wid->row + 1) / wid->maxrow);
	(void)snprintf(nitems, STATUS_SIZE, "%d items", wid->nitems - 1);
	selitem = "";
	status = nitems;
	if (wid->lastitemp != NULL) {
		lastitem = *wid->lastitemp;
		selitem = (lastitem > 0 ? wid->items[lastitem][ITEM_NAME] : "");
		status = (lastitem > 0 ? wid->items[lastitem][ITEM_STATUS] : nitems);
	}
	(void)snprintf(
		title, TITLE_SIZE,
		"%s%s%s (%s) - %s (%d%%)",
		wid->title,
		(strcmp(wid->title, "/") != 0 ? "/" : ""),
		selitem,
		status,
		wid->class,
		scrollpct
	);
	XmbSetWMProperties(wid->dpy, wid->win, title, title, NULL, 0, NULL, NULL, NULL);
	XChangeProperty(
		wid->dpy,
		wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		title,
		strlen(title)
	);
}

static int
gethandlepos(Widget wid)
{
	int row;

	if (wid->ydiff >= wid->itemh)
		row = wid->maxrow;
	else
		row = wid->row;
	return (HANDLE_MAX_SIZE - wid->handlew) * ((double)row / wid->maxrow);
}

static void
drawscroller(Widget wid, int y)
{
	Pixmap pix;

	if ((pix = XCreatePixmap(wid->dpy, wid->scroller, SCROLLER_SIZE, SCROLLER_SIZE, DEPTH(wid->dpy))) == None)
		return;
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, pix, wid->gc, 0, 0, SCROLLER_SIZE, SCROLLER_SIZE);
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_FG].pixel);
	XFillRectangle(wid->dpy, pix, wid->gc, 1, y + 1, HANDLE_MAX_SIZE, wid->handlew);
	XSetWindowBackgroundPixmap(wid->dpy, wid->scroller, pix);
	XClearWindow(wid->dpy, wid->scroller);
	XFreePixmap(wid->dpy, pix);
}

static int
scroll(struct Widget *wid, int y)
{
	int prevhand, newhand;          /* position of the scroller handle */
	int prevrow, newrow;

	if (wid->nitems / wid->ncols + (wid->nitems % wid->ncols != 0 ? 1 : 0) < wid->nrows)
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
		if (newrow >= wid->maxrow) {
			wid->ydiff = wid->itemh;
			newrow = wid->maxrow - 1;
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
	*x -= wid->x0;
	if (*x < 0 || *x >= wid->ncols * wid->itemw)
		return -1;
	if (*y < 0 || *y >= wid->h)
		return -1;
	*y += ydiff;
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
	int i, j;
	int iconx, textx, texty;

	if ((i = getitem(wid, wid->row, wid->ydiff, &x, &y)) < 0)
		return -1;
	if (i < 0 || i >= wid->nitems)
		return -1;
	iconx = (wid->itemw - THUMBSIZE) / 2;
	if (x >= iconx && x < iconx + THUMBSIZE && y >= 0 && y < THUMBSIZE)
		return i;
	if (wid->linelens == NULL)
		return -1;
	for (j = 0; j < NLINES; j++) {
		textx = (wid->itemw - wid->linelens[i][j]) / 2;
		texty = wid->itemh - (2.5 - j) * wid->fonth;
		if (x >= textx && x < textx + wid->linelens[i][j] &&
		    y >= texty && y < texty + wid->fonth) {
			return i;
		}
	}
	return -1;
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
		free(tmp);
	}
	sel = wid->sel;
	while (sel != NULL) {
		tmp = sel;
		sel = sel->next;
		free(tmp);
	}
	wid->sel = NULL;
	free(wid->thumbs);
	wid->thumbs = NULL;
	free(wid->linelens);
	wid->linelens = NULL;
	free(wid->issel);
	wid->issel = NULL;
}

Widget
initwidget(const char *appclass, const char *appname, const char *geom, const char *states[], size_t nstates, int argc, char *argv[], unsigned long *icon, size_t iconsize, int hasthumb)
{
	XSyncSystemCounter *counters;
	XpmAttributes xa;
	Widget wid;
	int ncounters, tmp, i;

	wid = NULL;
	if ((wid = malloc(sizeof(*wid))) == NULL)
		goto error_pre;
	*wid = (struct Widget){
		.win = None,
		.scroller = None,
		.start = 0,
		.hasthumb = hasthumb,
		.states = states,
		.nstates = nstates,
		.rowlock = PTHREAD_MUTEX_INITIALIZER,
		.thumbs = NULL,
		.thumbhead = NULL,
		.linelens = NULL,
		.icons = NULL,
		.lastitemp = NULL,
		.clickx = 0,
		.clicky = 0,
		.title = "",
		.class = appclass,
		.sel = NULL,
		.issel = NULL,
		.seltime = 0,
		.pix = None,
		.state = STATE_NORMAL,
		.rectbord = None,
		.stipple = None,
		.syncattr = (XSyncAlarmAttributes){
			.trigger.counter        = None,
			.trigger.value_type     = XSyncRelative,
			.trigger.test_type      = XSyncPositiveComparison,
		},
	};
	if (!XInitThreads())
		goto error_pre;
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		goto error_pre;
	if ((wid->dpy = XOpenDisplay(NULL)) == NULL)
		goto error_pre;
	if (!XSyncQueryExtension(wid->dpy, &wid->syncevent, &tmp))
		goto error_pre;
	if (!XSyncInitialize(wid->dpy, &tmp, &tmp))
		goto error_pre;
	XInternAtoms(wid->dpy, atomnames, ATOM_LAST, False, wid->atoms);
	if (fcntl(XConnectionNumber(wid->dpy), F_SETFD, FD_CLOEXEC) == -1)
		goto error_dpy;
	if ((wid->gc = XCreateGC(wid->dpy, ROOT(wid->dpy), GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None)
		goto error_dpy;
	if (inittheme(wid, appclass, appname) == -1)
		goto error_dpy;
	if (createwin(wid, appclass, appname, geom, argc, argv, icon, iconsize) == -1)
		goto error_dpy;
	wid->scroller = XCreateWindow(
		wid->dpy, wid->win,
		0, 0, SCROLLER_SIZE, SCROLLER_SIZE, 1,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWBorderPixel | CWEventMask,
		&(XSetWindowAttributes){
			.background_pixel = wid->normal[COLOR_BG].pixel,
			.border_pixel = wid->normal[COLOR_FG].pixel,
			.event_mask = ButtonPressMask | PointerMotionMask,
		}
	);
	if (wid->scroller == None)
		goto error_win;
	memset(&xa, 0, sizeof(xa));
	if (XpmCreatePixmapFromData(wid->dpy, ROOT(wid->dpy), fileicon_xpm, &wid->deficon.pix, &wid->deficon.mask, &xa) != XpmSuccess)
		goto error_win;
	if (!(xa.valuemask & XpmSize))
		goto error_pix;
	if ((wid->stipple = XCreatePixmap(wid->dpy, ROOT(wid->dpy), STIPPLE_SIZE, STIPPLE_SIZE, CLIP_DEPTH)) == None)
		goto error_pix;
	if ((wid->stipgc = XCreateGC(wid->dpy, wid->stipple, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None)
		goto error_stip;
	XSetForeground(wid->dpy, wid->stipgc, 0);
	XFillRectangle(wid->dpy, wid->stipple, wid->stipgc, 0, 0, STIPPLE_SIZE, STIPPLE_SIZE);
	XSetForeground(wid->dpy, wid->stipgc, 1);
	XFillRectangle(wid->dpy, wid->stipple, wid->stipgc, 0, 0, 1, 1);
	XSetStipple(wid->dpy, wid->gc, wid->stipple);
	wid->cursors[CURSOR_NORMAL] = XCreateFontCursor(wid->dpy, XC_left_ptr);
	wid->cursors[CURSOR_WATCH] = XCreateFontCursor(wid->dpy, XC_watch);
	if ((counters = XSyncListSystemCounters(wid->dpy, &ncounters)) != NULL) {
		for (i = 0; i < ncounters; i++) {
			if (strcmp(counters[i].name, "SERVERTIME") == 0) {
				wid->syncattr.trigger.counter = counters[i].counter;
				break;
			}
		}
		XSyncFreeSystemCounterList(counters);
	}
	if (wid->syncattr.trigger.counter == None)
		goto error_stip;
	XSyncIntToValue(&wid->syncattr.trigger.wait_value, 128);
	XSyncIntToValue(&wid->syncattr.delta, 0);
	return wid;
error_stip:
	XFreePixmap(wid->dpy, wid->stipple);
error_pix:
	XFreePixmap(wid->dpy, wid->deficon.pix);
	XFreePixmap(wid->dpy, wid->deficon.mask);
error_win:
	if (wid->scroller != None)
		XDestroyWindow(wid->dpy, wid->scroller);
	if (wid->win != None)
		XDestroyWindow(wid->dpy, wid->win);
error_dpy:
	XCloseDisplay(wid->dpy);
error_pre:
	if (wid != NULL)
		free(wid);
	return NULL;
}

void
setwidget(Widget wid, const char *title, char ***items, int *foundicons, size_t nitems)
{
	size_t i;

	cleanwidget(wid);
	wid->start = 0;
	wid->items = items;
	wid->nitems = nitems;
	wid->foundicons = foundicons;
	wid->ydiff = 0;
	wid->row = 0;
	wid->ydiff = 0;
	wid->title = title;
	wid->lastitemp = NULL;
	wid->clickx = wid->clicky = 0;
	(void)calcsize(wid, -1, -1);
	wid->issel = calloc(wid->nitems, sizeof(*wid->issel));
	wid->linelens = calloc(wid->nitems, sizeof(*wid->linelens));
	if (wid->hasthumb && (wid->thumbs = malloc(nitems * sizeof(*wid->thumbs))) != NULL) {
		for (i = 0; i < nitems; i++)
			wid->thumbs[i] = NULL;
		wid->thumbhead = NULL;
	}
	settitle(wid);
	drawitems(wid);
	commitdraw(wid);
}

void
mapwidget(Widget wid)
{
	XMapWindow(wid->dpy, wid->win);
}

static void
selectitem(struct Widget *wid, int index, int select)
{
	struct Selection *sel;

	if (wid->issel == NULL || index <= 0 || index >= wid->nitems)
		return;
	if (select && wid->issel[index] == NULL) {
		if ((sel = malloc(sizeof(*sel))) == NULL)
			return;
		*sel = (struct Selection){
			.next = wid->sel,
			.prev = NULL,
			.index = index,
		};
		if (wid->sel != NULL)
			wid->sel->prev = sel;
		wid->sel = sel;
		wid->issel[index] = sel;
	} else if (!select && wid->issel[index] != NULL) {
		sel = wid->issel[index];
		if (sel->next != NULL)
			sel->next->prev = sel->prev;
		if (sel->prev != NULL)
			sel->prev->next = sel->next;
		if (wid->sel == sel)
			wid->sel = sel->next;
		free(sel);
		wid->issel[index] = NULL;
	}
}

static void
selectitems(struct Widget *wid, int a, int b, int select)
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
		selectitem(wid, i, select);
	}
}

static void
unselectitems(struct Widget *wid)
{
	while (wid->sel) {
		selectitem(wid, wid->sel->index, FALSE);
	}
}

static void
ownselection(Widget wid, Time time)
{
	if (wid->sel == NULL)
		return;
	wid->seltime = time;
	XSetSelectionOwner(wid->dpy, XA_PRIMARY, wid->win, time);
}

static unsigned long
getatompairs(Widget wid, Window win, Atom prop, Atom **pairs)
{
	unsigned char *p;
	unsigned long len;
	unsigned long dl;   /* dummy variable */
	int di;             /* dummy variable */
	Atom type;
	size_t size;

	if (XGetWindowProperty(wid->dpy, win, prop, 0L, 0x1FFFFFFF, False, wid->atoms[ATOM_PAIR], &type, &di, &len, &dl, &p) != Success ||
	    len == 0 || p == NULL || type != wid->atoms[ATOM_PAIR]) {
		XFree(p);
		*pairs = NULL;
		return 0;
	}
	size = len * sizeof(**pairs);
	*pairs = emalloc(size);
	memcpy(*pairs, p, size);
	XFree(p);
	return len;
}

static Bool
convert(Widget wid, Window requestor, Atom target, Atom property)
{
	struct Selection *sel;

	if (target == wid->atoms[MULTIPLE]) {
		/*
		 * A MULTIPLE should be handled when processing a
		 * SelectionRequest event.  We do not support nested
		 * MULTIPLE targets.
		 */
		return False;
	}
	if (target == wid->atoms[TIMESTAMP]) {
		/*
		 * According to ICCCM, to avoid some race conditions, it
		 * is important that requestors be able to discover the
		 * timestamp the owner used to acquire ownership.
		 * Requester do that by requesting sellection owners to
		 * convert to `TIMESTAMP`.  Selections owners must
		 * return the timestamp as an `XA_INTEGER`.
		 */
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			XA_INTEGER, 32,
			PropModeReplace,
			(unsigned char *)&wid->seltime,
			1
		);
		return True;
	}
	if (target == wid->atoms[TARGETS]) {
		/*
		 * According to ICCCM, when requested for the `TARGETS`
		 * target, the selection owner should return a list of
		 * atoms representing the targets for which an attempt
		 * to convert the selection will (hopefully) succeed.
		 */
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)(Atom[]){
				XA_STRING,
				wid->atoms[DELETE],
				wid->atoms[MULTIPLE],
				wid->atoms[TARGETS],
				wid->atoms[TEXT],
				wid->atoms[TIMESTAMP],
				wid->atoms[UTF8_STRING],
			},
			NTARGETS
		);
		return True;
	}
	if (target == wid->atoms[DELETE]) {
		unselectitems(wid);
		drawitems(wid);
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			wid->atoms[_NULL],
			8L,
			PropModeReplace,
			"",
			0
		);
		return True;
	}
	if (target == wid->atoms[TEXT] ||
	    target == wid->atoms[UTF8_STRING] ||
	    target == wid->atoms[COMPOUND_TEXT] ||
	    target == XA_STRING) {
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			wid->atoms[UTF8_STRING],
			8L,
			PropModeReplace,
			NULL,
			0
		);
		for (sel = wid->sel; sel != NULL; sel = sel->next) {
			XChangeProperty(
				wid->dpy,
				requestor,
				property,
				wid->atoms[UTF8_STRING],
				8L,
				PropModeAppend,
				wid->items[sel->index][ITEM_PATH],
				strlen(wid->items[sel->index][ITEM_PATH])
			);
			XChangeProperty(
				wid->dpy,
				requestor,
				property,
				wid->atoms[UTF8_STRING],
				8L,
				PropModeAppend,
				"\n",
				1
			);
		}
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			wid->atoms[UTF8_STRING],
			8L,
			PropModeAppend,
			(unsigned char[]){ '\0' },
			1
		);
		return True;
	}
	return False;
}

static void
sendselection(Widget wid, XSelectionRequestEvent *xev)
{
	enum { PAIR_TARGET, PAIR_PROPERTY, PAIR_LAST };
	;
	XSelectionEvent sev;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	unsigned long natoms, i;
	Bool success;

	sev = (XSelectionEvent){
		.type           = SelectionNotify,
		.display        = xev->display,
		.requestor      = xev->requestor,
		.selection      = xev->selection,
		.time           = xev->time,
		.target         = xev->target,
		.property       = None,
	};
	if (xev->time != CurrentTime && xev->time < wid->seltime) {
		/*
		 * According to ICCCM, the selection owner should
		 * compare the timestamp with the period it has owned
		 * the selection and, if the time is outside, refuse the
		 * `SelectionRequest` by sending the requestor window a
		 * `SelectionNotify` event with the property set to
		 * `None` (by means of a `SendEvent` request with an
		 * empty event mask).
		 */
		goto done;
	}
	if (xev->target == wid->atoms[MULTIPLE]) {
		if (xev->property == None)
			goto done;
		natoms = getatompairs(wid, xev->requestor, xev->property, &pairs);
	} else {
		pair[PAIR_TARGET] = xev->target;
		pair[PAIR_PROPERTY] = xev->property;
		pairs = pair;
		natoms = 2;
	}
	success = True;
	for (i = 0; i < natoms; i += 2) {
		if (!convert(wid, xev->requestor, pairs[i + PAIR_TARGET], pairs[i + PAIR_PROPERTY])) {
			success = False;
			pairs[i + PAIR_PROPERTY] = None;
		}
	}
	if (xev->target == wid->atoms[MULTIPLE]) {
		XChangeProperty(
			xev->display,
			xev->requestor,
			xev->property,
			wid->atoms[ATOM_PAIR],
			32, PropModeReplace,
			(unsigned char *)pairs, natoms
		);
		free(pairs);
	}
	if (success) {
		sev.property = (xev->property == None) ? xev->target : xev->property;
	}
done:
	XSendEvent(xev->display, xev->requestor, False, NoEventMask, (XEvent *)&sev);
}

static int
mouseclick(Widget wid, XButtonPressedEvent *ev, Time *lasttime, int *lastitem)
{
	int index, ret;

	ret = -1;
	if (!(ev->state & (ControlMask | ShiftMask)))
		unselectitems(wid);
	if ((index = getpointerclick(wid, ev->x, ev->y)) == -1)
		goto done;
	if (ev->state & ShiftMask)
		selectitems(wid, index, *lastitem, 1);
	selectitem(wid, index, ((ev->state & ControlMask) ? wid->issel[index] == NULL : 1));
	if (!(ev->state & (ControlMask | ShiftMask)) &&
	    index == (*lastitem) && ev->time - (*lasttime) <= DOUBLECLICK) {
		ret = index;
	}
done:
	*lasttime = ev->time;
	*lastitem = index;
	wid->clickx = ev->x;
	wid->clicky = ev->y;
	settitle(wid);
	return ret;
}

static void
rectdraw(Widget wid, int row, int ydiff, int x, int y)
{
	int x0, y0, w, h;

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
	y0 = wid->clicky;
	x0 = wid->clickx;
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

static void
rectselect(Widget wid, int srcrow, int srcydiff, int x, int y)
{
	int row, col, tmp, i, j;

	/* x,y positions of the vertices of the rectangular selection */
	int minx, maxx, miny;

	/*
	 * Those variables begin describing the x,y positions of the
	 * source and destination vertices of the rectangular selection.
	 * But after calling getitem(), they are converted to the
	 * positions of the vertices of the inner item area of the
	 * item below each vertex.
	 */
	int srcx, srcy, dstx, dsty;

	/*
	 * Indices of items at the top left of the rectangular selection
	 * and at bottom right of the rectangular selection.
	 */
	int indexmin, indexmax;

	/*
	 * First and last columns and rows of the items at the
	 * rectangular selection.
	 */
	int colmin, colmax;
	int rowmin;

	miny = min(wid->clicky, y);
	minx = min(wid->clickx, x);
	maxx = max(wid->clickx, x);
	srcx = wid->clickx;
	srcy = wid->clicky;
	dstx = x;
	dsty = y;
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
	if ((i = getitem(wid, srcrow, srcydiff, &srcx, &srcy)) < 0)
		return;
	if ((j = getitem(wid, wid->row, wid->ydiff, &dstx, &dsty)) < 0)
		return;
	indexmin = min(i, j);
	indexmax = max(i, j);
	colmin = indexmin % wid->ncols;
	colmax = indexmax % wid->ncols;
	indexmin = min(indexmin, wid->nitems - 1);
	indexmax = min(indexmax, wid->nitems - 1);
	rowmin = indexmin / wid->ncols;
	for (i = indexmin; i <= indexmax; i++) {
		row = i / wid->ncols;
		col = i % wid->ncols;
		x = wid->x0 + col * wid->itemw + (wid->itemw - THUMBSIZE) / 2;
		y = (row - wid->row + 1) * wid->itemh - 1.5 * wid->fonth + MARGIN - wid->ydiff;
		if ((col == colmin && (minx > x + THUMBSIZE || maxx < x)) ||
		    (col == colmax && (maxx < x || minx > x + THUMBSIZE)) ||
		    (col == colmax && (maxx < x || minx > x + THUMBSIZE)) ||
		    col < colmin || col > colmax){
			continue;
		}
		if (row == rowmin && row >= wid->row && miny > y) {
			continue;
		}
		selectitem(wid, i, TRUE);
	}
}

static int
processevent(Widget wid, XEvent *ev, int *close)
{
	/*
	 * Return TRUE if an event was processed.
	 * Set *close to TRUE if we were asked to close the window.
	 */
	*close = FALSE;
	switch (ev->type) {
	case ClientMessage:
		if ((Atom)ev->xclient.data.l[0] == wid->atoms[WM_DELETE_WINDOW])
			*close = TRUE;
		return TRUE;
	case Expose:
		if (ev->xexpose.count == 0)
			commitdraw(wid);
		return TRUE;
	case ConfigureNotify:
		if (calcsize(wid, ev->xconfigure.width, ev->xconfigure.height)) {
			if (wid->row >= wid->maxrow)
				setrow(wid, wid->maxrow - 1);
			drawitems(wid);
			commitdraw(wid);
		}
		return TRUE;
	case SelectionRequest:
		if (ev->xselectionrequest.selection == XA_PRIMARY)
			sendselection(wid, &ev->xselectionrequest);
		return TRUE;
	case SelectionClear:
		unselectitems(wid);
		drawitems(wid);
		commitdraw(wid);
		return TRUE;
	default:
		return FALSE;
	}
	return FALSE;
}

static int
rectmotion(Widget wid, Time lasttime)
{
	XEvent ev;
	int rectrow, rectydiff;
	int close;

	wid->state = STATE_SELECTING;
	rectrow = wid->row;
	rectydiff = wid->ydiff;
	while (!XNextEvent(wid->dpy, &ev)) {
		if (processevent(wid, &ev, &close)) {
			if (close)
				return WIDGET_CLOSE;
			continue;
		}
		switch (ev.type) {
		case ButtonPress:
		case ButtonRelease:
			wid->state = STATE_NORMAL;
			rectdraw(wid, rectrow, rectydiff, ev.xbutton.x, ev.xbutton.y);
			commitdraw(wid);
			return WIDGET_CONTINUE;
		case MotionNotify:
			if (ev.xmotion.time - lasttime < RECTTIME)
				break;
			unselectitems(wid);
			if (ev.xmotion.y > wid->h)
				(void)scroll(wid, (ev.xmotion.y - wid->h) / SCROLL_STEP);
			else if (ev.xmotion.y < 0)
				(void)scroll(wid, ev.xmotion.y / SCROLL_STEP);
			rectdraw(wid, rectrow, rectydiff, ev.xmotion.x, ev.xmotion.y);
			rectselect(wid, rectrow, rectydiff, ev.xmotion.x, ev.xmotion.y);
			drawitems(wid);
			commitdraw(wid);
			lasttime = ev.xmotion.time;
			break;
		}
	}
	return WIDGET_CONTINUE;
}

static int
scrollerpos(Widget wid)
{
	Window root, child;
	unsigned int mask;
	int rootx, rooty;
	int x, y;
	Bool state;

	state = XQueryPointer(
		wid->dpy,
		wid->scroller,
		&root, &child,
		&rootx, &rooty,
		&x, &y,
		&mask
	);
	if (state == True) {
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
	pos -= wid->handlew / 2;
	pos = max(pos, 0);
	pos = min(pos, maxpos);
	newrow = pos * wid->maxrow / maxpos;
	newrow = max(newrow, 0);
	newrow = min(newrow, wid->maxrow);
	if (newrow == wid->maxrow) {
		wid->ydiff = wid->itemh;
		newrow = wid->maxrow - 1;
	} else {
		wid->ydiff = 0;
	}
	prevrow = wid->row;
	setrow(wid, newrow);
	drawscroller(wid, pos);
	if (prevrow != newrow)
		settitle(wid);
	drawitems(wid);
	commitdraw(wid);
}

static int
scrollmotion(Widget wid, int x, int y)
{
	XSyncAlarm alarm;
	XEvent ev;
	int pos, close, left;

	wid->state = STATE_SCROLLING;
	drawscroller(wid, gethandlepos(wid));
	XMoveWindow(wid->dpy, wid->scroller, x - SCROLLER_SIZE / 2 - 1, y - SCROLLER_SIZE / 2 - 1);
	XMapRaised(wid->dpy, wid->scroller);
	alarm = XSyncCreateAlarm(wid->dpy, ALARMFLAGS, &wid->syncattr);
	left = FALSE;
	while (!XNextEvent(wid->dpy, &ev)) {
		if (processevent(wid, &ev, &close)) {
			if (close)
				return WIDGET_CLOSE;
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
				scrollerset(wid, ev.xmotion.y);
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
			if (ev.xbutton.button == Button2)
				goto done;
			if (ev.xbutton.button != Button1)
				break;
			if (ev.xbutton.window == wid->win)
				goto done;
			if (ev.xbutton.window == wid->scroller) {
				scrollerset(wid, ev.xmotion.y);
				left = TRUE;
			}
			break;
		}
	}
done:
	wid->state = STATE_NORMAL;
	drawitems(wid);
	commitdraw(wid);
	XSyncDestroyAlarm(wid->dpy, alarm);
	XUnmapWindow(wid->dpy, wid->scroller);
	return WIDGET_CONTINUE;
}

WidgetEvent
pollwidget(Widget wid, int *index)
{
	XEvent ev;
	Time lasttime = 0;
	int close = FALSE;
	int lastitem = -1;
	int ignoremotion;

	if (!wid->start)
		XSync(wid->dpy, True);
	wid->start = 1;
	wid->lastitemp = &lastitem;
	ignoremotion = FALSE;
	while (!XNextEvent(wid->dpy, &ev)) {
		if (processevent(wid, &ev, &close)) {
			if (close)
				return WIDGET_CLOSE;
			continue;
		}
		switch (ev.type) {
		case ButtonPress:
			if (ev.xbutton.button == Button1) {
				*index = mouseclick(wid, &ev.xbutton, &lasttime, &lastitem);
				ownselection(wid, ev.xbutton.time);
				drawitems(wid);
				commitdraw(wid);
				if (*index != -1) {
					return WIDGET_OPEN;
				}
			} else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
				if (scroll(wid, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP)))
					drawitems(wid);
				commitdraw(wid);
			} else if (ev.xbutton.button == Button2) {
				if (scrollmotion(wid, ev.xmotion.x, ev.xmotion.y) == WIDGET_CLOSE)
					return WIDGET_CLOSE;
				ignoremotion = TRUE;
			}
			break;
		case ButtonRelease:
			if (ignoremotion)
				ignoremotion = FALSE;
			break;
		case MotionNotify:
			if (ignoremotion)
				break;
			if (lastitem != -1)
				break;
			if (ev.xmotion.state != Button1Mask)
				break;
			if (rectmotion(wid, ev.xmotion.time) == WIDGET_CLOSE)
				return WIDGET_CLOSE;
			lastitem = -1;
			break;
		}
	}
	return WIDGET_CLOSE;
}

void
closewidget(Widget wid)
{
	int i;

	for (i = 0; i < wid->nicons; i++) {
		if (wid->icons[i].pix != None) {
			XFreePixmap(wid->dpy, wid->icons[i].pix);
		}
		if (wid->icons[i].mask != None) {
			XFreePixmap(wid->dpy, wid->icons[i].mask);
		}
	}
	free(wid->icons);
	XFreePixmap(wid->dpy, wid->deficon.pix);
	XFreePixmap(wid->dpy, wid->deficon.mask);
	XFreePixmap(wid->dpy, wid->pix);
	XFreePixmap(wid->dpy, wid->namepix);
	XFreePixmap(wid->dpy, wid->stipple);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_BG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_FG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_BG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_FG]);
	XftFontClose(wid->dpy, wid->font);
	XDestroyWindow(wid->dpy, wid->scroller);
	XDestroyWindow(wid->dpy, wid->win);
	XFreeGC(wid->dpy, wid->gc);
	XCloseDisplay(wid->dpy);
	free(wid);
}

void
xpmtopixmap(Widget wid, char *path, Pixmap *pix, Pixmap *mask)
{
	XpmAttributes xa = { 0 };

	if (XpmReadFileToPixmap(wid->dpy, ROOT(wid->dpy), path, pix, mask, &xa) != XpmSuccess) {
		*pix = None;
		*mask = None;
	}
}

void
openicons(Widget wid, char **paths, int nicons)
{
	int i;

	if (nicons > MAXICONS)
		nicons = MAXICONS;
	if ((wid->icons = calloc(nicons, sizeof(*wid->icons))) == NULL)
		return;
	wid->nicons = nicons;
	for (i = 0; i < nicons; i++) {
		xpmtopixmap(wid, paths[i], &wid->icons[i].pix, &wid->icons[i].mask);
	}
}

static int
checkheader(FILE *fp, char *header, size_t size)
{
	char buf[PPM_BUFSIZE];

	if (fread(buf, 1, size, fp) != size)
		return RET_ERROR;
	if (memcmp(buf, header, size) != 0)
		return RET_ERROR;
	return RET_OK;
}

void
setthumbnail(Widget wid, char *path, int item)
{
	FILE *fp;
	size_t size, i;
	int w, h;
	char buf[DATA_DEPTH];
	char *data;

	if (wid->thumbs == NULL)
		return;
	if ((fp = fopen(path, "rb")) == NULL)
		return;
	if (checkheader(fp, PPM_HEADER, PPM_HEADER_SIZE) == -1)
		goto error_fp;
	w = readsize(fp);
	h = readsize(fp);
	if (w <= 0 || w > THUMBSIZE || h <= 0 || h > THUMBSIZE)
		goto error_fp;
	if (checkheader(fp, PPM_COLOR, PPM_COLOR_SIZE) == -1)
		goto error_fp;
	size = w * h;
	if ((data = malloc(size * DATA_DEPTH)) == NULL)
		goto error_fp;
	for (i = 0; i < size; i++) {
		if (fread(buf, 1, PPM_DEPTH, fp) != PPM_DEPTH)
			goto error_fp;
		data[i * DATA_DEPTH + 0] = buf[2];   /* B */
		data[i * DATA_DEPTH + 1] = buf[1];   /* G */
		data[i * DATA_DEPTH + 2] = buf[0];   /* R */
		data[i * DATA_DEPTH + 3] = '\0';     /* A */
	}
	fclose(fp);
	if ((wid->thumbs[item] = malloc(sizeof(*wid->thumbs[item]))) == NULL)
		goto error_data;
	*wid->thumbs[item] = (struct Thumb){
		.w = w,
		.h = h,
		.img = NULL,
		.next = wid->thumbhead,
	};
	wid->thumbs[item]->img = XCreateImage(
		wid->dpy,
		VISUAL(wid->dpy),
		DEPTH(wid->dpy),
		ZPixmap,
		0, data,
		w, h,
		DATA_DEPTH * BYTE,
		0
	);
	if (wid->thumbs[item]->img == NULL)
		goto error_thumb;
	XInitImage(wid->thumbs[item]->img);
	wid->thumbhead = wid->thumbs[item];
	etlock(&wid->rowlock);
	if (item >= wid->row * wid->ncols && item < wid->row * wid->ncols + wid->nrows * wid->ncols) {
		drawitem(wid, item);
		commitdraw(wid);
		XFlush(wid->dpy);
	}
	etunlock(&wid->rowlock);
	return;
error_thumb:
	free(wid->thumbs[item]);
	wid->thumbs[item] = NULL;
error_data:
	free(data);
	return;
error_fp:
	fclose(fp);
}

void
widgetcursor(Widget wid, int cursor)
{
	if (cursor < 0 || cursor >= CURSOR_LAST)
		cursor = CURSOR_NORMAL;
	XDefineCursor(wid->dpy, wid->win, wid->cursors[cursor]);
}
