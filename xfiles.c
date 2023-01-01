#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <fnmatch.h>
#include <err.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "widget.h"

#define HOME            "HOME"
#define FILE_ICONS      "FILE_ICONS"
#define APPCLASS        "XFiles"
#define DEF_OPENER      "xdg-open"
#define OPENER          "OPENER"
#define THUMBNAILER     "THUMBNAILER"
#define THUMBNAILDIR    "THUMBNAILDIR"
#define DEV_NULL        "/dev/null"
#define DOTDOT          ".."
#define LAST_ARG        "--"
#define UNIT_LAST       7
#define SIZE_BUFSIZE    6       /* 4 digits + suffix char + nul */
#define TIME_BUFSIZE    128

enum {
	STATE_NAME,     /* entry used by widget.c */
	STATE_PATH,     /* entry used by widget.c */
	STATE_SIZE,     /* entry used by widget.c */
	STATE_MODE,     /* entry used here just for matching icon spec */
	STATE_TIME,     /* not used rn, I'll either remove it or find some use */
	STATE_OWNER,    /* not used rn, I'll either remove it or find some use */
	STATE_LAST,
};

struct IconPattern {
	struct IconPattern *next;
	struct Pattern {
		struct Pattern *next;
		char *s;
	} *patt;
	char *path;
	char mode;
};

struct FM {
	Widget wid;
	char ***entries;
	int *foundicons;
	int capacity;           /* capacity of entries */
	int nentries;           /* number of entries */
	char path[PATH_MAX];
	char here[PATH_MAX];
	char *home;
	size_t homelen;
	struct IconPattern *icons;

	pthread_mutex_t thumblock;
	pthread_t thumbthread;
	int thumbexit;
	char *thumbnailer;
	char *thumbnaildir;
	size_t thumbnaildirlen;

	char *opener;
};

static int hide = 1;
static struct {
	char u;
	long long int n;
} units[UNIT_LAST] = {
	{ 'B', 1LL },
	{ 'K', 1024LL },
	{ 'M', 1024LL * 1024 },
	{ 'G', 1024LL * 1024 * 1024 },
	{ 'T', 1024LL * 1024 * 1024 * 1024 },
	{ 'P', 1024LL * 1024 * 1024 * 1024 * 1024 },
	{ 'E', 1024LL * 1024 * 1024 * 1024 * 1024 * 1024 },
};

static void
usage(void)
{
	(void)fprintf(stderr, "usage: xfiles [-a] [-g geometry] [-n name] [path]\n");
	exit(1);
}

static int
wchdir(const char *path)
{
	if (chdir(path) == -1) {
		warn("%s", path);
		return RET_ERROR;
	}
	return RET_OK;
}

static int
direntselect(const struct dirent *dp)
{
	if (strcmp(dp->d_name, ".") == 0)
		return FALSE;
	if (strcmp(dp->d_name, "..") == 0)
		return TRUE;
	if (hide && dp->d_name[0] == '.')
		return FALSE;
	return TRUE;
}

static void
freeentries(struct FM *fm)
{
	int i, j;

	for (i = 0; i < fm->nentries; i++) {
		for (j = 0; j < STATE_LAST; j++)
			free(fm->entries[i][j]);
		free(fm->entries[i]);
	}
}

static char *
fullpath(char *dir, char *file)
{
	char buf[PATH_MAX];

	(void)snprintf(buf, sizeof(buf), "%s/%s", dir, file);
	return estrdup(buf);
}

static char *
sizefmt(off_t size)
{
	int i;
	char buf[SIZE_BUFSIZE] = "0B";
	long long int number, fract;

	if (size <= 0)
		return estrdup("0B");
	for (i = 0; i < UNIT_LAST; i++)
		if (size < units[i + 1].n)
			break;
	if (i == UNIT_LAST)
		return estrdup("inf");
	fract = (i == 0) ? 0 : size % units[i].n;
	fract /= (i == 0) ? 1 : units[i - 1].n;
	fract = (10 * fract + 512) / 1024;
	number = size / units[i].n;
	if (number <= 0)
		return estrdup("0B");
	if (fract >= 10 || (fract >= 5 && number >= 100)) {
		number++;
		fract = 0;
	} else if (fract < 0) {
		fract = 0;
	}
	if (number == 0)
		return estrdup("0B");
	if (number >= 100)
		(void)snprintf(buf, sizeof(buf), "%lld%c", number, units[i].u);
	else
		(void)snprintf(buf, sizeof(buf), "%lld.%lld%c", number, fract, units[i].u);
	return estrdup(buf);
}

