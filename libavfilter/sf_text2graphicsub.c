/*
 * Copyright (c) 2021 tcoza
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
 * convert text subtitles to bitmap subtitles filter
 */

#include <ass/ass.h>
#include "libavutil/avstring.h"

#include "internal.h"
#include "avfilter.h"
#include "drawutils.h"
#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/internal.h"
#include "libavformat/avformat.h"
#include "libavcodec/elbg.h"
#include "libavutil/ass_split_internal.h"
#include "libavutil/ass_internal.h"
#include "libavutil/lfg.h"

typedef struct PalettizeContext {
    int *codeword, *codebook;
    int *codeword_closest_codebook_idxs;
    int r_idx, g_idx, b_idx, a_idx;
    struct ELBGContext *elbg;
    AVLFG lfg;
} PalettizeContext;

typedef struct Text2GraphicSubContext {
    const AVClass *class;
    ASS_Library *library;
    ASS_Renderer *renderer;
    ASS_Track *track;
    PalettizeContext *palettize_context;
    FFDrawContext draw_context;
    struct { int width; int height; } size;
    int num_colors, stripstyles;
    char *filename, *fontsdir, *force_style;
    int got_header;
} Text2GraphicSubContext;

typedef struct DialogContext {
    int is_animated;
    int has_text;
} DialogContext;

static void dialog_text_cb(void *priv, const char *text, int len)
{
    DialogContext *s = priv;
    if (!s->is_animated )
        s->has_text = 1;
}

static void dialog_drawing_mode_cb(void *priv, int scale)
{
    DialogContext *s = priv;
    s->is_animated = 1;
}

static void dialog_animate_cb(void *priv, int t1, int t2, int accel, char *style)
{
    DialogContext *s = priv;
    s->is_animated = 1;
}

static void dialog_move_cb(void *priv, int x1, int y1, int x2, int y2, int t1, int t2)
{
    DialogContext *s = priv;
    if (t1 >= 0 || t2 >= 0)
        s->is_animated = 1;
}

static const ASSCodesCallbacks dialog_callbacks = {
    .text             = dialog_text_cb,
    .drawing_mode     = dialog_drawing_mode_cb,
    .animate          = dialog_animate_cb,
    .move             = dialog_move_cb,
};

static char *process_dialog(const char *ass_line)
{
    DialogContext dlg_ctx = { 0 };
    ASSDialog *dialog = avpriv_ass_split_dialog(NULL, ass_line);
    AVBPrint buffer;
    char *result = NULL;

    if (!dialog)
        return NULL;

    av_bprint_init(&buffer, 512, AV_BPRINT_SIZE_UNLIMITED);

    avpriv_ass_filter_override_codes(&dialog_callbacks, &dlg_ctx, dialog->text, &buffer, ASS_SPLIT_BASIC);

    if (av_bprint_is_complete(&buffer) && buffer.len > 0 && dlg_ctx.has_text > 0)
        result = avpriv_ass_get_dialog_ex(dialog->readorder, dialog->layer, dialog->style, dialog->name, dialog->margin_l, dialog->margin_r, dialog->margin_v, dialog->effect, buffer.str);

    av_bprint_finalize(&buffer, NULL);
    avpriv_ass_free_dialog(&dialog);

    return result;
}

