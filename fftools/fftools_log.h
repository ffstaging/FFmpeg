/*
 * Copyright (c) The FFmpeg developers
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

#ifndef FFTOOLS_FFTOOLS_LOG_H
#define FFTOOLS_FFTOOLS_LOG_H

#include <stdint.h>

#include "config.h"
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"


/**
 * Custom logging callback for fftools.
 */
void fftools_log_callback(void* ptr, int level, const char* fmt, va_list vl);

/**
 * Sets the logging callback function.
 */
void init_logging(void);


/**
 * Get the current log level
 *
 * @see lavu_log_constants
 *
 * @return Current log level
 */
int ff_log_get_level(void);

/**
 * Set the log level
 *
 * @see lavu_log_constants
 *
 * @param level Logging level
 */
void ff_log_set_level(int level);

void ff_log_set_flags(int arg);

int ff_log_get_flags(void);


/**
 * Skip repeated messages, this requires the user app to use av_log() instead of
 * (f)printf as the 2 would otherwise interfere and lead to
 * "Last message repeated x times" messages below (f)printf messages with some
 * bad luck.
 * Also to receive the last, "last repeated" line if any, the user app must
 * call av_log(NULL, AV_LOG_QUIET, "%s", ""); at the end
 */
#define FF_LOG_SKIP_REPEATED 1

/**
 * Include the log severity in messages originating from codecs.
 *
 * Results in messages such as:
 * [rawvideo @ 0xDEADBEEF] [error] encode did not produce valid pts
 */
#define FF_LOG_PRINT_LEVEL 2

/**
 * Include system time in log output.
 */
#define FF_LOG_PRINT_TIME 4

/**
 * Include system date and time in log output.
 */
#define FF_LOG_PRINT_DATETIME 8

/**
 * Print memory addresses instead of logical ids in the AVClass prefix.
 */
#define FF_LOG_PRINT_MEMADDRESSES 16


#endif /* FFTOOLS_FFTOOLS_LOG_H */
