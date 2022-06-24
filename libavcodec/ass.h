/*
 * SSA/ASS common functions
 * Copyright (c) 2010  Aurelien Jacobs <aurel@gnuage.org>
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

#ifndef AVCODEC_ASS_H
#define AVCODEC_ASS_H

#include "avcodec.h"
#include "libavutil/ass_internal.h"

typedef struct FFASSDecoderContext {
    int readorder;
} FFASSDecoderContext;

static inline int ff_ass_subtitle_header_full(AVCodecContext *avctx,
                                int play_res_x, int play_res_y,
                                const char *font, int font_size,
                                int primary_color, int secondary_color,
                                int outline_color, int back_color,
                                int bold, int italic, int underline,
                                int border_style, int alignment)
{
    avctx->subtitle_header = (uint8_t *)avpriv_ass_get_subtitle_header_full(
                                play_res_x, play_res_y, font, font_size,
                                primary_color, secondary_color, outline_color,
                                back_color, bold,italic,underline,border_style,alignment,
                                !(avctx->flags & AV_CODEC_FLAG_BITEXACT));

    if (!avctx->subtitle_header)
        return AVERROR(ENOMEM);
    avctx->subtitle_header_size = strlen((char *)avctx->subtitle_header);
    return 0;
}

static inline int ff_ass_subtitle_header_default(AVCodecContext *avctx)
{
    avctx->subtitle_header = (uint8_t *)avpriv_ass_get_subtitle_header_default(!(avctx->flags & AV_CODEC_FLAG_BITEXACT));

    if (!avctx->subtitle_header)
        return AVERROR(ENOMEM);
    avctx->subtitle_header_size = strlen((char *)avctx->subtitle_header);
    return 0;
}

static inline void ff_ass_decoder_flush(AVCodecContext *avctx)
{
    FFASSDecoderContext *s = avctx->priv_data;
    if (!(avctx->flags2 & AV_CODEC_FLAG2_RO_FLUSH_NOOP))
        s->readorder = 0;
}

/**
 * Add an ASS dialog to a subtitle.
 */
static inline int avpriv_ass_add_rect(AVSubtitle *sub, const char *dialog,
                    int readorder, int layer, const char *style,
                    const char *speaker)
{
    char *ass_str;
    AVSubtitleRect **rects;

    rects = av_realloc_array(sub->rects, sub->num_rects+1, sizeof(*sub->rects));
    if (!rects)
        return AVERROR(ENOMEM);
    sub->rects = rects;
    rects[sub->num_rects]       = av_mallocz(sizeof(*rects[0]));
    if (!rects[sub->num_rects])
        return AVERROR(ENOMEM);
    rects[sub->num_rects]->type = SUBTITLE_ASS;
    ass_str = avpriv_ass_get_dialog(readorder, layer, style, speaker, dialog);
    if (!ass_str)
        return AVERROR(ENOMEM);
    rects[sub->num_rects]->ass = ass_str;
    sub->num_rects++;
    return 0;
}

#endif /* AVCODEC_ASS_H */
