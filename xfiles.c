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

/* actions for the controller command */
#define DROPCOPY         "drop-copy"
#define DROPMOVE         "drop-move"
#define DROPLINK         "drop-link"
#define DROPASK          "drop-ask"
#define MENU             "menu"

#define APPCLASS         "XFiles"
#define WINDOWID         "WINDOWID"
#define URI_PREFIX       "file://"
#define INCRSIZE         512
#define NCMDARGS         3
#define WINDOWID_BUFSIZE 16
#define DEF_OPENER       "xdg-open"
#define THUMBNAILDIR     "XFILES_THUMBNAILDIR"
#define CONTEXTCMD       "xfilesctl"
#define DEV_NULL         "/dev/null"
#define UNIT_LAST        7
#define SIZE_BUFSIZE     6       /* 4 digits + suffix char + nul */
#define TIME_BUFSIZE     128

enum {
	CONFIG_PATTERN,
	CONFIG_DATA,
	CONFIG_LAST,
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
	MODE_LINK     = 4,
	MODE_BUFSIZE  = 6,      /* 5 elems +1 for the nul byte */
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
	struct timespec time;   /* ctime of current directory */

	uid_t uid;
	gid_t gid;
	gid_t grps[NGROUPS_MAX];
	int ngrps;

	pthread_mutex_t thumblock;
	pthread_t thumbthread;
	int thumbexit;
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

extern char **thumbs[][CONFIG_LAST];
extern char **icons[][CONFIG_LAST];
extern size_t nicons;

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

