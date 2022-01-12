/*
 * Copyright (c) 2021 softworkz (derived from vf_overlay)
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2007 Bobby Bingham
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
 * overlay graphical subtitles on top of a video frame
 */

#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "drawutils.h"
#include "framesync.h"

enum var_name {
    VAR_MAIN_W,    VAR_MW,
    VAR_MAIN_H,    VAR_MH,
    VAR_OVERLAY_W, VAR_OW,
    VAR_OVERLAY_H, VAR_OH,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_N,
    VAR_POS,
    VAR_T,
    VAR_VARS_NB
};

typedef struct OverlaySubsContext {
    const AVClass *class;
    int x, y;                   ///< position of overlaid picture
    int w, h;
    AVFrame *outpicref;

    int main_is_packed_rgb;
    uint8_t main_rgba_map[4];
    int main_has_alpha;
    uint8_t overlay_rgba_map[4];
    int eval_mode;              ///< EvalMode
    int use_caching;
    AVFrame *cache_frame;

    FFFrameSync fs;

    int main_pix_step[4];       ///< steps per pixel for each plane of the main output
    int hsub, vsub;             ///< chroma subsampling values
    const AVPixFmtDescriptor *main_desc; ///< format descriptor for main input

    double var_values[VAR_VARS_NB];
    char *x_expr, *y_expr;

    AVExpr *x_pexpr, *y_pexpr;

    int pic_counter;
} OverlaySubsContext;

static const char *const var_names[] = {
    "main_w",    "W", ///< width  of the main    video
    "main_h",    "H", ///< height of the main    video
    "overlay_w", "w", ///< width  of the overlay video
    "overlay_h", "h", ///< height of the overlay video
    "hsub",
    "vsub",
    "x",
    "y",
    "n",            ///< number of frame
    "pos",          ///< position in the file
    "t",            ///< timestamp expressed in seconds
    NULL
};

#define MAIN    0
#define OVERLAY 1

#define R 0
#define G 1
#define B 2
#define A 3

#define Y 0
#define U 1
#define V 2

enum EvalMode {
    EVAL_MODE_INIT,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

static av_cold void overlay_graphicsubs_uninit(AVFilterContext *ctx)
{
    OverlaySubsContext *s = ctx->priv;

    av_frame_free(&s->cache_frame);
    ff_framesync_uninit(&s->fs);
    av_expr_free(s->x_pexpr); s->x_pexpr = NULL;
    av_expr_free(s->y_pexpr); s->y_pexpr = NULL;
}

static inline int normalize_xy(double d, int chroma_sub)
{
    if (isnan(d))
        return INT_MAX;
    return (int)d & ~((1 << chroma_sub) - 1);
}

static void eval_expr(AVFilterContext *ctx)
{
    OverlaySubsContext *s = ctx->priv;

    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
    /* It is necessary if x is expressed from y  */
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->x = normalize_xy(s->var_values[VAR_X], s->hsub);
    s->y = normalize_xy(s->var_values[VAR_Y], s->vsub);
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names,
                        NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when evaluating the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }

    av_expr_free(old);
    return 0;
}

static int overlay_graphicsubs_query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterLink *inlink0 = ctx->inputs[0];
    AVFilterLink *inlink1 = ctx->inputs[1];
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;
    static const enum AVSubtitleType subtitle_fmts[] = { AV_SUBTITLE_FMT_BITMAP, AV_SUBTITLE_FMT_NONE };
    static const enum AVPixelFormat supported_pix_fmts[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_ARGB,  AV_PIX_FMT_RGBA,
        AV_PIX_FMT_ABGR,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_NONE
    };

    /* set input0 video formats */
    formats = ff_make_format_list(supported_pix_fmts);
    if ((ret = ff_formats_ref(formats, &inlink0->outcfg.formats)) < 0)
        return ret;

    /* set output0 video formats */
    if ((ret = ff_formats_ref(formats, &outlink->incfg.formats)) < 0)
        return ret;

    /* set input1 subtitle formats */
    formats = ff_make_format_list(subtitle_fmts);
    if ((ret = ff_formats_ref(formats, &inlink1->outcfg.formats)) < 0)
        return ret;

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    OverlaySubsContext *s = ctx->priv;
    int ret;

    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;

    outlink->w = ctx->inputs[MAIN]->w;
    outlink->h = ctx->inputs[MAIN]->h;
    outlink->time_base = ctx->inputs[MAIN]->time_base;
    outlink->frame_rate = ctx->inputs[MAIN]->frame_rate;

    return ff_framesync_configure(&s->fs);
}

