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
 * overlay text subtitles on top of a video frame
 */

#include "config_components.h"

#include <ass/ass.h>
#include "libavutil/ass_internal.h"
#include "libavutil/thread.h"

#include "drawutils.h"
#include "filters.h"

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

typedef struct TextSubsContext {
    const AVClass *class;
    AVMutex mutex;
    int is_mutex_initialized;

    ASS_Library   *library;
    ASS_Renderer  *renderer;
    ASS_Track     *track;

    char *default_font_path;
    char *fonts_dir;
    char *fc_file;
    double font_size;
    char *force_style;
    char *language;
    int margin;
    int render_latest_only;

    int alpha;
    FFDrawContext draw;

    int got_header;
    int out_w, out_h;
    AVRational frame_rate;
    AVFrame *last_frame;
    int need_frame;
    int eof;
} TextSubsContext;

/* libass supports a log level ranging from 0 to 7 */
static const int ass_libavfilter_log_level_map[] = {
    AV_LOG_QUIET,               /* 0 */
    AV_LOG_PANIC,               /* 1 */
    AV_LOG_FATAL,               /* 2 */
    AV_LOG_ERROR,               /* 3 */
    AV_LOG_WARNING,             /* 4 */
    AV_LOG_INFO,                /* 5 */
    AV_LOG_VERBOSE,             /* 6 */
    AV_LOG_DEBUG,               /* 7 */
};

static void ass_log(int ass_level, const char *fmt, va_list args, void *ctx)
{
    const int ass_level_clip = av_clip(ass_level, 0, FF_ARRAY_ELEMS(ass_libavfilter_log_level_map) - 1);
    const int level = ass_libavfilter_log_level_map[ass_level_clip];

    av_vlog(ctx, level, fmt, args);
    av_log(ctx, level, "\n");
}

static av_cold void uninit(AVFilterContext *ctx)
{
    TextSubsContext *s = ctx->priv;

    if (s->track)
        ass_free_track(s->track);
    if (s->renderer)
        ass_renderer_done(s->renderer);
    if (s->library)
        ass_library_done(s->library);

    s->track = NULL;
    s->renderer = NULL;
    s->library = NULL;

    if (s->is_mutex_initialized) {
        ff_mutex_destroy(&s->mutex);
        s->is_mutex_initialized = 0;
    }

    av_frame_free(&s->last_frame);
}

static int overlay_textsubs_query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterLink *inlink0 = ctx->inputs[0];
    AVFilterLink *inlink1 = ctx->inputs[1];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSubtitleType subtitle_fmts[] = { AV_SUBTITLE_FMT_ASS, AV_SUBTITLE_FMT_NONE };
    int ret;

    /* set input0 and output0 video formats */
    formats = ff_draw_supported_pixel_formats(0);
    if ((ret = ff_formats_ref(formats, &inlink0->outcfg.formats)) < 0)
        return ret;
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

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->time_base = ctx->inputs[0]->time_base;
    outlink->frame_rate = ctx->inputs[0]->frame_rate;

    return 0;
}

static int config_input_main(AVFilterLink *inlink)
{
    AVFilterContext *ctx  = inlink->dst;
    TextSubsContext *s = inlink->dst->priv;
    int ret;

    ret = ff_draw_init(&s->draw, inlink->format, s->alpha ? FF_DRAW_PROCESS_ALPHA : 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Could not initialize ff_draw.\n");
        return ret;
    }

    ass_set_frame_size  (s->renderer, inlink->w, inlink->h);
    ass_set_pixel_aspect(s->renderer, av_q2d(inlink->sample_aspect_ratio));

    av_log(ctx, AV_LOG_VERBOSE, "Subtitle screen: %dx%d\n\n\n\n", inlink->w, inlink->h);

    return 0;
}

/* libass stores an RGBA color in the format RRGGBBTT, where TT is the transparency level */
#define AR(c)  ( (c)>>24)
#define AG(c)  (((c)>>16)&0xFF)
#define AB(c)  (((c)>>8) &0xFF)
#define AA(c)  ((0xFF-(c)) &0xFF)

static void overlay_ass_image(TextSubsContext *s, AVFrame *picref,
                              const ASS_Image *image)
{
    for (; image; image = image->next) {
        uint8_t rgba_color[] = {AR(image->color), AG(image->color), AB(image->color), AA(image->color)};
        FFDrawColor color;
        ff_draw_color(&s->draw, &color, rgba_color);
        ff_blend_mask(&s->draw, &color,
                      picref->data, picref->linesize,
                      picref->width, picref->height,
                      image->bitmap, image->stride, image->w, image->h,
                      3, 0, image->dst_x, image->dst_y);
    }
}

