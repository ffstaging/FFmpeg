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

#ifndef AVFILTER_SUBTITLES_H
#define AVFILTER_SUBTITLES_H

#include "avfilter.h"
#include "internal.h"

/** default handler for get_subtitles_buffer() for subtitle inputs */
AVFrame *ff_default_get_subtitles_buffer(AVFilterLink *link, int format);

/** get_subtitles_buffer() handler for filters which simply pass subtitles along */
AVFrame *ff_null_get_subtitles_buffer(AVFilterLink *link, int format);

/**
 * Request a subtitles frame with a specific set of permissions.
 *
 * @param link           the output link to the filter from which the buffer will
 *                       be requested
 * @param format         The subtitles format.
 * @return               A reference to the frame. This must be unreferenced with
 *                       av_frame_free when you are finished with it.
*/
AVFrame *ff_get_subtitles_buffer(AVFilterLink *link, int format);

#endif /* AVFILTER_SUBTITLES_H */