// divide by 255 and round to nearest
// apply a fast variant: (X+127)/255 = ((X+127)*257+257)>>16 = ((X+128)*257)>>16
#define FAST_DIV255(x) ((((x) + 128) * 257) >> 16)

// calculate the non-pre-multiplied alpha, applying the general equation:
// alpha = alpha_overlay / ( (alpha_main + alpha_overlay) - (alpha_main * alpha_overlay) )
// (((x) << 16) - ((x) << 9) + (x)) is a faster version of: 255 * 255 * x
// ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)) is a faster version of: 255 * (x + y)
#define UNPREMULTIPLY_ALPHA(x, y) ((((x) << 16) - ((x) << 9) + (x)) / ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)))

/**
 * Blend image in src to destination buffer dst at position (x, y).
 */
static av_always_inline void blend_packed_rgb(const AVFilterContext *ctx,
    const AVFrame *dst, const AVSubtitleArea *src,
    int x, int y,
    int is_straight)
{
    OverlaySubsContext *s = ctx->priv;
    int i, imax, j, jmax;
    const int src_w = src->w;
    const int src_h = src->h;
    const int dst_w = dst->width;
    const int dst_h = dst->height;
    uint8_t alpha;          ///< the amount of overlay to blend on to main
    const int dr = s->main_rgba_map[R];
    const int dg = s->main_rgba_map[G];
    const int db = s->main_rgba_map[B];
    const int da = s->main_rgba_map[A];
    const int dstep = s->main_pix_step[0];
    const int sr = s->overlay_rgba_map[R];
    const int sg = s->overlay_rgba_map[G];
    const int sb = s->overlay_rgba_map[B];
    const int sa = s->overlay_rgba_map[A];
    int slice_start, slice_end;
    uint8_t *S, *sp, *d, *dp;

    i = FFMAX(-y, 0);
    imax = FFMIN3(-y + dst_h, FFMIN(src_h, dst_h), y + src_h);

    slice_start = i;
    slice_end = i + imax;

    sp = src->buf[0]->data + slice_start       * src->linesize[0];
    dp = dst->data[0] + (slice_start + y) * dst->linesize[0];

    for (i = slice_start; i < slice_end; i++) {
        j = FFMAX(-x, 0);
        S = sp + j;
        d = dp + ((x + j) * dstep);

        for (jmax = FFMIN(-x + dst_w, src_w); j < jmax; j++) {
            uint32_t val = src->pal[*S];
            const uint8_t *sval = (uint8_t *)&val;
            alpha = sval[sa];

            // if the main channel has an alpha channel, alpha has to be calculated
            // to create an un-premultiplied (straight) alpha value
            if (s->main_has_alpha && alpha != 0 && alpha != 255) {
                const uint8_t alpha_d = d[da];
                alpha = UNPREMULTIPLY_ALPHA(alpha, alpha_d);
            }

            switch (alpha) {
            case 0:
                break;
            case 255:
                d[dr] = sval[sr];
                d[dg] = sval[sg];
                d[db] = sval[sb];
                break;
            default:
                // main_value = main_value * (1 - alpha) + overlay_value * alpha
                // since alpha is in the range 0-255, the result must divided by 255
                d[dr] = is_straight ? FAST_DIV255(d[dr] * (255 - alpha) + sval[sr] * alpha) :
                        FFMIN(FAST_DIV255(d[dr] * (255 - alpha)) + sval[sr], 255);
                d[dg] = is_straight ? FAST_DIV255(d[dg] * (255 - alpha) + sval[sg] * alpha) :
                        FFMIN(FAST_DIV255(d[dg] * (255 - alpha)) + sval[sg], 255);
                d[db] = is_straight ? FAST_DIV255(d[db] * (255 - alpha) + sval[sb] * alpha) :
                        FFMIN(FAST_DIV255(d[db] * (255 - alpha)) + sval[sb], 255);
            }

            if (s->main_has_alpha) {
                switch (alpha) {
                case 0:
                    break;
                case 255:
                    d[da] = sval[sa];
                    break;
                default:
                    // apply alpha compositing: main_alpha += (1-main_alpha) * overlay_alpha
                    d[da] += FAST_DIV255((255 - d[da]) * S[sa]);
                }
            }
            d += dstep;
            S += 1;
        }
        dp += dst->linesize[0];
        sp += src->linesize[0];
    }
}

