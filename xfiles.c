#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>
#include <Imlib2.h>
#include "file.xpm"
#include "folder.xpm"

#define ELLIPSIS     "…"
#define CLASS        "XFiles"
#define TITLE        "XFiles"
#define THUMBBORDER  3          /* thumbnail border (for highlighting) */
#define DOUBLECLICK  250
#define DEV_NULL     "/dev/null"
#define DOTDOT       ".."

/* fg and bg colors */
enum {
	COLOR_FG,
	COLOR_BG,
	COLOR_LAST
};

/* atoms */
enum {
	UTF8_STRING,
	WM_DELETE_WINDOW,
	_NET_WM_NAME,
	_NET_WM_PID,
	_NET_WM_WINDOW_TYPE,
	_NET_WM_WINDOW_TYPE_NORMAL,
	ATOM_LAST,
};

/* unselected and selected pixmaps */
enum {
	PIX_UNSEL = 0,
	PIX_SEL = 1,
	PIX_LAST = 2,
};

/* history navigation direction */
enum {
	BACK,
	FORTH,
};

/* extra mouse buttons, not covered by XLIB */
enum {
	BUTTON8 = 8,
	BUTTON9 = 9,
};

/* working directory history entry */
struct Histent {
	struct Histent *prev, *next;
	char *cwd;
};

/* position and size of a rectangle */
struct Rect {
	int x, y, w, h;
};

/* horizontal position and size of a line segment */
struct Line {
	int x, w;
};

/* entry of queue of thumbnail images to be rendered to their entries' pixmap */
struct ThumbImg {
	struct ThumbImg *next;          /* pointer to next entry */
	Imlib_Image img;                /* image to be rendered */
	int index;                      /* index of entry */
};

/* file manager */
struct FM {
	struct Entry **entries; /* array of pointer to entries */
	struct Entry *selected; /* list of selected entries */
	struct Rect dirrect;    /* size and position of default thumbnail for directories */
	struct Rect filerect;   /* size and position of default thumbnail for files */
	struct Histent *hist;   /* cwd history; pointer to last cwd history entry */
	struct Histent *curr;   /* current point in history */
	struct ThumbImg *queue; /* first item on queue of thumbnail images */
	struct ThumbImg *last;  /* last item on queue of thumbnail images */
	Window win;             /* main window */
	Pixmap main, scroll;    /* pixmap for main window and scroll bar */
	Pixmap dir[PIX_LAST];   /* default pixmap for directories thumbnails */
	Pixmap file[PIX_LAST];  /* default pixmap for file thumbnails */
	int capacity;           /* capacity of entries */
	int nentries;           /* number of entries */
	int row;                /* index of first visible column */
	int ydiff;              /* how much the icons are scrolled up */
	int winw, winh;         /* size of main window */
	int thumbx, thumby;     /* position of thumbnail from entry top left corner */
	int x0;                 /* position of first entry */
	int entryw, entryh;     /* size of each entry */
	int scrollh, scrolly;   /* size and position of scroll bar handle */
	int textw;              /* width of each file name */
	int textx;              /* position of each name from entry top left corner */
	int texty0, texty1;     /* position of each line from entry top left corner */
	int ncol, nrow;         /* number of columns and rows visible at a time */
	int maxrow;             /* maximum value fm->row can scroll */
};

/* directory entry */
struct Entry {
	struct Entry *sprev;    /* for the linked list of selected entries */
	struct Entry *snext;    /* for the linked list of selected entries */
	struct Rect thumb;      /* position and size of the thumbnail */
	struct Line line[2];    /* position and size of both text lines */
	Pixmap pix[PIX_LAST];   /* unselected and selected content of the widget */
	int issel;              /* whether entry is selected */
	int isdir;              /* whether entry is a directory */
	int drawn;              /* whether unsel and sel pixmaps were drawn */
	char *name;             /* file name */
};

/* draw context */
struct DC {
	GC gc;
	XftColor normal[COLOR_LAST];
	XftColor select[COLOR_LAST];
	XftColor scroll[COLOR_LAST];
	FcPattern *pattern;
	XftFont **fonts;
	size_t nfonts;
	int fonth;
};

/* configuration */
struct Config {
	const char *thumbnailer;
	const char *opener;

	const char *dirthumb_path;
	const char *filethumb_path;

	const char *font;
	const char *background_color;
	const char *foreground_color;
	const char *selbackground_color;
	const char *selforeground_color;
	const char *scrollbackground_color;
	const char *scrollforeground_color;

	int thumbsize_pixels;   /* size of icons and thumbnails */
	int scroll_pixels;      /* scroll bar width */
	int width_pixels;       /* initial window width */
	int height_pixels;      /* initial window height */
	int hide;               /* whether to hide .* entries */
};

/* ellipsis size and font structure */
struct Ellipsis {
	char *s;
	size_t len;     /* length of s */
	int width;      /* size of ellipsis string */
	XftFont *font;  /* font containing ellipsis */
};

/* X11 constant values */
static struct DC dc;
static struct Ellipsis ellipsis;
static Display *dpy;
static Visual *visual;
static Window root;
static Colormap colormap;
static Atom atoms[ATOM_LAST];
static XrmDatabase xdb;
static int screen;
static int depth;
static char *xrm;

/* threads */
static pthread_mutex_t thumblock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queuelock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thumbt;
static int thumbexit = 0;

/* flags */
static int running = 1;         /* whether xfiles is running */

#include "config.h"

/* show usage and exit */
static void
usage(void)
{
	(void)fprintf(stderr, "usage: xfiles [-a] [-g geometry] [path]\n");
	exit(1);
}

/* check whether x is between a and b */
static int
between(int x, int a, int b)
{
	return a <= x && x <= b;
}

/* get maximum */
static int
max(int x, int y)
{
	return x > y ? x : y;
}

/* get minimum */
static int
min(int x, int y)
{
	return x < y ? x : y;
}

/* call strdup checking for error; exit on error */
static char *
estrdup(const char *s)
{
	char *t;

	if ((t = strdup(s)) == NULL)
		err(1, "strdup");
	return t;
}

/* call malloc checking for error; exit on error */
static void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(1, "malloc");
	return p;
}

/* call reallocarray checking for error; exit on error */
static void *
ereallocarray(void *ptr, size_t nmemb, size_t size)
{
	void *p;

	if ((p = reallocarray(ptr, nmemb, size)) == NULL)
		err(1, "reallocarray");
	return p;
}

/* call getcwd checking for error; exit on error */
static void
egetcwd(char *path, size_t size)
{
	if (getcwd(path, size) == NULL) {
		err(1, "getcwd");
	}
}

/* call pipe checking for error; exit on error */
static void
epipe(int fd[])
{
	if (pipe(fd) == -1) {
		err(1, "pipe");
	}
}

/* call fork checking for error; exit on error */
static pid_t
efork(void)
{
	pid_t pid;

	if ((pid = fork()) < 0)
		err(1, "fork");
	return pid;
}

/* call dup2 checking for error; exit on error */
static void
edup2(int fd1, int fd2)
{
	if (dup2(fd1, fd2) == -1)
		err(1, "dup2");
	close(fd1);
}

