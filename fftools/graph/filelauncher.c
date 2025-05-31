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

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <sys/time.h>
#  include <time.h>
#  include <errno.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include "_system.h"
#endif
#include "graphprint.h"
#include "libavutil/bprint.h"

int ff_open_html_in_browser(const char *html_path)
{
    if (!html_path || !*html_path)
        return -1;

#if defined(_WIN32)

    {
        HINSTANCE rc = ShellExecuteA(NULL, "open", html_path, NULL, NULL, SW_SHOWNORMAL);
        if ((UINT_PTR)rc <= 32) {
            // Fallback: system("start ...")
            char cmd[1024];
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "start \"\" \"%s\"", html_path);
            if (system(cmd) != 0)
                return -1;
        }
        return 0;
    }

#elif defined(__APPLE__)

    {
        av_log(NULL, AV_LOG_WARNING, "Browser launch not implemented...\n");
        return 0;
    }

#else

    // --- Linux / Unix-like -----------------------
    {
        static const char safe_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/-._";
        AVBPrint buf;

        // Due to the way how the temp path and file name are constructed, this check is not
        // actually required and just for illustration of which chars can even occur in the path.
        for (const char *p = html_path; *p; ++p) {
            if (strchr(safe_chars, (unsigned char)*p) == NULL) {
                av_log(NULL, AV_LOG_ERROR, "Invalid file path: '%s'.\n", html_path);
                return -1;
            }
        }

        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(&buf, "xdg-open '%s' </dev/null 1>/dev/null 2>&1 &", html_path);

        int ret = __libc_system(buf.str);

        if (ret != -1 && WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
            return 0;

        av_log(NULL, AV_LOG_WARNING, "Could not open '%s' in a browser.\n", html_path);
        return -1;
    }

#endif
}


int ff_get_temp_dir(char *buf, size_t size)
{
#if defined(_WIN32)

    // --- Windows ------------------------------------
    {
        // GetTempPathA returns length of the string (including trailing backslash).
        // If the return value is greater than buffer size, it's an error.
        DWORD len = GetTempPathA((DWORD)size, buf);
        if (len == 0 || len > size) {
            // Could not retrieve or buffer is too small
            return -1;
        }
        return 0;
    }

#else

    static const char *bases[] = { "/tmp", "/var/tmp", NULL };
    AVBPrint bp;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    uid_t uid = getuid();

    for (int i = 0; bases[i]; i++) {
        av_bprint_clear(&bp);
        av_bprintf(&bp, "%s/ffmpeg-%u", bases[i], uid);

        if (mkdir(bp.str, 0700) == -1 && errno != EEXIST)
            continue;

        av_bprint_chars(&bp, '/', 1);

        if (bp.len > size - 1)
            return -1;

        snprintf(buf, size, "%s", bp.str);
        return 0;
    }

    av_log(NULL, AV_LOG_ERROR, "Unable to determine temp directory.\n");
    av_bprint_clear(&bp);
    return -1;

#endif
}

int ff_make_timestamped_html_name(char *buf, size_t size)
{
#if defined(_WIN32)

    /*----------- Windows version -----------*/
    SYSTEMTIME st;
    GetLocalTime(&st);
    /*
      st.wYear, st.wMonth, st.wDay,
      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
    */
    int written = _snprintf_s(buf, size, _TRUNCATE,
                              "ffmpeg_graph_%04d-%02d-%02d_%02d-%02d-%02d_%03d.html",
                              st.wYear,
                              st.wMonth,
                              st.wDay,
                              st.wHour,
                              st.wMinute,
                              st.wSecond,
                              st.wMilliseconds);
    if (written < 0)
        return -1; /* Could not write into buffer */
    return 0;

#else

    /*----------- macOS / Linux / Unix version -----------*/
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return -1; /* gettimeofday failed */
    }

    struct tm local_tm;
    localtime_r(&tv.tv_sec, &local_tm);

    int ms = (int)(tv.tv_usec / 1000); /* convert microseconds to milliseconds */

    /*
       local_tm.tm_year is years since 1900,
       local_tm.tm_mon  is 0-based (0=Jan, 11=Dec)
    */
    int written = snprintf(buf, size,
                           "ffmpeg_graph_%04d-%02d-%02d_%02d-%02d-%02d_%03d.html",
                           local_tm.tm_year + 1900,
                           local_tm.tm_mon + 1,
                           local_tm.tm_mday,
                           local_tm.tm_hour,
                           local_tm.tm_min,
                           local_tm.tm_sec,
                           ms);
    if (written < 0 || (size_t)written >= size) {
        return -1; /* Buffer too small or formatting error */
    }
    return 0;

#endif
}
