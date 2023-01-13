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
#include "icons/file-core.xpm"
#include "icons/file-gear.xpm"
#include "icons/file-image.xpm"
#include "icons/file-info.xpm"
#include "icons/file-text.xpm"
#include "icons/file-video.xpm"
#include "icons/file.xpm"
#include "icons/folder-apps.xpm"
#include "icons/folder-book.xpm"
#include "icons/folder-code.xpm"
#include "icons/folder-db.xpm"
#include "icons/folder-download.xpm"
#include "icons/folder-game.xpm"
#include "icons/folder-gear.xpm"
#include "icons/folder-home.xpm"
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
static char *file_app[]        = { "fx", NULL };
static char *file_archive[]    = { "f", "*.zip", "*.tar", "*.gz", "*.rar", NULL };
static char *file_audio[]      = { "f", "*.mp[23]", "*.ogg", "*.opus", "*.flac", NULL };
static char *file_code[]       = { "f", "*.c", "*.h", "*.s", NULL };
static char *file_core[]       = { "f", "*.core", NULL };
static char *file_image[]      = { "f", "*.x[pb]m", "*.png", "*.jpg", "*.jpeg", "*.ppm", "*.gif", NULL };
static char *file_svg[]        = { "f", "*.svg", NULL };
static char *file_readme[]     = { "f", "README", "README.md", NULL };
static char *file_makefile[]   = { "f", "[Mm]akefile", NULL };
static char *file_pdf[]        = { "f", "*.pdf", NULL };
static char *file_text[]       = { "f", "*.epub", "*.txt", "*.ps", "*.eps", "*.djvu", NULL };
static char *file_video[]      = { "f", "*.mp4", "*.webm", "*.mkv", "*.mov", "*.ogv", NULL };
static char *folder_bin[]      = { "d", "~/usr", "~/bin", NULL };
static char *folder_code[]     = { "d", "~/prj", "~/proj", "~/code", "~/[Pp]rojects", NULL };
static char *folder_db[]       = { "d", "~/var", "~/.local", "~/.local/share", NULL };
static char *folder_docs[]     = { "d", "~/doc", "~/docs", "~/[Dd]ox", "~/[Dd]ocuments", NULL };
static char *folder_download[] = { "d", "~/tmp", "~/dl", "~/[Dd]ownloads", NULL };
static char *folder_game[]     = { "d", "~/game", "~/[Gg]ames", NULL };
static char *folder_config[]   = { "d", "~/.config", "/etc", "~/etc", "~/lib", NULL };
static char *folder_meme[]     = { "d", "~/mem", "~/meme", "~/[Mm]emes", NULL };
static char *folder_mount[]    = { "d", "~/mnt", "~/mount", "/[Mm]edia", "/mnt", NULL };
static char *folder_music[]    = { "d", "~/mus", "~/music", "/[Mm]usic", NULL };
static char *folder_video[]    = { "d", "~/vid", "~/[Vv]ideo", "/[Vv]ideos", NULL };
static char *folder_home[]     = { "d", "~", NULL };
static char *folder_up[]       = { "d", "..", NULL };
static char *folder_link[]     = { "dl", NULL };
static char *folder[]          = { "d", NULL };

/*
 * Finally, link file patterns with icons.
 *
 * Last element must have null pattern.
 */
char **icons[][2] = {
	{ file_app,        file_app_xpm,        },
	{ file_archive,    file_archive_xpm,    },
	{ file_audio,      file_audio_xpm,      },
	{ file_code,       file_code_xpm,       },
	{ file_core,       file_core_xpm,       },
	{ file_makefile,   file_gear_xpm,       },
	{ file_image,      file_image_xpm,      },
	{ file_svg,        file_image_xpm,      },
	{ file_readme,     file_info_xpm,       },
	{ file_pdf,        file_text_xpm,       },
	{ file_text,       file_text_xpm,       },
	{ file_video,      file_video_xpm,      },
	{ folder_up,       folder_up_xpm,       },
	{ folder_bin,      folder_apps_xpm,     },
	{ folder_code,     folder_code_xpm,     },
	{ folder_db,       folder_db_xpm,       },
	{ folder_docs,     folder_book_xpm,     },
	{ folder_download, folder_download_xpm, },
	{ folder_game,     folder_game_xpm,     },
	{ folder_config,   folder_gear_xpm,     },
	{ folder_home,     folder_home_xpm,     },
	{ folder_meme,     folder_meme_xpm,     },
	{ folder_mount,    folder_mount_xpm,    },
	{ folder_music,    folder_music_xpm,    },
	{ folder_video,    folder_video_xpm,    },
	{ folder_link,     folder_link_xpm,     },
	{ folder,          folder_xpm,          },
	{ NULL,            file_xpm,            },
};
size_t nicons = LEN(icons);

/*
 * The following are commands to be run to generate thumbnails.
 */
static char *imagemagick[NCMDARGS] = {
	"/bin/sh", "-c",
	"convert \"${1}\" -background '#0A0A0A' -flatten " \
	"-format ppm -thumbnail \""THUMBSIZE"x"THUMBSIZE"\" " \
	"-define filename:literal=true \"${2}\"",
};
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
	{ file_image,      imagemagick, },
	{ file_svg,        rsvgconvert, },
	{ file_pdf,        pdftoppm,    },
	{ NULL,            NULL, },
};