static av_always_inline void blend_plane_8_8bits(const AVFilterContext *ctx, const AVFrame *dst, const AVSubtitleArea *area,
    const uint32_t *yuv_pal, int src_w, int src_h, int dst_w, int dst_h, int plane, int hsub, int vsub,
    int x, int y, int dst_plane, int dst_offset, int dst_step)
{
    const int src_wp = AV_CEIL_RSHIFT(src_w, hsub);
    const int src_hp = AV_CEIL_RSHIFT(src_h, vsub);
    const int dst_wp = AV_CEIL_RSHIFT(dst_w, hsub);
    const int dst_hp = AV_CEIL_RSHIFT(dst_h, vsub);
    const int yp = y >> vsub;
    const int xp = x >> hsub;
    uint8_t *s, *sp, *d, *dp, *dap;
    int imax, i, j, jmax;
    int slice_start, slice_end;

    i = FFMAX(-yp, 0);                                                                                     \
    imax = FFMIN3(-yp + dst_hp, FFMIN(src_hp, dst_hp), yp + src_hp);                                       \

    slice_start = i;
    slice_end = i + imax;

    sp = area->buf[0]->data + (slice_start << vsub) * area->linesize[0];
    dp = dst->data[dst_plane] + (yp + slice_start) * dst->linesize[dst_plane] + dst_offset;

    dap = dst->data[3] + ((yp + slice_start) << vsub) * dst->linesize[3];

    for (i = slice_start; i < slice_end; i++) {
        j = FFMAX(-xp, 0);
        d = dp + (xp + j) * dst_step;
        s = sp + (j << hsub);
        jmax = FFMIN(-xp + dst_wp, src_wp);

        for (; j < jmax; j++) {
            uint32_t val = yuv_pal[*s];
            const uint8_t *sval = (uint8_t *)&val;
            const int alpha = sval[3];
            const int max = 255, mid = 128;
            const int d_int = *d;
            const int sval_int = sval[plane];

            switch (alpha) {
            case 0:
                break;
            case 255:
                *d = sval[plane];
                break;
            default:
                if (plane > 0)
                    *d = av_clip(FAST_DIV255((d_int - mid) * (max - alpha) + (sval_int - mid) * alpha) , -mid, mid) + mid;
                else
                    *d = FAST_DIV255(d_int * (max - alpha) + sval_int * alpha);
                break;
            }

            d += dst_step;
            s += 1 << hsub;
        }
        dp += dst->linesize[dst_plane];
        sp +=  (1 << vsub) * area->linesize[0];
        dap += (1 << vsub) * dst->linesize[3];
    }
}

#define RGB2Y(r, g, b) (uint8_t)(((66 * (r) + 129 * (g) +  25 * (b) + 128) >> 8) +  16)
#define RGB2U(r, g, b) (uint8_t)(((-38 * (r) - 74 * (g) + 112 * (b) + 128) >> 8) + 128)
#define RGB2V(r, g, b) (uint8_t)(((112 * (r) - 94 * (g) -  18 * (b) + 128) >> 8) + 128)
/* Converts R8 G8 B8 color to YUV. */
static av_always_inline void rgb_2_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t* y, uint8_t* u, uint8_t* v)
{
    *y = RGB2Y((int)r, (int)g, (int)b);
    *u = RGB2U((int)r, (int)g, (int)b);
    *v = RGB2V((int)r, (int)g, (int)b);
}


static av_always_inline void blend_yuv_8_8bits(AVFilterContext *ctx, AVFrame *dst, const AVSubtitleArea *area, int hsub, int vsub, int x, int y)
{
    OverlaySubsContext *s = ctx->priv;
    const int src_w = area->w;
    const int src_h = area->h;
    const int dst_w = dst->width;
    const int dst_h = dst->height;
    const int sr = s->overlay_rgba_map[R];
    const int sg = s->overlay_rgba_map[G];
    const int sb = s->overlay_rgba_map[B];
    const int sa = s->overlay_rgba_map[A];
    uint32_t yuvpal[256];

    for (int i = 0; i < 256; ++i) {
        const uint8_t *rgba = (const uint8_t *)&area->pal[i];
        uint8_t *yuva = (uint8_t *)&yuvpal[i];
        rgb_2_yuv(rgba[sr], rgba[sg], rgba[sb], &yuva[Y], &yuva[U], &yuva[V]);
        yuva[3] = rgba[sa];
    }

    blend_plane_8_8bits(ctx, dst, area, yuvpal, src_w, src_h, dst_w, dst_h, Y, 0,    0,    x, y, s->main_desc->comp[Y].plane, s->main_desc->comp[Y].offset, s->main_desc->comp[Y].step);
    blend_plane_8_8bits(ctx, dst, area, yuvpal, src_w, src_h, dst_w, dst_h, U, hsub, vsub, x, y, s->main_desc->comp[U].plane, s->main_desc->comp[U].offset, s->main_desc->comp[U].step);
    blend_plane_8_8bits(ctx, dst, area, yuvpal, src_w, src_h, dst_w, dst_h, V, hsub, vsub, x, y, s->main_desc->comp[V].plane, s->main_desc->comp[V].offset, s->main_desc->comp[V].step);
}