/* call execlp checking for error; exit on error */
static void
eexec(const char *cmd, const char *arg)
{
	if (execlp(cmd, cmd, arg, NULL) == -1) {
		err(1, "%s", cmd);
	}
}

/* set FD_CLOEXEC on file descriptor; exit on error */
static void
esetcloexec(int fd)
{
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
		err(1, "fcntl");
	}
}

/* get color from color string */
static void
ealloccolor(const char *s, XftColor *color)
{
	if(!XftColorAllocName(dpy, visual, colormap, s, color))
		errx(1, "could not allocate color: %s", s);
}

/* call pthread_create checking for error */
void
etcreate(pthread_t *tid, void *(*thrfn)(void *), void *arg)
{
	int errn;

	if ((errn = pthread_create(tid, NULL, thrfn, arg)) != 0) {
		errno = errn;
		err(1, "could not create thread");
	}
}

/* call pthread_join checking for error */
void
etjoin(pthread_t tid, void **rval)
{
	int errn;

	if ((errn = pthread_join(tid, rval)) != 0) {
		errno = errn;
		err(1, "could not join with thread");
	}
}

/* call chdir checking for error; print warning on error */
static int
wchdir(const char *path)
{
	if (chdir(path) == -1) {
		warn("%s", path);
		return -1;
	}
	return 0;
}

/* wrapper around XGrabPointer; e.g., used to grab pointer when scrolling */
static int
grabpointer(struct FM *fm, unsigned int evmask)
{
	return XGrabPointer(dpy, fm->win, True, evmask | ButtonReleaseMask,
		         GrabModeAsync, GrabModeAsync, None,
		         None, CurrentTime) == GrabSuccess ? 0 : -1;
}

/* ungrab pointer */
static void
ungrab(void)
{
	XUngrabPointer(dpy, CurrentTime);
}

/* parse color string */
static void
parsefonts(const char *s)
{
	const char *p;
	char buf[1024];
	size_t nfont = 0;

	dc.nfonts = 1;
	for (p = s; *p; p++)
		if (*p == ',')
			dc.nfonts++;

	if ((dc.fonts = calloc(dc.nfonts, sizeof *dc.fonts)) == NULL)
		err(1, "calloc");

	p = s;
	while (*p != '\0') {
		size_t i;

		i = 0;
		while (isspace(*p))
			p++;
		while (i < sizeof buf && *p != '\0' && *p != ',')
			buf[i++] = *p++;
		if (i >= sizeof buf)
			errx(1, "font name too long");
		if (*p == ',')
			p++;
		buf[i] = '\0';
		if (nfont == 0)
			if ((dc.pattern = FcNameParse((FcChar8 *)buf)) == NULL)
				errx(1, "the first font in the cache must be loaded from a font string");
		if ((dc.fonts[nfont++] = XftFontOpenName(dpy, screen, buf)) == NULL)
			errx(1, "could not load font");
	}
	dc.fonth = dc.fonts[0]->height;
}

/* parse geometry string, return *width and *height */
static void
parsegeometry(const char *str, int *width, int *height)
{
	int w, h;
	char *end;
	const char *s;

	s = str;
	w = strtol(s, &end, 10);
	if (w < 1 || w > INT_MAX || *s == '\0' || *end != 'x')
		goto error;
	s = end + 1;
	h = strtol(s, &end, 10);
	if (h < 1 || h > INT_MAX || *s == '\0' || *end != '\0')
		goto error;
	*width = w;
	*height = h;
error:
	return;
}

/* parse icon size string, return *size */
static void
parseiconsize(const char *str, int *size)
{
	int m, n;
	char *end;
	const char *s;

	s = str;
	m = strtol(s, &end, 10);
	if (m < 1 || m > INT_MAX || *s == '\0' || *end != 'x')
		goto error;
	s = end + 1;
	n = strtol(s, &end, 10);
	if (n < 1 || n > INT_MAX || *s == '\0' || *end != '\0')
		goto error;
	if (m != n)
		goto error;
	*size = m;
error:
	return;
}

/* parse path for directory and file icons */
static void
parseiconpath(const char *s, int size, const char **dir, const char **file)
{
	static char dirpath[PATH_MAX];
	static char filepath[PATH_MAX];

	if (s != NULL) {
		snprintf(dirpath, sizeof(dirpath), "%s/%dx%d/places/folder.png", s, size, size);
		snprintf(filepath, sizeof(filepath), "%s/%dx%d/mimetypes/unknown.png", s, size, size);
	}
	*dir = dirpath;
	*file = filepath;
}

/* get configuration from environment variables */
static void
parseenviron(void)
{
	char *s;

	if ((s = getenv("ICONSIZE")) != NULL)
		parseiconsize(s, &config.thumbsize_pixels);
	if ((s = getenv("ICONPATH")) != NULL)
		parseiconpath(s, config.thumbsize_pixels, &config.dirthumb_path, &config.filethumb_path);
	if ((s = getenv("OPENER")) != NULL)
		config.opener = s;
	if ((s = getenv("THUMBNAILER")) != NULL)
		config.thumbnailer = s;
}

/* get configuration from X resources */
static void
parseresources(void)
{
	XrmValue xval;
	long n;
	char *type;

	if (XrmGetResource(xdb, "xfiles.faceName", "*", &type, &xval) == True)
		config.font = xval.addr;
	if (XrmGetResource(xdb, "xfiles.background", "*", &type, &xval) == True)
		config.background_color = xval.addr;
	if (XrmGetResource(xdb, "xfiles.foreground", "*", &type, &xval) == True)
		config.foreground_color = xval.addr;
	if (XrmGetResource(xdb, "xfiles.geometry", "*", &type, &xval) == True)
		parsegeometry(xval.addr, &config.width_pixels, &config.height_pixels);
	if (XrmGetResource(xdb, "xfiles.scrollbar.background", "XFiles.Scrollbar.background", &type, &xval) == True)
		config.scrollbackground_color = xval.addr;
	if (XrmGetResource(xdb, "xfiles.scrollbar.foreground", "XFiles.Scrollbar.foreground", &type, &xval) == True)
		config.scrollforeground_color = xval.addr;
	if (XrmGetResource(xdb, "xfiles.scrollbar.thickness", "XFiles.Scrollbar.thickness", &type, &xval) == True)
		if ((n = strtol(xval.addr, NULL, 10)) > 0)
			config.scroll_pixels = n;
	if (XrmGetResource(xdb, "xfiles.selbackground", "*", &type, &xval) == True)
		config.selbackground_color = xval.addr;
	if (XrmGetResource(xdb, "xfiles.selforeground", "*", &type, &xval) == True)
		config.selforeground_color = xval.addr;
}

