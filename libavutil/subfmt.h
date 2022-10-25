/*
 * Copyright (c) 2021 softworkz
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

#ifndef AVUTIL_SUBFMT_H
#define AVUTIL_SUBFMT_H

#include <stdint.h>

#include "buffer.h"
#include "version.h"

enum AVSubtitleType {

    /**
     * Subtitle format unknown.
     */
    AV_SUBTITLE_FMT_NONE = -1,

    /**
     * Subtitle format unknown.
     */
    AV_SUBTITLE_FMT_UNKNOWN = 0,
#if FF_API_OLD_SUBTITLES
    SUBTITLE_NONE = 0,          ///< Deprecated, use AV_SUBTITLE_FMT_NONE instead.
#endif

    /**
     * Bitmap area in AVSubtitleRect.data, pixfmt AV_PIX_FMT_PAL8.
     */
    AV_SUBTITLE_FMT_BITMAP = 1,
#if FF_API_OLD_SUBTITLES
    SUBTITLE_BITMAP = 1,        ///< Deprecated, use AV_SUBTITLE_FMT_BITMAP instead.
#endif

    /**
     * Plain text in AVSubtitleRect.text.
     */
    AV_SUBTITLE_FMT_TEXT = 2,
#if FF_API_OLD_SUBTITLES
    SUBTITLE_TEXT = 2,          ///< Deprecated, use AV_SUBTITLE_FMT_TEXT instead.
#endif

    /**
     * Text Formatted as per ASS specification, contained AVSubtitleRect.ass.
     */
    AV_SUBTITLE_FMT_ASS = 3,
#if FF_API_OLD_SUBTITLES
    SUBTITLE_ASS = 3,           ///< Deprecated, use AV_SUBTITLE_FMT_ASS instead.
#endif

    AV_SUBTITLE_FMT_NB,         ///< number of subtitle formats, DO NOT USE THIS if you want to link with shared libav* because the number of formats might differ between versions.
};

typedef struct AVSubtitleArea {
#define AV_NUM_BUFFER_POINTERS 1

    enum AVSubtitleType type;
    int flags;

    int x;         ///< top left corner  of area.
    int y;         ///< top left corner  of area.
    int w;         ///< width            of area.
    int h;         ///< height           of area.
    int nb_colors; ///< number of colors in bitmap palette (@ref pal).

    /**
     * Buffers and line sizes for the bitmap of this subtitle.
     *
     * @{
     */
    AVBufferRef *buf[AV_NUM_BUFFER_POINTERS];
    int linesize[AV_NUM_BUFFER_POINTERS];
    /**
     * @}
     */

    uint32_t pal[256]; ///< RGBA palette for the bitmap.

    char *text;        ///< 0-terminated plain UTF-8 text
    char *ass;         ///< 0-terminated ASS/SSA compatible event line.

} AVSubtitleArea;

/**
 * Return the name of sub_fmt, or NULL if sub_fmt is not
 * recognized.
 */
const char *av_get_subtitle_fmt_name(enum AVSubtitleType sub_fmt);

/**
 * Return a subtitle format corresponding to name, or AV_SUBTITLE_FMT_NONE
 * on error.
 *
 * @param name Subtitle format name.
 */
enum AVSubtitleType av_get_subtitle_fmt(const char *name);

#endif /* AVUTIL_SUBFMT_H */
