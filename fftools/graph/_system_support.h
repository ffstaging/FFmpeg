/*
 * Copyright (c) 2025 - softworkz
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef FFTOOLS_GRAPH_SYSTEM_SUPPORT_H
#define FFTOOLS_GRAPH_SYSTEM_SUPPORT_H

#include <signal.h>
#include <errno.h>
#include <stdlib.h>

/* Platform-specific includes */
#ifdef _WIN32
#include <windows.h>
#include "../../compat/w32pthreads.h"

/* Windows doesn't have these POSIX headers - provide minimal compatibility */
/* Visual Studio doesn't define pid_t, so define it ourselves */
////#ifndef _PID_T_DEFINED
////typedef int pid_t;
////#define _PID_T_DEFINED
////#endif

typedef unsigned int sigset_t;

/* Windows signal compatibility */
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGCHLD
#define SIGCHLD 0  /* Not used on Windows */
#endif
#define SIG_BLOCK 0
#define SIG_SETMASK 1

/* Windows sigaction compatibility */
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
};

/* Windows signal function stubs */
static inline int sigemptyset(sigset_t *set) { *set = 0; return 0; }
static inline int sigaddset(sigset_t *set, int sig) { *set |= (1 << sig); return 0; }
static inline int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) { 
    if (oldset) *oldset = 0; 
    return 0; 
}
static inline int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    if (oldact) {
        oldact->sa_handler = SIG_DFL;
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
    }
    return 0;
}

/* Windows process function stubs */
static inline pid_t fork(void) { 
    errno = ENOSYS; 
    return -1; 
}

/* Don't redefine execve if it already exists - use a wrapper name instead */
static inline int __execve_wrapper(const char *path, char *const argv[], char *const envp[]) {
    /* Use Windows system() as fallback - argv[2] should be the command */
    if (argv && argv[2]) {
        return system(argv[2]);
    }
    return -1;
}

static inline pid_t waitpid(pid_t pid, int *status, int options) {
    if (status) *status = 0;
    return pid;
}

#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#endif

/* Ensure environ is declared - but don't conflict with existing declarations */
#ifndef _WIN32
extern char **environ;
#endif

/* glibc compatibility: TEMP_FAILURE_RETRY macro */
#ifndef TEMP_FAILURE_RETRY
#ifdef _MSC_VER
/* Visual Studio compatible version - Windows doesn't have EINTR, so no retry needed */
#define TEMP_FAILURE_RETRY(expression) (expression)
#elif defined(__GNUC__)
/* GCC version with statement expressions */
#define TEMP_FAILURE_RETRY(expression) \
  (__extension__                       \
    ({ long int __result;              \
       do __result = (long int) (expression); \
       while (__result == -1L && errno == EINTR); \
       __result; }))
#else
/* Fallback for other compilers */
#define TEMP_FAILURE_RETRY(expression) (expression)
#endif
#endif

/* glibc compatibility: libc-lock macros */
#define __libc_lock_define_initialized(CLASS, NAME) \
  CLASS pthread_mutex_t NAME = PTHREAD_MUTEX_INITIALIZER

#define __libc_lock_lock(NAME) pthread_mutex_lock(&(NAME))
#define __libc_lock_unlock(NAME) pthread_mutex_unlock(&(NAME))
#define __libc_lock_init(NAME) pthread_mutex_init(&(NAME), NULL)

/* glibc compatibility: signal functions */
#define __sigemptyset(set) sigemptyset(set)
#define __sigaddset(set, sig) sigaddset(set, sig)
#define __sigprocmask(how, set, oldset) sigprocmask(how, set, oldset)
#define __sigaction(sig, act, oldact) sigaction(sig, act, oldact)

/* glibc compatibility: process functions */
#define __fork fork
#ifdef _WIN32
#define __execve __execve_wrapper
#define __environ _environ
#else
#define __execve execve
#define __environ environ
#endif
#define __waitpid waitpid

/* glibc compatibility: errno setting */
#define __set_errno(val) (errno = (val))

/* glibc compatibility: weak alias macro - simplified for Windows */
#ifdef _WIN32
#define weak_alias(name, aliasname) \
    int aliasname(const char *line) { return name(line); }
#else
#define weak_alias(name, aliasname) \
    extern __typeof (name) aliasname __attribute__ ((weak, alias (#name)))
#endif

/* Threading and cancellation support */
#ifndef _LIBC_REENTRANT
#define _LIBC_REENTRANT 1
#endif

/* Optional cleanup handler support - can be defined by user if needed */
#ifndef CLEANUP_HANDLER
#define CLEANUP_HANDLER
#endif

#ifndef CLEANUP_RESET
#define CLEANUP_RESET
#endif

/* Optional fork override - can be defined by user if needed */
#ifndef FORK
#define FORK __fork
#endif

/* Legacy wrappers for compatibility (keeping existing interface) */
typedef pthread_mutex_t simple_lock_t;
#define SIMPLE_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void simple_lock_lock(simple_lock_t *lock)   { pthread_mutex_lock(lock); }
static inline void simple_lock_unlock(simple_lock_t *lock) { pthread_mutex_unlock(lock); }
static inline void simple_lock_init(simple_lock_t *lock)   { pthread_mutex_init(lock, NULL); }

static inline int sigaction_wrap(int sig, const struct sigaction *act, struct sigaction *oact) {
    return sigaction(sig, act, oact);
}
static inline int sigprocmask_wrap(int how, const sigset_t *set, sigset_t *oldset) {
    return sigprocmask(how, set, oldset);
}
static inline pid_t waitpid_wrap(pid_t pid, int *status, int options) {
    return waitpid(pid, status, options);
}

#endif // FFTOOLS_GRAPH_SYSTEM_SUPPORT_H