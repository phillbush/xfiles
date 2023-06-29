/* file mode */
enum Mode {
	/* file type, 0x00 ~ 0x07 */
	MODE_ANY      = 0x00,   /* any type */
	MODE_DIR      = 0x01,   /* directory */
	MODE_FIFO     = 0x02,   /* named pipe */
	MODE_SOCK     = 0x03,   /* socket */
	MODE_DEV      = 0x04,   /* block or character devices */
	MODE_BROK     = 0x05,   /* broken links */
	MODE_FILE     = 0x07,   /* regular file */

	/* file mask */
	MODE_MASK     = 0x07,

	/* file mode bit */
	MODE_LINK     = 0x08,
	MODE_READ     = 0x10,
	MODE_WRITE    = 0x20,
	MODE_EXEC     = 0x40,
};

/* mapping of filetype names into pixmaps */
extern struct IconType {
	char   *name;
	char  **xpm;
} icon_types[];

/* size of the icon_types[] array */
extern size_t   nicon_types;

/* mapping of globbing patterns into indices from the icon_types[] array */
extern struct IconPatt {
	unsigned char mode;
	char   *patt;
	size_t  index;
} icon_patts[];

/* size of the icon_patts[] array */
extern size_t   nicon_patts;

/* array of window icons */
extern struct WinIcon {
	int size;
	unsigned long *data;
} win_icons[];

/* size of the win_icons[] array */
extern size_t   nwin_icons;

/* indices from the icon_types[] array for a few default icons */
extern size_t icon_for_updir;   /* icon for the parent directory */
extern size_t icon_for_dir;     /* icon for child directories */
extern size_t icon_for_file;    /* icon for files */
