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

#include <string.h>
#include "common.h"
#include "subfmt.h"

static const char sub_fmt_info[AV_SUBTITLE_FMT_NB][24] = {
    [AV_SUBTITLE_FMT_UNKNOWN] = "Unknown subtitle format",
    [AV_SUBTITLE_FMT_BITMAP]  = "Graphical subtitles",
    [AV_SUBTITLE_FMT_TEXT]    = "Text subtitles (plain)",
    [AV_SUBTITLE_FMT_ASS]     = "Text subtitles (ass)",
};

const char *av_get_subtitle_fmt_name(enum AVSubtitleType sub_fmt)
{
    if (sub_fmt < 0 || sub_fmt >= AV_SUBTITLE_FMT_NB)
        return NULL;
    return sub_fmt_info[sub_fmt];
}

enum AVSubtitleType av_get_subtitle_fmt(const char *name)
{
    for (int i = 0; i < AV_SUBTITLE_FMT_NB; i++)
        if (!strcmp(sub_fmt_info[i], name))
            return i;
    return AV_SUBTITLE_FMT_NONE;
}
