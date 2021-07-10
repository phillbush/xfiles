#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <Imlib2.h>
#include "xfiles.h"

static struct DC dc;
static struct Ellipsis ellipsis;
static Display *dpy;
static Visual *visual;
static Window root;
static Colormap colormap;
static Atom atoms[ATOM_LAST];
static int screen;
static int depth;

static int running = 1;         /* whether xfiles is running */

#include "config.h"

/* show usage and exit */
static void
usage(void)
{
	(void)fprintf(stderr, "usage: xfiles [-g geometry] [path]\n");
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

	if ((pid = fork()) == -1)
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

/* call chdir checking for error; print warning on error */
static int
wchdir(const char *path)
{
	if (chdir(path) == -1) {
		warn(NULL);
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

/* parse options; return argument (if given) or NULL */
static char *
parseoptions(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "g:")) != -1) {
		switch (ch) {
		case 'g':
			// TODO
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

/* load variable from environment variable */
static int
loadenv(char *name, char **retval, int *retint, unsigned int *retuint)
{
    int tempint;
    char *tempval, *dummy;

    tempval = getenv(name);
    if (!tempval)
        return 0;
    if (retval)
        (*retval) = tempval;
    if (retint || retuint) {
        errno = 0;
        tempint = strtol(tempval, &dummy, 0);
        if (!tempint && errno)
            return 0;
    }
    if (retint)
        (*retint) = tempint;
    if (retuint)
        (*retuint) = tempint;
    return 1;
}

/* load config from env variables */
static void
loadconfig(struct Config *cfg) {
    loadenv("XFILES_OPENER", &cfg->opener, NULL, NULL);
    loadenv("XFILES_THUMBNAILER", &cfg->thumbnailer, NULL, NULL);
    loadenv("XFILES_DIRTHUMB", &cfg->dirthumb_path, NULL, NULL);
    loadenv("XFILES_FILETHUMB", &cfg->filethumb_path, NULL, NULL);

    loadenv("XFILES_FONT", &cfg->font, NULL, NULL);
    loadenv("XFILES_BACKGROUND", &cfg->background_color, NULL, NULL);
    loadenv("XFILES_FOREGROUND", &cfg->foreground_color, NULL, NULL);
    loadenv("XFILES_SELECTEDBG", &cfg->selbackground_color, NULL, NULL);
    loadenv("XFILES_SELECTEDFG", &cfg->selforeground_color, NULL, NULL);
    loadenv("XFILES_SCROLLBG", &cfg->scrollbackground_color, NULL, NULL);
    loadenv("XFILES_SCROLLFG", &cfg->scrollforeground_color, NULL, NULL);

    loadenv("XFILES_SCROLLPX", NULL, &cfg->scroll_pixels, NULL);
    loadenv("XFILES_WIDTHPX", NULL, &cfg->width_pixels, NULL);
    loadenv("XFILES_HEIGHTPX", NULL, &cfg->height_pixels, NULL);
    loadenv("XFILES_HIDE", NULL, &cfg->hide, NULL);
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
	thumb->x = fm->thumbx + max((THUMBSIZE - thumb->w) / 2, 0);
	thumb->y = fm->thumby + max((THUMBSIZE - thumb->h) / 2, 0);

	/* clean pixmaps */
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[UNSEL], dc.gc, fm->thumbx, fm->thumby, THUMBSIZE, THUMBSIZE);
	XFillRectangle(dpy, pix[SEL], dc.gc, fm->thumbx - THUMBBORDER, fm->thumby - THUMBBORDER, THUMBSIZE + 2 * THUMBBORDER, THUMBSIZE + 2 * THUMBBORDER);

	/* draw border on sel pixmap around thumbnail */
	XSetForeground(dpy, dc.gc, dc.select[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[SEL], dc.gc, thumb->x - THUMBBORDER, thumb->y - THUMBBORDER, thumb->w + 2 * THUMBBORDER, thumb->h + 2 * THUMBBORDER);
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, pix[SEL], dc.gc, thumb->x, thumb->y, thumb->w, thumb->h);

	/* draw thumbnail on both pixmaps */
	imlib_context_set_image(img);
	imlib_image_set_changes_on_disk();
	imlib_context_set_drawable(pix[UNSEL]);
	imlib_render_image_on_drawable(thumb->x, thumb->y);
	imlib_context_set_drawable(pix[SEL]);
	imlib_render_image_on_drawable(thumb->x, thumb->y);

	/* free image */
	imlib_free_image();
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
	fm->entries = NULL;
	fm->selected = NULL;
	fm->capacity = 0;
	fm->nentries = 0;
	fm->row = 0;
	fm->ydiff = 0;
	fm->entryw = THUMBSIZE * 2;
	fm->entryh = THUMBSIZE + 3 * dc.fonth + 2 * THUMBBORDER;
	fm->thumbx = THUMBSIZE / 2;
	fm->thumby = dc.fonth / 2;
	fm->textw = fm->entryw - dc.fonth;
	fm->textx = dc.fonth / 2;
	fm->texty0 = dc.fonth / 2 + THUMBSIZE + 2 * THUMBBORDER;
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
	fm->file[UNSEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	fm->file[SEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	fm->dir[UNSEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	fm->dir[SEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	XSetForeground(dpy, dc.gc, dc.normal[COLOR_BG].pixel);
	XFillRectangle(dpy, fm->file[UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh);
	XFillRectangle(dpy, fm->file[SEL], dc.gc, 0, 0, fm->entryw, fm->entryh);
	XFillRectangle(dpy, fm->dir[UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh);
	XFillRectangle(dpy, fm->dir[SEL], dc.gc, 0, 0, fm->entryw, fm->entryh);

	/* draw default thumbnails */
	fileimg = loadimg(config.filethumb_path, THUMBSIZE, &fm->filerect.w, &fm->filerect.h);
	drawthumb(fm, fileimg, &fm->filerect, fm->file);
	dirimg = loadimg(config.dirthumb_path, THUMBSIZE, &fm->dirrect.w, &fm->dirrect.h);
	drawthumb(fm, dirimg, &fm->dirrect, fm->dir);
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
	ent->pix[UNSEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	ent->pix[SEL] = XCreatePixmap(dpy, fm->win, fm->entryw, fm->entryh, depth);
	ent->issel = 0;
	ent->isdir = isdir;
	ent->drawn = 0;
	ent->name = estrdup(name);
	memset(ent->line, 0, sizeof(ent->line));
	if (ent->isdir) {
		ent->thumb = fm->dirrect;
		XCopyArea(dpy, fm->dir[UNSEL], ent->pix[UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
		XCopyArea(dpy, fm->dir[SEL], ent->pix[SEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
	} else {
		ent->thumb = fm->filerect;
		XCopyArea(dpy, fm->file[UNSEL], ent->pix[UNSEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
		XCopyArea(dpy, fm->file[SEL], ent->pix[SEL], dc.gc, 0, 0, fm->entryw, fm->entryh, 0, 0);
	}
	return ent;
}

/* destroy entry */
static void
freeentries(struct FM *fm)
{
	int i;

	for (i = 0; i < fm->nentries; i++) {
		XFreePixmap(dpy, fm->entries[i]->pix[UNSEL]);
		XFreePixmap(dpy, fm->entries[i]->pix[SEL]);
		free(fm->entries[i]->name);
		free(fm->entries[i]);
	}
}

/* populate list of entries on fm; return -1 on error */
static int
listentries(struct FM *fm)
{
	struct dirent **array;
	struct stat sb;
	int i, n, isdir;
	char path[PATH_MAX];

	if (getcwd(path, sizeof(path)) == NULL) {
		warn("getcwd");
		return -1;
	}
	if ((n = scandir(path, &array, direntselect, NULL)) == -1) {
		warn("scandir");
		return -1;
	}
	freeentries(fm);
	if (n > fm->capacity) {
		fm->entries = ereallocarray(fm->entries, n, sizeof *fm->entries);
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
	return 0;
}

/* get index of entry from pointer position; return -1 if not found */
static int
getentry(struct FM *fm, int x, int y)
{
	struct Entry *ent;
	int i, n, w, h;

	if (x < fm->x0 || x >= fm->x0 + fm->winw)
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
	pix = (fm->entries[i]->issel ? fm->entries[i]->pix[SEL] : fm->entries[i]->pix[UNSEL]);
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
		for (; ent->name[len0] != '\0' && !isspace(ent->name[len0]); len0++)
			;
		textw0 = drawtext(NULL, NULL, 0, 0, 0, ent->name, len0);
		while (isspace(ent->name[len0]))
			len0++;
	} while (textw0 < fm->textw && ent->name[len0] != '\0' && prevw != textw0);
	i = len0 = prevlen;
	textw0 = prevw;
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

	draw = XftDrawCreate(dpy, ent->pix[UNSEL], visual, colormap);
	drawtext(draw, &dc.normal[COLOR_FG], x0, fm->texty0, fm->textw, ent->name, len0);
	if (i > 0 && ent->name[i] != '\0')
		drawtext(draw, &dc.normal[COLOR_FG], x1, fm->texty1, fm->textw, ent->name + i, len1);
	XftDrawDestroy(draw);
	ent->line[0].x = x0;
	ent->line[0].w = textw0;

	XSetForeground(dpy, dc.gc, dc.select[COLOR_BG].pixel);
	XFillRectangle(dpy, ent->pix[SEL], dc.gc, x0, fm->texty0, textw0, dc.fonth);
	draw = XftDrawCreate(dpy, ent->pix[SEL], visual, colormap);
	drawtext(draw, &dc.select[COLOR_FG], x0, fm->texty0, fm->textw, ent->name, len0);
	if (i > 0 && ent->name[i] != '\0') {
		XFillRectangle(dpy, ent->pix[SEL], dc.gc, x1, fm->texty1, textw1, dc.fonth);
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
	return -1;
}

/* read image path from file descriptor and open image; return its size on *w and *h */
static Imlib_Image
openimg(int fd, int *w, int *h)
{
	FILE *fp;
	int len;
	char path[PATH_MAX];

	if ((fp = fdopen(fd, "r")) == NULL) {
		warn("fdopen");
		return NULL;
	}
	if (fgets(path, sizeof(path), fp) == NULL)
		return NULL;
	len = strlen(path);
	if (path[len - 1] == '\n')
		path[len - 1] = '\0';
	return loadimg(path, THUMBSIZE, w, h);
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
	fm->row =  y * fm->maxrow / fm->winh;
	fm->ydiff = (y - fm->row * fm->winh / fm->maxrow) * fm->entryh / fm->scrollh;
	if (fm->row >= fm->maxrow - 1) {
		fm->row = fm->maxrow - 1;
		fm->ydiff = 0;
	}
	return prevrow != fm->row;
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
			if (ev.xexpose.count == 0)
				commitdraw(fm);
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

/* change directory, reset fd of thumbnailer and index of entry whose thumbnail is being read */
static void
diropen(struct FM *fm, const char *name, int *fd, int *thumbi)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "./%s", name);
	while (fm->selected)
		selectentry(fm, fm->selected, 0);
	if (*thumbi != -1 && *fd != -1) {/* close current thumbnailer fd and wait for it */
		close(*fd);
		wait(NULL);
		*thumbi = -1;
	}
	if (wchdir(path) != -1 && listentries(fm) != -1) {
		calcsize(fm, -1, -1);
		fm->row = 0;
		fm->ydiff = 0;
		*thumbi = 0;
		*fd = forkthumb(fm->entries[*thumbi]->name);
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
		} else {
			waitpid(pid2, NULL, 0);
		}
		exit(0);
	}
	waitpid(pid1, NULL, 0);
}

/* process X11 and poll events */
static void
runevent(struct FM *fm, struct pollfd pfd[], int *thumbi)
{
	static int lastent = -1;        /* index of last clicked entry; -1 if none */
	static Time lasttime = 0;
	struct Entry *ent;
	XEvent ev;
	Imlib_Image img;
	int setlastent;
	int i;

	if (pfd[POLL_STDIN].revents & POLLHUP)
		return;
	if (pfd[POLL_STDIN].revents & POLLIN) {
		// TODO;
	}
	if (pfd[POLL_THUMB].revents & POLLIN) {
		ent = fm->entries[*thumbi];
		img = openimg(pfd[POLL_THUMB].fd, &ent->thumb.w, &ent->thumb.h);
		close(pfd[POLL_THUMB].fd);
		wait(NULL);
		if (img != NULL) {
			drawthumb(fm, img, &ent->thumb, ent->pix);
			copyentry(fm, *thumbi);
			if (*thumbi >= fm->row * fm->ncol && *thumbi < fm->row * fm->ncol + fm->nrow * fm->ncol) {
				commitdraw(fm);
			}
		}
		if (*thumbi != -1 && ++(*thumbi) < fm->nentries) {
			pfd[POLL_THUMB].fd = forkthumb(fm->entries[*thumbi]->name);
		} else {
			pfd[POLL_THUMB].fd = -1;
			*thumbi = -1;
		}
	}
	while (XPending(dpy) && !XNextEvent(dpy, &ev)) {
		setlastent = 1;
		if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == atoms[WM_DELETE_WINDOW]) {
			/* file manager window was closed */
			running = 0;
			break;
		} else if (ev.type == Expose && ev.xexpose.count == 0) {
			/* window was exposed; redraw its content */
			commitdraw(fm);
		} else if (ev.type == ConfigureNotify) {
			/* file manager window was (possibly) resized */
			calcsize(fm, ev.xconfigure.width, ev.xconfigure.height);
			if (fm->row >= fm->maxrow)
				fm->row = fm->maxrow - 1;
			drawentries(fm);
			commitdraw(fm);
		} else if (ev.type == ButtonPress && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
			/* mouse wheel was scrolled */
			if (scroll(fm, fm->scrolly + fm->scrollh / 2 + (ev.xbutton.button == Button4 ? -5 : +5)))
				drawentries(fm);
			commitdraw(fm);
		} else if (ev.type == ButtonPress && ev.xbutton.button == Button1 && ev.xbutton.x > fm->winw) {
			/* scrollbar was manipulated */
			grabscroll(fm, ev.xbutton.y);
		} else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
			/* mouse left button was pressed */
			if (!(ev.xbutton.state & (ControlMask | ShiftMask)))
				while (fm->selected)
					selectentry(fm, fm->selected, 0);
			i = getentry(fm, ev.xbutton.x, ev.xbutton.y);
			if (ev.xbutton.state & ShiftMask)
				selectentries(fm, i, lastent, 1);
			if (i != -1)
				selectentry(fm, fm->entries[i], 1);
			if (i != -1 && i == lastent && ev.xbutton.time - lasttime <= DOUBLECLICK) {
				ent = fm->entries[i];
				if (ent->isdir) {
					diropen(fm, ent->name, &pfd[POLL_THUMB].fd, thumbi);
					setlastent = 0;
				} else {
					fileopen(ent);
				}
			}
			drawentries(fm);
			commitdraw(fm);
			lastent = (setlastent) ? i : -1;
			lasttime = ev.xbutton.time;
		}
	}
}

/* clean up drawing context */
static void
cleandc(void)
{
	XftColorFree(dpy, visual, colormap, &dc.normal[COLOR_BG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[COLOR_FG]);
	XftColorFree(dpy, visual, colormap, &dc.select[COLOR_BG]);
	XftColorFree(dpy, visual, colormap, &dc.select[COLOR_FG]);
	XftColorFree(dpy, visual, colormap, &dc.scroll[COLOR_BG]);
	XftColorFree(dpy, visual, colormap, &dc.scroll[COLOR_FG]);
	XFreeGC(dpy, dc.gc);
}

/* xfiles: X11 file manager */
int
main(int argc, char *argv[])
{
	struct pollfd pfd[POLL_LAST];
	struct FM fm;
	char *path;
	int thumbi = -1;        /* index of current thumbnail, -1 if no thumbnail is being read */

	path = parseoptions(argc, argv);
	if (path != NULL)
		wchdir(path);

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support");
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "could not open display");
	screen = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, screen);
	depth = DefaultDepth(dpy, screen);
	root = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);

    loadconfig(&config);

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

	memset(pfd, 0, sizeof pfd);
	pfd[POLL_STDIN].fd = STDIN_FILENO;
	pfd[POLL_X11].fd = XConnectionNumber(dpy);
	pfd[POLL_THUMB].fd = -1;
	pfd[POLL_STDIN].events = pfd[POLL_X11].events = pfd[POLL_THUMB].events = POLLIN;
	esetcloexec(pfd[POLL_X11].fd);

	if (listentries(&fm) != -1) {
		drawentries(&fm);
		thumbi = 0;
		pfd[POLL_THUMB].fd = forkthumb(fm.entries[thumbi]->name);
	}

	XMapWindow(dpy, fm.win);

	while (running && (XPending(dpy) || poll(pfd, POLL_LAST, -1) != -1))
		runevent(&fm, pfd, &thumbi);

	cleandc();

	XCloseDisplay(dpy);

	return 0;
}