static void process_header(AVFilterContext *link, AVFrame *frame)
{
    TextSubsContext *s = link->priv;
    ASS_Track *track = s->track;
    ASS_Style *style;
    int sid = 0;

    if (!track)
        return;

    if (frame && frame->subtitle_header) {
        char *subtitle_header = (char *)frame->subtitle_header->data;
        ass_process_codec_private(s->track, subtitle_header, strlen(subtitle_header));
    }
    else {
        char* subtitle_header = avpriv_ass_get_subtitle_header_default(0);
        if (!subtitle_header)
            return;

        ass_process_codec_private(s->track, subtitle_header, strlen(subtitle_header));
        av_free(subtitle_header);
    }

    if (s->language)
        s->track->Language = av_strdup(s->language);

    if (!s->track->event_format) {
        s->track->event_format = av_strdup("ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text");
    }

    if (s->track->n_styles == 0) {
        sid = ass_alloc_style(track);
        style = &s->track->styles[sid];
        style->Name             = av_strdup("Default");
        style->PrimaryColour    = 0xffffff00;
        style->SecondaryColour  = 0x00ffff00;
        style->OutlineColour    = 0x00000000;
        style->BackColour       = 0x00000080;
        style->Bold             = 200;
        style->ScaleX           = 1.0;
        style->ScaleY           = 1.0;
        style->Spacing          = 0;
        style->BorderStyle      = 1;
        style->Outline          = 2;
        style->Shadow           = 3;
        style->Alignment        = 2;
    }
    else
        style = &s->track->styles[sid];

    style->FontSize         = s->font_size;
    style->MarginL = style->MarginR = style->MarginV = s->margin;

    track->default_style = sid;

    s->got_header = 1;
}

static int filter_video_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    TextSubsContext *s = ctx->priv;
    int detect_change = 0;
    ASS_Image *image;


    int64_t time_ms  = (int64_t)((double)frame->pts * av_q2d(inlink->time_base) * 1000);
    int64_t time_ms1 = (int64_t)((double)ctx->inputs[1]->current_pts * av_q2d(ctx->inputs[1]->time_base) * 1000);

    if (time_ms1 < time_ms + 1000)
        ff_request_frame(ctx->inputs[1]);

    av_log(ctx, AV_LOG_DEBUG, "filter_video_frame - video: %"PRId64"ms  sub: %"PRId64"ms  rel %d\n", time_ms, time_ms1, (time_ms1 < time_ms));

    ff_mutex_lock(&s->mutex);
    image = ass_render_frame(s->renderer, s->track, time_ms, &detect_change);
    ff_mutex_unlock(&s->mutex);

    if (detect_change)
        av_log(ctx, AV_LOG_DEBUG, "Change happened at time ms:%"PRId64"\n", time_ms);

    overlay_ass_image(s, frame, image);

    return ff_filter_frame(ctx->outputs[0], frame);
}

static int filter_subtitle_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    TextSubsContext *s = ctx->priv;
    const int64_t start_time = av_rescale_q(frame->subtitle_timing.start_pts, AV_TIME_BASE_Q, av_make_q(1, 1000));
    const int64_t duration   = av_rescale_q(frame->subtitle_timing.duration, AV_TIME_BASE_Q, av_make_q(1, 1000));
    const int64_t frame_time = (int64_t)((double)frame->pts * av_q2d(inlink->time_base) * 1000);

    // Postpone header processing until we receive a frame with content
    if (!s->got_header && frame->num_subtitle_areas > 0)
        process_header(ctx, frame);

    av_log(ctx, AV_LOG_DEBUG, "filter_subtitle_frame dur: %"PRId64"ms frame: %"PRId64"ms  sub: %"PRId64"ms  repeat_sub %d\n", duration, frame_time, start_time, frame->repeat_sub);

    if (frame->repeat_sub)
        goto exit;

    ff_mutex_lock(&s->mutex);

    if (s->render_latest_only && s->track->n_events > 0) {
        const int64_t previous_start_time = s->track->events[s->track->n_events - 1].Start;
        const int64_t diff = start_time - previous_start_time;
        for (int i = s->track->n_events - 1; i >= 0; i--) {
            if (previous_start_time != s->track->events[i].Start)
                break;

            if (s->track->events[i].Duration > diff)
                s->track->events[i].Duration = diff;
        }
    }

    for (unsigned i = 0; i < frame->num_subtitle_areas; i++) {
        char *ass_line = frame->subtitle_areas[i]->ass;
        if (!ass_line)
            continue;

        ass_process_chunk(s->track, ass_line, strlen(ass_line), start_time, duration);
    }

    ff_mutex_unlock(&s->mutex);

