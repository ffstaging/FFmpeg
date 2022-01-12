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

/**
 * @file
 * scale graphical subtitles filter
 */

#include <string.h>

#include "drawutils.h"
#include "internal.h"
#include "scale_eval.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"

#include "libavcodec/elbg.h"

static const char *const var_names[] = {
    "in_w",   "iw",
    "in_h",   "ih",
    "out_w",  "ow",
    "out_h",  "oh",
    "a",
    "sar",
    "dar",
    "margin_h",
    "margin_v",
    NULL
};

enum var_name {
    VAR_IN_W,   VAR_IW,
    VAR_IN_H,   VAR_IH,
    VAR_OUT_W,  VAR_OW,
    VAR_OUT_H,  VAR_OH,
    VAR_A,
    VAR_SAR,
    VAR_DAR,
    VARS_B_H,
    VARS_B_V,
    VARS_NB
};

enum EvalMode {
    EVAL_MODE_INIT,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

enum SubScaleMode {
    SSM_NONE,
    SSM_UNIFORM,
    SSM_UNIFORM_NO_REPOSITION,
};

enum SubArrangeMode {
    SAM_NONE,
    SAM_ENSUREMARGIN_NO_SCALE,
    SAM_ENSUREMARGIN_AND_SCALE,
    SAM_SNAPALIGNMENT_NO_SCALE,
    SAM_SNAPALIGNMENT_AND_SCALE,
};

typedef struct SubScaleContext {
    const AVClass *class;
    struct SwsContext *sws;
    AVDictionary *opts;

    int w, h;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string
    AVExpr *w_pexpr;
    AVExpr *h_pexpr;
    double var_values[VARS_NB];

    int force_original_aspect_ratio;
    int eval_mode;               ///< expression evaluation mode

    int use_caching;

    // Scale Options
    enum SubScaleMode scale_mode;

    // Arrange Options
    enum SubArrangeMode arrange_mode_h;
    enum SubArrangeMode arrange_mode_v;
    int margin_h;
    int margin_v;
    char *margin_h_expr;
    char *margin_v_expr;
    AVExpr *margin_h_pexpr;
    AVExpr *margin_v_pexpr;

    // Bitmap Options
    int num_output_colors;
    int bitmap_width_align;
    int bitmap_height_align;

    // Color Quantization Fields
    struct ELBGContext *ctx;
    AVLFG lfg;
    int *codeword;
    int *codeword_closest_codebook_idxs;
    int *codebook;
    int r_idx, g_idx, b_idx, a_idx;
    AVFrame *cache_frame;
} SubScaleContext;


static int config_output(AVFilterLink *outlink);

static int check_exprs(AVFilterContext *ctx)
{
    const SubScaleContext *s = ctx->priv;
    unsigned vars_w[VARS_NB] = { 0 }, vars_h[VARS_NB] = { 0 };

    if (!s->w_pexpr && !s->h_pexpr)
        return AVERROR(EINVAL);

    if (s->w_pexpr)
        av_expr_count_vars(s->w_pexpr, vars_w, VARS_NB);
    if (s->h_pexpr)
        av_expr_count_vars(s->h_pexpr, vars_h, VARS_NB);

    if (vars_w[VAR_OUT_W] || vars_w[VAR_OW]) {
        av_log(ctx, AV_LOG_ERROR, "Width expression cannot be self-referencing: '%s'.\n", s->w_expr);
        return AVERROR(EINVAL);
    }

    if (vars_h[VAR_OUT_H] || vars_h[VAR_OH]) {
        av_log(ctx, AV_LOG_ERROR, "Height expression cannot be self-referencing: '%s'.\n", s->h_expr);
        return AVERROR(EINVAL);
    }

    if ((vars_w[VAR_OUT_H] || vars_w[VAR_OH]) &&
        (vars_h[VAR_OUT_W] || vars_h[VAR_OW])) {
        av_log(ctx, AV_LOG_WARNING, "Circular references detected for width '%s' and height '%s' - possibly invalid.\n", s->w_expr, s->h_expr);
    }

    if (s->margin_h_pexpr)
        av_expr_count_vars(s->margin_h_pexpr, vars_w, VARS_NB);
    if (s->margin_v_pexpr)
        av_expr_count_vars(s->margin_v_pexpr, vars_h, VARS_NB);

    return 0;
}

static int scale_parse_expr(AVFilterContext *ctx, char *str_expr, AVExpr **pexpr_ptr, const char *var, const char *args)
{
    SubScaleContext *s = ctx->priv;
    int ret, is_inited = 0;
    char *old_str_expr = NULL;
    AVExpr *old_pexpr = NULL;

    if (str_expr) {
        old_str_expr = av_strdup(str_expr);
        if (!old_str_expr)
            return AVERROR(ENOMEM);
        av_opt_set(s, var, args, 0);
    }

    if (*pexpr_ptr) {
        old_pexpr = *pexpr_ptr;
        *pexpr_ptr = NULL;
        is_inited = 1;
    }

    ret = av_expr_parse(pexpr_ptr, args, var_names,
                        NULL, NULL, NULL, NULL, 0, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot parse expression for %s: '%s'\n", var, args);
        goto revert;
    }

    ret = check_exprs(ctx);
    if (ret < 0)
        goto revert;

    if (is_inited && (ret = config_output(ctx->outputs[0])) < 0)
        goto revert;

    av_expr_free(old_pexpr);
    av_freep(&old_str_expr);

    return 0;

revert:
    av_expr_free(*pexpr_ptr);
    *pexpr_ptr = NULL;
    if (old_str_expr) {
        av_opt_set(s, var, old_str_expr, 0);
        av_free(old_str_expr);
    }
    if (old_pexpr)
        *pexpr_ptr = old_pexpr;

    return ret;
}

static av_cold int init_dict(AVFilterContext *ctx, AVDictionary **opts)
{
    SubScaleContext *s = ctx->priv;
    uint8_t rgba_map[4];
    int ret;

    if (!s->w_expr)
        av_opt_set(s, "w", "iw", 0);
    if (!s->h_expr)
        av_opt_set(s, "h", "ih", 0);

    ret = scale_parse_expr(ctx, NULL, &s->w_pexpr, "width", s->w_expr);
    if (ret < 0)
        return ret;

    ret = scale_parse_expr(ctx, NULL, &s->h_pexpr, "height", s->h_expr);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "w:%s h:%s\n",
           s->w_expr, s->h_expr);

