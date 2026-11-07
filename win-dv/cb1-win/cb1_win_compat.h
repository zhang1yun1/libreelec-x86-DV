/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows (_WIN32) POSIX & EGL Compatibility Shim for CB1 / DVBridge
 */
#ifndef CB1_WIN_COMPAT_H
#define CB1_WIN_COMPAT_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__cplusplus)
#ifndef _Alignof
#define _Alignof __alignof
#endif
typedef SSIZE_T ssize_t;
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#define fileno _fileno
#define read   _read
#define write  _write
#define close  _close
#define unlink _unlink

/* Windows replacement for mkstemp(template_path) used in dvbridge_cache.c */
static inline int cb1_win_mkstemp(char *tmpl)
{
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0)
        return -1;
    int fd = -1;
    if (_sopen_s(&fd, tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                 _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
        return -1;
    return fd;
}
#define mkstemp cb1_win_mkstemp

/* Windows directory-relative open shim for cb1_l1l3_model.c */
static inline int cb1_win_open_in_dir(const char *dir_path, const char *name)
{
    char full_path[MAX_PATH];
    int n = snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, name);
    if (n <= 0 || n >= (int)sizeof(full_path))
        return -1;
    int fd = -1;
    if (_sopen_s(&fd, full_path, _O_RDONLY | _O_BINARY | _O_NOINHERIT,
                 _SH_DENYWR, _S_IREAD) != 0)
        return -1;
    return fd;
}
#endif /* _WIN32 */

#endif /* CB1_WIN_COMPAT_H */
