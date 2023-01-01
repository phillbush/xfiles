#include <sys/types.h>

#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#ifndef RET_ERROR
#define RET_ERROR (-1)
#endif
#ifndef RET_OK
#define RET_OK 0
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

pid_t efork(void);
int between(int x, int a, int b);
int max(int x, int y);
int min(int x, int y);
int diff(int x, int y);
int escandir(const char *dirname, struct dirent ***namelist, int (*select)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **));
void *emalloc(size_t size);
void *ecalloc(size_t nmemb, size_t size);
char *estrdup(const char *s);
void *ereallocarray(void *ptr, size_t nmemb, size_t size);
void egetcwd(char *path, size_t size);
void eexec(char *const argv[]);
void etcreate(pthread_t *tid, void *(*thrfn)(void *), void *arg);
void etjoin(pthread_t tid, void **rval);
void etlock(pthread_mutex_t *mutex);
void etunlock(pthread_mutex_t *mutex);