static void palettize_image(PalettizeContext *const s,
                           const int w, const int h,
                           uint8_t *src_data, const int src_linesize,
                           uint8_t *dst_data, const int dst_linesize,
                           uint32_t *dst_pal, const int num_colors)
{
    const int codeword_length = w * h;

    /* Re-Initialize */
    s->codeword = av_realloc_f(s->codeword, codeword_length, 4 * sizeof(*s->codeword));
    s->codeword_closest_codebook_idxs = av_realloc_f(
        s->codeword_closest_codebook_idxs, codeword_length,
        sizeof(*s->codeword_closest_codebook_idxs));
    s->codebook = av_realloc_f(s->codebook, num_colors, 4 * sizeof(*s->codebook));

    /* build the codeword */
    for (int k = 0, i = 0; i < h; i++)
        for (int j = 0; j < w; j++) {
            s->codeword[k++] = src_data[i * src_linesize + j * 4 + s->b_idx];
            s->codeword[k++] = src_data[i * src_linesize + j * 4 + s->g_idx];
            s->codeword[k++] = src_data[i * src_linesize + j * 4 + s->r_idx];
            s->codeword[k++] = src_data[i * src_linesize + j * 4 + s->a_idx];
        }

    /* compute the codebook */
    avpriv_elbg_do(&s->elbg, s->codeword, 4,
        codeword_length, s->codebook, num_colors, 1,
        s->codeword_closest_codebook_idxs, &s->lfg, 0);

    /* Write Palette */
    for (int i = 0; i < num_colors; i++)
        dst_pal[i] = (s->codebook[i*4+3] << 24) |
                     (s->codebook[i*4+2] << 16) |
                     (s->codebook[i*4+1] <<  8) |
                     (s->codebook[i*4+0] <<  0);

    /* Write Image */
    for (int k = 0, i = 0; i < h; i++)
        for (int j = 0; j < w; j++)
            dst_data[i * dst_linesize + j] =
                s->codeword_closest_codebook_idxs[k++];
}

static void init_palettizecontext(PalettizeContext **palettizecontext)
{
    uint8_t rgba_map[4];
    PalettizeContext *context = (PalettizeContext *)av_malloc(sizeof(PalettizeContext));
    context->codebook = NULL;
    context->codeword = NULL;
    context->codeword_closest_codebook_idxs = NULL;
    context->elbg = NULL;
    av_lfg_init(&context->lfg, 0xACBADF);
    ff_fill_rgba_map(&rgba_map[0], AV_PIX_FMT_RGB32);
    context->r_idx = rgba_map[0]; // R
    context->g_idx = rgba_map[1]; // G
    context->b_idx = rgba_map[2]; // B
    context->a_idx = rgba_map[3]; // A
    *palettizecontext = context;
}

static void free_palettizecontext(PalettizeContext **palettizecontext)
{
    PalettizeContext *context = *palettizecontext;
    av_freep(&context->codebook);
    av_freep(&context->codeword);
    av_freep(&context->codeword_closest_codebook_idxs);
    avpriv_elbg_free(&context->elbg);
    av_free(context);
    *palettizecontext = NULL;
}

/* libass supports a log level ranging from 0 to 7 */
static const int ass_libavfilter_log_level_map[] = {
    [0] = AV_LOG_FATAL,     /* MSGL_FATAL */
    [1] = AV_LOG_ERROR,     /* MSGL_ERR */
    [2] = AV_LOG_WARNING,   /* MSGL_WARN */
    [3] = AV_LOG_WARNING,   /* <undefined> */
    [4] = AV_LOG_INFO,      /* MSGL_INFO */
    [5] = AV_LOG_INFO,      /* <undefined> */
    [6] = AV_LOG_VERBOSE,   /* MSGL_V */
    [7] = AV_LOG_DEBUG,     /* MSGL_DBG2 */
};

static void ass_log(int ass_level, const char *fmt, va_list args, void *ctx)
{
    const int ass_level_clip = av_clip(ass_level, 0,
        FF_ARRAY_ELEMS(ass_libavfilter_log_level_map) - 1);
    const int level = ass_libavfilter_log_level_map[ass_level_clip];
    av_vlog(ctx, level, fmt, args);
    av_log(ctx, level, "\n");
}

static const char * const font_mimetypes[] = {
    "font/ttf",
    "font/otf",
    "font/sfnt",
    "font/woff",
    "font/woff2",
    "application/font-sfnt",
    "application/font-woff",
    "application/x-truetype-font",
    "application/vnd.ms-opentype",
    "application/x-font-ttf",
    NULL
};

static int stream_is_font(AVStream* st)
{
    const AVDictionaryEntry *tag = NULL;

    if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT)
        return 0;

    tag = av_dict_get(st->metadata, "mimetype", NULL, AV_DICT_MATCH_CASE);
    if (!tag) return 0;
    for (int i = 0; font_mimetypes[i]; i++)
        if (av_strcasecmp(font_mimetypes[i], tag->value) == 0)
            return 1;
    return 0;
}

