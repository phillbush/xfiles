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
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>

#include "util.h"
#include "widget.h"
#include "fileicon.xpm"

/* ellipsis has two dots rather than three; the third comes from the extension */
#define ELLIPSIS        ".."
#define DEF_FONT        "monospace:size=9,DejaVuSansMono:size=9"
#define DEF_BG          "#0A0A0A"
#define DEF_FG          "#FFFFFF"
#define DEF_SELBG       "#121212"
#define DEF_SELFG       "#707880"
#define PPM_HEADER      "P6\n"
#define PPM_COLOR       "255\n"
#define PPM_DEPTH       3
#define DATA_DEPTH      4
#define DATA_SIZE       (THUMBSIZE * THUMBSIZE * DATA_DEPTH)
#define PPM_HEADER_SIZE (sizeof(PPM_HEADER) - 1)
#define PPM_COLOR_SIZE  (sizeof(PPM_COLOR) - 1)
#define PPM_BUFSIZE     8
#define BUF_SIZE        512
#define MIN_WIDTH       (THUMBSIZE * 2)
#define MIN_HEIGHT      (THUMBSIZE * 3)
#define DEF_WIDTH       600
#define DEF_HEIGHT      460
#define MARGIN          16
#define MAXICONS        256
#define THUMBSIZE       64
#define BYTE            8
#define NAMEWIDTH       ((int)(THUMBSIZE * 1.75))
#define VISUAL(d)       (DefaultVisual((d), DefaultScreen((d))))
#define COLORMAP(d)     (DefaultColormap((d), DefaultScreen((d))))
#define DEPTH(d)        (DefaultDepth((d), DefaultScreen((d))))
#define WIDTH(d)        (DisplayWidth((d), DefaultScreen((d))))
#define HEIGHT(d)       (DisplayHeight((d), DefaultScreen((d))))
#define ROOT(d)         (DefaultRootWindow((d)))
#define FLAG(f, b)      (((f) & (b)) == (b))
#define ATOI(c)         (((c) >= '0' && (c) <= '9') ? (c) - '0' : -1)

enum {
	COLOR_BG,
	COLOR_FG,
	COLOR_LAST,
};

enum {
	UTF8_STRING,
	WM_DELETE_WINDOW,
	_NET_WM_ICON,
	_NET_WM_NAME,
	_NET_WM_PID,
	_NET_WM_WINDOW_TYPE,
	_NET_WM_WINDOW_TYPE_NORMAL,
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

struct Widget {
	Display *dpy;
	Atom atoms[ATOM_LAST];
	GC gc;
	Window win;
	XftColor normal[COLOR_LAST];
	XftColor select[COLOR_LAST];
	XftFont *font;
	Pixmap pix;
	Pixmap namepix;
	int ellipsisw;
	int fonth;
	int hasthumb;
	const char **states;

	pthread_mutex_t rowlock;
	struct Thumb **thumbs;
	struct Thumb *thumbhead;
	char ***items;
	size_t nstates;
	size_t nitems;

	int w, h;
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
};

static int
between(int x, int a, int b)
{
	return a <= x && x <= b;
}

static int
max(int x, int y)
{
	return x > y ? x : y;
}

static int
min(int x, int y)
{
	return x < y ? x : y;
}

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
			            | KeyPressMask | ButtonPressMask
			            | PointerMotionMask,
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
		return -1;
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
	return 0;
}

