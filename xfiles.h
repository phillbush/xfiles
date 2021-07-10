#define ELLIPSIS     "…"
#define CLASS        "XFiles"
#define TITLE        "XFiles"
#define THUMBSIZE    64         /* size of each thumbnail */
#define THUMBBORDER  3          /* thumbnail border (for highlighting) */
#define DOUBLECLICK  250
#define DEV_NULL     "/dev/null"
#define DOTDOT       ".."

/* poll entries */
enum {
	POLL_STDIN,
	POLL_X11,
	POLL_THUMB,
	POLL_LAST,
};

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
	UNSEL = 0,
	SEL = 1,
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

/* file manager */
struct FM {
	struct Entry **entries; /* array of pointer to entries */
	struct Entry *selected; /* list of selected entries */
	struct Rect dirrect;    /* size and position of default thumbnail for directories */
	struct Rect filerect;   /* size and position of default thumbnail for files */
	struct Histent *hist;   /* cwd history; pointer to last cwd history entry */
	struct Histent *curr;   /* current point in history */
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
