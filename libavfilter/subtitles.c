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

#include "libavutil/common.h"

#include "subtitles.h"
#include "avfilter.h"
#include "internal.h"


AVFrame *ff_null_get_subtitles_buffer(AVFilterLink *link, int format)
{
    return ff_get_subtitles_buffer(link->dst->outputs[0], format);
}

AVFrame *ff_default_get_subtitles_buffer(AVFilterLink *link, int format)
{
    AVFrame *frame;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = format;
    frame->type = AVMEDIA_TYPE_SUBTITLE;

    if (av_frame_get_buffer2(frame, 0) < 0) {
        av_frame_free(&frame);
        return NULL;
    }

    return frame;
}

AVFrame *ff_get_subtitles_buffer(AVFilterLink *link, int format)
{
    AVFrame *ret = NULL;

    if (link->dstpad->get_buffer.subtitle)
        ret = link->dstpad->get_buffer.subtitle(link, format);

    if (!ret)
        ret = ff_default_get_subtitles_buffer(link, format);

    return ret;
}
