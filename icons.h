/* mapping of filetype names into pixmaps */
extern struct IconType {
	char   *name;
	char  **xpm;
} icon_types[];

/* size of the icon_types[] array */
extern size_t   nicon_types;

/* mapping of globbing patterns into indices from the icon_types[] array */
extern struct IconPatt {
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