static int config_input_main(AVFilterLink *inlink)
{
    int ret;
    AVFilterContext *ctx  = inlink->dst;
    OverlaySubsContext *s = inlink->dst->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);

    av_image_fill_max_pixsteps(s->main_pix_step,    NULL, pix_desc);
    ff_fill_rgba_map(s->overlay_rgba_map, AV_PIX_FMT_RGB32); // it's actually AV_PIX_FMT_PAL8);

    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    s->main_desc = pix_desc;

    s->main_is_packed_rgb = ff_fill_rgba_map(s->main_rgba_map, inlink->format) >= 0;
    s->main_has_alpha = !!(pix_desc->flags & AV_PIX_FMT_FLAG_ALPHA);

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = ctx->inputs[MAIN   ]->w;
    s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = ctx->inputs[MAIN   ]->h;
    s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = ctx->inputs[OVERLAY]->w;
    s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = ctx->inputs[OVERLAY]->h;
    s->var_values[VAR_HSUB]  = 1<<pix_desc->log2_chroma_w;
    s->var_values[VAR_VSUB]  = 1<<pix_desc->log2_chroma_h;
    s->var_values[VAR_X]     = NAN;
    s->var_values[VAR_Y]     = NAN;
    s->var_values[VAR_N]     = 0;
    s->var_values[VAR_T]     = NAN;
    s->var_values[VAR_POS]   = NAN;

    if ((ret = set_expr(&s->x_pexpr,      s->x_expr,      "x",      ctx)) < 0 ||
        (ret = set_expr(&s->y_pexpr,      s->y_expr,      "y",      ctx)) < 0)
        return ret;

    if (s->eval_mode == EVAL_MODE_INIT) {
        eval_expr(ctx);
        av_log(ctx, AV_LOG_VERBOSE, "x:%f xi:%d y:%f yi:%d\n",
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y);
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "main w:%d h:%d fmt:%s overlay w:%d h:%d fmt:%s\n",
           ctx->inputs[MAIN]->w, ctx->inputs[MAIN]->h,
           av_get_pix_fmt_name(ctx->inputs[MAIN]->format),
           ctx->inputs[OVERLAY]->w, ctx->inputs[OVERLAY]->h,
           av_get_pix_fmt_name(ctx->inputs[OVERLAY]->format));
    return 0;
}