exit:
    av_frame_free(&frame);
    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    int ret;
    TextSubsContext *s = ctx->priv;

    s->library = ass_library_init();

    if (!s->library) {
        av_log(ctx, AV_LOG_ERROR, "Could not initialize libass.\n");
        return AVERROR(EINVAL);
    }

    ass_set_message_cb(s->library, ass_log, ctx);

    /* Initialize fonts */
    if (s->fonts_dir)
        ass_set_fonts_dir(s->library, s->fonts_dir);

    ass_set_extract_fonts(s->library, 1);

    s->renderer = ass_renderer_init(s->library);
    if (!s->renderer) {
        av_log(ctx, AV_LOG_ERROR, "Could not initialize libass renderer.\n");
        return AVERROR(EINVAL);
    }

    s->track = ass_new_track(s->library);
    if (!s->track) {
        av_log(ctx, AV_LOG_ERROR, "ass_new_track() failed!\n");
        return AVERROR(EINVAL);
    }

    ass_set_check_readorder(s->track, 0);

    ass_set_fonts(s->renderer, s->default_font_path, NULL, 1, s->fc_file, 1);

    if (s->force_style) {
        char **list = NULL;
        char *temp = NULL;
        char *ptr = av_strtok(s->force_style, ",", &temp);
        int i = 0;
        while (ptr) {
            av_dynarray_add(&list, &i, ptr);
            if (!list) {
                return AVERROR(ENOMEM);
            }
            ptr = av_strtok(NULL, ",", &temp);
        }
        av_dynarray_add(&list, &i, NULL);
        if (!list) {
            return AVERROR(ENOMEM);
        }
        ass_set_style_overrides(s->library, list);
        av_free(list);
    }

    ret = ff_mutex_init(&s->mutex, NULL);
    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "mutex initialiuzation failed! Error code: %d\n", ret);
        return AVERROR(EINVAL);
    }

    s->is_mutex_initialized = 1;

    return ret;
}

static int textsub2video_query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSubtitleType subtitle_fmts[] = { AV_SUBTITLE_FMT_ASS, AV_SUBTITLE_FMT_NONE };
    int ret;

    /* set input0 subtitle format */
    formats = ff_make_format_list(subtitle_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->outcfg.formats)) < 0)
        return ret;

    /* set output0 video format */
    formats = ff_draw_supported_pixel_formats(AV_PIX_FMT_FLAG_ALPHA);
    if ((ret = ff_formats_ref(formats, &outlink->incfg.formats)) < 0)
        return ret;

    return 0;
}

static int textsub2video_config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    TextSubsContext *s = ctx->priv;

    if (s->out_w <= 0 || s->out_h <= 0) {
        s->out_w = inlink->w;
        s->out_h = inlink->h;
    }

    return 0;
}

static int textsub2video_config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TextSubsContext *s = ctx->priv;
    int ret;

    ret = ff_draw_init(&s->draw, outlink->format, FF_DRAW_PROCESS_ALPHA);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Could not initialize ff_draw.\n");
        return ret;
    }

    if (s->out_w <= 0 || s->out_h <= 0) {
        av_log(ctx, AV_LOG_ERROR, "No output image size set.\n");
        return AVERROR(EINVAL);
    }

    ass_set_frame_size  (s->renderer, s->out_w, s->out_h);

    outlink->w = s->out_w;
    outlink->h = s->out_h;
    outlink->sample_aspect_ratio = (AVRational){1,1};
    outlink->frame_rate = s->frame_rate;

    return 0;
}

