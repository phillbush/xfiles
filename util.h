#include <sys/types.h>

#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#define LEN(a)          (sizeof(a) / sizeof((a)[0]))
#define FLAG(f, b)      (((f) & (b)) == (b))
#define RETURN_FAILURE  (-1)
#define RETURN_SUCCESS  0

pid_t efork(void);
int max(int x, int y);
int min(int x, int y);
int diff(int x, int y);
int between(int x, int y, int x0, int y0, int w0, int h0);
int escandir(const char *dirname, struct dirent ***namelist, int (*select)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **));
void *emalloc(size_t size);
void *ecalloc(size_t nmemb, size_t size);
char *estrdup(const char *s);
void *erealloc(void *ptr, size_t size);
void egetcwd(char *path, size_t size);
void eexec(char *const argv[]);
void etcreate(pthread_t *tid, void *(*thrfn)(void *), void *arg);
void etjoin(pthread_t tid, void **rval);
void etlock(pthread_mutex_t *mutex);
void etunlock(pthread_mutex_t *mutex);
int ewaitpid(pid_t pid);
void eclose(int fd);