    if (!s->margin_h_expr)
        av_opt_set(s, "margin_h", "0", 0);
    if (!s->margin_v_expr)
        av_opt_set(s, "margin_v", "0", 0);

    ret = scale_parse_expr(ctx, NULL, &s->margin_h_pexpr, "margin_h", s->margin_h_expr);
    if (ret < 0)
        return ret;

    ret = scale_parse_expr(ctx, NULL, &s->margin_v_pexpr, "margin_v", s->margin_v_expr);
    if (ret < 0)
        return ret;

    s->opts = *opts;
    *opts = NULL;

    ff_fill_rgba_map(&rgba_map[0], AV_PIX_FMT_RGB32);

    s->r_idx = rgba_map[0]; // R
    s->g_idx = rgba_map[1]; // G
    s->b_idx = rgba_map[2]; // B
    s->a_idx = rgba_map[3]; // A

    av_lfg_init(&s->lfg, 123456789);


    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SubScaleContext *s = ctx->priv;

    av_frame_free(&s->cache_frame);

    av_expr_free(s->w_pexpr);
    av_expr_free(s->h_pexpr);
    s->w_pexpr = s->h_pexpr = NULL;

    av_expr_free(s->margin_h_pexpr);
    av_expr_free(s->margin_v_pexpr);
    s->margin_h_pexpr = s->margin_v_pexpr = NULL;

    sws_freeContext(s->sws);
    s->sws = NULL;
    av_dict_free(&s->opts);

    avpriv_elbg_free(&s->ctx);

    av_freep(&s->codebook);
    av_freep(&s->codeword);
    av_freep(&s->codeword_closest_codebook_idxs);
}

static int config_input(AVFilterLink *inlink)
{
    ////const AVFilterContext *ctx = inlink->dst;
    ////SubScaleContext *s = ctx->priv;

    ////if (s->w <= 0 || s->h <= 0) {
    ////    s->w = inlink->w;
    ////    s->h = inlink->h;
    ////}
    return 0;
}