	if (strcmp(dir, "/") == 0)
		dir = "";
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
modefmt(struct FM *fm, mode_t m, uid_t uid, gid_t gid, int islink)
{
	char buf[MODE_BUFSIZE] = "     ";
	int i, ismember;
	size_t perm_off;

	switch (m & S_IFMT) {
	case S_IFBLK:   buf[MODE_TYPE] = 'b'; break;
	case S_IFCHR:   buf[MODE_TYPE] = 'c'; break;
	case S_IFDIR:   buf[MODE_TYPE] = 'd'; break;
	case S_IFIFO:   buf[MODE_TYPE] = 'p'; break;
	case S_IFSOCK:  buf[MODE_TYPE] = 's'; break;
	default:        buf[MODE_TYPE] = 'f'; break;
	}

	for (i = 0, ismember = FALSE; i < fm->ngrps && !ismember; i++) {
		if (fm->grps[i] == gid) {
			ismember = TRUE;
		}
	}

	if (uid == fm->uid) perm_off = 0;
	else if (gid == fm->gid || ismember) perm_off = 3;
	else perm_off = 6;

	if (m & (S_IRUSR >> perm_off)) buf[MODE_READ]  = 'r';
	if (m & (S_IWUSR >> perm_off)) buf[MODE_WRITE] = 'w';
	if (m & (S_IXUSR >> perm_off)) buf[MODE_EXEC]  = 'x';

	if (islink)
		buf[MODE_LINK] = 'l';

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
forkthumb(char **cmd, char *orig, char *thumb)
{
	char *argv[NCMDARGS + 4]; /* +4 for "sh", orig, thumb, and NULL */
	pid_t pid;
	int i;

	for (i = 0; cmd[i] != NULL; i++)
		argv[i] = cmd[i];
	argv[i++] = "sh";
	argv[i++] = orig;
	argv[i++] = thumb;
	argv[i++] = NULL;
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
strchrs(const char *a, const char *b)
{
	size_t i;

	/* return nonzero iff a contains all characters in b */

	for (i = 0; b[i] != '\0'; i++)
		if (strchr(a, b[i]) == NULL)
			return FALSE;
	return TRUE;
}

static size_t
getmatchingdata(struct FM *fm, char **tab[][CONFIG_LAST], char **entry)
{
	size_t i, j;
	int flags;
	char **patts;
	char *p, *s;

	for (i = 0; tab[i][CONFIG_PATTERN] != NULL; i++) {
		patts = tab[i][CONFIG_PATTERN];
		if (entry[STATE_MODE] != NULL && !strchrs(entry[STATE_MODE], patts[0]))
			continue;
		if (patts[1] == NULL)
			return i;
		for (j = 1; patts[j] != NULL; j++) {
			p = patts[j];
			if (p[0] == '~' || strchr(p, '/') != NULL) {
				flags = FNM_PATHNAME;
				s = entry[STATE_PATH];
			} else {
				flags = 0;
				s = entry[STATE_NAME];
			}
			if (s == NULL)
				continue;
			if (p[0] == '~') {
				if (strncmp(fm->home, s, fm->homelen) != 0)
					continue;
				p++;
				s += fm->homelen;
			}
			if (s != NULL && fnmatch(p, s, flags) == 0) {
				return i;
			}
		}
	}
	return i;
}

static int
thumbexists(struct FM *fm, char **entry, char *mime)
{
	struct stat sb;
	struct timespec origt, mimet;
	pid_t pid;
	size_t n;
	int status;
	char **cmd;

	if (stat(mime, &sb) == -1)
		goto forkthumbnailer;
	mimet = sb.st_mtim;
	if (stat(entry[STATE_PATH], &sb) == -1)
		goto forkthumbnailer;
	origt = sb.st_mtim;
	if (timespeclt(&origt, &mimet))
		return TRUE;
forkthumbnailer:
	n = getmatchingdata(fm, thumbs, entry);
	cmd = thumbs[n][CONFIG_DATA];
	if (cmd == NULL)
		return FALSE;
	pid = forkthumb(cmd, entry[STATE_PATH], mime);
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
		if (thumbexists(fm, fm->entries[i], path)) {
			setthumbnail(fm->wid, path, i);
		}
	}
	pthread_exit(0);
}

static void
closethumbthread(struct FM *fm)
{
	if (fm->thumbnaildir == NULL)
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
	if (fm->thumbnaildir == NULL)
		return;
	etcreate(&fm->thumbthread, thumbnailer, (void *)fm);
}

static int
diropen(struct FM *fm, struct Cwd *cwd, const char *path)
{
	struct dirent **array;
	struct stat sb;
	int islink, i;
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
		islink = lstat(array[i]->d_name, &sb) != -1 && S_ISLNK(sb.st_mode);
		if (stat(array[i]->d_name, &sb) == -1) {
			warn("%s", cwd->path);
			fm->entries[i][STATE_SIZE] = NULL;
			fm->entries[i][STATE_TIME] = NULL;
			fm->entries[i][STATE_MODE] = modefmt(fm, 0x0, 0x0, 0x0, islink);
			fm->entries[i][STATE_OWNER] = NULL;
		} else {
			fm->entries[i][STATE_SIZE] = sizefmt(sb.st_size);
			fm->entries[i][STATE_TIME] = timefmt(sb.st_mtim.tv_sec);
			fm->entries[i][STATE_MODE] = modefmt(fm, sb.st_mode, sb.st_uid, sb.st_gid, islink);
			fm->entries[i][STATE_OWNER] = ownerfmt(sb.st_uid, sb.st_gid);
		}
		free(array[i]);
	}
	free(array);
	qsort(fm->entries, fm->nentries, sizeof(*fm->entries), entrycmp);
	for (i = 0; i < fm->nentries; i++)
		fm->foundicons[i] = getmatchingdata(fm, icons, fm->entries[i]);
	if (strstr(cwd->path, fm->home) == cwd->path &&
	    (cwd->path[fm->homelen] == '/' || cwd->path[fm->homelen] == '\0')) {
		snprintf(buf, PATH_MAX, "~%s", cwd->path + fm->homelen);
	} else {
		snprintf(buf, PATH_MAX, "%s", cwd->path);
	}
	cwd->here = estrdup(buf);
	return RET_OK;
}

static void
initthumbnailer(struct FM *fm)
{
	if ((fm->thumbnaildir = getenv(THUMBNAILDIR)) != NULL) {
		fm->thumbnaildirlen = strlen(fm->thumbnaildir);
	} else {
		fm->thumbnaildirlen = 0;
	}
}

static void
forkexec(char *const argv[], char *path, int doublefork)
{
	pid_t pid1, pid2;
	int fd;

	if ((pid1 = efork()) == 0) {
		if (!doublefork || (pid2 = efork()) == 0) {
			if (path != NULL && wchdir(path) == RET_ERROR)
				exit(EXIT_FAILURE);
			fd = open(DEV_NULL, O_RDWR);
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
		NULL,
		TRUE
	);
}

static void
runcontext(struct FM *fm, char *contextcmd, int nselitems)
{
	int i;
	char **argv;

	i = 0;
	argv = emalloc((nselitems + 3) * sizeof(*argv));
	argv[0] = contextcmd;
	argv[1] = MENU;
	for (i = 0; i < nselitems; i++)
		argv[i+2] = fm->entries[fm->selitems[i]][STATE_NAME];
	argv[i+2] = NULL;
	forkexec(argv, NULL, FALSE);
	free(argv);
}

static void
runindrop(struct FM *fm, char *contextcmd, WidgetEvent event, int nitems)
{
	int i;
	char **argv;
	char *path;

	/*
	 * Drag-and-drop in the same window.
	 *
	 * fm->selitems[0] is the path where the files have been dropped
	 * into.  The other items are the files being dropped.
	 */
	path = fm->entries[fm->selitems[0]][STATE_PATH];
	if ((argv = malloc((nitems + 2) * sizeof(*argv))) == NULL)
		return;
	argv[0] = contextcmd;
	switch (event) {
	case WIDGET_DROPCOPY: argv[1] = DROPCOPY; break;
	case WIDGET_DROPMOVE: argv[1] = DROPMOVE; break;
	case WIDGET_DROPLINK: argv[1] = DROPLINK; break;
	default:              argv[1] = DROPASK;  break;
	}
	for (i = 1; i < nitems; i++)
		argv[i + 1] = fm->entries[fm->selitems[i]][STATE_PATH];
	argv[nitems + 1] = NULL;
	forkexec(argv, path, FALSE);
}

static void
runexdrop(char *contextcmd, WidgetEvent event, char *text, char *path)
{
	size_t capacity, argc, i, j, k;
	char **argv, **p;

	/*
	 * Drag-and-drop between different windows.
	 *
	 * We get text containing a sequence of URIs.  Each URI has the
	 * format "file:///path/to/file" and ends in "\r\n".  We need to
	 * remove the paths from the URIs and fill them into argv.
	 */
	argc = 0;
	capacity = INCRSIZE;
	if ((argv = malloc(capacity * sizeof(*argv))) == NULL)
		return;
	argv[argc++] = contextcmd;
	switch (event) {
	case WIDGET_DROPCOPY: argv[argc++] = DROPCOPY; break;
	case WIDGET_DROPMOVE: argv[argc++] = DROPMOVE; break;
	case WIDGET_DROPLINK: argv[argc++] = DROPLINK; break;
	default:              argv[argc++] = DROPASK;  break;
	}
	for (i = 0; text[i] != '\0'; i++) {
		if (strncmp(text + i, URI_PREFIX, sizeof(URI_PREFIX) - 1) == 0)
			i += sizeof(URI_PREFIX) - 1;
		j = i;
		while (text[j] != '\0' && text[j] != '\n')
			j++;
		if (j > i && text[j-1] == '\r')
			k = j - 1;
		else
			k = j;
		text[k] = '\0';
		if (argc + 1> capacity) {
			capacity += INCRSIZE;
			p = realloc(argv, capacity * sizeof(*argv));
			if (p == NULL) {
				free(argv);
				return;
			}
			argv = p;
		}
		argv[argc++] = text + i;
		i = j;
	}
	argv[argc++] = NULL;
	forkexec(argv, path, FALSE);
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
changedir(struct FM *fm, const char *path, int force_refresh)
{
	Scroll *scrl;
	struct stat sb;
	int keepscroll, retval;
	struct Cwd cwd = {
		.prev = NULL,
		.next = NULL,
		.path = NULL,
		.here = NULL,
	};

	if (!force_refresh && fm->last != NULL && path == fm->last->path) {
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
	if (fm->cwd->path != NULL && strcmp(cwd.path, fm->cwd->path) == 0) {
		/*
		 * We're changing to the directory we currently are.
		 * Keep the scroll position we have now.
		 */
		keepscroll = TRUE;
	} else if (fm->cwd->prev != NULL && fm->cwd->prev->path != NULL &&
	           strcmp(cwd.path, fm->cwd->prev->path) == 0) {
		/*
		 * We're changing to the directory we were previously.
		 * Keep the scroll position we had there.
		 */
		fm->cwd = fm->cwd->prev;
		keepscroll = TRUE;
	} else {
		/*
		 * We're changing to a new directory.
		 * Scroll to the top.
		 */
		newcwd(fm);
		keepscroll = FALSE;
	}
	free(fm->cwd->path);
	free(fm->cwd->here);
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

static int
openicons(struct FM *fm)
{
	size_t i;
	char ***xpms;

	if ((xpms = calloc(nicons, sizeof(*xpms))) == NULL)
		goto error;
	for (i = 0; i < nicons; i++)
		xpms[i] = icons[i][CONFIG_DATA];
	if (widopenicons(fm->wid, xpms, nicons) == RET_ERROR)
		goto error;
	free(xpms);
	return RET_OK;
error:
	free(xpms);
	return RET_ERROR;

}

int
main(int argc, char *argv[])
{
	struct FM fm;
	struct Cwd *cwd;
	int ch, nitems;
	int saveargc;
	int exitval = EXIT_SUCCESS;
	char *geom = NULL;
	char *name = NULL;
	char *path = NULL;
	char *home = NULL;
	char *contextcmd = NULL;
	char **saveargv;
	char winid[WINDOWID_BUFSIZE];
	char *text;
	WidgetEvent event;

	saveargv = argv;
	saveargc = argc;
	contextcmd = CONTEXTCMD;
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
	initthumbnailer(&fm);
	if ((fm.wid = initwidget(APPCLASS, name, geom, saveargc, saveargv)) == NULL)
		errx(EXIT_FAILURE, "could not initialize X widget");
	snprintf(winid, WINDOWID_BUFSIZE, "%lu", widgetwinid(fm.wid));
	if (setenv(WINDOWID, winid, TRUE) == RET_ERROR) {
		warn("setenv");
		exitval = EXIT_FAILURE;
		goto error;
	}
	if (openicons(&fm) == RET_ERROR)
		goto error;
	if (diropen(&fm, fm.cwd, path) == RET_ERROR)
		goto error;
	fm.last = fm.cwd;
	if (setwidget(fm.wid, fm.cwd->here, fm.entries, fm.foundicons, fm.nentries, NULL) == RET_ERROR)
		goto error;
	createthumbthread(&fm);
	mapwidget(fm.wid);
	while ((event = pollwidget(fm.wid, fm.selitems, &nitems, &fm.cwd->state, &text)) != WIDGET_CLOSE) {
		switch (event) {
		case WIDGET_ERROR:
			exitval = EXIT_FAILURE;
			goto done;
			break;
		case WIDGET_CONTEXT:
			if (contextcmd == NULL)
				break;
			runcontext(&fm, contextcmd, nitems);
			if (changedir(&fm, fm.cwd->path, FALSE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_PREV:
		case WIDGET_NEXT:
			if (event == WIDGET_PREV)
				cwd = fm.cwd->prev;
			else
				cwd = fm.cwd->next;
			if (cwd == NULL)
				break;
			fm.cwd = cwd;
			if (changedir(&fm, cwd->path, FALSE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_TOGGLE_HIDE:
			hide = !hide;
			/* FALLTHROUGH */
		case WIDGET_REFRESH:
			if (changedir(&fm, fm.cwd->path, TRUE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_PARENT:
			if (fm.cwd->path[0] == '\0')    /* cwd is root */
				break;
			if (changedir(&fm, "..", FALSE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_OPEN:
			if (nitems < 1)
				break;
			if (fm.selitems[0] < 0 || fm.selitems[0] >= fm.nentries)
				break;
			if (fm.entries[fm.selitems[0]][STATE_MODE] == NULL)
				break;
			if (fm.entries[fm.selitems[0]][STATE_MODE][MODE_TYPE] == 'd') {
				if (changedir(&fm, fm.entries[fm.selitems[0]][STATE_PATH], FALSE) == RET_ERROR) {
					exitval = EXIT_FAILURE;
					goto done;
				}
			} else if (fm.entries[fm.selitems[0]][STATE_MODE][MODE_TYPE] == 'f') {
				fileopen(&fm, fm.entries[fm.selitems[0]][STATE_PATH]);
			}
			break;
		case WIDGET_DROPASK:
		case WIDGET_DROPCOPY:
		case WIDGET_DROPMOVE:
		case WIDGET_DROPLINK:
			if (contextcmd == NULL)
				break;
			if (text != NULL) {
				/* drag-and-drop between different windows */
				if (nitems > 0 && (fm.selitems[0] < 0 || fm.selitems[0] >= fm.nentries))
					path = NULL;
				else
					path = fm.entries[fm.selitems[0]][STATE_PATH];
				runexdrop(contextcmd, event, text, path);
				free(text);
			} else if (nitems > 1 && fm.entries[fm.selitems[0]][STATE_MODE][MODE_TYPE] == 'd') {
				/* drag-and-drop in the same window */
				runindrop(&fm, contextcmd, event, nitems);
			}
			if (changedir(&fm, fm.cwd->path, FALSE) == RET_ERROR) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		default:
			break;
		}
	}
done:
	closethumbthread(&fm);
error:
	clearcwd(fm.hist);
	freeentries(&fm);
	free(fm.entries);
	free(fm.foundicons);
	free(fm.selitems);
	closewidget(fm.wid);
	return exitval;
}