/* parse options; return argument (if given) or NULL */
static char *
parseoptions(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "ag:")) != -1) {
		switch (ch) {
		case 'a':
			config.hide = !config.hide;
			break;
		case 'g':
			parsegeometry(optarg, &config.width_pixels, &config.height_pixels);
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	else if (argc == 1)
		return *argv;
	return NULL;
}

/* compute number of rows and columns for visible entries */
static void
calcsize(struct FM *fm, int w, int h)
{
	if (w >= 0 && h >= 0) {
		fm->winw = max(w - config.scroll_pixels, 1);
		fm->winh = h;
	}
	fm->ncol = max(fm->winw / fm->entryw, 1);
	fm->nrow = fm->winh / fm->entryh + (fm->winh % fm->entryh ? 2 : 1);
	fm->x0 = max((fm->winw - fm->ncol * fm->entryw) / 2, 0);
	if (fm->main != None)
		XFreePixmap(dpy, fm->main);
	fm->main = XCreatePixmap(dpy, fm->win, fm->winw, fm->nrow * fm->entryh, depth);
	if (fm->scroll != None)
		XFreePixmap(dpy, fm->scroll);
	fm->scroll = XCreatePixmap(dpy, fm->win, config.scroll_pixels, fm->winh, depth);
	fm->maxrow = fm->nentries / fm->ncol - fm->winh / fm->entryh + 1 + (fm->nentries % fm->ncol != 0);
	fm->maxrow = max(fm->maxrow, 1);
	fm->scrollh = max(fm->winh / fm->maxrow, 1);
}

/* get next utf8 char from s return its codepoint and set next_ret to pointer to end of character */
static FcChar32
getnextutf8char(const char *s, const char **next_ret)
{
	static const unsigned char utfbyte[] = {0x80, 0x00, 0xC0, 0xE0, 0xF0};
	static const unsigned char utfmask[] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
	static const FcChar32 utfmin[] = {0, 0x00,  0x80,  0x800,  0x10000};
	static const FcChar32 utfmax[] = {0, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};
	/* 0xFFFD is the replacement character, used to represent unknown characters */
	static const FcChar32 unknown = 0xFFFD;
	FcChar32 ucode;         /* FcChar32 type holds 32 bits */
	size_t usize = 0;       /* n' of bytes of the utf8 character */
	size_t i;

	*next_ret = s+1;

	/* get code of first byte of utf8 character */
	for (i = 0; i < sizeof utfmask; i++) {
		if (((unsigned char)*s & utfmask[i]) == utfbyte[i]) {
			usize = i;
			ucode = (unsigned char)*s & ~utfmask[i];
			break;
		}
	}

	/* if first byte is a continuation byte or is not allowed, return unknown */
	if (i == sizeof utfmask || usize == 0)
		return unknown;

	/* check the other usize-1 bytes */
	s++;
	for (i = 1; i < usize; i++) {
		*next_ret = s+1;
		/* if byte is nul or is not a continuation byte, return unknown */
		if (*s == '\0' || ((unsigned char)*s & utfmask[0]) != utfbyte[0])
			return unknown;
		/* 6 is the number of relevant bits in the continuation byte */
		ucode = (ucode << 6) | ((unsigned char)*s & ~utfmask[0]);
		s++;
	}

	/* check if ucode is invalid or in utf-16 surrogate halves */
	if (!between(ucode, utfmin[usize], utfmax[usize]) || between(ucode, 0xD800, 0xDFFF))
		return unknown;

	return ucode;
}

/* get which font contains a given code point */
static XftFont *
getfontucode(FcChar32 ucode)
{
	FcCharSet *fccharset = NULL;
	FcPattern *fcpattern = NULL;
	FcPattern *match = NULL;
	XftFont *retfont = NULL;
	XftResult result;
	size_t i;

	for (i = 0; i < dc.nfonts; i++)
		if (XftCharExists(dpy, dc.fonts[i], ucode) == FcTrue)
			return dc.fonts[i];

	/* create a charset containing our code point */
	fccharset = FcCharSetCreate();
	FcCharSetAddChar(fccharset, ucode);

	/* create a pattern akin to the dc.pattern but containing our charset */
	if (fccharset) {
		fcpattern = FcPatternDuplicate(dc.pattern);
		FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
	}

	/* find pattern matching fcpattern */
	if (fcpattern) {
		FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
		FcDefaultSubstitute(fcpattern);
		match = XftFontMatch(dpy, screen, fcpattern, &result);
	}

	/* if found a pattern, open its font */
	if (match) {
		retfont = XftFontOpenPattern(dpy, match);
		if (retfont && XftCharExists(dpy, retfont, ucode) == FcTrue) {
			if ((dc.fonts = realloc(dc.fonts, dc.nfonts+1)) == NULL)
				err(1, "realloc");
			dc.fonts[dc.nfonts] = retfont;
			return dc.fonts[dc.nfonts++];
		} else {
			XftFontClose(dpy, retfont);
		}
	}

	/* in case no fount was found, return the first one */
	return dc.fonts[0];
}

/* return width of *s in pixels; draw *s into draw if draw != NULL */
static int
drawtext(XftDraw *draw, XftColor *color, int x, int y, int w, const char *text, size_t maxlen)
{
	XftFont *currfont;
	XGlyphInfo ext;
	FcChar32 ucode;
	const char *next, *origtext, *t;
	size_t len;
	int textwidth = 0;
	int texty;

	origtext = text;
	while (text < origtext + maxlen) {
		/* get the next unicode character and the first font that supports it */
		ucode = getnextutf8char(text, &next);
		currfont = getfontucode(ucode);

		/* compute the width of the glyph for that character on that font */
		len = next - text;
		XftTextExtentsUtf8(dpy, currfont, (XftChar8 *)text, len, &ext);
		t = text;
		if (w && textwidth + ext.xOff > w) {
			t = ellipsis.s;
			len = ellipsis.len;
			currfont = ellipsis.font;
			while (*next)
				next++;
			textwidth += ellipsis.width;
		}
		textwidth += ext.xOff;

		if (draw) {
			texty = y + (dc.fonth - (currfont->ascent + currfont->descent))/2 + currfont->ascent;
			XftDrawStringUtf8(draw, color, currfont, x, texty, (XftChar8 *)t, len);
			x += ext.xOff;
		}

		text = next;
	}
	return textwidth;
}

/* load image from file and scale it to size; return the image and its size */
static Imlib_Image
loadimg(const char *file, int size, int *width_ret, int *height_ret)
{
	Imlib_Image img;
	Imlib_Load_Error errcode;
	const char *errstr;
	int width;
	int height;

	img = imlib_load_image_with_error_return(file, &errcode);
	if (*file == '\0') {
		return NULL;
	} else if (img == NULL) {
		switch (errcode) {
		case IMLIB_LOAD_ERROR_FILE_DOES_NOT_EXIST:
			errstr = "file does not exist";
			break;
		case IMLIB_LOAD_ERROR_FILE_IS_DIRECTORY:
			errstr = "file is directory";
			break;
		case IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_READ:
		case IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_WRITE:
			errstr = "permission denied";
			break;
		case IMLIB_LOAD_ERROR_NO_LOADER_FOR_FILE_FORMAT:
			errstr = "unknown file format";
			break;
		case IMLIB_LOAD_ERROR_PATH_TOO_LONG:
			errstr = "path too long";
			break;
		case IMLIB_LOAD_ERROR_PATH_COMPONENT_NON_EXISTANT:
		case IMLIB_LOAD_ERROR_PATH_COMPONENT_NOT_DIRECTORY:
		case IMLIB_LOAD_ERROR_PATH_POINTS_OUTSIDE_ADDRESS_SPACE:
			errstr = "improper path";
			break;
		case IMLIB_LOAD_ERROR_TOO_MANY_SYMBOLIC_LINKS:
			errstr = "too many symbolic links";
			break;
		case IMLIB_LOAD_ERROR_OUT_OF_MEMORY:
			errstr = "out of memory";
			break;
		case IMLIB_LOAD_ERROR_OUT_OF_FILE_DESCRIPTORS:
			errstr = "out of file descriptors";
			break;
		default:
			errstr = "unknown error";
			break;
		}
		warnx("could not load image (%s): %s", errstr, file);
		return NULL;
	}
	imlib_context_set_image(img);
	width = imlib_image_get_width();
	height = imlib_image_get_height();
	if (width > height) {
		*width_ret = size;
		*height_ret = (height * size) / width;
	} else {
		*width_ret = (width * size) / height;
		*height_ret = size;
	}
	img = imlib_create_cropped_scaled_image(0, 0, width, height,
	                                         *width_ret, *height_ret);
	return img;
}

/* draw thumbnail image on pixmaps; then free image */
static void
drawthumb(struct FM *fm, Imlib_Image img, struct Rect *thumb, Pixmap *pix)
{
	thumb->x = 0;
	thumb->y = 0;
	if (img == NULL)
		return;

	/* compute position of thumbnails */
	thumb->x = fm->thumbx + max((config.thumbsize_pixels - thumb->w) / 2, 0);
	thumb->y = fm->thumby + max((config.thumbsize_pixels - thumb->h) / 2, 0);

	/* draw border on sel pixmap around thumbnail */
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[PIX_UNSEL], dc.gc, fm->thumbx, fm->thumby, config.thumbsize_pixels, config.thumbsize_pixels);
	XFillRectangle(dpy, pix[PIX_SEL], dc.gc, fm->thumbx - THUMBBORDER, fm->thumby - THUMBBORDER, config.thumbsize_pixels + 2 * THUMBBORDER, config.thumbsize_pixels + 2 * THUMBBORDER);
	XSetForeground(dpy, dc.gc, dc.select[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[PIX_SEL], dc.gc, thumb->x - THUMBBORDER, thumb->y - THUMBBORDER, thumb->w + 2 * THUMBBORDER, thumb->h + 2 * THUMBBORDER);
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[PIX_SEL], dc.gc, thumb->x, thumb->y, thumb->w, thumb->h);

	/* draw thumbnail on both pixmaps */
	imlib_context_set_image(img);
	imlib_image_set_changes_on_disk();
	imlib_context_set_drawable(pix[PIX_UNSEL]);
	imlib_render_image_on_drawable(thumb->x, thumb->y);
	imlib_context_set_drawable(pix[PIX_SEL]);
	imlib_render_image_on_drawable(thumb->x, thumb->y);
	imlib_free_image();
}