static int textsub2video_request_frame(AVFilterLink *outlink)
{
    TextSubsContext *s = outlink->src->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    int64_t last_pts = outlink->current_pts;
    int64_t next_pts, time_ms;
    int i, detect_change = 0, status;
    AVFrame *out;
    ASS_Image *image;

    status = ff_outlink_get_status(inlink);
    if (status == AVERROR_EOF)
        return AVERROR_EOF;

    if (s->eof)
        return AVERROR_EOF;

    if (inlink->current_pts == AV_NOPTS_VALUE) { // || outlink->current_pts > inlink->current_pts) {
        int ret = ff_request_frame(inlink);
        if (ret == AVERROR_EOF) {
            s->eof = 1;
        }

        if (ret != 0)
            av_log(outlink->src, AV_LOG_DEBUG, "ff_request_frame returned: %d\n", ret);

        s->need_frame = 1;
        return 0;
    }

    if (last_pts == AV_NOPTS_VALUE)
        next_pts = last_pts = inlink->current_pts * av_q2d(inlink->time_base) / av_q2d(outlink->time_base);
    else
        next_pts = last_pts + (int64_t)(1.0 / av_q2d(outlink->frame_rate) / av_q2d(outlink->time_base));

    time_ms = (int64_t)((double)next_pts * av_q2d(outlink->time_base) * 1000);

    ff_mutex_lock(&s->mutex);
    image = ass_render_frame(s->renderer, s->track, time_ms, &detect_change);
    ff_mutex_unlock(&s->mutex);

    if (detect_change)
        av_log(outlink->src, AV_LOG_VERBOSE, "Change happened at time ms:%"PRId64" pts:%"PRId64"\n", time_ms, next_pts);
    else if (s->last_frame) {
        out = av_frame_clone(s->last_frame);
        if (!out)
            return AVERROR(ENOMEM);

        out->pts = out->pkt_dts = out->best_effort_timestamp = next_pts;
        return ff_filter_frame(outlink, out);
    }

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);

    for (i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (out->buf[i] && i != 1)
            memset(out->buf[i]->data, 0, out->buf[i]->size);
    }

    out->pts = out->pkt_dts = out->best_effort_timestamp = next_pts;

    if (image)
        overlay_ass_image(s, out, image);

    av_frame_free(&s->last_frame);

    s->last_frame = av_frame_clone(out);

    return ff_filter_frame(outlink, out);
}

static int textsub2video_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    TextSubsContext *s = ctx->priv;
    const int64_t start_time = av_rescale_q(frame->subtitle_timing.start_pts, AV_TIME_BASE_Q, av_make_q(1, 1000));
    const int64_t duration   = av_rescale_q(frame->subtitle_timing.duration, AV_TIME_BASE_Q, av_make_q(1, 1000));

    av_log(ctx, AV_LOG_VERBOSE, "textsub2video_filter_frame num_subtitle_rects: %d, start_time_ms: %"PRId64"\n", frame->num_subtitle_areas, start_time);

    if (!s->got_header && frame->num_subtitle_areas > 0)
        process_header(ctx, frame);

    if (frame->repeat_sub)
        goto exit;

    ff_mutex_lock(&s->mutex);

    if (s->render_latest_only && s->track->n_events > 0) {
        const int64_t previous_start_time = s->track->events[s->track->n_events - 1].Start;
        const int64_t diff = start_time - previous_start_time;
        for (int i = s->track->n_events - 1; i >= 0; i--) {
            if (previous_start_time != s->track->events[i].Start)
                break;

            if (s->track->events[i].Duration > diff)
                s->track->events[i].Duration = diff;

        }
    }

    for (unsigned i = 0; i < frame->num_subtitle_areas; i++) {
        char *ass_line = frame->subtitle_areas[i]->ass;
        if (!ass_line)
            continue;

        ass_process_chunk(s->track, ass_line, strlen(ass_line), start_time, duration);
    }


    ff_mutex_unlock(&s->mutex);

exit:
    av_frame_free(&frame);

    if (s->need_frame) {
        s->need_frame = 0;
        return textsub2video_request_frame(ctx->outputs[0]);
    }

    return 0;
}

#define OFFSET(x) offsetof(TextSubsContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption overlaytextsubs_options[] = {
    {"alpha",              "enable processing of alpha channel", OFFSET(alpha),              AV_OPT_TYPE_BOOL,   {.i64 = 0   }, 0,         1,        .flags =  FLAGS},
    {"font_size",          "default font size",                  OFFSET(font_size),          AV_OPT_TYPE_DOUBLE, {.dbl = 18.0}, 0.0,       100.0,    .flags =  FLAGS},
    {"force_style",        "force subtitle style",               OFFSET(force_style),        AV_OPT_TYPE_STRING, {.str = NULL}, 0,         0,        .flags =  FLAGS},
    {"margin",             "default margin",                     OFFSET(margin),             AV_OPT_TYPE_INT,    {.i64 = 20  }, 0,         INT_MAX,  .flags =  FLAGS},
    {"default_font_path",  "path to default font",               OFFSET(default_font_path),  AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"fonts_dir",          "directory to scan for fonts",        OFFSET(fonts_dir),          AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"fontsdir",           "directory to scan for fonts",        OFFSET(fonts_dir),          AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"fontconfig_file",    "fontconfig file to load",            OFFSET(fc_file),            AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"language",           "default language",                   OFFSET(language),           AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"render_latest_only", "newest sub event for each time",     OFFSET(render_latest_only), AV_OPT_TYPE_BOOL,   {.i64 = 0   }, 0,         1,        .flags =  FLAGS},
    { .name = NULL }
};