/* get color from color string */
static int
ealloccolor(Display *dpy, const char *s, XftColor *color)
{
	if(!XftColorAllocName(dpy, VISUAL(dpy), COLORMAP(dpy), s, color))
		return -1;
	return 0;
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
		goto error;
	if (ealloccolor(wid->dpy, fg, &wid->normal[COLOR_FG]) == -1)
		goto error;
	if (ealloccolor(wid->dpy, selbg, &wid->select[COLOR_BG]) == -1)
		goto error;
	if (ealloccolor(wid->dpy, selfg, &wid->select[COLOR_FG]) == -1)
		goto error;
	if ((wid->font = XftFontOpenXlfd(wid->dpy, DefaultScreen(wid->dpy), font)) == NULL)
		if ((wid->font = XftFontOpenName(wid->dpy, DefaultScreen(wid->dpy), font)) == NULL)
			goto error;
	wid->fonth = wid->font->height;
	wid->itemw = THUMBSIZE * 2;
	wid->itemh = THUMBSIZE + 3 * wid->fonth;
	wid->ellipsisw = textwidth(wid, ELLIPSIS, strlen(ELLIPSIS));
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return 0;
error:
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_BG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_FG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_BG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_FG]);
	XftFontClose(wid->dpy, wid->font);
	return -1;
}

