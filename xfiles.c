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
#include "icons/file.xpm"
#include "icons/folder.xpm"

#define APPCLASS         "XFiles"
#define WINDOWID         "WINDOWID"
#define WINDOWID_BUFSIZE 16
#define ICONS            "XFILES_ICONS"
#define DEF_OPENER       "xdg-open"
#define THUMBNAILER      "XFILES_THUMBNAILER"
#define THUMBNAILDIR     "XFILES_THUMBNAILDIR"
#define CONTEXTCMD       "XFILES_CONTEXTCMD"
#define DEV_NULL         "/dev/null"
#define UNIT_LAST        7
#define SIZE_BUFSIZE     6       /* 4 digits + suffix char + nul */
#define TIME_BUFSIZE     128

enum {
	FILE_XPM,
	FOLDER_XPM,
	XPM_LAST
};

enum {
	STATE_NAME,     /* entry used by widget.c */
	STATE_PATH,     /* entry used by widget.c */
	STATE_SIZE,     /* entry used by widget.c */
	STATE_MODE,     /* entry used here just for matching icon spec */
	STATE_TIME,     /* not used rn, I'll either remove it or find some use */
	STATE_OWNER,    /* not used rn, I'll either remove it or find some use */
	STATE_LAST,
};

enum {
	MODE_TYPE     = 0,
	MODE_READ     = 1,
	MODE_WRITE    = 2,
	MODE_EXEC     = 3,
	MODE_BUFSIZE  = 5,      /* +1 for the nul byte */
};

struct IconPattern {
	struct IconPattern *next;
	struct Pattern {
		struct Pattern *next;
		char *s;
	} *patt;
	char *path;
	char *mode;
};

struct Cwd {
	struct Cwd *prev, *next;
	Scroll state;
	char *path;
	char *here;
};

struct FM {
	Widget wid;
	char ***entries;
	int *foundicons;        /* array of indices to found icons */
	int *selitems;          /* array of indices to selected items */
	int capacity;           /* capacity of entries */
	int nentries;           /* number of entries */
	char *home;
	size_t homelen;
	struct Cwd *cwd;        /* pointer to current working directories */
	struct Cwd *hist;       /* list of working directories */
	struct Cwd *last;       /* last working directories */
	struct IconPattern *icons;
	struct timespec time;   /* ctime of current directory */

