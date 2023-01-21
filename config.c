#include <stddef.h>

#define THUMBSIZE       "64"
#define NCMDARGS        3
#define LEN(a)          (sizeof(a)/sizeof((a)[0]))

/*
 * First include the icon files.
 */
#include "icons/file-app.xpm"
#include "icons/file-archive.xpm"
#include "icons/file-audio.xpm"
#include "icons/file-code.xpm"
#include "icons/file-conf.xpm"
#include "icons/file-core.xpm"
#include "icons/file-gear.xpm"
#include "icons/file-image.xpm"
#include "icons/file-info.xpm"
#include "icons/file-obj.xpm"
#include "icons/file-text.xpm"
#include "icons/file-video.xpm"
#include "icons/file.xpm"
#include "icons/folder-apps.xpm"
#include "icons/folder-book.xpm"
#include "icons/folder-code.xpm"
#include "icons/folder-db.xpm"
#include "icons/folder-dl.xpm"
#include "icons/folder-game.xpm"
#include "icons/folder-gear.xpm"
#include "icons/folder-home.xpm"
#include "icons/folder-image.xpm"
#include "icons/folder-link.xpm"
#include "icons/folder-meme.xpm"
#include "icons/folder-mount.xpm"
#include "icons/folder-music.xpm"
#include "icons/folder-up.xpm"
#include "icons/folder-video.xpm"
#include "icons/folder.xpm"

/*
 * Then specify the file patterns.
 *
 * - Element [0] is type ("f" for regular file, "fx" for regular
 *   executable file, "d" for directory, etc).
 * - Elements [1] through [n-2] are globbing patterns.  Environment
 *   variables are not supported.   But a tilde in the beginning of
 *   the string expands to the home directory.
 * - Element [n-1] is the obligatory NULL.
 */
static char *file_app[]      = { "fx", NULL };
static char *file_archive[]  = { "f", "*.tar*", "*.t[bgx]", "*.bz2", "*.rar", "*.zip", NULL };
static char *file_audio[]    = { "f", "*.mp[23]", "*.m4a", "*.ogg", "*.opus", "*.flac", NULL };
static char *file_code[]     = { "f", "*.c", "*.cpp", "*.h", "*.hpp", "*.php", "*.lua", "*.rs", "*.ha", "*.sh", "*.s", "*.PL", "*.pl", NULL };
static char *file_conf[]     = { "f", "*.c*nf", "*.yaml", "*.toml", "*.ini", "*.xml", NULL };
static char *file_core[]     = { "f", "*.core", NULL };
static char *file_gear[]     = { "f", "[Mm]akefile", "configure", "*in", NULL };
#ifdef USE_NETPBM
static char *file_bmp[]      = { "f", "*.bmp", NULL };
static char *file_gif[]      = { "f", "*.gif", NULL };
static char *file_png[]      = { "f", "*.png", NULL };
static char *file_ppm[]      = { "f", "*.p[bgp]m", NULL };
static char *file_tiff[]     = { "f", "*.tiff", NULL };
static char *file_xbm[]      = { "f", "*.xbm", NULL };
static char *file__xpm[]     = { "f", "*.xpm", NULL };
#endif
static char *file_image[]    = { "f", "*.bmp", "*.gif", "*.png", "*.p[bgp]m",  "*.tiff", "*.x[pb]m", NULL };
static char *file_jpeg[]     = { "f", "*.jpeg", "*.jpe", "*.jpg", "*.jfi", "*.jif", "*.jfif", NULL };
static char *file_svg[]      = { "f", "*.svg", NULL };
static char *file_info[]     = { "f", "README*", "*.[1-9]", NULL };
static char *file_obj[]      = { "f", "*.so", "*.[So]", "*.a", NULL };
static char *file_text[]     = { "f", "*.epub", "*.txt", "*.ps", "*.eps", "*.djvu", "*.[f]odt", "*.doc*", "LICENSE*", "COPYING*", "*.md", NULL };
static char *file_pdf[]      = { "f", "*.pdf", NULL };
static char *file_video[]    = { "f", "*.mp4", "*.webm", "*.mkv", "*.mov", "*.ogv", NULL };
static char *folder_apps[]   = { "d", "bin", "usr", NULL };
static char *folder_book[]   = { "d", "~/doc", "~/docs", "~/[Dd]ox", "[Dd]ocuments", NULL };
static char *folder_code[]   = { "d", "src", "~/prj", "~/proj", "~/code", "[Pp]rojects", NULL };
static char *folder_db[]     = { "d", "var", "share", NULL };
static char *folder_dl[]     = { "d", "tmp", "~/dl", "[Dd]ownloads", NULL };
static char *folder_game[]   = { "d", "[Gg]ames", NULL };
static char *folder_gear[]   = { "d", "lib", "~/.config", "/etc", "~/etc", NULL };
static char *folder_image[]  = { "d", "img", "[Pp]ix", "[Pp]ic*", "[Ii]mage*", NULL };
static char *folder_home[]   = { "d", "~", NULL };
static char *folder_link[]   = { "dl", NULL };
static char *folder_meme[]   = { "d", "[Mm]eme*", NULL };
static char *folder_mount[]  = { "d", "mnt", "[Mm]edia", "mount", NULL };
static char *folder_music[]  = { "d", "mus", "music", "[Mm]usic", NULL };
static char *folder_up[]     = { "d", "..", NULL };
static char *folder_video[]  = { "d", "[Vv]id*", NULL };
static char *folder[]        = { "d", NULL };