static int do_blend(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFrame *mainpic, *second;
    OverlaySubsContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    unsigned i;
    int ret;

    ret = ff_framesync_dualinput_get_writable(fs, &mainpic, &second);
    if (ret < 0)
        return ret;
    if (!second)
        return ff_filter_frame(ctx->outputs[0], mainpic);

    if (s->eval_mode == EVAL_MODE_FRAME) {
        int64_t pos = mainpic->pkt_pos;

        s->var_values[VAR_N] = (double)inlink->frame_count_out;
        s->var_values[VAR_T] = mainpic->pts == AV_NOPTS_VALUE ?
            NAN :(double)mainpic->pts * av_q2d(inlink->time_base);
        s->var_values[VAR_POS] = pos == -1 ? NAN : (double)pos;

        s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = second->width;
        s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = second->height;
        s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = mainpic->width;
        s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = mainpic->height;

        eval_expr(ctx);
        av_log(ctx, AV_LOG_DEBUG, "n:%f t:%f pos:%f x:%f xi:%d y:%f yi:%d\n",
               s->var_values[VAR_N], s->var_values[VAR_T], s->var_values[VAR_POS],
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y);
    }

    for (i = 0; i < second->num_subtitle_areas; i++) {
        const AVSubtitleArea *sub_area = second->subtitle_areas[i];

        if (sub_area->type != AV_SUBTITLE_FMT_BITMAP) {
            av_log(NULL, AV_LOG_WARNING, "overlay_graphicsubs: non-bitmap subtitle\n");
            return AVERROR_INVALIDDATA;
        }

        switch (inlink->format) {
        case AV_PIX_FMT_YUV420P:
            blend_yuv_8_8bits(ctx, mainpic, sub_area, 1, 1, sub_area->x + s->x, sub_area->y + s->y);
            break;
        case AV_PIX_FMT_YUV422P:
            blend_yuv_8_8bits(ctx, mainpic, sub_area, 1, 0, sub_area->x + s->x, sub_area->y + s->y);
            break;
        case AV_PIX_FMT_YUV444P:
            blend_yuv_8_8bits(ctx, mainpic, sub_area, 0, 0, sub_area->x + s->x, sub_area->y + s->y);
            break;
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_ABGR:
            blend_packed_rgb(ctx, mainpic, sub_area, sub_area->x + s->x, sub_area->y + s->y, 1);
            break;
        default:
            av_log(NULL, AV_LOG_ERROR, "Unsupported input pix fmt: %d\n", inlink->format);
            return AVERROR(EINVAL);
        }
    }

    return ff_filter_frame(ctx->outputs[0], mainpic);
}

static av_cold int overlay_graphicsubs_init(AVFilterContext *ctx)
{
    OverlaySubsContext *s = ctx->priv;

    s->fs.on_event = do_blend;
    return 0;
}

static int overlay_graphicsubs_activate(AVFilterContext *ctx)
{
    OverlaySubsContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static int graphicsub2video_query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSubtitleType subtitle_fmts[] = { AV_SUBTITLE_FMT_BITMAP, AV_SUBTITLE_FMT_NONE };
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE };
    int ret;

    /* set input subtitle formats */
    formats = ff_make_format_list(subtitle_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->outcfg.formats)) < 0)
        return ret;

    /* set output video formats */
    formats = ff_make_format_list(pix_fmts);
    if ((ret = ff_formats_ref(formats, &outlink->incfg.formats)) < 0)
        return ret;

    return 0;
}

static int graphicsub2video_config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    OverlaySubsContext *s = ctx->priv;

    if (s->w <= 0 || s->h <= 0) {
        s->w = inlink->w;
        s->h = inlink->h;
    }
    return 0;
}

static int graphicsub2video_config_output(AVFilterLink *outlink)
{
    const AVFilterContext *ctx  = outlink->src;
    OverlaySubsContext *s = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(outlink->format);

    outlink->w = s->w;
    outlink->h = s->h;

    if (outlink->w == 0 && outlink->h == 0) {
        outlink->w = 1;
        outlink->h = 1;
    }
    outlink->sample_aspect_ratio = (AVRational){1,1};
    outlink->time_base = ctx->inputs[0]->time_base;
    outlink->frame_rate = ctx->inputs[0]->frame_rate;

    av_image_fill_max_pixsteps(s->main_pix_step, NULL, pix_desc);
    ff_fill_rgba_map(s->overlay_rgba_map, AV_PIX_FMT_RGB32);

    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    s->main_desc = pix_desc;

    s->main_is_packed_rgb = ff_fill_rgba_map(s->main_rgba_map, outlink->format) >= 0;
    s->main_has_alpha = !!(pix_desc->flags & AV_PIX_FMT_FLAG_ALPHA);

    return 0;
}

