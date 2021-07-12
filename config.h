static struct Config config = {
	.opener = "xdg-open",
	.thumbnailer = "thumbnail",

	.font = "monospace:size=9,DejaVuSansMono:size=9",
	.background_color = "#000000",
	.foreground_color = "#FFFFFF",
	.selbackground_color = "#3465a4",
	.selforeground_color = "#FFFFFF",
	.scrollbackground_color = "#121212",
	.scrollforeground_color = "#707880",

	.thumbsize_pixels = 64, /* size of thumbnails and icons */
	.scroll_pixels = 12,    /* scroll bar width */
	.width_pixels = 600,    /* initial window width */
	.height_pixels = 460,   /* initial window height */
	.hide = 1,              /* whether to hide .* entries */
};