static int scale_eval_dimensions(AVFilterContext *ctx)
{
    SubScaleContext *s = ctx->priv;
    const AVFilterLink *inlink = ctx->inputs[0];
    char *expr;
    int eval_w, eval_h, margin_h, margin_v;
    int ret;
    double res;

    s->var_values[VAR_IN_W]  = s->var_values[VAR_IW] = inlink->w;
    s->var_values[VAR_IN_H]  = s->var_values[VAR_IH] = inlink->h;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = NAN;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = NAN;
    s->var_values[VARS_B_H]  = s->var_values[VARS_B_V] = 0;
    s->var_values[VAR_A]     = (double) inlink->w / inlink->h;
    s->var_values[VAR_SAR]   = inlink->sample_aspect_ratio.num ?
        (double) inlink->sample_aspect_ratio.num / inlink->sample_aspect_ratio.den : 1;
    s->var_values[VAR_DAR]   = s->var_values[VAR_A] * s->var_values[VAR_SAR];

    res = av_expr_eval(s->w_pexpr, s->var_values, NULL);
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = (int) res == 0 ? inlink->w : (int) res;

    res = av_expr_eval(s->h_pexpr, s->var_values, NULL);
    if (isnan(res)) {
        expr = s->h_expr;
        ret = AVERROR(EINVAL);
        goto fail;
    }
    eval_h = s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = (int) res == 0 ? inlink->h : (int) res;

    res = av_expr_eval(s->w_pexpr, s->var_values, NULL);
    if (isnan(res)) {
        expr = s->w_expr;
        ret = AVERROR(EINVAL);
        goto fail;
    }
    eval_w = s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = (int) res == 0 ? inlink->w : (int) res;

    s->w = eval_w;
    s->h = eval_h;

    res = av_expr_eval(s->margin_h_pexpr, s->var_values, NULL);
    s->var_values[VARS_B_H] = (int)res;

    res = av_expr_eval(s->margin_v_pexpr, s->var_values, NULL);
    if (isnan(res)) {
        expr = s->margin_v_expr;
        ret = AVERROR(EINVAL);
        goto fail;
    }
    margin_v = s->var_values[VARS_B_V] = (int)res;

    res = av_expr_eval(s->margin_h_pexpr, s->var_values, NULL);
    if (isnan(res)) {
        expr = s->margin_h_expr;
        ret = AVERROR(EINVAL);
        goto fail;
    }
    margin_h = s->var_values[VARS_B_H] = (int)res;

    s->margin_h = margin_h;
    s->margin_v = margin_v;

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n", expr);
    return ret;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink  = outlink->src->inputs[0];
    SubScaleContext *s = ctx->priv;
    int ret;

    outlink->format = AV_SUBTITLE_FMT_BITMAP;
    outlink->time_base = ctx->inputs[0]->time_base;
    outlink->frame_rate = ctx->inputs[0]->frame_rate;

    if ((ret = scale_eval_dimensions(ctx)) < 0)
        goto fail;

    ff_scale_adjust_dimensions(inlink, &s->w, &s->h,
                               s->force_original_aspect_ratio, 2);

    if (s->w > INT_MAX ||
        s->h > INT_MAX ||
        (s->h * inlink->w) > INT_MAX ||
        (s->w * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    outlink->w = s->w;
    outlink->h = s->h;

    if (s->sws)
        sws_freeContext(s->sws);

    s->sws = sws_alloc_context();
    if (!s->sws)
        return AVERROR(ENOMEM);

    av_opt_set_pixel_fmt(s->sws, "src_format", AV_PIX_FMT_PAL8, 0);
    av_opt_set_int(s->sws, "dst_format", AV_PIX_FMT_RGB32, 0);
    av_opt_set_int(s->sws, "threads", ff_filter_get_nb_threads(ctx), 0);

    if (s->opts) {
        const AVDictionaryEntry *e = NULL;
        while ((e = av_dict_get(s->opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
            if ((ret = av_opt_set(s->sws, e->key, e->value, 0)) < 0)
                return ret;
        }
    }

    if ((ret = sws_init_context(s->sws, NULL, NULL)) < 0)
        return ret;

    if (inlink->sample_aspect_ratio.num){
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    } else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    av_log(ctx, AV_LOG_VERBOSE, "Output size set to %dx%d.\n", outlink->w, outlink->h);

    return 0;
fail:
    return ret;
}

static int palettize_image(SubScaleContext *const s, const int w, const int h, const int src_linesize, uint8_t *src_data,
                          int dst_linesize, uint8_t *dst_data, uint32_t *dst_pal)
{
    int k, ret;
    const int codeword_length = w * h;

    /* Re-Initialize */
    s->codeword = av_realloc_f(s->codeword, codeword_length, 4 * sizeof(*s->codeword));
    if (!s->codeword)
        return AVERROR(ENOMEM);

    s->codeword_closest_codebook_idxs =
        av_realloc_f(s->codeword_closest_codebook_idxs, codeword_length,
                     sizeof(*s->codeword_closest_codebook_idxs));
    if (!s->codeword_closest_codebook_idxs)
        return AVERROR(ENOMEM);

    s->codebook = av_realloc_f(s->codebook, s->num_output_colors, 4 * sizeof(*s->codebook));
    if (!s->codebook)
        return AVERROR(ENOMEM);

    /* build the codeword */
    k = 0;
    for (int i = 0; i < h; i++) {
        uint8_t *p = src_data;
        for (int j = 0; j < w; j++) {
            s->codeword[k++] = p[s->b_idx];
            s->codeword[k++] = p[s->g_idx];
            s->codeword[k++] = p[s->r_idx];
            s->codeword[k++] = p[s->a_idx];
            p += 4;
        }
        src_data += src_linesize;
    }

    /* compute the codebook */
    ret = avpriv_elbg_do(&s->ctx, s->codeword, 4,
        codeword_length, s->codebook,
        s->num_output_colors, 1,
        s->codeword_closest_codebook_idxs, &s->lfg, 0);

    if (ret < 0)
        return ret;

    /* Write Palette */
    for (int i = 0; i < s->num_output_colors; i++) {
        dst_pal[i] = s->codebook[i*4+3] << 24  |
                    (s->codebook[i*4+2] << 16) |
                    (s->codebook[i*4+1] <<  8) |
                     s->codebook[i*4  ];
    }

    /* Write Image */
    k = 0;
    for (int i = 0; i < h; i++) {
        uint8_t *p = dst_data;
        for (int j = 0; j < w; j++, p++) {
            p[0] = s->codeword_closest_codebook_idxs[k++];
        }

        dst_data += dst_linesize;
    }

    return ret;
}

static int rescale_size(int64_t a, AVRational factor)
{
    const int64_t res = av_rescale_rnd(a, factor.num, factor.den, AV_ROUND_NEAR_INF);
    if (res > INT32_MAX || res < 0)
        return 0;

    return (int)res;
}


static int scale_area(AVFilterLink *link, AVSubtitleArea *area, const int target_width, const int target_height)
{
    const AVFilterContext *ctx = link->dst;
    SubScaleContext *s = ctx->priv;
    int ret;

    AVBufferRef *dst_buffer;
    const uint8_t* data[2]    = { area->buf[0]->data, (uint8_t *)&area->pal };
    const int dstW            = FFALIGN(target_width, s->bitmap_width_align);
    const int dstH            = FFALIGN(target_height, s->bitmap_height_align);
    const int tmp_linesize[2] = { FFALIGN(dstW * 4, 32), 0 };
    const int dst_linesize[2] = { dstW, 0 };
    uint8_t* tmp[2] = { 0, 0 };

    AVBufferRef *tmp_buffer = av_buffer_allocz(tmp_linesize[0] * dstH);
    if (!tmp_buffer)
        return AVERROR(ENOMEM);

    if (!s->sws)
        return 0;

    tmp[0] = tmp_buffer->data;

    s->sws = sws_getCachedContext(s->sws, area->w, area->h, AV_PIX_FMT_PAL8,
        dstW, dstH, AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);
    if (!s->sws) {
        av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context. dstW=%d dstH=%d\n", dstW, dstH);
        return AVERROR(EINVAL);
    }

    // Rescale to ARGB
    ret = sws_scale(s->sws, data, area->linesize, 0, area->h, tmp, tmp_linesize);
    if (ret < 0) {
        av_buffer_unref(&tmp_buffer);
        return ret;
    }

    // Alloc output buffer
    dst_buffer = av_buffer_allocz(dst_linesize[0] * dstH);
    if (!dst_buffer) {
        av_buffer_unref(&tmp_buffer);
        return AVERROR(ENOMEM);
    }

    // Quantize to palettized image
    ret = palettize_image(s, dstW, dstH, tmp_linesize[0], tmp[0], dst_linesize[0], dst_buffer->data, area->pal);
    av_buffer_unref(&tmp_buffer);

    if (ret < 0) {
        av_buffer_unref(&dst_buffer);
        return ret;
    }

    av_buffer_unref(&area->buf[0]);
    ret = av_buffer_replace(&area->buf[0], dst_buffer);
    if (ret < 0) {
        av_buffer_unref(&dst_buffer);
        return ret;
    }

    area->w = dstW;
    area->h = dstH;
    area->linesize[0] = dst_linesize[0];
    area->nb_colors = s->num_output_colors;

    return ret;
}

static int process_area(AVFilterLink *inlink, AVSubtitleArea *area, AVRational x_factor, AVRational y_factor)
{
    AVFilterContext *ctx     = inlink->dst;
    const SubScaleContext *s = ctx->priv;
    int target_w, target_h, target_x, target_y;
    const int border_l = s->margin_h;
    const int border_r = s->w - s->margin_h;
    const int border_t = s->margin_v;
    const int border_b = s->h - s->margin_v;

    av_log(ctx, AV_LOG_DEBUG, "process_area -  start: x/y: (%d:%d) size: %dx%d scale_mode: %d x-factor: %d:%d y-factor: %d:%d\n",
        area->x, area->y, area->w, area->h, s->scale_mode, x_factor.num, x_factor.den, y_factor.num, y_factor.den);

    switch (s->scale_mode) {
    case SSM_NONE:
        target_w = area->w;
        target_h = area->h;
        target_x = area->x;
        target_y = area->y;
        break;
    case SSM_UNIFORM:
        target_w = rescale_size(area->w, x_factor);
        target_h = rescale_size(area->h, y_factor);
        target_x = rescale_size(area->x, x_factor);
        target_y = rescale_size(area->y, y_factor);
        break;
    case SSM_UNIFORM_NO_REPOSITION:
        target_w = rescale_size(area->w, x_factor);
        target_h = rescale_size(area->h, y_factor);
        target_x = area->x;
        target_y = area->y;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Invalid scale_mode: %d\n", s->scale_mode);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_DEBUG, "process_area - scaled: x/y: (%d:%d) size: %dx%d.\n", target_x, target_y, target_w, target_h);


    switch (s->arrange_mode_h) {
    case SAM_ENSUREMARGIN_AND_SCALE:
    case SAM_SNAPALIGNMENT_AND_SCALE:
        {
            // IF it doesn't fit - scale it
            int max_width = s->w - 2 * s->margin_h;

            if (max_width < 2)
                max_width = 2;

            if (target_w > max_width) {
                target_h = (int)av_rescale(target_h, max_width, target_w);
                target_w = max_width;
                target_x = s->margin_h;
            }
        }
        break;
    }

    switch (s->arrange_mode_h) {
    case SAM_NONE:
        break;
    case SAM_ENSUREMARGIN_NO_SCALE:
    case SAM_ENSUREMARGIN_AND_SCALE:
        // Left border
        if (target_x < border_l)
            target_x = border_l;

        // Right border
        if (target_x + target_w > border_r)
            target_x = border_r - target_w;

        break;
    case SAM_SNAPALIGNMENT_NO_SCALE:
    case SAM_SNAPALIGNMENT_AND_SCALE:
        {
            // Use original values to detect alignment
            const int left_margin          = area->x;
            const int right_margin         = inlink->w - area->x - area->w;
            const AVRational diff_factor_r = { left_margin - right_margin, area->w };
            const float diff_factor        = (float)av_q2d(diff_factor_r);

            if (diff_factor > 0.2f) {
                // Right aligned
                target_x = border_r - target_w;
            } else if (diff_factor < -0.2f) {
                // Left aligned
                target_x = border_l;
            } else {
                // Centered
                target_x = (inlink->w - area->w) / 2;
            }
        }

        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Invalid arrange_mode_h: %d\n", s->arrange_mode_h);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_DEBUG, "process_area -  arr_h: x/y: (%d:%d) size: %dx%d.\n", target_x, target_y, target_w, target_h);


    switch (s->arrange_mode_v) {
    case SAM_ENSUREMARGIN_AND_SCALE:
    case SAM_SNAPALIGNMENT_AND_SCALE:
        {
            // IF it doesn't fit - scale it
            int max_height = s->h - 2 * s->margin_v;

            if (max_height < 2)
                max_height = 2;

            if (target_h > max_height) {
                target_w = (int)av_rescale(target_w, max_height, target_h);
                target_h = max_height;
                target_y = s->margin_v;
            }
        }
        break;
    }

    switch (s->arrange_mode_v) {
    case SAM_NONE:
        break;
    case SAM_ENSUREMARGIN_NO_SCALE:
    case SAM_ENSUREMARGIN_AND_SCALE:
        // Top border
        if (target_y < border_t)
            target_y = border_t;

        // Bottom border
        if (target_y + target_h > border_b)
            target_y = border_b - target_h;

        break;
    case SAM_SNAPALIGNMENT_NO_SCALE:
    case SAM_SNAPALIGNMENT_AND_SCALE:
        {
            // Use original values to detect alignment
            const int top_margin           = area->y;
            const int bottom_margin        = inlink->h - area->y - area->h;
            const AVRational diff_factor_r = { top_margin - bottom_margin, area->h };
            const float diff_factor        = (float)av_q2d(diff_factor_r);

            if (diff_factor > 0.2f) {
                // Bottom aligned
                target_y = border_b - target_h;
            } else if (diff_factor < -0.2f) {
                // Top aligned
                target_y = border_t;
            } else {
                // Centered
                target_y = (inlink->h - area->h) / 2;
            }
        }

        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Invalid arrange_mode_v: %d\n", s->arrange_mode_v);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE, "process_area -  arr_v: x/y: (%d:%d) size: %dx%d.\n", target_x, target_y, target_w, target_h);

    area->x = target_x;
    area->y = target_y;

    if (area->w != target_w || area->h != target_h)
        return scale_area(inlink, area, target_w, target_h);

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    const AVFilterContext *ctx = inlink->dst;
    SubScaleContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    // just forward empty frames
    if (frame->num_subtitle_areas == 0) {
        av_frame_free(&s->cache_frame);
        return ff_filter_frame(outlink, frame);
    }

    if (s->use_caching && s->cache_frame && frame->repeat_sub
        && s->cache_frame->subtitle_timing.start_pts == frame->subtitle_timing.start_pts) {
        AVFrame *out = av_frame_clone(s->cache_frame);
        if (!out)
            return AVERROR(ENOMEM);

        ret = av_frame_copy_props(out, frame);
        if (ret < 0)
            return ret;

        av_log(inlink->dst, AV_LOG_DEBUG, "subscale CACHED - size %dx%d  pts: %"PRId64"  areas: %d\n", frame->width, frame->height, frame->subtitle_timing.start_pts, frame->num_subtitle_areas);
        av_frame_free(&frame);
        return ff_filter_frame(outlink, out);
    }

    ret = av_frame_make_writable(frame);
    if (ret >= 0) {
        const AVRational x_factor = { .num = outlink->w, .den = inlink->w} ;
        const AVRational y_factor = { .num = outlink->h, .den = inlink->h} ;

        for (unsigned i = 0; i < frame->num_subtitle_areas; ++i) {
            AVSubtitleArea *area = frame->subtitle_areas[i];

            ret = process_area(inlink, area, x_factor, y_factor);
            if (ret < 0)
                return ret;
        }

        av_log(inlink->dst, AV_LOG_DEBUG, "subscale output - size %dx%d  pts: %"PRId64"  areas: %d\n", frame->width, frame->height, frame->subtitle_timing.start_pts, frame->num_subtitle_areas);

        if (s->use_caching) {
            av_frame_free(&s->cache_frame);
            s->cache_frame = av_frame_clone(frame);
        }

        return ff_filter_frame(outlink, frame);
    }

    return ret;
}

#define OFFSET(x) offsetof(SubScaleContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption subscale_options[] = {
    { "margin_h", "horizontal border",        OFFSET(margin_h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "margin_v", "vertical border",          OFFSET(margin_v_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "w",     "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "width", "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "h",     "Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "height","Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 2, FLAGS, "force_oar" },
    { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
    { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
    { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "scale_mode", "specify how to scale subtitles", OFFSET(scale_mode), AV_OPT_TYPE_INT, {.i64 = SSM_UNIFORM}, 0, SSM_UNIFORM_NO_REPOSITION, FLAGS, "scale_mode" },
         { "none",  "no common scaling", 0, AV_OPT_TYPE_CONST, {.i64=SSM_NONE},  .flags = FLAGS, .unit = "scale_mode" },
         { "uniform",  "uniformly scale and reposition", 0, AV_OPT_TYPE_CONST, {.i64=SSM_UNIFORM},  .flags = FLAGS, .unit = "scale_mode" },
         { "uniform_no_reposition",  "uniformly scale but keep positions", 0, AV_OPT_TYPE_CONST, {.i64=SSM_UNIFORM_NO_REPOSITION},  .flags = FLAGS, .unit = "scale_mode" },
    { "use_caching", "Cache output frames",   OFFSET(use_caching), AV_OPT_TYPE_BOOL, { .i64 = 1}, 0, 1, .flags = FLAGS },

    { "arrange_h", "specify how to arrange subtitles horizontally", OFFSET(arrange_mode_h), AV_OPT_TYPE_INT, {.i64 = SAM_NONE}, 0, SAM_SNAPALIGNMENT_AND_SCALE, FLAGS, "arrange" },
    { "arrange_v", "specify how to arrange subtitles vertically", OFFSET(arrange_mode_v), AV_OPT_TYPE_INT, {.i64 = SAM_NONE}, 0, SAM_SNAPALIGNMENT_AND_SCALE, FLAGS, "arrange" },
         { "none",  "no repositioning", 0, AV_OPT_TYPE_CONST, {.i64=SAM_NONE},  .flags = FLAGS, .unit = "arrange" },
         { "margin_no_scale",  "move subs inside border when possible", 0, AV_OPT_TYPE_CONST, {.i64=SAM_ENSUREMARGIN_NO_SCALE,},  .flags = FLAGS, .unit = "arrange" },
         { "margin_and_scale",  "move subs inside border and scale as needed", 0, AV_OPT_TYPE_CONST, {.i64=SAM_ENSUREMARGIN_AND_SCALE,},  .flags = FLAGS, .unit = "arrange" },
         { "snapalign_no_scale",  "snap subs to near/far/center when possible", 0, AV_OPT_TYPE_CONST, {.i64=SAM_SNAPALIGNMENT_NO_SCALE,},  .flags = FLAGS, .unit = "arrange" },
         { "snapalign_and_scale", "snap subs to near/far/center and scale as needed", 0, AV_OPT_TYPE_CONST, {.i64=SAM_SNAPALIGNMENT_AND_SCALE,}, .flags = FLAGS, .unit = "arrange" },

    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT, {.i64 = EVAL_MODE_INIT}, 0, EVAL_MODE_NB-1, FLAGS, "eval" },
         { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
         { "frame", "eval expressions during initialization and per-frame", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
    { "num_colors",     "number of palette colors in output", OFFSET(num_output_colors),  AV_OPT_TYPE_INT, {.i64 = 256 }, 2, 256, .flags = FLAGS },
    { "bitmap_width_align",     "Bitmap width alignment", OFFSET(bitmap_width_align),  AV_OPT_TYPE_INT, {.i64 = 2 }, 1, 256, .flags = FLAGS },
    { "bitmap_height_align",     "Bitmap height alignment", OFFSET(bitmap_height_align),  AV_OPT_TYPE_INT, {.i64 = 2 }, 1, 256, .flags = FLAGS },
    { .name =  NULL }
};

static const AVClass subscale_class = {
    .class_name       = "subscale",
    .item_name        = av_default_item_name,
    .option           = subscale_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .config_props = config_output,
    },
};

const AVFilter ff_sf_subscale = {
    .name            = "subscale",
    .description     = NULL_IF_CONFIG_SMALL("Scale graphical subtitles."),
    .init_dict       = init_dict,
    .uninit          = uninit,
    .priv_size       = sizeof(SubScaleContext),
    .priv_class      = &subscale_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_SUBFMT(AV_SUBTITLE_FMT_BITMAP),
};