static int graphicsub2video_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterLink *outlink = inlink->dst->outputs[0];
    const AVFilterContext *ctx  = outlink->src;
    OverlaySubsContext *s = ctx->priv;
    AVFrame *out;
    const unsigned num_rects = frame->num_subtitle_areas;
    unsigned int i;
    int ret;

    if (s->use_caching && s->cache_frame && frame->repeat_sub
        && s->cache_frame->subtitle_timing.start_pts == frame->subtitle_timing.start_pts) {
        out = av_frame_clone(s->cache_frame);
        if (!out)
            return AVERROR(ENOMEM);

        ret = av_frame_copy_props(out, frame);
        if (ret < 0)
            return ret;

        av_log(inlink->dst, AV_LOG_DEBUG, "graphicsub2video CACHED - size %dx%d  pts: %"PRId64"  areas: %d\n", frame->width, frame->height, frame->subtitle_timing.start_pts, frame->num_subtitle_areas);
        av_frame_free(&frame);
        return ff_filter_frame(outlink, out);
    }


    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    memset(out->data[0], 0, (size_t)out->linesize[0] * out->height);

    out->pts = out->pkt_dts = out->best_effort_timestamp = frame->pts;
    out->coded_picture_number = out->display_picture_number = s->pic_counter++;

    for (i = 0; i < num_rects; i++) {
        const AVSubtitleArea  *sub_rect = frame->subtitle_areas[i];

        if (sub_rect->type != AV_SUBTITLE_FMT_BITMAP) {
            av_log(NULL, AV_LOG_WARNING, "graphicsub2video: non-bitmap subtitle\n");
            av_frame_free(&frame);
            return AVERROR_INVALIDDATA;
        }

        blend_packed_rgb(inlink->dst, out, sub_rect, sub_rect->x, sub_rect->y, 1);
    }

    av_log(inlink->dst, AV_LOG_DEBUG, "graphicsub2video output - size %dx%d  pts: %"PRId64"  areas: %d\n", out->width, out->height, out->pts, frame->num_subtitle_areas);

    if (s->use_caching) {
        av_frame_free(&s->cache_frame);
        s->cache_frame = av_frame_clone(out);
    }

    av_frame_free(&frame);
    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(OverlaySubsContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption overlaygraphicsubs_options[] = {
    { "x", "set the x expression", OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, 0, 0, .flags = FLAGS },
    { "y", "set the y expression", OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, 0, 0, .flags = FLAGS },
    { "eof_action", "Action to take when encountering EOF from secondary input ",
        OFFSET(fs.opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, "eof_action" },
        { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },
        { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },
        { "pass",   "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS },   .flags = FLAGS, "eof_action" },
    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT, {.i64 = EVAL_MODE_FRAME}, 0, EVAL_MODE_NB-1, FLAGS, "eval" },
         { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
         { "frame", "eval expressions per-frame",                  0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
    { "shortest", "force termination when the shortest input terminates", OFFSET(fs.opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, .flags = FLAGS },
    { "repeatlast", "repeat overlay of the last overlay frame", OFFSET(fs.opt_repeatlast), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, .flags = FLAGS },
    { NULL }
};

static const AVOption graphicsub2video_options[] = {
    { "size",        "set output frame size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, .flags = FLAGS },
    { "s",           "set output frame size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, .flags = FLAGS },
    { "use_caching", "Cache output frames",   OFFSET(use_caching), AV_OPT_TYPE_BOOL, { .i64 = 1}, 0, 1, .flags = FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(overlaygraphicsubs, OverlaySubsContext, fs);

static const AVFilterPad overlaygraphicsubs_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_main,
        .flags        = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_SUBTITLE,
    },
};

static const AVFilterPad overlaygraphicsubs_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_overlaygraphicsubs = {
    .name          = "overlaygraphicsubs",
    .description   = NULL_IF_CONFIG_SMALL("Overlay graphical subtitles on top of the input."),
    .preinit       = overlaygraphicsubs_framesync_preinit,
    .init          = overlay_graphicsubs_init,
    .uninit        = overlay_graphicsubs_uninit,
    .priv_size     = sizeof(OverlaySubsContext),
    .priv_class    = &overlaygraphicsubs_class,
    .activate      = overlay_graphicsubs_activate,
    FILTER_INPUTS(overlaygraphicsubs_inputs),
    FILTER_OUTPUTS(overlaygraphicsubs_outputs),
    FILTER_QUERY_FUNC(overlay_graphicsubs_query_formats),
};

AVFILTER_DEFINE_CLASS(graphicsub2video);

static const AVFilterPad graphicsub2video_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .filter_frame = graphicsub2video_filter_frame,
        .config_props = graphicsub2video_config_input,
    },
};

static const AVFilterPad graphicsub2video_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = graphicsub2video_config_output,
    },
};

const AVFilter ff_svf_graphicsub2video = {
    .name          = "graphicsub2video",
    .description   = NULL_IF_CONFIG_SMALL("Convert graphical subtitles to video"),
    .priv_size     = sizeof(OverlaySubsContext),
    .priv_class    = &graphicsub2video_class,
    FILTER_INPUTS(graphicsub2video_inputs),
    FILTER_OUTPUTS(graphicsub2video_outputs),
    FILTER_QUERY_FUNC(graphicsub2video_query_formats),
};
