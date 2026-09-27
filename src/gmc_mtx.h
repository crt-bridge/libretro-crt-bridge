/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gmc_mtx.h -- portable mutex, SHARED by gmclient.c and gmclient_input.c
 * (the receive thread owns both sockets, 2026-09-07).
 *
 * Extracted from gmclient.c rather than copied into gmclient_input.c. The
 * repo already carries an unguarded duplication debt (`struct frame_slot`
 * defined twice) and there is no reason to open a second one -- even though
 * macros do not risk the ABI divergence a duplicated struct would.
 *
 * Include AFTER <winsock2.h> on Windows: <windows.h> pulls in <winsock.h> if
 * winsock2 is not already there, and the two contradict each other. Both
 * files that include it do so after their own platform block.
 */
#ifndef GMC_MTX_H
#define GMC_MTX_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef CRITICAL_SECTION gmc_mutex;
#  define MTX_INIT(m)     InitializeCriticalSection(m)
#  define MTX_LOCK(m)     EnterCriticalSection(m)
#  define MTX_UNLOCK(m)   LeaveCriticalSection(m)
#  define MTX_DESTROY(m)  DeleteCriticalSection(m)
#else
#  include <pthread.h>
typedef pthread_mutex_t  gmc_mutex;
#  define MTX_INIT(m)     pthread_mutex_init(m, NULL)
#  define MTX_LOCK(m)     pthread_mutex_lock(m)
#  define MTX_UNLOCK(m)   pthread_mutex_unlock(m)
#  define MTX_DESTROY(m)  pthread_mutex_destroy(m)
#endif

#endif /* GMC_MTX_H */