static char *
timefmt(time_t time)
{
	struct tm *tm;
	char buf[TIME_BUFSIZE];

	tm = localtime(&time);
	(void)strftime(buf, sizeof(buf), "%F %R", tm);
	return estrdup(buf);
}

static char *
modefmt(mode_t m)
{
	enum {
		MODE_TYPE = 0,
		MODE_RUSR = 1,
		MODE_WUSR = 2,
		MODE_XUSR = 3,
		MODE_RGRP = 4,
		MODE_WGRP = 5,
		MODE_XGRP = 6,
		MODE_ROTH = 7,
		MODE_WOTH = 8,
		MODE_XOTH = 9,
	};

	char buf[] = "----------";
	if (S_ISBLK(m))
		buf[MODE_TYPE] = 'b';
	else if (S_ISCHR(m))
		buf[MODE_TYPE] = 'c';
	else if (S_ISDIR(m))
		buf[MODE_TYPE] = 'd';
	else if (S_ISLNK(m))
		buf[MODE_TYPE] = 'l';
	else if (S_ISFIFO(m))
		buf[MODE_TYPE] = 'p';
	else if (S_ISSOCK(m))
		buf[MODE_TYPE] = 's';

	if (m & S_IRUSR)
		buf[MODE_RUSR] = 'r';
	if (m & S_IWUSR)
		buf[MODE_WUSR] = 'w';
	if ((m & S_IXUSR) && (m & S_ISUID))
		buf[MODE_XUSR] = 's';
	else if (m & S_ISUID)
		buf[MODE_XUSR] = 'S';
	else if (m & S_IXUSR)
		buf[MODE_XUSR] = 'x';

	if (m & S_IRGRP)
		buf[MODE_RGRP] = 'r';
	if (m & S_IWGRP)
		buf[MODE_WGRP] = 'w';
	if ((m & S_IXGRP) && (m & S_ISGID))
		buf[MODE_XGRP] = 's';
	else if (m & S_ISUID)
		buf[MODE_XGRP] = 'S';
	else if (m & S_IXGRP)
		buf[MODE_XGRP] = 'x';

	if (m & S_IROTH)
		buf[MODE_ROTH] = 'r';
	if (m & S_IWOTH)
		buf[MODE_WOTH] = 'w';
	if ((m & S_IXOTH) && (m & S_ISVTX))
		buf[MODE_XOTH] = 't';
	else if (m & S_ISUID)
		buf[MODE_XOTH] = 'T';
	else if (m & S_IXOTH)
		buf[MODE_XOTH] = 'x';

	return estrdup(buf);
}

static char *
ownerfmt(uid_t uid, gid_t gid)
{
	struct passwd *pw;
	struct group *gr;
	char buf[128];

	pw = NULL;
	if ((pw = getpwuid(uid)) == NULL)
		goto error;
	if ((gr = getgrgid(gid)) == NULL)
		goto error;
	(void)snprintf(buf, sizeof(buf), "%s:%s", pw->pw_name, gr->gr_name);
	return estrdup(buf);
error:
	return estrdup("");
}

static int
entrycmp(const void *ap, const void *bp)
{
	int aisdir, bisdir;
	char **a, **b;

	a = *(char ***)ap;
	b = *(char ***)bp;
	/* dotdot (parent directory) first */
	if (strcmp(a[STATE_NAME], "..") == 0)
		return -1;
	if (strcmp(b[STATE_NAME], "..") == 0)
		return 1;

	/* directories first */
	aisdir = a[STATE_MODE] != NULL && a[STATE_MODE][0] == 'd';
	bisdir = b[STATE_MODE] != NULL && b[STATE_MODE][0] == 'd';
	if (aisdir && !bisdir)
		return -1;
	if (bisdir && !aisdir)
		return 1;

	/* dotentries (hidden entries) first */
	if (a[STATE_NAME][0] == '.' && b[STATE_NAME][0] != '.')
		return -1;
	if (b[STATE_NAME][0] == '.' && a[STATE_NAME][0] != '.')
		return 1;
	return strcoll(a[STATE_NAME], b[STATE_NAME]);
}