static int
calcsize(Widget wid, int w, int h)
{
	int ncols, nrows, ret;

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
	if (ncols != wid->ncols || nrows != wid->nrows) {
		if (wid->pix != None)
			XFreePixmap(wid->dpy, wid->pix);
		wid->pixw = wid->ncols * wid->itemw;
		wid->pixh = wid->nrows * wid->itemh;
		wid->pix = XCreatePixmap(wid->dpy, wid->win, wid->pixw, wid->pixh, DEPTH(wid->dpy));
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

	XSetForeground(wid->dpy, wid->gc, color[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->namepix, wid->gc, 0, 0, NAMEWIDTH, wid->fonth);
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
drawentry(Widget wid, int index)
{
	XGCValues val;
	Pixmap pix, mask;
	int i, j, x, y, nlines;
	int textx;
	int textw, w, textlen, len;
	int extensionw, extensionlen;
	char *text, *extension;

	if (wid->foundicons[index] >= 0 && wid->foundicons[index] < wid->nicons &&
	    wid->icons[wid->foundicons[index]].pix != None && wid->icons[wid->foundicons[index]].mask != None) {
		pix = wid->icons[wid->foundicons[index]].pix;
		mask = wid->icons[wid->foundicons[index]].mask;
	} else {
		pix = wid->deficon.pix;
		mask = wid->deficon.mask;
	}
	text = wid->items[index][0];
	textlen = strlen(text);
	textw = textwidth(wid, text, textlen);
	nlines = 1;
	i = index - wid->row * wid->ncols;
	x = i % wid->ncols;
	y = (i / wid->ncols) % wid->nrows;
	x *= wid->itemw;
	y *= wid->itemh;
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->pix, wid->gc, x, y, wid->itemw, wid->itemh);
	textx = x + wid->itemw / 2 - NAMEWIDTH / 2;
	extension = NULL;
	if (textw >= NAMEWIDTH) {
		textlen = len = 0;
		w = 0;
		while (w < NAMEWIDTH) {
			while (isspace(text[len]))
				len++;
			textlen = len;
			textw = w;
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
	for (j = 0; j < nlines; j++) {
		drawtext(
			wid,
			wid->namepix, wid->normal,
			max(NAMEWIDTH / 2 - textw / 2, 0),
			text, textlen
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			NAMEWIDTH, wid->fonth,
			textx, y + wid->itemh - (2.5 - j) * wid->fonth
		);
		if (j + 1 < nlines) {
			text += textlen;
			textlen = strlen(text);
			textw = textwidth(wid, text, textlen);
		}
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
			wid->namepix, wid->normal,
			0,
			ELLIPSIS, strlen(ELLIPSIS)
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			wid->ellipsisw, wid->fonth,
			textx + min(NAMEWIDTH, textw) - extensionw - wid->ellipsisw,
			y + wid->itemh - (3.5 - nlines) * wid->fonth
		);

		/* draw extension */
		drawtext(
			wid,
			wid->namepix, wid->normal,
			0, extension, extensionlen
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			extensionw, wid->fonth,
			textx + min(NAMEWIDTH, textw) - extensionw,
			y + wid->itemh - (3.5 - nlines) * wid->fonth
		);
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
		val.clip_mask = None;
		XChangeGC(wid->dpy, wid->gc, GCClipMask, &val);
	}
}

static void
drawentries(Widget wid)
{
	int i, n;

	etlock(&wid->rowlock);
	XSetForeground(wid->dpy, wid->gc, wid->normal[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->pix, wid->gc, 0, 0, wid->w, wid->nrows * wid->itemh);
	n = min(wid->nitems, wid->row * wid->ncols + wid->nrows * wid->ncols);
	for (i = wid->row * wid->ncols; i < n; i++) {
		drawentry(wid, i);
	}
	etunlock(&wid->rowlock);
}

static void
commitdraw(Widget wid)
{
	XCopyArea(
		wid->dpy,
		wid->pix, wid->win,
		wid->gc,
		0, wid->ydiff - MARGIN,
		wid->pixw, wid->pixh,
		wid->x0, 0
	);
}

static int
scroll(struct Widget *wid, int y)
{
	int prevrow;

	prevrow = wid->row;
	if (y > 0 && wid->row < wid->maxrow && (wid->ydiff += y) >= wid->itemh) {
		wid->ydiff = 0;
		if (++wid->row == wid->maxrow) {
			wid->ydiff = wid->itemh;
			setrow(wid, wid->maxrow - 1);
		}
	} else if (y < 0 && (wid->ydiff += y) < 0) {
		wid->ydiff = wid->itemh + y;
		if (wid->row-- == 0) {
			wid->ydiff = 0;
			setrow(wid, 0);
		}
	}
	return prevrow != wid->row;
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

Widget
initwidget(const char *appclass, const char *appname, const char *geom, const char *states[], size_t nstates, int argc, char *argv[], unsigned long *icon, size_t iconsize, int hasthumb)
{
	Widget wid;
	XpmAttributes xa;
	char *atomnames[ATOM_LAST] = {
		[UTF8_STRING]                = "UTF8_STRING",
		[WM_DELETE_WINDOW]           = "WM_DELETE_WINDOW",
		[_NET_WM_ICON]               = "_NET_WM_ICON",
		[_NET_WM_NAME]               = "_NET_WM_NAME",
		[_NET_WM_PID]                = "_NET_WM_PID",
		[_NET_WM_WINDOW_TYPE]        = "_NET_WM_WINDOW_TYPE",
		[_NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	};

	wid = NULL;
	if ((wid = malloc(sizeof(*wid))) == NULL)
		goto error_pre;
	*wid = (struct Widget){
		.hasthumb = hasthumb,
		.states = states,
		.nstates = nstates,
		.rowlock = PTHREAD_MUTEX_INITIALIZER,
		.thumbs = NULL,
		.thumbhead = NULL,
		.icons = NULL,
	};
	if (!XInitThreads())
		goto error_pre;
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		goto error_pre;
	if ((wid->dpy = XOpenDisplay(NULL)) == NULL)
		goto error_pre;
	XInternAtoms(wid->dpy, atomnames, ATOM_LAST, False, wid->atoms);
	if (fcntl(XConnectionNumber(wid->dpy), F_SETFD, FD_CLOEXEC) == -1)
		goto error_dpy;
	if ((wid->gc = XCreateGC(wid->dpy, ROOT(wid->dpy), 0, NULL)) == None)
		goto error_dpy;
	if (inittheme(wid, appclass, appname) == -1)
		goto error_dpy;
	if (createwin(wid, appclass, appname, geom, argc, argv, icon, iconsize) == -1)
		goto error_dpy;
	memset(&xa, 0, sizeof(xa));
	if (XpmCreatePixmapFromData(wid->dpy, ROOT(wid->dpy), fileicon_xpm, &wid->deficon.pix, &wid->deficon.mask, &xa) != XpmSuccess)
		goto error_win;
	if (!(xa.valuemask & XpmSize))
		goto error_pix;
	return wid;
error_pix:
	XFreePixmap(wid->dpy, wid->deficon.pix);
	XFreePixmap(wid->dpy, wid->deficon.mask);
error_win:
	XDestroyWindow(wid->dpy, wid->win);
error_dpy:
	XCloseDisplay(wid->dpy);
error_pre:
	if (wid != NULL)
		free(wid->states);
	free(wid);
	return NULL;
}

void
setwidget(Widget wid, const char *doc, char ***items, int *foundicons, size_t nitems)
{
	struct Thumb *thumb, *tmp;
	size_t i;

	thumb = wid->thumbhead;
	while (thumb != NULL) {
		tmp = thumb;
		thumb = thumb->next;
		XDestroyImage(tmp->img);
		free(tmp);
	}
	if (wid->thumbs != NULL)
		free(wid->thumbs);
	XmbSetWMProperties(wid->dpy, wid->win, doc, doc, NULL, 0, NULL, NULL, NULL);
	XChangeProperty(
		wid->dpy,
		wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		doc,
		strlen(doc)
	);
	XChangeProperty(
		wid->dpy,
		wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING],
		8,
		PropModeAppend,
		" - XFiles", 10
	);
	wid->items = items;
	wid->nitems = nitems;
	wid->foundicons = foundicons;
	wid->ydiff = 0;
	(void)calcsize(wid, -1, -1);
	if (wid->hasthumb) {
		wid->thumbs = malloc(nitems * sizeof(*wid->thumbs));
		for (i = 0; i < nitems; i++)
			wid->thumbs[i] = NULL;
		wid->thumbhead = NULL;
	}
	drawentries(wid);
	commitdraw(wid);
}

void
mapwidget(Widget wid)
{
	XMapWindow(wid->dpy, wid->win);
}

WidgetEvent
pollwidget(Widget wid, char ***entry)
{
	XEvent ev;

	XSync(wid->dpy, True);
	while (!XNextEvent(wid->dpy, &ev)) {
		switch (ev.type) {
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wid->atoms[WM_DELETE_WINDOW])
				return WIDGET_CLOSE;
			break;
		case Expose:
			if (ev.xexpose.count == 0)
				commitdraw(wid);
			break;
		case ConfigureNotify:
			if (calcsize(wid, ev.xconfigure.width, ev.xconfigure.height)) {
				if (wid->row >= wid->maxrow)
					setrow(wid, wid->maxrow - 1);
				drawentries(wid);
				commitdraw(wid);
			}
			break;
		case ButtonPress:
			if (ev.xbutton.button == Button1) {
				// TODO
			} else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
				if (scroll(wid, (ev.xbutton.button == Button4 ? -5 : +5)))
					drawentries(wid);
				commitdraw(wid);
			}
			break;
		case MotionNotify:
			// TODO
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
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_BG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->normal[COLOR_FG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_BG]);
	XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->select[COLOR_FG]);
	XftFontClose(wid->dpy, wid->font);
	XDestroyWindow(wid->dpy, wid->win);
	XFreeGC(wid->dpy, wid->gc);
	XCloseDisplay(wid->dpy);
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
		return -1;
	if (memcmp(buf, header, size) != 0)
		return -1;
	return 0;
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
		goto error;
	w = readsize(fp);
	h = readsize(fp);
	if (w <= 0 || w > THUMBSIZE || h <= 0 || h > THUMBSIZE)
		goto error;
	if (checkheader(fp, PPM_COLOR, PPM_COLOR_SIZE) == -1)
		goto error;
	size = w * h;
	if ((data = malloc(size * DATA_DEPTH)) == NULL)
		goto error;
	for (i = 0; i < size; i++) {
		if (fread(buf, 1, PPM_DEPTH, fp) != PPM_DEPTH)
			goto error;
		data[i * DATA_DEPTH + 0] = buf[2];   /* B */
		data[i * DATA_DEPTH + 1] = buf[1];   /* G */
		data[i * DATA_DEPTH + 2] = buf[0];   /* R */
		data[i * DATA_DEPTH + 3] = '\0';     /* A */
	}
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
		drawentry(wid, item);
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
error:
	fclose(fp);
}
