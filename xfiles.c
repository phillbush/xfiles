#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <poll.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "widget.h"
#include "icons.h"

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
	char *patt, *name;
};

struct Cwd {
	struct Cwd *prev, *next;
	Scroll scrl;            /* scrolling position on this directory */
	char *path;             /* absolute path */
	char *here;             /* display path (~/foo rather than /home/user/foo) */
};

struct FM {
	Widget *widget;
	Item *entries;
	int widgetfd;           /* file descriptor for widget events */
	int *selitems;          /* array of indices to selected items */
	int capacity;           /* capacity of entries */
	int nentries;           /* number of entries */
	char *home;
	size_t homelen;
	struct Cwd *cwd;        /* pointer to current working directories */
	struct Cwd *hist;       /* list of working directories */
	struct Cwd *last;       /* last working directories */
	struct timespec time;   /* ctime of current directory */

	/* user-defined icon globbing patterns */
	struct IconPatt *userpatts;
	size_t nuserpatts;

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

	enum SortBy {
		SORTBY_NAME = 0,
		SORTBY_TIME,
		SORTBY_SIZE
	} sortby;
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
		free(fm->entries[i].name);
		free(fm->entries[i].fullname);
		free(fm->entries[i].status);
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
isdir(Item *entry)
{
	return (entry->mode & MODE_MASK) == MODE_DIR;
}

static int
entrycmp(const void *ap, const void *bp, void *fmp)
{
	int aisdir, bisdir;
	Item *a, *b;
	struct FM *fm;
	int retval = 0;

	a = (Item *)ap;
	b = (Item *)bp;
	fm = (struct FM *)fmp;
	/* dotdot (parent directory) first */
	if (strcmp(a->name, "..") == 0)
		return -1;
	if (strcmp(b->name, "..") == 0)
		return 1;

	/* directories first */
	aisdir = isdir(a);
	bisdir = isdir(b);
	if (aisdir && !bisdir)
		return -1;
	if (bisdir && !aisdir)
		return 1;

	/* dotentries (hidden entries) first */
	if (a->name[0] == '.' && b->name[0] != '.')
		return -1;
	if (b->name[0] == '.' && a->name[0] != '.')
		return 1;

	switch (fm->sortby) {
	case SORTBY_NAME:
		retval = strcoll(a->name, b->name);
		break;
	case SORTBY_TIME:
		retval = difftime(a->mtime.tv_sec, b->mtime.tv_sec);
		if (!retval)
			retval = a->mtime.tv_nsec - b->mtime.tv_nsec;
		break;
	case SORTBY_SIZE:
		retval = a->size - b->size;
		break;
	}

	return retval;
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

	if ((pid = efork()) == 0) {
		/* child */
		eclose(STDOUT_FILENO);
		eclose(STDIN_FILENO);
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

static bool
checkicon(struct FM *fm, Item *entry, struct IconPatt *icon)
{
	int flags;
	char *s, *t;

	t = icon->patt;
	if (t == NULL)
		return false;
	if (t[0] == '~' || strchr(t, '/') != NULL) {
		flags = FNM_CASEFOLD | FNM_PATHNAME;
		s = entry->fullname;
	} else {
		flags = FNM_CASEFOLD;
		s = entry->name;
	}
	if (s == NULL)
		return false;
	if (t[0] == '~') {
		if (strncmp(fm->home, s, fm->homelen) != 0)
			return false;
		t++;
		s += fm->homelen;
	}
	return s != NULL &&
		((icon->mode & MODE_MASK) == MODE_ANY ||
		(icon->mode & MODE_MASK) == (entry->mode & MODE_MASK)) &&
		(!(icon->mode & MODE_LINK) || (entry->mode & MODE_LINK)) &&
		(!(icon->mode & MODE_EXEC) || (entry->mode & MODE_EXEC)) &&
		(!(icon->mode & MODE_READ) || (entry->mode & MODE_READ)) &&
		(!(icon->mode & MODE_WRITE) || (entry->mode & MODE_WRITE)) &&
		fnmatch(t, s, flags) == 0;
}

static size_t
geticon(struct FM *fm, Item *entry)
{
	size_t i;

	/* parent directory is a special case */
	if (strcmp(entry->name, "..") == 0)
		return icon_for_updir;
	/* first check user-defined matches */
	for (i = 0; i < fm->nuserpatts; i++)
		if (checkicon(fm, entry, &fm->userpatts[i]))
			return fm->userpatts[i].index;
	/* then check hardcoded icon matches */
	for (i = 0; i < nicon_patts; i++)
		if (checkicon(fm, entry, &icon_patts[i]))
			return icon_patts[i].index;
	/* just return directory or regular file then */
	if (isdir(entry))
		return icon_for_dir;
	return icon_for_file;
}

static int
thumbexists(Item *entry, char *mime)
{
	struct stat sb;
	struct timespec origt, mimet;
	pid_t pid;
	int status;

	if (stat(mime, &sb) == -1)
		goto forkthumbnailer;
	mimet = sb.st_mtim;
	if (stat(entry->fullname, &sb) == -1)
		goto forkthumbnailer;
	origt = sb.st_mtim;
	if (timespeclt(&origt, &mimet))
		return true;
forkthumbnailer:
	pid = forkthumb(entry->fullname, mime);
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
		if (fm->entries[i].fullname == NULL)
			continue;
		if (strncmp(fm->entries[i].fullname, fm->thumbnaildir, fm->thumbnaildirlen) == 0)
			continue;
		if (setthumbpath(fm, fm->entries[i].fullname, path) == RETURN_FAILURE)
			continue;
		if (thumbexists(&fm->entries[i], path)) {
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

static unsigned char
filemode(struct FM *fm, struct stat *sb, char *name)
{
	bool ismember;
	struct stat lsb;
	size_t perm_off;
	unsigned char mask, type;
	int i;

	mask = 0x00;
	if (S_ISLNK(sb->st_mode)) {
		mask |= MODE_LINK;
		if (stat(name, &lsb) == -1) {
			type = MODE_BROK;
			goto done;
		}
		sb = &lsb;
	}
	switch (sb->st_mode & S_IFMT) {
	case S_IFBLK:   type = MODE_DEV;     break;
	case S_IFCHR:   type = MODE_DEV;     break;
	case S_IFDIR:   type = MODE_DIR;     break;
	case S_IFIFO:   type = MODE_FIFO;    break;
	case S_IFSOCK:  type = MODE_SOCK;    break;
	default:        type = MODE_FILE;    break;
	}
done:
	ismember = false;
	for (i = 0; i < fm->ngrps; i++) {
		if (fm->grps[i] == sb->st_gid) {
			ismember = true;
			break;
		}
	}
	if (sb->st_uid == fm->uid)
		perm_off = 0;
	else if (sb->st_gid == fm->gid || ismember)
		perm_off = 3;
	else
		perm_off = 6;
	if (sb->st_mode & (S_IRUSR >> perm_off))
		mask |= MODE_READ;
	if (sb->st_mode & (S_IWUSR >> perm_off))
		mask |= MODE_WRITE;
	if (sb->st_mode & (S_IXUSR >> perm_off))
		mask |= MODE_EXEC;
	return type | mask;
}

static int
diropen(struct FM *fm, struct Cwd *cwd, const char *path)
{
	struct dirent **array;
	struct stat sb;
	int i;
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
		if (lstat(array[i]->d_name, &sb) == -1) {
			warn("%s", array[i]->d_name);
			fm->entries[i].status = NULL;
			fm->entries[i].mode = 0;
			memset(&fm->entries[i].mtime, 0, sizeof(fm->entries[i].mtime));
			fm->entries[i].size = 0;
		} else {
			fm->entries[i].status = statusfmt(&sb);
			fm->entries[i].mode = filemode(fm, &sb, array[i]->d_name);
			fm->entries[i].mtime = sb.st_mtim;
			fm->entries[i].size = sb.st_size;
		}
		fm->entries[i].name = estrdup(array[i]->d_name);
		fm->entries[i].fullname = fullpath(cwd->path, array[i]->d_name);
		fm->entries[i].icon = geticon(fm, &fm->entries[i]);
		free(array[i]);
	}
	free(array);
	qsort_s(fm->entries, fm->nentries, sizeof(*fm->entries), entrycmp, fm);
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
	int mkdir_errno;
	bool done;
	char path[PATH_MAX];
	char *slash, *str;

	if ((str = getenv("CACHEDIR")) == NULL)
		if ((str = getenv("XDG_CACHE_HOME")) == NULL)
			return;
	len = strlen(str);
	if (PATH_MAX < len + 12)        /* strlen("/thumbnails") + '\0' */
		return;
	mode = 0700;
	dir_mode = 0755;
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
		if (mkdir(path, done ? mode : dir_mode) == -1) {
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
fileopen(struct FM *fm, char *path)
{
	pid_t pid;

	if ((pid = efork()) == 0) {
		if (efork() == 0) {
			eexec((char *[]){
				fm->opener,
				path,
				NULL,
			});
			exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
	}
	waitpid(pid, NULL, 0);
}

static WidgetEvent
runxfilesctl(struct FM *fm, char **argv, char *path)
{
	enum { FILE_WIDGET, FILE_CHILD };       /* indices for polling files */
	enum { END_READ, END_WRITE };           /* indices for pipe ends */
	struct pollfd pollfds[2];
	int pipefds[2];
	pid_t pid;
	WidgetEvent retval = WIDGET_NONE;
	extern char **environ;

	/*
	 * We use a self-pipe to wait for xfilesctl to terminate while
	 * we handle widget events (like responding to window resizing
	 * or to activation of the close button, etc).
	 *
	 * The self-pipe is handled by two processes (parent and child).
	 * The child spawns xfilesctl, waits for it to terminate, and
	 * then closes write end of the pipe.  The parent process polls
	 * both the widget socket and read end of the pipe.  When the
	 * child exits, it closes the write end, making the read end
	 * become widowed and deliver an EOF, at which point we know
	 * that xfilesctl has terminated.
	 *
	 * Thanks @emanuele6 for this trick!
	 */
	if (pipe2(pipefds, O_CLOEXEC) == RETURN_FAILURE)
		err(EXIT_FAILURE, "pipe2");
	if ((pid = efork()) == 0) {
		/* waiting child */
		eclose(pipefds[END_READ]);
		if (path != NULL)
			wchdir(path);
		if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0)
			err(EXIT_FAILURE, "posix_spawnp");
		(void)ewaitpid(pid);
		exit(EXIT_SUCCESS);
	}
	eclose(pipefds[END_WRITE]);
	pollfds[FILE_CHILD].fd = pipefds[END_READ];
	pollfds[FILE_WIDGET].fd = fm->widgetfd;
	pollfds[FILE_WIDGET].events = pollfds[FILE_CHILD].events = POLLIN;
	for (;;) {
		if (poll(pollfds, LEN(pollfds), -1) == -1) {
			if (errno == EINTR)
				continue;
			err(EXIT_FAILURE, "poll");
		}
		if (pollfds[FILE_WIDGET].revents & (POLLERR | POLLNVAL))
			errx(EXIT_FAILURE, "%d: bad fd", pollfds[FILE_WIDGET].fd);
		if (pollfds[FILE_CHILD].revents & (POLLERR | POLLNVAL))
			errx(EXIT_FAILURE, "%d: bad fd", pollfds[FILE_CHILD].fd);
		if (pollfds[FILE_WIDGET].revents & POLLHUP)
			pollfds[FILE_WIDGET].fd = -1;
		if (pollfds[FILE_CHILD].revents & POLLHUP) {
			(void)ewaitpid(pid);
			break;                  /* xfilesctl terminated */
		}
		if (pollfds[FILE_WIDGET].revents & POLLIN) {
			if ((retval = widget_wait(fm->widget)) == WIDGET_CLOSE) {
				break;          /* window closed */
			}
		}
	}
	eclose(pipefds[END_READ]);
	return retval;
}

static WidgetEvent
runcontext(struct FM *fm, char *cmd, int nselitems)
{
	int i;
	char **argv;
	WidgetEvent retval;

	argv = emalloc((nselitems + 3) * sizeof(*argv));
	argv[0] = CONTEXTCMD;
	argv[1] = cmd;
	for (i = 0; i < nselitems; i++)
		argv[i+2] = fm->entries[fm->selitems[i]].fullname;
	argv[i+2] = NULL;
	retval = runxfilesctl(fm, argv, NULL);
	free(argv);
	return retval;
}

static WidgetEvent
runindrop(struct FM *fm, WidgetEvent event, int nitems)
{
	int i;
	char **argv;
	char *path;
	WidgetEvent retval;

	/*
	 * Drag-and-drop in the same window.
	 *
	 * fm->selitems[0] is the path where the files have been dropped
	 * into.  The other items are the files being dropped.
	 */
	path = fm->entries[fm->selitems[0]].fullname;
	if ((argv = malloc((nitems + 2) * sizeof(*argv))) == NULL)
		return WIDGET_NONE;
	argv[0] = CONTEXTCMD;
	switch (event) {
	case WIDGET_DROPCOPY: argv[1] = DROPCOPY; break;
	case WIDGET_DROPMOVE: argv[1] = DROPMOVE; break;
	case WIDGET_DROPLINK: argv[1] = DROPLINK; break;
	default:              argv[1] = DROPASK;  break;
	}
	for (i = 1; i < nitems; i++)
		argv[i + 1] = fm->entries[fm->selitems[i]].fullname;
	argv[nitems + 1] = NULL;
	retval = runxfilesctl(fm, argv, path);
	free(argv);
	return retval;
}

static WidgetEvent
runexdrop(struct FM *fm, WidgetEvent event, char *text, char *path)
{
	size_t capacity, argc, i, j;
	char **argv, **p;
	WidgetEvent retval;

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
		return WIDGET_NONE;
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
		if (argc + 1> capacity) {
			capacity += INCRSIZE;
			p = realloc(argv, capacity * sizeof(*argv));
			if (p == NULL) {
				warn("realloc");
				free(argv);
				return WIDGET_NONE;
			}
			argv = p;
		}
		argv[argc++] = text + i;
		j = i + strcspn(text + i, "\r\n");
		if (text[j] == '\0')
			break;
		i = j;
		if (text[j] == '\r' && text[j + 1] == '\n')
			i++;
		text[j] = '\0';
	}
	argv[argc++] = NULL;
	retval = runxfilesctl(fm, argv, path);
	free(argv);
	return retval;
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
	scrl = keepscroll ? &fm->cwd->scrl : NULL;
	retval = widget_set(
		fm->widget,
		fm->cwd->path,
		fm->cwd->here,
		fm->entries,
		fm->nentries,
		scrl
	);
done:
	createthumbthread(fm);
	return retval;
}

static enum SortBy sortby_decode(const char *str)
{
	if (!str || !strcmp(str, "name"))
		return SORTBY_NAME;
	else if (!strcmp(str, "time"))
		return SORTBY_TIME;
	else if (!strcmp(str, "size"))
		return SORTBY_SIZE;

	return SORTBY_NAME;
}

static int
sortby(struct FM *fm, const char *sortby)
{
	int retval;

	fm->sortby = sortby_decode(sortby);

	qsort_s(fm->entries, fm->nentries, sizeof(*fm->entries), entrycmp, fm);

	retval = widget_set(
		fm->widget,
		fm->cwd->path,
		fm->cwd->here,
		fm->entries,
		fm->nentries,
		&fm->cwd->scrl
	);

	createthumbthread(fm);
	return retval;
}

static void
inituserpatts(struct FM *fm)
{
	size_t i, j, npatts;
	char *s, *t, *icon, *patt, *buf;
	unsigned char type, mask;

	fm->nuserpatts = 0;
	fm->userpatts = NULL;
	buf = NULL;
	if ((buf = widget_geticons(fm->widget)) == NULL)
		return;
	npatts = 1;
	for (s = buf; *s != '\0'; s++)
		if (*s == ':' || *s == '\n')
			npatts++;
	fm->userpatts = calloc(npatts, sizeof(*fm->userpatts));
	if (fm->userpatts == NULL) {
		warn("could not set file icons");
		goto error;
	}
	i = 0;
	for (s = strtok(buf, ":\n"); s != NULL; s = strtok(NULL, ":\n")) {
		while (*s == ' ' || *s == '\t')
			s++;
		type = MODE_FILE;
		mask = 0x00;
		icon = strchr(s, '=');
		patt = s;
		if (icon == NULL)
			continue;
		if (icon == patt)
			continue;
		for (t = icon - 1; t > patt; t--) {
			switch (*t) {
			case '-':
				type = MODE_ANY;
				break;
			case '/':
				type = MODE_DIR;
				break;
			case '|':
				type = MODE_FIFO;
				break;
			case '=':
				type = MODE_SOCK;
				break;
			case '#':
				type = MODE_DEV;
				break;
			case '^':
				type = MODE_BROK;
				break;
			case '@':
				mask |= MODE_LINK;
				break;
			case '!':
				mask |= MODE_EXEC;
				break;
			default:
				goto loopout;
			}
			*t = '\0';
		}
loopout:
		*icon = '\0';
		icon++;
		icon[strcspn(icon, " \t")] = '\0';
		for (j = 0; j < nicon_types; j++) {
			if (strcmp(icon, icon_types[j].name) != 0)
				continue;
			fm->userpatts[i].index = j;
			fm->userpatts[i].patt = strdup(patt);
			fm->userpatts[i].mode = type | mask;
			if (fm->userpatts[i].patt == NULL)
				warn("strdup");
			else
				i++;
			break;
		}
	}
	fm->nuserpatts = i;
	free(buf);
	return;
error:
	free(fm->userpatts);
	free(buf);
	fm->nuserpatts = 0;
	fm->userpatts = NULL;
	return;
}

static void
freefm(struct FM *fm)
{
	size_t i;

	clearcwd(fm->hist);
	freeentries(fm);
	free(fm->entries);
	free(fm->selitems);
	free(fm->thumbnaildir);
	for (i = 0; i < fm->nuserpatts; i++)
		free(fm->userpatts[i].patt);
	free(fm->userpatts);
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
		exit(EXIT_FAILURE);
	fm.widgetfd = widget_fd(fm.widget);
#if __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) == RETURN_FAILURE)
		err(EXIT_FAILURE, "pledge");
#endif
	inituserpatts(&fm);
	fm.sortby = sortby_decode(widget_get_sortby(fm.widget));
	if (diropen(&fm, fm.cwd, path) == RETURN_FAILURE)
		goto error;
	fm.last = fm.cwd;
	if (widget_set(fm.widget, fm.cwd->path, fm.cwd->here, fm.entries, fm.nentries, NULL) == RETURN_FAILURE)
		goto error;
	createthumbthread(&fm);
	widget_map(fm.widget);
	text = NULL;
	while ((event = widget_poll(fm.widget, fm.selitems, &nitems, &fm.cwd->scrl, &text)) != WIDGET_CLOSE) {
		if (event == WIDGET_GOTO && strcmp(text, "-") == 0)
			event = WIDGET_PREV;
		else if (event == WIDGET_GOTO && strcmp(text, "+") == 0)
			event = WIDGET_NEXT;
		switch (event) {
		case WIDGET_ERROR:
			exitval = EXIT_FAILURE;
			goto done;
			break;
		case WIDGET_CONTEXT:
			if (runcontext(&fm, MENU, nitems) == WIDGET_CLOSE)
				goto done;
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
		case WIDGET_OPEN:
			if (nitems < 1)
				break;
			if (fm.selitems[0] < 0 || fm.selitems[0] >= fm.nentries)
				break;
			if (isdir(&fm.entries[fm.selitems[0]])) {
				if (changedir(&fm, fm.entries[fm.selitems[0]].fullname, false) == RETURN_FAILURE) {
					exitval = EXIT_FAILURE;
					goto done;
				}
			} else {
				fileopen(&fm, fm.entries[fm.selitems[0]].fullname);
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
					path = fm.entries[fm.selitems[0]].fullname;
				if (runexdrop(&fm, event, text, path) == WIDGET_CLOSE) {
					goto done;
				}
			} else if (nitems > 1 && isdir(&fm.entries[fm.selitems[0]])) {
				/* drag-and-drop in the same window */
				if (runindrop(&fm, event, nitems) == WIDGET_CLOSE) {
					goto done;
				}
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
				if (runcontext(&fm, text, nitems) == WIDGET_CLOSE)
					goto done;
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
		case WIDGET_SORTBY:
			if (sortby(&fm, text) == RETURN_FAILURE) {
				exitval = EXIT_FAILURE;
				goto done;
			}
			break;
		default:
			break;
		}
		text = NULL;
	}
done:
	closethumbthread(&fm);
error:
	freefm(&fm);
	widget_free(fm.widget);
	return exitval;
}