static void
setthumbpath(struct FM *fm, char *orig, char *thumb)
{
	char buf[PATH_MAX];
	int i;

	snprintf(buf, PATH_MAX, "%s", orig);
	for (i = 0; buf[i] != '\0'; i++)
		if (buf[i] == '/')
			buf[i] = '%';
	snprintf(thumb, PATH_MAX, "%s/%s.ppm", fm->thumbnaildir, buf);
}

static int
thumbexit(struct FM *fm)
{
	int ret;

	ret = 0;
	etlock(&fm->thumblock);
	if (fm->thumbexit)
		ret = 1;
	etunlock(&fm->thumblock);
	return ret;
}

static pid_t
forkthumb(struct FM *fm, char *orig, char *thumb)
{
	pid_t pid;

	if ((pid = efork()) == 0) {     /* children */
		close(STDOUT_FILENO);
		close(STDIN_FILENO);
		eexec(fm->thumbnailer, orig, thumb);
		exit(EXIT_FAILURE);
	}
	return pid;
}

static int
timespeclt(struct timespec *tsp, struct timespec *usp)
{
	return (tsp->tv_sec == usp->tv_sec)
	     ? (tsp->tv_nsec < usp->tv_nsec)
	     : (tsp->tv_sec < usp->tv_sec);
}