/* fake expose event to force redrawing */
static void
sendevent(struct FM *fm)
{
	XEvent ev;

	ev.type = Expose;
	ev.xexpose.window = fm->win;
	ev.xexpose.count = 0;

	XSendEvent(dpy, fm->win, False, NoEventMask, &ev);
	XFlush(dpy);
}

/* add thumbnail image to queue */
static void
addthumbimg(struct FM *fm, Imlib_Image img, int index)
{
	struct ThumbImg *ti;

	ti = emalloc(sizeof(*ti));
	ti->next = NULL;
	ti->img = img;
	ti->index = index;
	pthread_mutex_lock(&queuelock);
	if (fm->last != NULL)
		fm->last->next = ti;
	else
		fm->queue = ti;
	fm->last = ti;
	pthread_mutex_unlock(&queuelock);
}

/* delete thumbnail image from queue */
static struct ThumbImg *
delthumbimg(struct FM *fm)
{
	struct ThumbImg *ret;

	pthread_mutex_lock(&queuelock);
	ret = fm->queue;
	if (fm->queue != NULL) {
		if (fm->last == fm->queue)
			fm->last = NULL;
		fm->queue = fm->queue->next;
	} else {
		fm->last = NULL;
	}
	pthread_mutex_unlock(&queuelock);
	return ret;
}

/* clear thumbnail image queue */
static void
clearqueue(struct FM *fm)
{
	struct ThumbImg *tmp;

	pthread_mutex_lock(&queuelock);
	while (fm->queue != NULL) {
		tmp = fm->queue;
		fm->queue = fm->queue->next;
		imlib_context_set_image(tmp->img);
		imlib_free_image();
		free(tmp);
	}
	pthread_mutex_unlock(&queuelock);
}

/* call sigaction on sig */
static void
initsignal(int sig, void (*handler)(int sig))
{
	struct sigaction sa;

	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(sig, &sa, 0) == -1) {
		err(1, "signal %d", sig);
	}
}

