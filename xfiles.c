#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <fnmatch.h>
#include <err.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "widget.h"

/* actions for the controller command */
#define DROPCOPY        "drop-copy"
#define DROPMOVE        "drop-move"
#define DROPLINK        "drop-link"
#define DROPASK         "drop-ask"
#define MENU            "menu"

#define APPCLASS        "XFiles"
#define APPNAME         "xfiles"
#define MAX_RESOURCES   32      /* actually 31, the last must be NULL */
#define URI_PREFIX      "file://"
#define INCRSIZE        512
#define DEF_OPENER      "xdg-open"
#define CONTEXTCMD      "xfilesctl"
#define THUMBNAILERCMD  "xfilesthumb"
#define DEV_NULL        "/dev/null"
#define UNIT_LAST       7
#define STATUS_BUFSIZE  1024

struct FileType {
	char *patt, *type;
};

struct Cwd {
	struct Cwd *prev, *next;
	Scroll state;
	char *path;
	char *here;
};

struct FM {
	Widget *widget;
	char ***entries;
	int *selitems;          /* array of indices to selected items */
	int capacity;           /* capacity of entries */
	int nentries;           /* number of entries */
	char *home;
	size_t homelen;
	struct Cwd *cwd;        /* pointer to current working directories */
	struct Cwd *hist;       /* list of working directories */
	struct Cwd *last;       /* last working directories */
	struct timespec time;   /* ctime of current directory */