static int
thumbexists(struct FM *fm, char *orig, char *mime)
{
	struct stat sb;
	struct timespec origt, mimet;
	pid_t pid;
	int status;

	if (stat(mime, &sb) == -1)
		goto forkthumbnailer;
	mimet = sb.st_mtim;
	if (stat(orig, &sb) == -1)
		goto forkthumbnailer;
	origt = sb.st_mtim;
	if (timespeclt(&origt, &mimet))
		return TRUE;
forkthumbnailer:
	pid = forkthumb(fm, orig, mime);
	if (waitpid(pid, &status, 0) == -1)
		return FALSE;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void *
thumbnailer(void *arg)
{
	struct FM *fm;
	int i;
	char path[PATH_MAX];

	fm = (struct FM *)arg;
	for (i = 0; i < fm->nentries; i++) {
		if (thumbexit(fm))
			break;
		if (fm->entries[i][STATE_PATH] == NULL)
			continue;
		if (strncmp(fm->entries[i][STATE_PATH], fm->thumbnaildir, fm->thumbnaildirlen) == 0)
			continue;
		setthumbpath(fm, fm->entries[i][STATE_PATH], path);
		if (thumbexists(fm, fm->entries[i][STATE_PATH], path)) {
			setthumbnail(fm->wid, path, i);
		}
	}
	pthread_exit(0);
}

static void
closethumbthread(struct FM *fm)
{
	if (fm->thumbnailer == NULL || fm->thumbnaildir == NULL)
		return;
	etlock(&fm->thumblock);
	fm->thumbexit = 1;
	etunlock(&fm->thumblock);
	etjoin(fm->thumbthread, NULL);
	etlock(&fm->thumblock);
	fm->thumbexit = 0;
	etunlock(&fm->thumblock);
}

static void
createthumbthread(struct FM *fm)
{
	if (fm->thumbnailer == NULL || fm->thumbnaildir == NULL)
		return;
	etcreate(&fm->thumbthread, thumbnailer, (void *)fm);
}

static void
diropen(struct FM *fm, const char *path, int savecwd)
{
	struct IconPattern *icon;
	struct Pattern *patt;
	struct dirent **array;
	struct stat sb;
	int flags, i, j;
	char *s;

	if (path != NULL && wchdir(path) == -1)
		return;
	egetcwd(fm->path, sizeof(fm->path));
	(void)savecwd;                  // XXX: delete-me
	freeentries(fm);
	fm->nentries = escandir(fm->path, &array, direntselect, NULL);
	if (fm->nentries > fm->capacity) {
		fm->foundicons = ereallocarray(fm->foundicons, fm->nentries, sizeof(*fm->foundicons));
		fm->entries = ereallocarray(fm->entries, fm->nentries, sizeof(*fm->entries));
		fm->capacity = fm->nentries;
	}
	for (i = 0; i < fm->nentries; i++) {
		fm->entries[i] = emalloc(sizeof(*fm->entries[i]) * STATE_LAST);
		fm->entries[i][STATE_NAME] = estrdup(array[i]->d_name);
		fm->entries[i][STATE_PATH] = fullpath(fm->path, array[i]->d_name);
		if (stat(array[i]->d_name, &sb) == -1) {
			warn("%s", fm->path);
			fm->entries[i][STATE_SIZE] = NULL;
			fm->entries[i][STATE_TIME] = NULL;
			fm->entries[i][STATE_MODE] = NULL;
			fm->entries[i][STATE_OWNER] = NULL;
		} else {
			fm->entries[i][STATE_SIZE] = sizefmt(sb.st_size);
			fm->entries[i][STATE_TIME] = timefmt(sb.st_mtim.tv_sec);
			fm->entries[i][STATE_MODE] = modefmt(sb.st_mode);
			fm->entries[i][STATE_OWNER] = ownerfmt(sb.st_uid, sb.st_gid);
		}
		fm->foundicons[i] = -1;
		free(array[i]);
	}
	free(array);
	qsort(fm->entries, fm->nentries, sizeof(*fm->entries), entrycmp);
	for (i = 0; i < fm->nentries; i++) {
		for (j = 0, icon = fm->icons; fm->foundicons[i] == -1 && icon != NULL; icon = icon->next, j++) {
			if (icon->mode != '\0' && (fm->entries[i][STATE_MODE] == NULL || fm->entries[i][STATE_MODE][0] != icon->mode))
				continue;
			for (patt = icon->patt; patt != NULL; patt = patt->next) {
				if (strchr(patt->s, '/') != NULL) {
					flags = FNM_CASEFOLD | FNM_PATHNAME;
					s = fm->entries[i][STATE_PATH];
				} else {
					flags = FNM_CASEFOLD;
					s = fm->entries[i][STATE_NAME];
				}
				if (s != NULL && !fnmatch(patt->s, s, flags)) {
					fm->foundicons[i] = j;
					break;
				}
			}
		}
	}
	if (strstr(fm->path, fm->home) == fm->path)
		snprintf(fm->here, PATH_MAX, "~%s", fm->path + fm->homelen);
	else
		snprintf(fm->here, PATH_MAX, "%s", fm->path);
}

static char **
parseicons(struct FM *fm, const char *s, size_t *nicons)
{
	struct IconPattern *icon, *picon;
	struct Pattern *patt, *ppatt;
	size_t i;
	char *str, *p, *q, *path;
	char *out, *in;
	char **tab;

	*nicons = 0;
	if (s == NULL)
		return NULL;
	str = estrdup(s);
	picon = icon = NULL;
	for (p = strtok_r(str, "\n", &out); p != NULL; p = strtok_r(NULL, "\n", &out)) {
		ppatt = patt = NULL;
		if (*p == '\0')
			continue;
		path = strchr(p, '=');
		if (path == NULL)
			continue;
		if (p == path)
			continue;
		if (path[1] == '\0')
			continue;
		*(path++) = '\0';
		icon = emalloc(sizeof(*icon));
		*icon = (struct IconPattern){
			.next = NULL,
			.patt = NULL,
			.path = estrdup(path),
			.mode = '\0',
		};
		if (p[0] != '\0' && p[1] == ':') {
			icon->mode = p[0];
			p += 2;
		}
		for (q = strtok_r(p, "|", &in); q != NULL; q = strtok_r(NULL, "|", &in)) {
			patt = emalloc(sizeof(*patt));
			*patt = (struct Pattern){
				.next = NULL,
				.s = estrdup(q),
			};
			if (ppatt != NULL)
				ppatt->next = patt;
			else
				icon->patt = patt;
			ppatt = patt;
		}
		if (picon != NULL)
			picon->next = icon;
		else
			fm->icons = icon;
		picon = icon;
		(*nicons)++;
	}
	if (*nicons == 0)
		return NULL;
	tab = ecalloc(*nicons, sizeof(*tab));
	for (i = 0, icon = fm->icons; icon != NULL; icon = icon->next, i++)
		tab[i] = icon->path;
	return tab;
}

static void
initthumbnailer(struct FM *fm)
{
	fm->thumbnailer = getenv(THUMBNAILER);
	if ((fm->thumbnaildir = getenv(THUMBNAILDIR)) != NULL)
		fm->thumbnaildirlen = strlen(fm->thumbnaildir);
	else
		fm->thumbnaildirlen = 0;
}

static void
fileopen(struct FM *fm, char *path)
{
	pid_t pid1, pid2;
	int fd;

	if ((pid1 = efork()) == 0) {
		if ((pid2 = efork()) == 0) {
			fd = open(DEV_NULL, O_RDWR);
			(void)dup2(fd, STDOUT_FILENO);
			(void)dup2(fd, STDIN_FILENO);
			if (fd > 2)
				(void)close(fd);
			eexec(fm->opener, LAST_ARG, path);
			exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
	}
	waitpid(pid1, NULL, 0);
}

int
main(int argc, char *argv[])
{
	struct FM fm;
	size_t nicons;
	int ch, index, state;
	int saveargc;
	char *geom, *name, *path, *home, *iconpatts;
	char **saveargv;
	char **icons;

	saveargv = argv;
	saveargc = argc;
	iconpatts = getenv(FILE_ICONS);
	home = getenv(HOME);
	geom = NULL;
	path = NULL;
	name = NULL;
	fm = (struct FM){
		.capacity = 0,
		.nentries = 0,
		.entries = NULL,
		.foundicons = NULL,
		.home = home,
		.homelen = ((home != NULL) ? strlen(home) : 0),

		.thumblock = PTHREAD_MUTEX_INITIALIZER,
		.thumbexit = 0,
		.thumbnailer = NULL,
		.thumbnaildir = NULL,
	};
	if ((fm.opener = getenv(OPENER)) == NULL)
		fm.opener = DEF_OPENER;
	while ((ch = getopt(argc, argv, "ag:n:")) != -1) {
		switch (ch) {
		case 'a':
			hide = 0;
			break;
		case 'g':
			geom = optarg;
			break;
		case 'n':
			name = optarg;
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
		path = *argv;
	icons = parseicons(&fm, iconpatts, &nicons);
	initthumbnailer(&fm);
	if ((fm.wid = initwidget(APPCLASS, name, geom, saveargc, saveargv)) == NULL)
		errx(EXIT_FAILURE, "could not initialize X widget");
	openicons(fm.wid, icons, nicons);
	free(icons);
	diropen(&fm, path, 1);
	setwidget(fm.wid, fm.here, fm.entries, fm.foundicons, fm.nentries);
	createthumbthread(&fm);
	mapwidget(fm.wid);
	while ((state = pollwidget(fm.wid, &index)) != WIDGET_CLOSE) {
		switch (state) {
		case WIDGET_OPEN:
			if (index < 0 || index >= fm.nentries)
				break;
			if (fm.entries[index][STATE_MODE][0] == 'd') {
				widgetcursor(fm.wid, CURSOR_WATCH);
				closethumbthread(&fm);
				diropen(&fm, fm.entries[index][STATE_PATH], 1);
				setwidget(fm.wid, fm.here, fm.entries, fm.foundicons, fm.nentries);
				createthumbthread(&fm);
				widgetcursor(fm.wid, CURSOR_NORMAL);
			} else if (fm.entries[index][STATE_MODE][0] == '-') {
				fileopen(&fm, fm.entries[index][STATE_PATH]);
			}
			break;
		}
	}
	closethumbthread(&fm);
	freeentries(&fm);
	free(fm.entries);
	free(fm.foundicons);
	closewidget(fm.wid);
	return EXIT_SUCCESS;
}