	uid_t uid;
	gid_t gid;
	gid_t grps[NGROUPS_MAX];
	int ngrps;

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
	(void)fprintf(stderr, "usage: xfiles [-a] [-c cmd] [-g geometry] [-n name] [path]\n");
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
modefmt(struct FM *fm, mode_t m, uid_t uid, gid_t gid)
{
	char buf[MODE_BUFSIZE] = "    ";
	int i, ismember;

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
	else
		buf[MODE_TYPE] = '-';
	ismember = FALSE;
	for (i = 0; i < fm->ngrps; i++) {
		if (fm->grps[i] == gid) {
			ismember = TRUE;
			break;
		}
	}
	if (uid == fm->uid) {
		if (m & S_IRUSR) {
			buf[MODE_READ] = 'r';
		}
		if (m & S_IWUSR) {
			buf[MODE_WRITE] = 'w';
		}
		if (m & S_IXUSR) {
			buf[MODE_EXEC] = 'x';
		}
	} else if (gid == fm->gid || ismember) {
		if (m & S_IRGRP) {
			buf[MODE_READ] = 'r';
		}
		if (m & S_IWGRP) {
			buf[MODE_WRITE] = 'w';
		}
		if (m & S_IXGRP) {
			buf[MODE_EXEC] = 'x';
		}
	} else {
		if (m & S_IROTH) {
			buf[MODE_READ] = 'r';
		}
		if (m & S_IWOTH) {
			buf[MODE_WRITE] = 'w';
		}
		if (m & S_IXOTH) {
			buf[MODE_EXEC] = 'x';
		}
	}
	buf[MODE_BUFSIZE - 1] = '\0';
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
	aisdir = a[STATE_MODE] != NULL && a[STATE_MODE][MODE_TYPE] == 'd';
	bisdir = b[STATE_MODE] != NULL && b[STATE_MODE][MODE_TYPE] == 'd';
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
	char *argv[] = {
		fm->thumbnailer,
		orig,
		thumb,
		NULL,
	};
	pid_t pid;

	if ((pid = efork()) == 0) {     /* children */
		close(STDOUT_FILENO);
		close(STDIN_FILENO);
		eexec(argv);
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

static int
strchrs(const char *a, const char *b)
{
	size_t i;

	/* return nonzero iff a contains all characters in b */

	for (i = 0; b[i] != '\0'; i++)
		if (strchr(a, b[i]) == NULL)
			return FALSE;
	return TRUE;
}

static int
diropen(struct FM *fm, struct Cwd *cwd, const char *path)
{
	struct IconPattern *icon;
	struct Pattern *patt;
	struct dirent **array;
	struct stat sb;
	int flags, i, j;
	char *s;
	char buf[PATH_MAX];

	if (path != NULL && wchdir(path) == RET_ERROR)
		return RET_ERROR;
	free(cwd->path);
	free(cwd->here);
	egetcwd(buf, sizeof(buf));
	if (stat(buf, &sb) == -1)
		err(EXIT_FAILURE, "stat");
	cwd->path = estrdup(buf);
	fm->time = sb.st_ctim;
	freeentries(fm);
	fm->nentries = escandir(cwd->path, &array, direntselect, NULL);
	if (fm->nentries > fm->capacity) {
		fm->foundicons = erealloc(fm->foundicons, fm->nentries * sizeof(*fm->foundicons));
		fm->selitems = erealloc(fm->selitems, fm->nentries * sizeof(*fm->selitems));
		fm->entries = erealloc(fm->entries, fm->nentries * sizeof(*fm->entries));
		fm->capacity = fm->nentries;
	}
	for (i = 0; i < fm->nentries; i++) {
		fm->entries[i] = emalloc(sizeof(*fm->entries[i]) * STATE_LAST);
		fm->entries[i][STATE_NAME] = estrdup(array[i]->d_name);
		fm->entries[i][STATE_PATH] = fullpath(cwd->path, array[i]->d_name);
		if (stat(array[i]->d_name, &sb) == -1) {
			warn("%s", cwd->path);
			fm->entries[i][STATE_SIZE] = NULL;
			fm->entries[i][STATE_TIME] = NULL;
			fm->entries[i][STATE_MODE] = NULL;
			fm->entries[i][STATE_OWNER] = NULL;
		} else {
			fm->entries[i][STATE_SIZE] = sizefmt(sb.st_size);
			fm->entries[i][STATE_TIME] = timefmt(sb.st_mtim.tv_sec);
			fm->entries[i][STATE_MODE] = modefmt(fm, sb.st_mode, sb.st_uid, sb.st_gid);
			fm->entries[i][STATE_OWNER] = ownerfmt(sb.st_uid, sb.st_gid);
		}
		free(array[i]);
	}
	free(array);
	qsort(fm->entries, fm->nentries, sizeof(*fm->entries), entrycmp);
	for (i = 0; i < fm->nentries; i++) {
		if (fm->entries[i][STATE_MODE] != NULL && fm->entries[i][STATE_MODE][MODE_TYPE] == 'd')
			fm->foundicons[i] = ICON_PACK(FOLDER_XPM, FOLDER_XPM);
		else
			fm->foundicons[i] = ICON_PACK(FILE_XPM, FILE_XPM);
		for (j = 0, icon = fm->icons; icon != NULL; icon = icon->next, j++) {
			if (icon->mode != NULL && fm->entries[i][STATE_MODE] != NULL)
				if (!strchrs(fm->entries[i][STATE_MODE], icon->mode))
					continue;
			for (patt = icon->patt; patt != NULL; patt = patt->next) {
				if (strchr(patt->s, '/') != NULL) {
					flags = FNM_PATHNAME;
					s = fm->entries[i][STATE_PATH];
				} else {
					flags = 0;
					s = fm->entries[i][STATE_NAME];
				}
				if (s != NULL && !fnmatch(patt->s, s, flags)) {
					break;
				}
			}
			if (patt != NULL) {
				fm->foundicons[i] = ICON_PACK(XPM_LAST + j, fm->foundicons[i]);
				break;
			}
		}
	}
	if (strstr(cwd->path, fm->home) == cwd->path)
		snprintf(buf, PATH_MAX, "~%s", cwd->path + fm->homelen);
	else
		snprintf(buf, PATH_MAX, "%s", cwd->path);
	cwd->here = estrdup(buf);
	return RET_OK;
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
			.mode = NULL,
		};
		q = strchr(p, ':');
		if (q != NULL) {
			*q = '\0';
			icon->mode = estrdup(p);
			p = q + 1;
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
forkexec(char *const argv[], int doublefork)
{
	pid_t pid1, pid2;
	int fd;

	if ((pid1 = efork()) == 0) {
		if (!doublefork || (pid2 = efork()) == 0) {
			fd = open(DEV_NULL, O_RDWR);
			(void)dup2(fd, STDOUT_FILENO);
			(void)dup2(fd, STDIN_FILENO);
			if (fd > 2)
				(void)close(fd);
			eexec(argv);
			exit(EXIT_FAILURE);
		}
		if (doublefork) {
			exit(EXIT_SUCCESS);
		}
	}
	waitpid(pid1, NULL, 0);
}

static void
fileopen(struct FM *fm, char *path)
{
	forkexec(
		(char *[]){
			fm->opener,
			path,
			NULL,
		},
		TRUE
	);
}

static void
runcontext(struct FM *fm, char *contextcmd, int nselitems)
{
	int i;
	char **argv;

	argv = emalloc((nselitems + 2) * sizeof(*argv));
	argv[0] = contextcmd;
	for (i = 0; i < nselitems; i++)
		argv[i+1] = fm->entries[fm->selitems[i]][STATE_NAME];
	argv[i+1] = NULL;
	forkexec(argv, FALSE);
	free(argv);
}

static void
clearcwd(struct Cwd *cwd)
{
	struct Cwd *tmp;

	while (cwd != NULL) {
		tmp = cwd;
		cwd = cwd->next;
		free(tmp->path);
		free(tmp->here);
		free(tmp);
	}
}

static void
newcwd(struct FM *fm)
{
	struct Cwd *cwd;

	clearcwd(fm->cwd->next);
	cwd = emalloc(sizeof(*cwd));
	*cwd = (struct Cwd){
		.next = NULL,
		.prev = fm->cwd,
		.path = NULL,
		.here = NULL,
	};
	fm->cwd->next = cwd;
	fm->cwd = cwd;
}

static int
changedir(struct FM *fm, const char *path, int keepscroll)
{
	Scroll *scrl;
	struct stat sb;
	int retval;
	struct Cwd cwd = {
		.prev = NULL,
		.next = NULL,
		.path = NULL,
		.here = NULL,
	};

	if (fm->last != NULL && path == fm->last->path) {
		/*
		 * We're cd'ing to the place we are currently at; only
		 * continue if the directory's ctime has changed
		 */
		if (stat(path, &sb) == -1) {
			return RET_ERROR;
		}
		if (!timespeclt(&fm->time, &sb.st_ctim)) {
			return RET_OK;
		}
	}
	widgetcursor(fm->wid, CURSOR_WATCH);
	retval = RET_OK;
	closethumbthread(fm);
	if (diropen(fm, &cwd, path) == RET_ERROR)
		goto done;
	if (fm->cwd->prev != NULL && fm->cwd->prev->path != NULL
	    && strcmp(cwd.path, fm->cwd->prev->path) == 0) {
		fm->cwd = fm->cwd->prev;
		keepscroll = TRUE;
	} else {
		newcwd(fm);
	}
	fm->cwd->path = cwd.path;
	fm->cwd->here = cwd.here;
	fm->last = fm->cwd;
	scrl = keepscroll ? &fm->cwd->state : NULL;
	if (setwidget(fm->wid, fm->cwd->here, fm->entries, fm->foundicons, fm->nentries, scrl) == RET_ERROR) {
		retval = RET_ERROR;
		goto done;
	}
done:
	createthumbthread(fm);
	widgetcursor(fm->wid, CURSOR_NORMAL);
	return retval;
}

static void
freeicons(struct FM *fm)
{
	struct IconPattern *icon;
	struct Pattern *patt;

	while (fm->icons != NULL) {
		icon = fm->icons;
		fm->icons = fm->icons->next;
		while (icon->patt != NULL) {
			patt = icon->patt;
			icon->patt = icon->patt->next;
			free(patt->s);
			free(patt);
		}
		free(icon->path);
		free(icon->mode);
		free(icon);
	}
}

int
main(int argc, char *argv[])
{
	struct FM fm;
	struct Cwd *cwd;
	size_t nicons;
	int ch, nitems, state;
	int saveargc;
	int exitval = EXIT_SUCCESS;
	char *geom = NULL;
	char *name = NULL;
	char *path = NULL;
	char *home = NULL;
	char *iconpatts = NULL;
	char *contextcmd = NULL;
	char **saveargv;
	char **icons;
	char winid[WINDOWID_BUFSIZE];

	saveargv = argv;
	saveargc = argc;
	contextcmd = getenv(CONTEXTCMD);
	iconpatts = getenv(ICONS);
	home = getenv("HOME");
	fm = (struct FM){
		.capacity = 0,
		.nentries = 0,
		.cwd = emalloc(sizeof(*fm.cwd)),
		.last = NULL,
		.entries = NULL,
		.selitems = NULL,
		.foundicons = NULL,
		.home = home,
		.homelen = ((home != NULL) ? strlen(home) : 0),

		.uid = getuid(),
		.gid = getgid(),

		.thumblock = PTHREAD_MUTEX_INITIALIZER,
		.thumbexit = 0,
		.thumbnailer = NULL,
		.thumbnaildir = NULL,
	};
	(*fm.cwd) = (struct Cwd){
		.next = NULL,
		.prev = NULL,
		.path = NULL,
		.here = NULL,
	};
	fm.hist = fm.cwd;
	fm.ngrps = getgroups(NGROUPS_MAX, fm.grps);
	if ((fm.opener = getenv("OPENER")) == NULL)
		fm.opener = DEF_OPENER;
	while ((ch = getopt(argc, argv, "ac:g:n:")) != -1) {
		switch (ch) {
		case 'a':
			hide = 0;
			break;
		case 'c':
			contextcmd = optarg;
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
	snprintf(winid, WINDOWID_BUFSIZE, "%lu", widgetwinid(fm.wid));
	if (setenv(WINDOWID, winid, TRUE) == RET_ERROR) {
		warn("setenv");
		exitval = EXIT_FAILURE;
		goto error;
	}
	openicons(
		fm.wid,
		(char **[]){ file_xpm, folder_xpm },
		icons,
		XPM_LAST,
		nicons
	);
	free(icons);
	(void)diropen(&fm, fm.cwd, path);
	fm.last = fm.cwd;
	setwidget(fm.wid, fm.cwd->here, fm.entries, fm.foundicons, fm.nentries, NULL);
	createthumbthread(&fm);
	mapwidget(fm.wid);
	while ((state = pollwidget(fm.wid, fm.selitems, &nitems, &fm.cwd->state)) != WIDGET_CLOSE) {
		switch (state) {
		case WIDGET_ERROR:
			exitval = EXIT_FAILURE;
			goto done;
			break;
		case WIDGET_CONTEXT:
			if (contextcmd == NULL)
				break;
			runcontext(&fm, contextcmd, nitems);
			if (changedir(&fm, fm.cwd->path, TRUE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_PREV:
		case WIDGET_NEXT:
			if (state == WIDGET_PREV)
				cwd = fm.cwd->prev;
			else
				cwd = fm.cwd->next;
			if (cwd == NULL)
				break;
			fm.cwd = cwd;
			if (changedir(&fm, cwd->path, TRUE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_OPEN:
			if (nitems < 1)
				break;
			if (fm.selitems[0] < 0 || fm.selitems[0] >= fm.nentries)
				break;
			if (fm.entries[fm.selitems[0]][STATE_MODE][MODE_TYPE] == 'd') {
				if (changedir(&fm, fm.entries[fm.selitems[0]][STATE_PATH], FALSE) == RET_ERROR) {
					exitval = EXIT_FAILURE;
					goto done;
				}
			} else if (fm.entries[fm.selitems[0]][STATE_MODE][MODE_TYPE] == '-') {
				fileopen(&fm, fm.entries[fm.selitems[0]][STATE_PATH]);
			}
			break;
		}
	}
done:
	closethumbthread(&fm);
error:
	clearcwd(fm.hist);
	freeentries(&fm);
	freeicons(&fm);
	free(fm.entries);
	free(fm.foundicons);
	free(fm.selitems);
	closewidget(fm.wid);
	return exitval;
}