/*
 * Finally, link file patterns with icons.
 *
 * Last element must have null pattern.
 */
char **icons[][2] = {
	{ file_app,        file_app_xpm,     },
	{ file_archive,    file_archive_xpm, },
	{ file_audio,      file_audio_xpm,   },
	{ file_code,       file_code_xpm,    },
	{ file_conf,       file_conf_xpm,    },
	{ file_core,       file_core_xpm,    },
	{ file_gear,       file_gear_xpm,    },
	{ file_image,      file_image_xpm,   },
	{ file_jpeg,       file_image_xpm,   },
	{ file_svg,        file_image_xpm,   },
	{ file_info,       file_info_xpm,    },
	{ file_obj,        file_obj_xpm,     },
	{ file_text,       file_text_xpm,    },
	{ file_pdf,        file_text_xpm,    },
	{ file_video,      file_video_xpm,   },
	{ folder_apps,     folder_apps_xpm,  },
	{ folder_book,     folder_book_xpm,  },
	{ folder_code,     folder_code_xpm,  },
	{ folder_db,       folder_db_xpm,    },
	{ folder_dl,       folder_dl_xpm,    },
	{ folder_game,     folder_game_xpm,  },
	{ folder_gear,     folder_gear_xpm,  },
	{ folder_image,    folder_image_xpm, },
	{ folder_home,     folder_home_xpm,  },
	{ folder_link,     folder_link_xpm,  },
	{ folder_meme,     folder_meme_xpm,  },
	{ folder_mount,    folder_mount_xpm, },
	{ folder_music,    folder_music_xpm, },
	{ folder_up,       folder_up_xpm,    },
	{ folder_video,    folder_video_xpm, },
	{ folder,          folder_xpm,       },
	{ NULL,            file_xpm,         },
};
size_t nicons = LEN(icons);

/*
 * The following are commands to be run to generate thumbnails.
 */
#define PNMSCALE " 2>/dev/null | pnmscalefixed -xysize "THUMBSIZE" "THUMBSIZE" "

#ifdef USE_NETPBM
static char *ppmtoppm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" ppmtoppm"PNMSCALE">\"${2}\""
};
static char *xbmtopbm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" xbmtopbm | ppmtoppm"PNMSCALE">\"${2}\""
};
static char *xpmtoppm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" xpmtoppm"PNMSCALE">\"${2}\""
};
static char *pngtopnm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" pngtopnm"PNMSCALE">\"${2}\""
};
static char *bmptopnm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" bmptopnm"PNMSCALE">\"${2}\""
};
static char *giftopnm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" giftopnm"PNMSCALE">\"${2}\""
};
static char *tifftopnm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" tifftopnm"PNMSCALE">\"${2}\""
};
static char *jpegtopnm[NCMDARGS] = {
	"/bin/sh", "-c",
	"<\"${1}\" jpegtopnm"PNMSCALE">\"${2}\""
};
#else
static char *imagemagick[NCMDARGS] = {
	"/bin/sh", "-c",
	"convert \"${1}[0]\" -background '#0A0A0A' -flatten " \
	"-format ppm -thumbnail \""THUMBSIZE"x"THUMBSIZE"\" " \
	"-define filename:literal=true \"${2}\"",
};
#endif
static char *ffmpegthumb[NCMDARGS] = {
	"/bin/sh", "-c",
	"ffmpegthumbnailer -c png -i \"${1}\" " \
	"-o - -s "THUMBSIZE" | convert - -define " \
	"filename:literal=true -format ppm \"${2}\"",
};
static char *rsvgconvert[NCMDARGS] = {
	"/bin/sh", "-c",
	"rsvg-convert -h "THUMBSIZE" \"${1}\" " \
	"| convert - -format ppm \"${2}\"",
};
static char *pdftoppm[NCMDARGS] = {
	"/bin/sh", "-c",
	"pdftoppm -f 1 -l 1 -scale-to "THUMBSIZE" " \
	"-singlefile \"${1}\" \"${2%.ppm}\"",
};

/*
 * The following table links file patterns with commands to be run to
 * generate thumbnails.
 *
 * Last element must be all NULL.
 */
char **thumbs[][2] = {
	{ file_video,      ffmpegthumb, },
#ifdef USE_NETPBM
	{ file_xbm,        xbmtopbm, },
	{ file__xpm,       xpmtoppm, },
	{ file_ppm,        ppmtoppm, },
	{ file_png,        pngtopnm, },
	{ file_bmp,        bmptopnm, },
	{ file_gif,        giftopnm, },
	{ file_tiff,       tifftopnm, },
	{ file_jpeg,       jpegtopnm, },
#else
	{ file_image,      imagemagick, },
	{ file_jpeg,       imagemagick, },
#endif
	{ file_svg,        rsvgconvert, },
	{ file_pdf,        pdftoppm,    },
	{ NULL,            NULL, },
};