static const AVOption textsub2video_options[] = {
    {"rate",               "set frame rate",                   OFFSET(frame_rate),         AV_OPT_TYPE_VIDEO_RATE, {.str="8"},   0,         INT_MAX,   .flags =  FLAGS},
    {"r",                  "set frame rate",                   OFFSET(frame_rate),         AV_OPT_TYPE_VIDEO_RATE, {.str="8"},   0,         INT_MAX,   .flags =  FLAGS},
    {"size",               "set video size",                   OFFSET(out_w),              AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0,         0,        .flags =  FLAGS},
    {"s",                  "set video size",                   OFFSET(out_w),              AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0,         0,        .flags =  FLAGS},
    {"font_size",          "default font size",                OFFSET(font_size),          AV_OPT_TYPE_DOUBLE,     {.dbl = 18.0}, 0.0,       100.0,    .flags =  FLAGS},
    {"force_style",        "force subtitle style",             OFFSET(force_style),        AV_OPT_TYPE_STRING,     {.str = NULL}, 0,         0,        .flags =  FLAGS},
    {"margin",             "default margin",                   OFFSET(margin),             AV_OPT_TYPE_INT,        {.i64 = 20  }, 0,         INT_MAX,  .flags =  FLAGS},
    {"default_font_path",  "path to default font",             OFFSET(default_font_path),  AV_OPT_TYPE_STRING,     {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"fonts_dir",          "directory to scan for fonts",      OFFSET(fonts_dir),          AV_OPT_TYPE_STRING,     {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"fontsdir",           "directory to scan for fonts",      OFFSET(fonts_dir),          AV_OPT_TYPE_STRING,     {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"fontconfig_file",    "fontconfig file to load",          OFFSET(fc_file),            AV_OPT_TYPE_STRING,     {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"language",           "default language",                 OFFSET(language),           AV_OPT_TYPE_STRING,     {.str = NULL}, CHAR_MIN,  CHAR_MAX, .flags =  FLAGS},
    {"render_latest_only", "newest sub event for each time",   OFFSET(render_latest_only), AV_OPT_TYPE_BOOL,       {.i64 = 0   }, 0,         1,        .flags =  FLAGS},
    { .name = NULL }
};

#if CONFIG_OVERLAYTEXTSUBS_FILTER

AVFILTER_DEFINE_CLASS(overlaytextsubs);

static const AVFilterPad overlaytextsubs_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_main,
        .flags        = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
        .filter_frame = filter_video_frame,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .filter_frame = filter_subtitle_frame,
    },
};

static const AVFilterPad overlaytextsubs_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_overlaytextsubs = {
    .name          = "overlaytextsubs",
    .description   = NULL_IF_CONFIG_SMALL("Overlay textual subtitles on top of the input."),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(TextSubsContext),
    .priv_class    = &overlaytextsubs_class,
    FILTER_INPUTS(overlaytextsubs_inputs),
    FILTER_OUTPUTS(overlaytextsubs_outputs),
    FILTER_QUERY_FUNC(overlay_textsubs_query_formats),
};
#endif

#if CONFIG_TEXTSUB2VIDEO_FILTER

AVFILTER_DEFINE_CLASS(textsub2video);

static const AVFilterPad textsub2video_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .config_props = textsub2video_config_input,
        .filter_frame = textsub2video_filter_frame,
    },
};

static const AVFilterPad textsub2video_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = textsub2video_config_output,
        .request_frame = textsub2video_request_frame,
    },
};

const AVFilter ff_svf_textsub2video = {
    .name          = "textsub2video",
    .description   = NULL_IF_CONFIG_SMALL("Convert textual subtitles to video frames"),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(TextSubsContext),
    .priv_class    = &textsub2video_class,
    FILTER_INPUTS(textsub2video_inputs),
    FILTER_OUTPUTS(textsub2video_outputs),
    FILTER_QUERY_FUNC(textsub2video_query_formats),
};
#endif
