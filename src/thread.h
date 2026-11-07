/* thread.h — primitives de threads minimalistes (Win32 / POSIX) */
#ifndef LIRE_THREAD_H
#define LIRE_THREAD_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>

typedef CRITICAL_SECTION lire_mutex;
typedef CONDITION_VARIABLE lire_cond;
typedef HANDLE lire_thread;

static inline void mutex_init(lire_mutex *m) { InitializeCriticalSectionAndSpinCount(m, 2000); }
static inline void mutex_free(lire_mutex *m) { DeleteCriticalSection(m); }
static inline void mutex_lock(lire_mutex *m) { EnterCriticalSection(m); }
static inline void mutex_unlock(lire_mutex *m) { LeaveCriticalSection(m); }
static inline void cond_init(lire_cond *c) { InitializeConditionVariable(c); }
static inline void cond_free(lire_cond *c) { (void)c; }
static inline void cond_wait(lire_cond *c, lire_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void cond_signal(lire_cond *c) { WakeConditionVariable(c); }
static inline void cond_broadcast(lire_cond *c) { WakeAllConditionVariable(c); }

typedef struct { void (*fn)(void *); void *arg; } lire_thread_start;
static unsigned __stdcall lire_thread_tramp(void *p)
{
	lire_thread_start s = *(lire_thread_start *)p;
	HeapFree(GetProcessHeap(), 0, p);
	s.fn(s.arg);
	return 0;
}
/* low_prio : les threads de rendu passent sous le thread d'interface */
static inline int thread_create(lire_thread *t, void (*fn)(void *), void *arg, int low_prio, size_t stack)
{
	lire_thread_start *s = HeapAlloc(GetProcessHeap(), 0, sizeof *s);
	s->fn = fn; s->arg = arg;
	*t = (HANDLE)_beginthreadex(NULL, (unsigned)stack, lire_thread_tramp, s, 0, NULL);
	if (!*t) return -1;
	if (low_prio) SetThreadPriority(*t, THREAD_PRIORITY_BELOW_NORMAL);
	return 0;
}
static inline void thread_join(lire_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static inline int cpu_count(void)
{
	DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	return n > 0 ? (int)n : 1;
}

#else /* POSIX */
#include <pthread.h>
#include <unistd.h>

typedef pthread_mutex_t lire_mutex;
typedef pthread_cond_t lire_cond;
typedef pthread_t lire_thread;

static inline void mutex_init(lire_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void mutex_free(lire_mutex *m) { pthread_mutex_destroy(m); }
static inline void mutex_lock(lire_mutex *m) { pthread_mutex_lock(m); }
static inline void mutex_unlock(lire_mutex *m) { pthread_mutex_unlock(m); }
static inline void cond_init(lire_cond *c) { pthread_cond_init(c, NULL); }
static inline void cond_free(lire_cond *c) { pthread_cond_destroy(c); }
static inline void cond_wait(lire_cond *c, lire_mutex *m) { pthread_cond_wait(c, m); }
static inline void cond_signal(lire_cond *c) { pthread_cond_signal(c); }
static inline void cond_broadcast(lire_cond *c) { pthread_cond_broadcast(c); }

typedef struct { void (*fn)(void *); void *arg; } lire_thread_start;
#include <stdlib.h>
static void *lire_thread_tramp(void *p)
{
	lire_thread_start s = *(lire_thread_start *)p;
	free(p);
	s.fn(s.arg);
	return NULL;
}
static inline int thread_create(lire_thread *t, void (*fn)(void *), void *arg, int low_prio, size_t stack)
{
	pthread_attr_t a;
	lire_thread_start *s = malloc(sizeof *s);
	(void)low_prio;
	s->fn = fn; s->arg = arg;
	pthread_attr_init(&a);
	if (stack) pthread_attr_setstacksize(&a, stack);
	int r = pthread_create(t, &a, lire_thread_tramp, s);
	pthread_attr_destroy(&a);
	return r;
}
static inline void thread_join(lire_thread t) { pthread_join(t, NULL); }
static inline int cpu_count(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int)n : 1;
}
#endif

#endif