	char *filetypebuf;
	struct FileType *filetypes;
	size_t nfiletypes;

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

static void
usage(void)
{
	(void)fprintf(stderr, "usage: xfiles [-a] [-N name] [-X resources] [path]\n");
	exit(1);
}

static int
wchdir(const char *path)
{
	if (chdir(path) == -1) {
		warn("%s", path);
		return RETURN_FAILURE;
	}
	return RETURN_SUCCESS;
}

static int
direntselect(const struct dirent *dp)
{
	if (strcmp(dp->d_name, ".") == 0)
		return false;
	if (strcmp(dp->d_name, "..") == 0)
		return true;
	if (hide && dp->d_name[0] == '.')
		return false;
	return true;
}

static void
freeentries(struct FM *fm)
{
	int i;

	for (i = 0; i < fm->nentries; i++) {
		free(fm->entries[i][ITEM_NAME]);
		free(fm->entries[i][ITEM_PATH]);
		free(fm->entries[i][ITEM_STATUS]);
		free(fm->entries[i]);
	}
}

static char *
fullpath(char *dir, char *file, int isdir)
{
	char buf[PATH_MAX];

	if (strcmp(dir, "/") == 0)
		dir = "";
	(void)snprintf(buf, sizeof(buf), "%s/%s%c", dir, file, isdir ? '/' : '\0');
	return estrdup(buf);
}

static char *
statusfmt(struct stat *sb)
{
	int i;
	time_t time;
	long long int number, fract;
	struct passwd *pw = NULL;
	struct group *gr = NULL;
	struct tm tm;
	char *user = "?";
	char *group = "";
	char *sep = "";
	char timebuf[128];
	char buf[STATUS_BUFSIZE] = "???";

	number = 0;
	if (sb->st_size <= 0)
		goto done;
	for (i = 0; i < UNIT_LAST; i++)
		if (sb->st_size < units[i + 1].n)
			break;
	if (i == UNIT_LAST)
		goto done;
	fract = (i == 0) ? 0 : sb->st_size % units[i].n;
	fract /= (i == 0) ? 1 : units[i - 1].n;
	fract = (10 * fract + 512) / 1024;
	number = sb->st_size / units[i].n;
	if (number <= 0)
		goto done;
	if (fract >= 10 || (fract >= 5 && number >= 100)) {
		number++;
		fract = 0;
	} else if (fract < 0) {
		fract = 0;
	}
done:
	pw = getpwuid(sb->st_uid);
	gr = getgrgid(sb->st_gid);
	if (gr != NULL && gr->gr_name != NULL)
		group = gr->gr_name;
	if (pw != NULL && pw->pw_name != NULL) {
		user = pw->pw_name;
		if (strcmp(user, group) == 0) {
			group = "";
		} else {
			sep = ":";
		}
	}
	time = sb->st_mtim.tv_sec;
	(void)localtime_r(&time, &tm);
	(void)strftime(timebuf, sizeof(timebuf), "%F %R", &tm);
	if (number <= 0) {
		(void)snprintf(
			buf,
			sizeof(buf),
			"0B - %s%s%s - %s",
			user,
			sep,
			group,
			timebuf
		);
	} else if (number >= 100) {
		(void)snprintf(
			buf,
			sizeof(buf),
			"%lld%c - %s%s%s - %s",
			number,
			units[i].u,
			user,
			sep,
			group,
			timebuf
		);
	} else {
		(void)snprintf(
			buf,
			sizeof(buf),
			"%lld.%lld%c - %s%s%s - %s",
			number,
			fract,
			units[i].u,
			user,
			sep,
			group,
			timebuf
		);
	}
	return estrdup(buf);
}

static int
isdir(char **entry)
{
	char *s;

	s = strrchr(entry[ITEM_PATH], '/');
	return s != NULL && s[1] == '\0';
}

static int
entrycmp(const void *ap, const void *bp)
{
	int aisdir, bisdir;
	char **a, **b;

	a = *(char ***)ap;
	b = *(char ***)bp;
	/* dotdot (parent directory) first */
	if (strcmp(a[ITEM_NAME], "..") == 0)
		return -1;
	if (strcmp(b[ITEM_NAME], "..") == 0)
		return 1;

	/* directories first */
	aisdir = isdir(a);
	bisdir = isdir(b);
	if (aisdir && !bisdir)
		return -1;
	if (bisdir && !aisdir)
		return 1;

	/* dotentries (hidden entries) first */
	if (a[ITEM_NAME][0] == '.' && b[ITEM_NAME][0] != '.')
		return -1;
	if (b[ITEM_NAME][0] == '.' && a[ITEM_NAME][0] != '.')
		return 1;
	return strcoll(a[ITEM_NAME], b[ITEM_NAME]);
}

static int
setthumbpath(struct FM *fm, char *orig, char *thumb)
{
	char buf[PATH_MAX];
	int i;

	if (realpath(orig, buf) == NULL)
		return RETURN_FAILURE;
	for (i = 0; buf[i] != '\0'; i++)
		if (buf[i] == '/')
			buf[i] = '%';
	snprintf(thumb, PATH_MAX, "%s/%s.ppm", fm->thumbnaildir, buf);
	return RETURN_SUCCESS;
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
forkthumb(char *orig, char *thumb)
{
	pid_t pid;

	if ((pid = efork()) == 0) {     /* children */
		close(STDOUT_FILENO);
		close(STDIN_FILENO);
		eexec((char *[]){
			THUMBNAILERCMD,
			orig,
			thumb,
			NULL,
		});
		err(EXIT_FAILURE, "%s", THUMBNAILERCMD);
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
checkicon(struct FM *fm, char **entry, char *patt)
{
	int flags;
	char *s;

	if (patt[0] == '~' || strchr(patt, '/') != NULL) {
		flags = FNM_CASEFOLD | FNM_PATHNAME | FNM_LEADING_DIR;
		s = entry[ITEM_PATH];
	} else if (isdir(entry)) {
		return false;
	} else {
		flags = FNM_CASEFOLD;
		s = entry[ITEM_NAME];
	}
	if (s == NULL)
		return false;
	if (patt[0] == '~') {
		if (strncmp(fm->home, s, fm->homelen) != 0)
			return false;
		patt++;
		s += fm->homelen;
	}
	if (s != NULL && fnmatch(patt, s, flags) == 0)
		return true;
	return false;
}

static char *
geticon(struct FM *fm, char **entry)
{
	extern size_t defdirtype, defupdirtype;
	extern char *deffiletypes[];
	extern struct { char *patt; int type; } deffilepatts[];
	size_t i;
	char *patt;

	if (strcmp(entry[ITEM_NAME], "..") == 0)
		return deffiletypes[defupdirtype];
	for (i = 0; i < fm->nfiletypes; i++) {
		patt = fm->filetypes[i].patt;
		if (checkicon(fm, entry, patt)) {
			return fm->filetypes[i].type;
		}
	}
	for (i = 0; deffilepatts[i].patt != NULL; i++) {
		patt = deffilepatts[i].patt;
		if (checkicon(fm, entry, patt)) {
			return deffiletypes[deffilepatts[i].type];
		}
	}
	if (isdir(entry))
		return deffiletypes[defdirtype];
	return deffiletypes[deffilepatts[i].type];
}

static int
thumbexists(char **entry, char *mime)
{
	struct stat sb;
	struct timespec origt, mimet;
	pid_t pid;
	int status;

	if (stat(mime, &sb) == -1)
		goto forkthumbnailer;
	mimet = sb.st_mtim;
	if (stat(entry[ITEM_PATH], &sb) == -1)
		goto forkthumbnailer;
	origt = sb.st_mtim;
	if (timespeclt(&origt, &mimet))
		return true;
forkthumbnailer:
	pid = forkthumb(entry[ITEM_PATH], mime);
	if (waitpid(pid, &status, 0) == -1)
		return false;
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
		if (fm->entries[i][ITEM_PATH] == NULL)
			continue;
		if (strncmp(fm->entries[i][ITEM_PATH], fm->thumbnaildir, fm->thumbnaildirlen) == 0)
			continue;
		if (setthumbpath(fm, fm->entries[i][ITEM_PATH], path) == RETURN_FAILURE)
			continue;
		if (thumbexists(fm->entries[i], path)) {
			widget_thumb(fm->widget, path, i);
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
	int isdir, i;
	char buf[PATH_MAX];

	if (path != NULL && wchdir(path) == RETURN_FAILURE)
		return RETURN_FAILURE;
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
		fm->selitems = erealloc(fm->selitems, fm->nentries * sizeof(*fm->selitems));
		fm->entries = erealloc(fm->entries, fm->nentries * sizeof(*fm->entries));
		fm->capacity = fm->nentries;
	}
	for (i = 0; i < fm->nentries; i++) {
		fm->entries[i] = emalloc(sizeof(*fm->entries[i]) * ITEM_LAST);
		isdir = false;
		if (stat(array[i]->d_name, &sb) == -1) {
			warn("%s", array[i]->d_name);
			fm->entries[i][ITEM_STATUS] = NULL;
		} else {
			isdir = S_ISDIR(sb.st_mode);
			fm->entries[i][ITEM_STATUS] = statusfmt(&sb);
		}
		fm->entries[i][ITEM_NAME] = estrdup(array[i]->d_name);
		fm->entries[i][ITEM_PATH] = fullpath(cwd->path, array[i]->d_name, isdir);
		fm->entries[i][ITEM_TYPE] = geticon(fm, fm->entries[i]);
		free(array[i]);
	}
	free(array);
	qsort(fm->entries, fm->nentries, sizeof(*fm->entries), entrycmp);
	if (strstr(cwd->path, fm->home) == cwd->path &&
	    (cwd->path[fm->homelen] == '/' || cwd->path[fm->homelen] == '\0')) {
		snprintf(buf, PATH_MAX, "~%s", cwd->path + fm->homelen);
	} else {
		snprintf(buf, PATH_MAX, "%s", cwd->path);
	}
	cwd->here = estrdup(buf);
	return RETURN_SUCCESS;
}

static void
initthumbnailer(struct FM *fm)
{
	struct stat sb;
	mode_t mode, dir_mode;
	size_t len;
	int mkdir_errno, done;
	char path[PATH_MAX];
	char *slash, *str;

	if ((str = getenv("CACHEDIR")) == NULL)
		if ((str = getenv("XDG_CACHE_HOME")) == NULL)
			return;
	len = strlen(str);
	if (PATH_MAX < len + 12)        /* strlen("/thumbnails") + '\0' */
		return;
	mode = 0777 & ~umask(0);
	dir_mode = mode | S_IWUSR | S_IXUSR;
	(void)snprintf(path, PATH_MAX, "%s", str);
	slash = strrchr(path, '\0');
	while (--slash > path && *slash == '/')
		*slash = '\0';
	(void)snprintf(path + len, PATH_MAX - len, "/thumbnails");
	fm->thumbnaildir = estrdup(path);
	slash = path;
	for (;;) {
		slash += strspn(slash, "/");
		slash += strcspn(slash, "/");
		done = (*slash == '\0');
		*slash = '\0';
		if (mkdir(path, done ? mode : dir_mode) == 0) {
			if (mode > 0777 && chmod(path, mode) == -1) {
				warn("%s", fm->thumbnaildir);
				goto error;
			}
		} else {
			mkdir_errno = errno;
			if (stat(path, &sb) == -1) {
				errno = mkdir_errno;
				warn("%s", fm->thumbnaildir);
				goto error;
			}
			if (!S_ISDIR(sb.st_mode)) {
				errno = ENOTDIR;
				warn("%s", fm->thumbnaildir);
				goto error;
			}
		}
		if (done)
			break;
		*slash = '/';
	}
	fm->thumbnaildirlen = strlen(fm->thumbnaildir);
	return;
error:
	free(fm->thumbnaildir);
	fm->thumbnaildir = NULL;
	fm->thumbnaildirlen = 0;
	return;
}

static void
forkexec(char *const argv[], char *path, int doublefork)
{
	pid_t pid1, pid2;
	int fd;

	if ((pid1 = efork()) == 0) {
		if (!doublefork || (pid2 = efork()) == 0) {
			if (path != NULL && wchdir(path) == RETURN_FAILURE)
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
		true
	);
}

static void
runcontext(struct FM *fm, char *cmd, int nselitems)
{
	int i;
	char **argv;

	i = 0;
	argv = emalloc((nselitems + 3) * sizeof(*argv));
	argv[0] = CONTEXTCMD;
	argv[1] = cmd;
	for (i = 0; i < nselitems; i++)
		argv[i+2] = fm->entries[fm->selitems[i]][ITEM_PATH];
	argv[i+2] = NULL;
	forkexec(argv, NULL, false);
	free(argv);
}

static void
runindrop(struct FM *fm, WidgetEvent event, int nitems)
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
	path = fm->entries[fm->selitems[0]][ITEM_PATH];
	if ((argv = malloc((nitems + 2) * sizeof(*argv))) == NULL)
		return;
	argv[0] = CONTEXTCMD;
	switch (event) {
	case WIDGET_DROPCOPY: argv[1] = DROPCOPY; break;
	case WIDGET_DROPMOVE: argv[1] = DROPMOVE; break;
	case WIDGET_DROPLINK: argv[1] = DROPLINK; break;
	default:              argv[1] = DROPASK;  break;
	}
	for (i = 1; i < nitems; i++)
		argv[i + 1] = fm->entries[fm->selitems[i]][ITEM_PATH];
	argv[nitems + 1] = NULL;
	forkexec(argv, path, false);
}

static void
runexdrop(WidgetEvent event, char *text, char *path)
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
	argv[argc++] = CONTEXTCMD;
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
	forkexec(argv, path, false);
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
			return RETURN_FAILURE;
		}
		if (!timespeclt(&fm->time, &sb.st_ctim)) {
			return RETURN_SUCCESS;
		}
	}
	widget_busy(fm->widget);
	retval = RETURN_SUCCESS;
	closethumbthread(fm);
	if (diropen(fm, &cwd, path) == RETURN_FAILURE)
		goto done;
	if (fm->cwd->path != NULL && strcmp(cwd.path, fm->cwd->path) == 0) {
		/*
		 * We're changing to the directory we currently are.
		 * Keep the scroll position we have now.
		 */
		keepscroll = true;
	} else if (fm->cwd->prev != NULL && fm->cwd->prev->path != NULL &&
	           strcmp(cwd.path, fm->cwd->prev->path) == 0) {
		/*
		 * We're changing to the directory we were previously.
		 * Keep the scroll position we had there.
		 */
		fm->cwd = fm->cwd->prev;
		keepscroll = true;
	} else {
		/*
		 * We're changing to a new directory.
		 * Scroll to the top.
		 */
		newcwd(fm);
		keepscroll = false;
	}
	free(fm->cwd->path);
	free(fm->cwd->here);
	fm->cwd->path = cwd.path;
	fm->cwd->here = cwd.here;
	fm->last = fm->cwd;
	scrl = keepscroll ? &fm->cwd->state : NULL;
	if (widget_set(fm->widget, fm->cwd->here, fm->entries, fm->nentries, scrl) == RETURN_FAILURE)
		retval = RETURN_FAILURE;
done:
	createthumbthread(fm);
	return retval;
}

static void
initfiletypes(struct FM *fm)
{
	size_t i, ntypes;
	char *s, *t;

	fm->nfiletypes = 0;
	fm->filetypes = NULL;
	fm->filetypebuf = NULL;
	if ((fm->filetypebuf = widget_gettypes(fm->widget)) == NULL)
		return;
	ntypes = 1;
	for (s = fm->filetypebuf; *s != '\0'; s++)
		if (*s == ':' || *s == '\n')
			ntypes++;
	fm->filetypes = calloc(ntypes, sizeof(*fm->filetypes));
	if (fm->filetypes == NULL) {
		warn("could not set file types");
		goto error;
	}
	i = 0;
	for (s = fm->filetypebuf; *s != '\0'; s++) {
		t = NULL;
		while (*s == ' ' || *s == '\t')
			s++;
		fm->filetypes[i].patt = s;
		while (*s != '\0' && *s != ':' && *s != '\n') {
			if (*s == '=')
				t = s;
			s++;
		}
		*s = '\0';
		if (t == NULL || t + 1 == s)
			continue;
		*t = '\0';
		t++;
		fm->filetypes[i].type = t;
		t[strcspn(t, " \t")] = '\0';
		i++;
	}
	fm->nfiletypes = i;
	return;
error:
	free(fm->filetypes);
	free(fm->filetypebuf);
	fm->filetypes = NULL;
	fm->filetypebuf = NULL;
	fm->nfiletypes = 0;
	return;
}

static void
freefm(struct FM *fm)
{
	clearcwd(fm->hist);
	freeentries(fm);
	free(fm->entries);
	free(fm->selitems);
	free(fm->thumbnaildir);
	free(fm->filetypebuf);
	free(fm->filetypes);
}

int
main(int argc, char *argv[])
{
	struct FM fm;
	struct Cwd *cwd;
	int ch, nitems;
	int saveargc, force_refresh;
	int nresources = 0;
	int exitval = EXIT_SUCCESS;
	const char *resources[MAX_RESOURCES];
	char *name;
	char *path = NULL;
	char *home = NULL;
	char **saveargv;
	char *text;
	WidgetEvent event;

	saveargv = argv;
	saveargc = argc;
	home = getenv("HOME");
	name = getenv("RESOURCES_NAME");
	if (argv[0] != NULL && argv[0][0] != '\0') {
		if ((name = strrchr(argv[0], '/')) != NULL) {
			name++;
		} else {
			name = argv[0];
		}
	}
	if (name == NULL)
		name = APPNAME;
	resources[0] = getenv("RESOURCES_DATA");
	if (resources[0] != NULL)
		nresources++;
	fm = (struct FM){
		.cwd = emalloc(sizeof(*fm.cwd)),
		.home = home,
		.homelen = ((home != NULL) ? strlen(home) : 0),
		.uid = getuid(),
		.gid = getgid(),
		.thumblock = PTHREAD_MUTEX_INITIALIZER,
	};
	(*fm.cwd) = (struct Cwd){ 0 };
	fm.hist = fm.cwd;
	fm.ngrps = getgroups(NGROUPS_MAX, fm.grps);
	if ((fm.opener = getenv("OPENER")) == NULL)
		fm.opener = DEF_OPENER;
	while ((ch = getopt(argc, argv, "aN:X:")) != -1) {
		switch (ch) {
		case 'a':
			hide = 0;
			break;
		case 'N':
			name = optarg;
			break;
		case 'X':
			if (nresources < MAX_RESOURCES - 1)
				resources[nresources++] = optarg;
			break;
		default:
			usage();
			break;
		}
	}
	resources[nresources] = NULL;
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	else if (argc == 1)
		path = *argv;
	initthumbnailer(&fm);
	if ((fm.widget = widget_create(APPCLASS, name, saveargc, saveargv, resources)) == NULL)
		errx(EXIT_FAILURE, "could not initialize X widget");
#if __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) == RETURN_FAILURE)
		err(EXIT_FAILURE, "pledge");
#endif
	initfiletypes(&fm);
	if (diropen(&fm, fm.cwd, path) == RETURN_FAILURE)
		goto error;
	fm.last = fm.cwd;
	if (widget_set(fm.widget, fm.cwd->here, fm.entries, fm.nentries, NULL) == RETURN_FAILURE)
		goto error;
	createthumbthread(&fm);
	widget_map(fm.widget);
	text = NULL;
	while ((event = widget_poll(fm.widget, fm.selitems, &nitems, &fm.cwd->state, &text)) != WIDGET_CLOSE) {
		switch (event) {
		case WIDGET_ERROR:
			exitval = EXIT_FAILURE;
			goto done;
			break;
		case WIDGET_CONTEXT:
			runcontext(&fm, MENU, nitems);
			if (changedir(&fm, fm.cwd->path, false) == RETURN_FAILURE) {
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
			if (changedir(&fm, cwd->path, false) == RETURN_FAILURE) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_REFRESH:
			if (changedir(&fm, fm.cwd->path, true) == RETURN_FAILURE) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_OPEN:
			if (nitems < 1)
				break;
			if (fm.selitems[0] < 0 || fm.selitems[0] >= fm.nentries)
				break;
			if (isdir(fm.entries[fm.selitems[0]])) {
				if (changedir(&fm, fm.entries[fm.selitems[0]][ITEM_PATH], false) == RETURN_FAILURE) {
					exitval = EXIT_FAILURE;
					goto done;
				}
			} else {
				fileopen(&fm, fm.entries[fm.selitems[0]][ITEM_PATH]);
			}
			break;
		case WIDGET_DROPASK:
		case WIDGET_DROPCOPY:
		case WIDGET_DROPMOVE:
		case WIDGET_DROPLINK:
			if (text != NULL) {
				/* drag-and-drop between different windows */
				if (nitems > 0 && (fm.selitems[0] < 0 || fm.selitems[0] >= fm.nentries))
					path = NULL;
				else
					path = fm.entries[fm.selitems[0]][ITEM_PATH];
				runexdrop(event, text, path);
			} else if (nitems > 1 && isdir(fm.entries[fm.selitems[0]])) {
				/* drag-and-drop in the same window */
				runindrop(&fm, event, nitems);
			}
			if (changedir(&fm, fm.cwd->path, false) == RETURN_FAILURE) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_KEYPRESS:
			if (strcmp(text, "^period") == 0) {
				hide = !hide;
				force_refresh = true;
			} else {
				runcontext(&fm, text, nitems);
				force_refresh = false;
			}
			if (changedir(&fm, fm.cwd->path, force_refresh) == RETURN_FAILURE) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		case WIDGET_GOTO:
			if (changedir(&fm, text, true) == RETURN_FAILURE) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		default:
			break;
		}
		free(text);
		text = NULL;
	}
done:
	free(text);
	closethumbthread(&fm);
error:
	freefm(&fm);
	widget_free(fm.widget);
	return exitval;
}