#define AR(c)  (((c)>>24)&0xFF)
#define AG(c)  (((c)>>16)&0xFF)
#define AB(c)  (((c)>> 8)&0xFF)
#define AA(c)  ((0xFF-(c))&0xFF)

static void set_area_bounds(ASS_Image *image, AVSubtitleArea *area)
{
    int x_min = INT_MAX, y_min = INT_MAX;
    int x_max = 0, y_max = 0;
    int stride_max = 0;

    for (ASS_Image *img = image; img != NULL; img = img->next)
    {
        x_min = FFMIN(x_min, img->dst_x);
        y_min = FFMIN(y_min, img->dst_y);
        x_max = FFMAX(x_max, img->dst_x + img->w);
        y_max = FFMAX(y_max, img->dst_y + img->h);
        stride_max = FFMAX(stride_max, img->dst_x + img->stride);
    }

    area->x = x_min;
    area->y = y_min;
    area->w = FFALIGN(x_max - x_min, 2);
    area->h = FFALIGN(y_max - y_min, 2);
    area->linesize[0] = area->w;    //stride_max - x_min;
}

static void ass_image_to_area_palletization(Text2GraphicSubContext *context, ASS_Image *image, AVSubtitleArea *area)
{
    size_t image_rgba_size;
    uint8_t *image_rgba;

    set_area_bounds(image, area);
    av_log(context, AV_LOG_VERBOSE, "set_area_bounds %d,%d %dx%d\n", area->x, area->y, area->w, area->h);

    // Create rgba image
    image_rgba_size = (area->linesize[0] * area->h) * 4 * sizeof(uint8_t);
    image_rgba = (uint8_t *)av_mallocz(image_rgba_size);
    for (; image != NULL; image = image->next)
    {
        uint8_t rgba_color[] = {AR(image->color), AG(image->color), AB(image->color), AA(image->color)};
        int linesize = area->linesize[0] * 4;
        FFDrawColor color;
        ff_draw_color(&context->draw_context, &color, rgba_color);
        ff_blend_mask(&context->draw_context, &color,
                      &image_rgba, &linesize, area->w, area->h,
                      image->bitmap, image->stride, image->w, image->h,
                      3, 0, image->dst_x - area->x, image->dst_y - area->y);
    }

    area->nb_colors = context->num_colors;
    area->buf[0] = av_buffer_alloc(image_rgba_size / 4);

    palettize_image(context->palettize_context,
                    area->w, area->h,
                    image_rgba, area->linesize[0] * 4,
                    area->buf[0]->data, area->linesize[0],
                    &area->pal[0], context->num_colors);

    // Clean up
    av_free(image_rgba);
}

static void process_header(const AVFilterContext *link, const AVFrame *frame)
{
    Text2GraphicSubContext *s = link->priv;
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
        track->default_style = sid;
    }

    s->got_header = 1;
}

// Main interface functions

