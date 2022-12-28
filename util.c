#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int
between(int x, int a, int b)
{
	return a <= x && x <= b;
}

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
		err(1, "malloc");
	return p;
}

void *
ecalloc(size_t nmemb, size_t size)
{
	void *p;

	if ((p = calloc(nmemb, size)) == NULL)
		err(1, "calloc");
	return p;
}

char *
estrdup(const char *s)
{
	char *t;

	if ((t = strdup(s)) == NULL)
		err(1, "strdup");
	return t;
}

void *
ereallocarray(void *ptr, size_t nmemb, size_t size)
{
	void *p;

	if ((p = reallocarray(ptr, nmemb, size)) == NULL)
		err(1, "reallocarray");
	return p;
}

void
egetcwd(char *path, size_t size)
{
	if (getcwd(path, size) == NULL) {
		err(1, "getcwd");
	}
}

int
escandir(const char *dirname, struct dirent ***namelist, int (*select)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))
{
	int ret;

	if ((ret = scandir(dirname, namelist, select, compar)) == -1)
		err(1, "scandir");
	return ret;
}

pid_t
efork(void)
{
	pid_t pid;

	if ((pid = fork()) < 0)
		err(1, "fork");
	return pid;
}

void
eexec(const char *cmd, const char *arg1, const char *arg2)
{
	if (execlp(cmd, cmd, arg1, arg2, NULL) == -1) {
		err(1, "%s", cmd);
	}
}

void
etcreate(pthread_t *tid, void *(*thrfn)(void *), void *arg)
{
	int errn;

	if ((errn = pthread_create(tid, NULL, thrfn, arg)) != 0) {
		errno = errn;
		err(1, "could not create thread");
	}
}

void
etjoin(pthread_t tid, void **rval)
{
	int errn;

	if ((errn = pthread_join(tid, rval)) != 0) {
		errno = errn;
		err(1, "could not join with thread");
	}
}

void
etlock(pthread_mutex_t *mutex)
{
	int errn;

	if ((errn = pthread_mutex_lock(mutex)) != 0) {
		errno = errn;
		err(1, "could not lock mutex");
	}
}

void
etunlock(pthread_mutex_t *mutex)
{
	int errn;

	if ((errn = pthread_mutex_unlock(mutex)) != 0) {
		errno = errn;
		err(1, "could not unlock mutex");
	}
}
