#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int
max(int x, int y)
{
	return x > y ? x : y;
}

int
min(int x, int y)
{
	return x < y ? x : y;
}

int
diff(int x, int y)
{
	return x > y ? x - y : y - x;
}

void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(EXIT_FAILURE, "malloc");
	return p;
}

void *
ecalloc(size_t nmemb, size_t size)
{
	void *p;

	if ((p = calloc(nmemb, size)) == NULL)
		err(EXIT_FAILURE, "calloc");
	return p;
}

char *
estrdup(const char *s)
{
	char *t;

	if ((t = strdup(s)) == NULL)
		err(EXIT_FAILURE, "strdup");
	return t;
}

void *
erealloc(void *ptr, size_t size)
{
	void *p;

	if ((p = realloc(ptr, size)) == NULL)
		err(EXIT_FAILURE, "realloc");
	return p;
}

void
egetcwd(char *path, size_t size)
{
	if (getcwd(path, size) == NULL) {
		err(EXIT_FAILURE, "getcwd");
	}
}

int
escandir(const char *dirname, struct dirent ***namelist, int (*select)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))
{
	int ret;

	if ((ret = scandir(dirname, namelist, select, compar)) == -1)
		err(EXIT_FAILURE, "scandir");
	return ret;
}

pid_t
efork(void)
{
	pid_t pid;

	if ((pid = fork()) < 0)
		err(EXIT_FAILURE, "fork");
	return pid;
}

void
eexec(char *const argv[])
{
	execvp(argv[0], argv);
	err(EXIT_FAILURE, "%s", argv[0]);
}

void
etcreate(pthread_t *tid, void *(*thrfn)(void *), void *arg)
{
	int errn;

	if ((errn = pthread_create(tid, NULL, thrfn, arg)) != 0) {
		errno = errn;
		err(EXIT_FAILURE, "could not create thread");
	}
}

void
etjoin(pthread_t tid, void **rval)
{
	int errn;

	if ((errn = pthread_join(tid, rval)) != 0) {
		errno = errn;
		err(EXIT_FAILURE, "could not join with thread");
	}
}

void
etlock(pthread_mutex_t *mutex)
{
	int errn;

	if ((errn = pthread_mutex_lock(mutex)) != 0) {
		errno = errn;
		err(EXIT_FAILURE, "could not lock mutex");
	}
}

void
etunlock(pthread_mutex_t *mutex)
{
	int errn;

	if ((errn = pthread_mutex_unlock(mutex)) != 0) {
		errno = errn;
		err(EXIT_FAILURE, "could not unlock mutex");
	}
}

int
ewaitpid(pid_t pid)
{
	int status;

	for (;;) {
		if (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR) {
				err(EXIT_FAILURE, "waitpid");
			}
		} else if (!WIFSTOPPED(status)) {
			return status;
		}
	}
}

void
eclose(int fd)
{
	while (close(fd) == -1) {
		if (errno != EINTR) {
			err(EXIT_FAILURE, "close");
		}
	}
}