static av_cold int init(AVFilterContext *ctx)
{
    Text2GraphicSubContext *context = ctx->priv;

    context->library = ass_library_init();
    ass_set_message_cb(context->library, ass_log, context);
    ass_set_fonts_dir(context->library, context->fontsdir);
    ass_set_extract_fonts(context->library, 1);

    if (context->filename) {
        AVFormatContext *video = NULL;
        int ret = avformat_open_input(&video, context->filename, NULL, NULL);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Unable to open %s\n", context->filename);
            return ret;
        }
        for (int i = 0; i < video->nb_streams; i++) {
            const AVDictionaryEntry *tag;
            AVStream *st = video->streams[i];
            if (!stream_is_font(st)) continue;
            tag = av_dict_get(st->metadata, "filename", NULL, AV_DICT_MATCH_CASE);
            if (!tag) continue;
            av_log(NULL, AV_LOG_DEBUG, "Loading attached font: %s\n", tag->value);
            ass_add_font(context->library, tag->value,
                 (char *)st->codecpar->extradata,
                 st->codecpar->extradata_size);
        }
        avformat_close_input(&video);
    }

    context->renderer = ass_renderer_init(context->library);
    ass_set_pixel_aspect(context->renderer, 1);
    ass_set_shaper(context->renderer, 0);
    ass_set_fonts(context->renderer, NULL, NULL, 1, NULL, 1);

    context->track = ass_new_track(context->library);
    if (!context->track) {
        av_log(ctx, AV_LOG_ERROR, "ass_new_track() failed!\n");
        return AVERROR(EINVAL);
    }

    ass_set_check_readorder(context->track, 0);

    if (context->force_style) {
        char **list = NULL;
        char *temp = NULL;
        char *ptr = av_strtok(context->force_style, ",", &temp);
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
        ass_set_style_overrides(context->library, list);
        av_free(list);
    }

    init_palettizecontext(&context->palettize_context);

    ff_draw_init(&context->draw_context, AV_PIX_FMT_BGRA, FF_DRAW_PROCESS_ALPHA);

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    const AVFilterContext *ctx = inlink->dst;
    Text2GraphicSubContext *context = ctx->priv;
    if (!context->size.width)  context->size.width  = inlink->w;
    if (!context->size.height) context->size.height = inlink->h;
    if (!context->size.height || !context->size.width) {
        av_log(NULL, AV_LOG_ERROR, "A positive height and width are required to render subtitles\n");
        return AVERROR_EXIT;
    }
    ass_set_frame_size(context->renderer, context->size.width, context->size.height);
    ass_set_storage_size(context->renderer, inlink->w, inlink->h);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    Text2GraphicSubContext *context = ctx->priv;

    outlink->w = context->size.width;
    outlink->h = context->size.height;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    const AVFilterContext *ctx = inlink->dst;
    Text2GraphicSubContext *context = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVSubtitleArea *area;
    int ret;
    unsigned processed_area_cnt = 0;
    const int64_t start_time = av_rescale_q(frame->subtitle_timing.start_pts, AV_TIME_BASE_Q, av_make_q(1, 1000));
    const int64_t duration   = av_rescale_q(frame->subtitle_timing.duration, AV_TIME_BASE_Q, av_make_q(1, 1000));
    ASS_Image *image;

    // Postpone header processing until we receive a frame with content
    if (!context->got_header && frame->num_subtitle_areas > 0)
        process_header(ctx, frame);

    if (frame->repeat_sub || frame->num_subtitle_areas == 0) {
        av_frame_free(&frame);
        return 0;
    }

    ret = av_frame_make_writable(frame);
    if (ret < 0)
        return ret;

    if (context->stripstyles) {
        for (unsigned r = 0; r < frame->num_subtitle_areas; r++) {

            area = frame->subtitle_areas[r];

            if (area->ass) {
                char *tmp = area->ass;
                area->ass = process_dialog(area->ass);

                if (area->ass) {
                    av_log(inlink->dst, AV_LOG_DEBUG, "original: %d %s\n", r, tmp);
                    av_log(inlink->dst, AV_LOG_DEBUG, "stripped: %d %s\n", r, area->ass);
                }

                av_free(tmp);
            }
        }
    }

    for (unsigned r = 0; r < frame->num_subtitle_areas; r++)
    {
        area = frame->subtitle_areas[r];
        if (area->type != AV_SUBTITLE_FMT_ASS || area->ass == NULL)
            continue;

        ass_process_chunk(context->track, area->ass, strlen(area->ass), start_time, duration);
        processed_area_cnt++;

    }

    if (processed_area_cnt == 0) {
        av_frame_free(&frame);
        return 0;
    }

    for (unsigned r = 1; r < frame->num_subtitle_areas; r++)
        av_free(frame->subtitle_areas[r]);

    frame->num_subtitle_areas = 1;
    area = frame->subtitle_areas[0];

    image = ass_render_frame(context->renderer, context->track, start_time + duration / 2, NULL);
    if (image == NULL) {
        av_log(NULL, AV_LOG_WARNING, "failed to render ass: %s\n", area->ass);
        return 0;
    }

    // TODO: Split into multiple bitmaps

    ass_image_to_area_palletization(context, image, area);
    area->type = AV_SUBTITLE_FMT_BITMAP;

    av_log(NULL, AV_LOG_DEBUG, "successfully rendered ass: %s\n", area->ass);

    frame->width = context->size.width;
    frame->height = context->size.height;
    frame->format = AV_SUBTITLE_FMT_BITMAP;
    return ff_filter_frame(outlink, frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    Text2GraphicSubContext *context = ctx->priv;
    free_palettizecontext(&context->palettize_context);
    if (context->track) ass_free_track(context->track);
    if (context->renderer) ass_renderer_done(context->renderer);
    if (context->library) ass_library_done(context->library);
    context->track = NULL;
    context->renderer = NULL;
    context->library = NULL;
}

