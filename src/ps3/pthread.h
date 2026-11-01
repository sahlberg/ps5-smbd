/* Enough of pthread for names.c and wsd.c. The PS3 libc has the header and not the functions;
   ps3.c implements these on sysThreadCreate and sysMutex. */
#pragma once

typedef unsigned long long pthread_t;

typedef struct {
	volatile int state; /* 0 uninitialised, 1 in progress, 2 ready */
	int mutex;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0, 0}

int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg);
int pthread_detach(pthread_t thread);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
