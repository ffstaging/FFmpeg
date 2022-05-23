/*
 * MPEG-1/2 common code
 * Copyright (c) 2007 Aurelien Jacobs <aurel@gnuage.org>
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

#ifndef AVCODEC_MPEG12_H
#define AVCODEC_MPEG12_H

#include "mpegvideo.h"
#include "libavutil/stereo3d.h"

/* Start codes. */
#define SEQ_END_CODE            0x000001b7
#define SEQ_START_CODE          0x000001b3
#define GOP_START_CODE          0x000001b8
#define PICTURE_START_CODE      0x00000100
#define SLICE_MIN_START_CODE    0x00000101
#define SLICE_MAX_START_CODE    0x000001af
#define EXT_START_CODE          0x000001b5
#define USER_START_CODE         0x000001b2

typedef struct Mpeg1Context {
    MpegEncContext mpeg_enc_ctx;
    int mpeg_enc_ctx_allocated; /* true if decoding context allocated */
    int repeat_field;           /* true if we must repeat the field */
    AVPanScan pan_scan;         /* some temporary storage for the panscan */
    AVStereo3D stereo3d;
    int has_stereo3d;
    AVBufferRef *a53_buf_ref;
    uint8_t afd;
    int has_afd;
    int slice_count;
    unsigned aspect_ratio_info;
    AVRational save_aspect;
    int save_width, save_height, save_progressive_seq;
    int rc_buffer_size;
    AVRational frame_rate_ext;  /* MPEG-2 specific framerate modificator */
    unsigned frame_rate_index;
    int sync;                   /* Did we reach a sync point like a GOP/SEQ/KEYFrame? */
    int closed_gop;
    int tmpgexs;
    int first_slice;
    int extradata_decoded;
    int64_t timecode_frame_start;  /*< GOP timecode frame start number, in non drop frame format */
} Mpeg1Context;

void ff_mpeg12_common_init(MpegEncContext *s);

void ff_mpeg1_clean_buffers(MpegEncContext *s);
#if FF_API_FLAG_TRUNCATED
int ff_mpeg1_find_frame_end(ParseContext *pc, const uint8_t *buf, int buf_size, AVCodecParserContext *s);
#endif

void ff_mpeg12_find_best_frame_rate(AVRational frame_rate,
                                    int *code, int *ext_n, int *ext_d,
                                    int nonstandard);

void ff_mpeg_decode_user_data(AVCodecContext *avctx, Mpeg1Context *s1, const uint8_t *p, int buf_size);

#endif /* AVCODEC_MPEG12_H */