// Copied from sf_graphicsub2text, with formats swapped
static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats, *formats2;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSubtitleType in_fmts[] = { AV_SUBTITLE_FMT_ASS, AV_SUBTITLE_FMT_NONE };
    static const enum AVSubtitleType out_fmts[] = { AV_SUBTITLE_FMT_BITMAP, AV_SUBTITLE_FMT_NONE };
    int ret;

    /* set input format */
    formats = ff_make_format_list(in_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->outcfg.formats)) < 0)
        return ret;

    /* set output format */
    formats2 = ff_make_format_list(out_fmts);
    if ((ret = ff_formats_ref(formats2, &outlink->incfg.formats)) < 0)
        return ret;

    return 0;
}

// Filter structures

#define OFFSET(x) offsetof(Text2GraphicSubContext, x)
#define FLAGS (AV_OPT_FLAG_SUBTITLE_PARAM|AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption text2graphicsub_options[] = {
        { "s",           "output size",                          OFFSET(size),        AV_OPT_TYPE_IMAGE_SIZE,                         .default_val.str = NULL, .flags = FLAGS },
        { "size",        "output size",                          OFFSET(size),        AV_OPT_TYPE_IMAGE_SIZE,                         .default_val.str = NULL, .flags = FLAGS },
        { "n",           "number of output colors",              OFFSET(num_colors),  AV_OPT_TYPE_INT,        .min = 2, .max = 256,   .default_val.i64 = 16,   .flags = FLAGS },
        { "num_colors",  "number of output colors",              OFFSET(num_colors),  AV_OPT_TYPE_INT,        .min = 2, .max = 256,   .default_val.i64 = 16,   .flags = FLAGS },
        { "ss",          "strip animations and blur styles",     OFFSET(stripstyles), AV_OPT_TYPE_BOOL,       .min = 0, .max = 1,     .default_val.i64 = 1,    .flags = FLAGS },
        { "stripstyles", "strip animations and blur styles",     OFFSET(stripstyles), AV_OPT_TYPE_BOOL,       .min = 0, .max = 1,     .default_val.i64 = 1,    .flags = FLAGS },
        { "force_style", "enforce subtitle styles",              OFFSET(force_style), AV_OPT_TYPE_STRING,                             .default_val.str = NULL, .flags = FLAGS },
        { "f",           "media file from which to load fonts",  OFFSET(filename),    AV_OPT_TYPE_STRING,                             .default_val.str = NULL, .flags = FLAGS },
        { "filename",    "media file from which to load fonts",  OFFSET(filename),    AV_OPT_TYPE_STRING,                             .default_val.str = NULL, .flags = FLAGS },
        { "fd",          "fonts directory",                      OFFSET(fontsdir),    AV_OPT_TYPE_STRING,                             .default_val.str = NULL, .flags = FLAGS },
        { "fontsdir",    "fonts directory",                      OFFSET(fontsdir),    AV_OPT_TYPE_STRING,                             .default_val.str = NULL, .flags = FLAGS },
        { .name =  NULL }
};

static const AVClass text2graphicsub_class = {
        .class_name       = "text2graphicsub",
        .item_name        = av_default_item_name,
        .option           = text2graphicsub_options,
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

const AVFilter ff_sf_text2graphicsub = {
        .name            = "text2graphicsub",
        .description     = NULL_IF_CONFIG_SMALL("Convert text subtitles to bitmap subtitles."),
        .init            = init,
        .uninit          = uninit,
        .priv_size       = sizeof(Text2GraphicSubContext),
        .priv_class      = &text2graphicsub_class,
        FILTER_INPUTS(inputs),
        FILTER_OUTPUTS(outputs),
        FILTER_QUERY_FUNC(query_formats)
};