/* intern atoms */
static void
initatoms(void)
{
	char *atomnames[ATOM_LAST] = {
		[UTF8_STRING]                = "UTF8_STRING",
		[WM_DELETE_WINDOW]           = "WM_DELETE_WINDOW",
		[_NET_WM_NAME]               = "_NET_WM_NAME",
		[_NET_WM_PID]                = "_NET_WM_PID",
		[_NET_WM_WINDOW_TYPE]        = "_NET_WM_WINDOW_TYPE",
		[_NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	};

	XInternAtoms(dpy, atomnames, ATOM_LAST, False, atoms);
}

/* initialize drawing context (colors and graphics context) */
static void
initdc(void)
{
	dc.gc = XCreateGC(dpy, root, 0, NULL);
	ealloccolor(config.background_color, &dc.normal[COLOR_BG]);
	ealloccolor(config.foreground_color, &dc.normal[COLOR_FG]);
	ealloccolor(config.selbackground_color, &dc.select[COLOR_BG]);
	ealloccolor(config.selforeground_color, &dc.select[COLOR_FG]);
	ealloccolor(config.scrollbackground_color, &dc.scroll[COLOR_BG]);
	ealloccolor(config.scrollforeground_color, &dc.scroll[COLOR_FG]);
	parsefonts(config.font);
}

/* draw fallback icons from .xpm files */
static void
xpmread(struct FM *fm, struct Rect *thumb, Pixmap *pix, const char **data)
{
	XGCValues val;
	XpmAttributes xa;
	XImage *img;
	Pixmap orig;
	int status;

	memset(&xa, 0, sizeof xa);
	status = XpmCreateImageFromData(dpy, (char **)data, &img, NULL, &xa);
	if (status != XpmSuccess)
		errx(1, "could not load default icon");

	/* create Pixmap from XImage */
	orig = XCreatePixmap(dpy, fm->win, config.thumbsize_pixels, config.thumbsize_pixels, img->depth);
	if (!(xa.valuemask & (XpmSize | XpmHotspot)))
		errx(1, "could not load default icon");
	val.foreground = 1;
	val.background = 0;
	XChangeGC(dpy, dc.gc, GCForeground | GCBackground, &val);
	XPutImage(dpy, orig, dc.gc, img, 0, 0, 0, 0, img->width, img->height);

	/* compute size and position of thumbnails */
	thumb->w = xa.width;
	thumb->h = xa.height;
	thumb->x = fm->thumbx + max((config.thumbsize_pixels - thumb->w) / 2, 0);
	thumb->y = fm->thumby + max((config.thumbsize_pixels - thumb->h) / 2, 0);

	/* draw border on sel pixmap around thumbnail */
	XSetForeground(dpy, dc.gc, dc.select[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[PIX_SEL], dc.gc, thumb->x - THUMBBORDER, thumb->y - THUMBBORDER, thumb->w + 2 * THUMBBORDER, thumb->h + 2 * THUMBBORDER);
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[PIX_SEL], dc.gc, thumb->x, thumb->y, thumb->w, thumb->h);

	XCopyArea(dpy, orig, pix[PIX_UNSEL], dc.gc, 0, 0, thumb->w, thumb->h, thumb->x, thumb->y);
	XCopyArea(dpy, orig, pix[PIX_SEL], dc.gc, 0, 0, thumb->w, thumb->h, thumb->x, thumb->y);

	XDestroyImage(img);
	XFreePixmap(dpy, orig);
}

/* create window and set its properties */
static void
initfm(struct FM *fm, int argc, char *argv[])
{
	XSetWindowAttributes swa;
	XClassHint classh;
	Imlib_Image fileimg;
	Imlib_Image dirimg;
	pid_t pid;

	fm->main = None;
	fm->scroll = None;
	fm->hist = fm->curr = NULL;
	fm->entries = NULL;
	fm->selected = NULL;
	fm->queue = fm->last = NULL;
	fm->capacity = 0;
	fm->nentries = 0;
	fm->row = 0;
	fm->ydiff = 0;
	fm->entryw = config.thumbsize_pixels * 2;
	fm->entryh = config.thumbsize_pixels + 3 * dc.fonth + 2 * THUMBBORDER;
	fm->thumbx = config.thumbsize_pixels / 2;
	fm->thumby = dc.fonth / 2;
	fm->textw = fm->entryw - dc.fonth;
	fm->textx = dc.fonth / 2;
	fm->texty0 = dc.fonth / 2 + config.thumbsize_pixels + 2 * THUMBBORDER;
	fm->texty1 = fm->texty0 + dc.fonth;
	memset(&fm->dirrect, 0, sizeof(fm->dirrect));
	memset(&fm->filerect, 0, sizeof(fm->filerect));

	swa.background_pixel = dc.normal[COLOR_BG].pixel;
	swa.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask | ButtonPressMask;
	fm->win = XCreateWindow(dpy, root, 0, 0, config.width_pixels, config.height_pixels, 0,
	                        CopyFromParent, CopyFromParent, CopyFromParent,
	                        CWBackPixel | CWEventMask, &swa);

	classh.res_class = CLASS;
	classh.res_name = NULL;
	pid = getpid();
	XmbSetWMProperties(dpy, fm->win, TITLE, TITLE, argv, argc, NULL, NULL, &classh);
	XSetWMProtocols(dpy, fm->win, &atoms[WM_DELETE_WINDOW], 1);
	XChangeProperty(dpy, fm->win, atoms[_NET_WM_WINDOW_TYPE], XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)&atoms[_NET_WM_WINDOW_TYPE_NORMAL], 1);
	XChangeProperty(dpy, fm->win, atoms[_NET_WM_PID], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);

	calcsize(fm, config.width_pixels, config.height_pixels);

	/* create and clean default thumbnails */
	fm->file[PIX_UNSEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	fm->file[PIX_SEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	fm->dir[PIX_UNSEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	fm->dir[PIX_SEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);

	/* clean pixmaps */
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, fm->file[PIX_UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh);
	XFillRectangle(dpy, fm->file[PIX_SEL], dc.gc, 0, 0, fm->entryw, fm->entryh);
	XFillRectangle(dpy, fm->dir[PIX_UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh);
	XFillRectangle(dpy, fm->dir[PIX_SEL], dc.gc, 0, 0, fm->entryw, fm->entryh);

	/* draw default thumbnails */
	if (config.filethumb_path != NULL && config.filethumb_path[0] != '\0') {
		fileimg = loadimg(config.filethumb_path, config.thumbsize_pixels, &fm->filerect.w, &fm->filerect.h);
		drawthumb(fm, fileimg, &fm->filerect, fm->file);
	} else {
		warnx("could not find icon for files; using default icon");
		xpmread(fm, &fm->filerect, fm->file, file);
	}
	if (config.dirthumb_path != NULL && config.dirthumb_path[0] != '\0') {
		dirimg = loadimg(config.dirthumb_path, config.thumbsize_pixels, &fm->dirrect.w, &fm->dirrect.h);
		drawthumb(fm, dirimg, &fm->dirrect, fm->dir);
	} else {
		warnx("could not find icon for directories; using default icon");
		xpmread(fm, &fm->dirrect, fm->dir, folder);
	}
}

/* compute width of ellipsis string */
static void
initellipsis(void)
{
	XGlyphInfo ext;
	FcChar32 ucode;
	const char *s;

	ellipsis.s = ELLIPSIS;
	ellipsis.len = strlen(ellipsis.s);
	ucode = getnextutf8char(ellipsis.s, &s);
	ellipsis.font = getfontucode(ucode);
	XftTextExtentsUtf8(dpy, ellipsis.font, (XftChar8 *)ellipsis.s, ellipsis.len, &ext);
	ellipsis.width = ext.xOff;
}

/* delete current cwd and previous from working directory history */
static void
delcwd(struct FM *fm)
{
	struct Histent *h, *tmp;

	if (fm->curr == NULL)
		return;
	h = fm->curr->next;
	while (h != NULL) {
		tmp = h;
		h = h->next;
		free(tmp->cwd);
		free(tmp);
	}
	fm->curr->next = NULL;
	fm->hist = fm->curr;
}

/* insert cwd into working directory history */
static void
addcwd(struct FM *fm, char *path) {
	struct Histent *h;

	delcwd(fm);
	h = emalloc(sizeof(*h));
	h->cwd = estrdup(path);
	h->next = NULL;
	h->prev = fm->hist;
	if (fm->hist)
		fm->hist->next = h;
	fm->curr = fm->hist = h;
}

/* select entries according to config.hide */
static int
direntselect(const struct dirent *dp)
{
	if (strcmp(dp->d_name, ".") == 0)
		return 0;
	if (strcmp(dp->d_name, "..") == 0)
		return 1;
	if (config.hide && dp->d_name[0] == '.')
		return 0;
	return 1;
}

/* compare entries with strcoll, directories first */
static int
entrycompar(const void *ap, const void *bp)
{
	struct Entry *a, *b;
	a = *(struct Entry **)ap;
	b = *(struct Entry **)bp;
	/* dotdot (parent directory) first */
	if (strcmp(a->name, "..") == 0)
		return -1;
	if (strcmp(b->name, "..") == 0)
		return 1;

	/* directories first */
	if (a->isdir && !b->isdir)
		return -1;
	if (b->isdir && !a->isdir)
		return 1;

	/* dotentries (hidden entries) first */
	if (a->name[0] == '.' && b->name[0] != '.')
		return -1;
	if (b->name[0] == '.' && a->name[0] != '.')
		return 1;
	return strcoll(a->name, b->name);
}

/* allocate entry */
static struct Entry *
allocentry(struct FM *fm, const char *name, int isdir)
{
	struct Entry *ent;

	ent = emalloc(sizeof *ent);
	ent->sprev = ent->snext = NULL;
	ent->pix[PIX_UNSEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	ent->pix[PIX_SEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	ent->issel = 0;
	ent->isdir = isdir;
	ent->drawn = 0;
	ent->name = estrdup(name);
	memset(ent->line, 0, sizeof(ent->line));
	if (ent->isdir) {
		ent->thumb = fm->dirrect;
		XCopyArea(dpy, fm->dir[PIX_UNSEL], ent->pix[PIX_UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
		XCopyArea(dpy, fm->dir[PIX_SEL], ent->pix[PIX_SEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
	} else {
		ent->thumb = fm->filerect;
		XCopyArea(dpy, fm->file[PIX_UNSEL], ent->pix[PIX_UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
		XCopyArea(dpy, fm->file[PIX_SEL], ent->pix[PIX_SEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
	}
	return ent;
}

/* destroy entry */
static void
freeentries(struct FM *fm)
{
	int i;

	for (i = 0; i < fm->nentries; i++) {
		XFreePixmap(dpy, fm->entries[i]->pix[PIX_UNSEL]);
		XFreePixmap(dpy, fm->entries[i]->pix[PIX_SEL]);
		free(fm->entries[i]->name);
		free(fm->entries[i]);
	}
}

/* populate list of entries on fm; return -1 on error */
static void
listentries(struct FM *fm, int savecwd)
{
	struct dirent **array;
	struct stat sb;
	int i, n, isdir;
	char path[PATH_MAX];

	egetcwd(path, sizeof(path));
	if (savecwd)
		addcwd(fm, path);
	if ((n = scandir(path, &array, direntselect, NULL)) == -1)
		err(1, "scandir");
	freeentries(fm);
	if (n > fm->capacity) {
		fm->entries = ereallocarray(fm->entries, n, sizeof(*fm->entries));
		fm->capacity = n;
	}
	fm->nentries = n;
	for (i = 0; i < fm->nentries; i++) {
		if (stat(array[i]->d_name, &sb) == -1) {
			warn("%s", path);
			isdir = 0;
		} else {
			isdir = S_ISDIR(sb.st_mode);
		}
		fm->entries[i] = allocentry(fm, array[i]->d_name, isdir);
		free(array[i]);
	}
	free(array);
	qsort(fm->entries, fm->nentries, sizeof(*fm->entries), entrycompar);
}

/* get index of entry from pointer position; return -1 if not found */
static int
getentry(struct FM *fm, int x, int y)
{
	struct Entry *ent;
	int i, n, w, h;

	if (x < fm->x0 || x >= fm->x0 + fm->ncol * fm->entryw)
		return -1;
	if (y < 0 || y >= fm->winh)
		return -1;
	x -= fm->x0;
	y += fm->ydiff;
	w = x / fm->entryw;
	h = y / fm->entryh;
	i = fm->row * fm->ncol + h * fm->ncol + w;
	n = min(fm->nentries, fm->row * fm->ncol + fm->nrow * fm->ncol);
	if (i < fm->row * fm->ncol || i >= n)
		return -1;
	x -= w * fm->entryw;
	y -= h * fm->entryh;
	ent = fm->entries[i];
	if ((x >= ent->thumb.x && x < ent->thumb.x + ent->thumb.w && y >= ent->thumb.y && y < ent->thumb.y + ent->thumb.h) ||
	    (x >= ent->line[0].x && x < ent->line[0].x + ent->line[0].w && y >= fm->texty0 && y < fm->texty0 + dc.fonth) ||
	    (x >= ent->line[1].x && x < ent->line[1].x + ent->line[1].w && y >= fm->texty1 && y < fm->texty1 + dc.fonth))
		return i;
	return -1;
}

/* copy entry pixmap into fm pixmap */
static void
copyentry(struct FM *fm, int i)
{
	Pixmap pix;
	int x, y;

	if (i < fm->row * fm->ncol || i >= fm->row * fm->ncol + fm->nrow * fm->ncol)
		return;
	pix = (fm->entries[i]->issel ? fm->entries[i]->pix[PIX_SEL] : fm->entries[i]->pix[PIX_UNSEL]);
	i -= fm->row * fm->ncol;
	x = i % fm->ncol;
	y = (i / fm->ncol) % fm->nrow;
	x *= fm->entryw;
	y *= fm->entryh;
	XCopyArea(dpy, pix, fm->main, dc.gc, 0, 0, fm->entryw, fm->entryh, fm->x0 + x, y);
}

/* commit pixmap into window */
static void
commitdraw(struct FM *fm)
{
	fm->scrolly = fm->row * fm->winh / fm->maxrow + fm->ydiff * fm->scrollh / fm->entryh;
	XSetForeground(dpy, dc.gc, dc.scroll[COLOR_BG].pixel);
	XFillRectangle(dpy, fm->scroll, dc.gc, 0, 0, config.scroll_pixels, fm->winh);
	XSetForeground(dpy, dc.gc, dc.scroll[COLOR_FG].pixel);
	XFillRectangle(dpy, fm->scroll, dc.gc, 0, fm->scrolly, config.scroll_pixels, fm->scrollh);

	XCopyArea(dpy, fm->main, fm->win, dc.gc, 0, fm->ydiff, fm->winw, fm->winh, 0, 0);
	XCopyArea(dpy, fm->scroll, fm->win, dc.gc, 0, 0, config.scroll_pixels, fm->winh, fm->winw, 0);
}

/* check if we can break line at the given character (is space, hyphen, etc) */
static int
isbreakable(char c)
{
	return c == '.' || c == '-' || c == '_';
}

/* draw names below thumbnail on its pixmaps */
static void
drawentry(struct FM *fm, struct Entry *ent)
{
	XftDraw *draw;
	int x0, x1, prevw, textw0, textw1, prevlen, len0, len1, i;

	textw1 = textw0 = len0 = len1 = 0;
	do {
		prevw = textw0;
		prevlen = len0;
		for (; ent->name[len0] != '\0' && !isspace(ent->name[len0]) && !isbreakable(ent->name[len0]); len0++)
			;
		textw0 = drawtext(NULL, NULL, 0, 0, 0, ent->name, len0);
		while (isspace(ent->name[len0]) || isbreakable(ent->name[len0]))
			len0++;
	} while (textw0 < fm->textw && ent->name[len0] != '\0' && prevw != textw0);
	if (textw0 >= fm->textw) {
		len0 = prevlen;
		textw0 = prevw;
	}
	while (len0 > 0 && isbreakable(ent->name[len0 - 1]))
		len0--;
	i = len0;
	while (len0 > 0 && isspace(ent->name[len0 - 1]))
		len0--;
	if (i > 0 && ent->name[i] != '\0') {
		len1 = strlen(ent->name+i);
		textw1 = drawtext(NULL, NULL, 0, 0, 0, ent->name + i, len1);
	} else {
		len0 = strlen(ent->name);
		textw0 = drawtext(NULL, NULL, 0, 0, 0, ent->name, len0);
	}

	x0 = fm->textx + max(0, (fm->textw - textw0) / 2);
	x1 = fm->textx + max(0, (fm->textw - textw1) / 2);

	draw = XftDrawCreate(dpy, ent->pix[PIX_UNSEL], visual, colormap);
	drawtext(draw, &dc.normal[COLOR_FG], x0, fm->texty0, fm->textw, ent->name, len0);
	if (i > 0 && ent->name[i] != '\0')
		drawtext(draw, &dc.normal[COLOR_FG], x1, fm->texty1, fm->textw, ent->name + i, len1);
	XftDrawDestroy(draw);
	ent->line[0].x = x0;
	ent->line[0].w = textw0;

	XSetForeground(dpy, dc.gc, dc.select[COLOR_BG].pixel);
	XFillRectangle(dpy, ent->pix[PIX_SEL], dc.gc, x0, fm->texty0, textw0, dc.fonth);
	draw = XftDrawCreate(dpy, ent->pix[PIX_SEL], visual, colormap);
	drawtext(draw, &dc.select[COLOR_FG], x0, fm->texty0, fm->textw, ent->name, len0);
	if (i > 0 && ent->name[i] != '\0') {
		XFillRectangle(dpy, ent->pix[PIX_SEL], dc.gc, x1, fm->texty1, textw1, dc.fonth);
		drawtext(draw, &dc.select[COLOR_FG], x1, fm->texty1, fm->textw, ent->name + i, len1);
	}
	ent->line[1].x = x1;
	ent->line[1].w = textw1;

	XftDrawDestroy(draw);

	ent->drawn = 1;
}

/* draw entries on main window */
static void
drawentries(struct FM *fm)
{
	int i, n;

	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, fm->main, dc.gc, 0, 0, fm->winw, fm->nrow * fm->entryh);
	n = min(fm->nentries, fm->row * fm->ncol + fm->nrow * fm->ncol);
	for (i = fm->row * fm->ncol; i < n; i++) {
		if (!fm->entries[i]->drawn)
			drawentry(fm, fm->entries[i]);
		copyentry(fm, i);
	}
	commitdraw(fm);
}

/* fork process that get thumbnail */
static int
forkthumb(const char *name)
{
	pid_t pid;
	int fd[2];
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "./%s", name);
	epipe(fd);
	if ((pid = efork()) > 0) {      /* parent */
		close(fd[1]);
		return fd[0];
	} else {                        /* children */
		close(fd[0]);
		if (fd[1] != STDOUT_FILENO)
			edup2(fd[1], STDOUT_FILENO);
		eexec(config.thumbnailer, path);
	}
	return -1;                      /* unreachable */
}

/* read image path from file descriptor */
static void
readpath(int fd, char *path)
{
	FILE *fp;
	int len;

	*path = '\0';
	if ((fp = fdopen(fd, "r")) == NULL) {
		warn("fdopen");
		return;
	}
	if (fgets(path, PATH_MAX, fp) == NULL) {
		fclose(fp);
		return;
	}
	fclose(fp);
	len = strlen(path);
	if (path[len - 1] == '\n') {
		path[len - 1] = '\0';
	}
}

/* set thumbexit */
static void
setthumbexit(void)
{
	pthread_mutex_lock(&thumblock);
	thumbexit = 1;
	pthread_mutex_unlock(&thumblock);
}

/* unset thumbexit */
static void
unsetthumbexit(void)
{
	pthread_mutex_lock(&thumblock);
	thumbexit = 0;
	pthread_mutex_unlock(&thumblock);
}

/* thumbnailer thread */
static void *
thumbnailer(void *arg)
{
	struct FM *fm;
	struct Entry *ent;
	Imlib_Image img;
	int fd;
	int ret;
	int i;
	char path[PATH_MAX];

	ret = 0;
	fm = (struct FM *)arg;
	for (i = 0; i < fm->nentries; i++) {
		pthread_mutex_lock(&thumblock);
		if (thumbexit)
			ret = 1;
		pthread_mutex_unlock(&thumblock);
		if (ret)
			break;
		ent = fm->entries[i];
		fd = forkthumb(fm->entries[i]->name);
		readpath(fd, path);
		close(fd);
		wait(NULL);
		img = loadimg(path, config.thumbsize_pixels, &ent->thumb.w, &ent->thumb.h);
		if (img != NULL) {
			addthumbimg(fm, img, i);
			sendevent(fm);
		}
	}
	pthread_exit(0);
}

/* scroll list by manipulating scroll bar with pointer; return 1 if fm->row changes */
static int
scroll(struct FM *fm, int y)
{
	int prevrow;
	int half;

	prevrow = fm->row;
	half = fm->scrollh / 2;
	y = max(half, min(y, fm->winh - half));
	y -= half;
	fm->row = y * fm->maxrow / fm->winh;
	fm->ydiff = (y - fm->row * fm->winh / fm->maxrow) * fm->entryh / fm->scrollh;
	if (fm->row >= fm->maxrow - 1) {
		fm->row = fm->maxrow - 1;
		fm->ydiff = 0;
	}
	return prevrow != fm->row;
}

/* mark or unmark entry as selected */
static void
selectentry(struct FM *fm, struct Entry *ent, int select)
{
	if ((ent->issel != 0) == (select != 0))
		return;
	if (select) {
		ent->snext = fm->selected;
		ent->sprev = NULL;
		if (fm->selected)
			fm->selected->sprev = ent;
		fm->selected = ent;
	} else {
		if (ent->snext)
			ent->snext->sprev = ent->sprev;
		if (ent->sprev)
			ent->sprev->snext = ent->snext;
		else if (fm->selected == ent)
			fm->selected = ent->snext;
		ent->sprev = ent->snext = NULL;
	}
	ent->issel = select;
}

/* mark or unmark entry as selected */
static void
selectentries(struct FM *fm, int a, int b, int select)
{
	int min;
	int max;
	int i;

	if (a == -1 || b == -1)
		return;
	if (a < b) {
		min = a;
		max = b;
	} else {
		min = b;
		max = a;
	}
	for (i = min; i >= 0 && i <= max; i++) {
		selectentry(fm, fm->entries[i], select);
	}
}

/* change directory */
static void
diropen(struct FM *fm, const char *path, int savecwd)
{
	while (fm->selected != NULL)            /* unselect entries */
		selectentry(fm, fm->selected, 0);
	if (path == NULL || wchdir(path) != -1) {
		/* close previous thumbnailer thread */
		setthumbexit();
		etjoin(thumbt, NULL);
		unsetthumbexit();

		clearqueue(fm);
		listentries(fm, savecwd);
		calcsize(fm, -1, -1);
		fm->row = 0;
		fm->ydiff = 0;
		etcreate(&thumbt, thumbnailer, (void *)fm);
	}
}

/* open file using config.opener */
static void
fileopen(struct Entry *ent)
{
	pid_t pid1, pid2;
	int fd;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "./%s", ent->name);
	if ((pid1 = efork()) == 0) {
		if ((pid2 = efork()) == 0) {
			close(STDOUT_FILENO);
			fd = open(DEV_NULL, O_RDWR);
			edup2(STDOUT_FILENO, fd);
			eexec(config.opener, path);
		}
		exit(0);
	}
	waitpid(pid1, NULL, 0);
}

/* go back (< 0) in cwd history; return nonzero when changing directory */
static int
navhistory(struct FM *fm, int dir)
{
	struct Histent *h;

	h = (dir == BACK) ? fm->curr->prev : fm->curr->next;
	if (h == NULL)
		return 0;
	diropen(fm, h->cwd, 0);
	fm->curr = h;
	return 1;
}

/* stop running */
static void
stoprunning(void)
{
	running = 0;
}

/* handle left mouse button click */
static void
mouseclick(struct FM *fm, XButtonPressedEvent *ev)
{
	static int lastent = -1;        /* index of last clicked entry; -1 if none */
	static Time lasttime = 0;       /* time of last click action */
	struct Entry *ent;
	int setlastent;
	int i;
	char path[PATH_MAX];

	setlastent = 1;
	if (!(ev->state & (ControlMask | ShiftMask)))
		while (fm->selected)
			selectentry(fm, fm->selected, 0);
	i = getentry(fm, ev->x, ev->y);
	if (ev->state & ShiftMask)
		selectentries(fm, i, lastent, 1);
	else if (i != -1)
		selectentry(fm, fm->entries[i], (ev->state & ControlMask) ? !fm->entries[i]->issel : 1);
	if (!(ev->state & (ControlMask | ShiftMask)) && i != -1 &&
	    i == lastent && ev->time - lasttime <= DOUBLECLICK) {
		ent = fm->entries[i];
		if (ent->isdir) {
			snprintf(path, sizeof(path), "./%s", ent->name);
			diropen(fm, path, 1);
			setlastent = 0;
		} else {
			fileopen(ent);
		}
	}
	drawentries(fm);
	commitdraw(fm);
	lastent = (setlastent) ? i : -1;
	lasttime = ev->time;
}

/* window is exposed; redraw its content */
static void
xeventexpose(struct FM *fm, XEvent *ev)
{
	struct ThumbImg *ti;

	if (ev->xexpose.count == 0) {
		if ((ti = delthumbimg(fm)) != NULL) {
			drawthumb(fm, ti->img, &fm->entries[ti->index]->thumb, fm->entries[ti->index]->pix);
			copyentry(fm, ti->index);
			if (ti->index >= fm->row * fm->ncol && ti->index < fm->row * fm->ncol + fm->nrow * fm->ncol) {
				commitdraw(fm);
			}
		} else {
			commitdraw(fm);
		}
	}
}

/* check if client message is window deletion; stop running if it is */
static void
xeventclientmessage(struct FM *fm, XEvent *ev)
{
	(void)fm;
	if ((Atom)ev->xclient.data.l[0] == atoms[WM_DELETE_WINDOW]) {
		stoprunning();
	}
}

/* resize file manager window */
static void
xeventconfigurenotify(struct FM *fm, XEvent *ev)
{
	calcsize(fm, ev->xconfigure.width, ev->xconfigure.height);
	if (fm->row >= fm->maxrow)
		fm->row = fm->maxrow - 1;
	drawentries(fm);
	commitdraw(fm);
}

/* grab pointer and handle scrollbar dragging with the left mouse button */
static void
grabscroll(struct FM *fm, int y)
{
	XEvent ev;
	int dy;

	if (grabpointer(fm, Button1MotionMask) == -1)
		return;
	dy = between(y, fm->scrolly, fm->scrolly + fm->scrollh) ? fm->scrolly + fm->scrollh / 2 - y : 0;
	if (scroll(fm, y + dy))
		drawentries(fm);
	commitdraw(fm);
	while (!XMaskEvent(dpy, ButtonReleaseMask | Button1MotionMask | ExposureMask, &ev)) {
		switch (ev.type) {
		case Expose:
			xeventexpose(fm, &ev);
			break;
		case MotionNotify:
			if (scroll(fm, ev.xbutton.y + dy))
				drawentries(fm);
			commitdraw(fm);
			break;
		case ButtonRelease:
			ungrab();
			return;
		}
	}
}

/* process mouse button press event */
static void
xeventbuttonpress(struct FM *fm, XEvent *e)
{
	XButtonPressedEvent *ev;

	ev = &e->xbutton;
	switch (ev->button) {
	case Button1:
		if (ev->x > fm->winw)   /* scrollbar was manipulated */
			grabscroll(fm, ev->y);
		else                    /* mouse left button was pressed */
			mouseclick(fm, ev);
		break;
	case Button2:
		// TODO
		break;
	case Button3:
		// TODO
		break;
	case Button4:
	case Button5:
		/* mouse wheel was scrolled */
		if (scroll(fm, fm->scrolly + fm->scrollh / 2 + (ev->button == Button4 ? -5 : +5)))
			drawentries(fm);
		commitdraw(fm);
		break;
	case BUTTON8:
	case BUTTON9:
		/* navigate through history with mouse back/forth buttons */
		if (navhistory(fm, (ev->button == BUTTON8) ? BACK : FORTH)) {
			drawentries(fm);
			commitdraw(fm);
		}
		break;
	default:
		break;
	}
}

/* clean up drawing context */
static void
cleandc(void)
{
	size_t i;

	XftColorFree(dpy, visual, colormap, &dc.normal[COLOR_BG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[COLOR_FG]);
	XftColorFree(dpy, visual, colormap, &dc.select[COLOR_BG]);
	XftColorFree(dpy, visual, colormap, &dc.select[COLOR_FG]);
	XftColorFree(dpy, visual, colormap, &dc.scroll[COLOR_BG]);
	XftColorFree(dpy, visual, colormap, &dc.scroll[COLOR_FG]);
	XFreeGC(dpy, dc.gc);
	for (i = 0; i < dc.nfonts; i++)
		XftFontClose(dpy, dc.fonts[i]);
	free(dc.fonts);
}

/* xfiles: X11 file manager */
int
main(int argc, char *argv[])
{
	struct FM fm;
	XEvent ev;
	void (*xevents[LASTEvent])(struct FM *, XEvent *) = {
		[ButtonPress]      = xeventbuttonpress,
		[ClientMessage]    = xeventclientmessage,
		[ConfigureNotify]  = xeventconfigurenotify,
		[Expose]           = xeventexpose,
	};
	char *path;

	if (!XInitThreads())
		errx(1, "XInitThreads");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support");
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "could not open display");
	screen = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, screen);
	depth = DefaultDepth(dpy, screen);
	root = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);
	esetcloexec(XConnectionNumber(dpy));

	XrmInitialize();
	if ((xrm = XResourceManagerString(dpy)) != NULL && (xdb = XrmGetStringDatabase(xrm)) != NULL)
		parseresources();
	parseenviron();
	path = parseoptions(argc, argv);

	imlib_set_cache_size(2048 * 1024);
	imlib_context_set_dither(1);
	imlib_context_set_display(dpy);
	imlib_context_set_visual(visual);
	imlib_context_set_colormap(colormap);

	initsignal(SIGCHLD, SIG_IGN);
	initatoms();
	initdc();
	initfm(&fm, argc, argv);
	initellipsis();

	etcreate(&thumbt, thumbnailer, (void *)&fm);
	diropen(&fm, path, 1);
	drawentries(&fm);

	XMapWindow(dpy, fm.win);

	while (running && !XNextEvent(dpy, &ev))
		if (xevents[ev.type] != NULL)
			(*xevents[ev.type])(&fm, &ev);

	cleandc();

	XCloseDisplay(dpy);

	return 0;
}
