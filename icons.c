#include <stddef.h>

#include "icons.h"

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* icons for files */
#include "icons/file-app.xpm"
#include "icons/file-archive.xpm"
#include "icons/file-audio.xpm"
#include "icons/file-code.xpm"
#include "icons/file-core.xpm"
#include "icons/file-config.xpm"
#include "icons/file-gear.xpm"
#include "icons/file-image.xpm"
#include "icons/file-info.xpm"
#include "icons/file-object.xpm"
#include "icons/file-text.xpm"
#include "icons/file-video.xpm"
#include "icons/file.xpm"

/* icons for directories */
#include "icons/folder-apps.xpm"
#include "icons/folder-book.xpm"
#include "icons/folder-code.xpm"
#include "icons/folder-db.xpm"
#include "icons/folder-download.xpm"
#include "icons/folder-game.xpm"
#include "icons/folder-gear.xpm"
#include "icons/folder-home.xpm"
#include "icons/folder-image.xpm"
#include "icons/folder-link.xpm"
#include "icons/folder-mail.xpm"
#include "icons/folder-meme.xpm"
#include "icons/folder-mount.xpm"
#include "icons/folder-music.xpm"
#include "icons/folder-trash.xpm"
#include "icons/folder-up.xpm"
#include "icons/folder-video.xpm"
#include "icons/folder.xpm"

/* icons for the window (used by the pager or window manager) */
#include "icons/winicon16x16.abgr"
#include "icons/winicon32x32.abgr"
#include "icons/winicon48x48.abgr"
#include "icons/winicon64x64.abgr"

#define TYPES                                        \
	X(executable,           file_app_xpm        )\
	X(object,               file_object_xpm     )\
	X(archive,              file_archive_xpm    )\
	X(audio,                file_audio_xpm      )\
	X(code,                 file_code_xpm       )\
	X(core,                 file_core_xpm       )\
	X(config,               file_config_xpm     )\
	X(makefile,             file_gear_xpm       )\
	X(image,                file_image_xpm      )\
	X(info,                 file_info_xpm       )\
	X(document,             file_text_xpm       )\
	X(video,                file_video_xpm      )\
	X(up_dir,               folder_up_xpm       )\
	X(apps_dir,             folder_apps_xpm     )\
	X(code_dir,             folder_code_xpm     )\
	X(database_dir,         folder_db_xpm       )\
	X(documents_dir,        folder_book_xpm     )\
	X(downloads_dir,        folder_download_xpm )\
	X(games_dir,            folder_game_xpm     )\
	X(images_dir,           folder_image_xpm    )\
	X(config_dir,           folder_gear_xpm     )\
	X(home_dir,             folder_home_xpm     )\
	X(mail_dir,             folder_mail_xpm     )\
	X(meme_dir,             folder_meme_xpm     )\
	X(mount_dir,            folder_mount_xpm    )\
	X(music_dir,            folder_music_xpm    )\
	X(trash_dir,            folder_trash_xpm    )\
	X(videos_dir,           folder_video_xpm    )\
	X(link_dir,             folder_link_xpm     )\
	X(dir,                  folder_xpm          )\
	X(file,                 file_xpm            )

#define PATTERNS                           \
	X("..",           up_dir          )\
	X("*.zip",        archive         )\
	X("*.tar",        archive         )\
	X("*.gz",         archive         )\
	X("*.bz2",        archive         )\
	X("*.rar",        archive         )\
	X("*.mp[23]",     audio           )\
	X("*.m4a",        audio           )\
	X("*m3u",         audio           )\
	X("*.ogg",        audio           )\
	X("*.opus",       audio           )\
	X("*.flac",       audio           )\
	X("*.core",       core            )\
	X("*.xbm",        image           )\
	X("*.xpm",        image           )\
	X("*.p[bgp]m",    image           )\
	X("*.png",        image           )\
	X("*.bmp",        image           )\
	X("*.gif",        image           )\
	X("*.tiff",       image           )\
	X("*.jpeg",       image           )\
	X("*.jpg",        image           )\
	X("*.gif",        image           )\
	X("*.svg",        image           )\
	X("*.[1-9]",      info            )\
	X("README",       info            )\
	X("README.md",    info            )\
	X("COPYING",      info            )\
	X("LICENSE",      info            )\
	X("COPYRIGHT",    info            )\
	X("[Mm]akefile",  makefile        )\
	X("configure",    makefile        )\
	X("configure",    makefile        )\
	X("*.pdf",        document        )\
	X("*.epub",       document        )\
	X("*.txt",        document        )\
	X("*.ps",         document        )\
	X("*.eps",        document        )\
	X("*.djvu",       document        )\
	X("*.o",          object          )\
	X("*.so",         object          )\
	X("*.a",          object          )\
	X("*.mp4",        video           )\
	X("*.webm",       video           )\
	X("*.mkv",        video           )\
	X("*.mov",        video           )\
	X("*.ogv",        video           )\
	X("~/",           home_dir        )\
	X(NULL,           file            )

#define WINICONS                       \
	X(16,             winicon16x16)\
	X(32,             winicon32x32)\
	X(48,             winicon48x48)\
	X(64,             winicon64x64)

enum {
#define X(type, xpm) type,
	TYPES
	NTYPES
#undef  X
};

struct IconType icon_types[NTYPES] = {
#define X(n, p) [n] = { .name = #n, .xpm = p },
	TYPES
#undef  X
};

struct IconPatt icon_patts[] = {
#define X(p, i) { .patt = p, .index = i },
	PATTERNS
#undef  X
};

struct WinIcon win_icons[] = {
#define X(s, d) { .size = s, .data = d },
	WINICONS
#undef  X
};

size_t nicon_types = NTYPES;
size_t nicon_patts = LEN(icon_patts);
size_t nwin_icons  = LEN(win_icons);

size_t icon_for_updir = up_dir;
size_t icon_for_dir   = dir;
size_t icon_for_file  = file;
