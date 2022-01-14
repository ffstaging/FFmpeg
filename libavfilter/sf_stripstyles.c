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
 * text subtitle filter which removes inline-styles from subtitles
 */

#include "libavutil/opt.h"
#include "internal.h"
#include "libavutil/ass_split_internal.h"
#include "libavutil/bprint.h"

typedef struct StripStylesContext {
    const AVClass *class;
    enum AVSubtitleType format;
    int remove_animated;
    int select_layer;
} StripStylesContext;

typedef struct DialogContext {
    StripStylesContext* ss_ctx;
    AVBPrint buffer;
    int drawing_scale;
    int is_animated;
} DialogContext;

static void dialog_text_cb(void *priv, const char *text, int len)
{
    DialogContext *s = priv;

    av_log(s->ss_ctx, AV_LOG_DEBUG, "dialog_text_cb: %s\n", text);

    if (!s->drawing_scale && (!s->is_animated || !s->ss_ctx->remove_animated))
        av_bprint_append_data(&s->buffer, text, len);
}

static void dialog_new_line_cb(void *priv, int forced)
{
    DialogContext *s = priv;
    if (!s->drawing_scale && !s->is_animated)
        av_bprint_append_data(&s->buffer, forced ? "\\N" : "\\n", 2);
}

static void dialog_drawing_mode_cb(void *priv, int scale)
{
    DialogContext *s = priv;
    s->drawing_scale = scale;
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
    .new_line         = dialog_new_line_cb,
    .drawing_mode     = dialog_drawing_mode_cb,
    .animate          = dialog_animate_cb,
    .move             = dialog_move_cb,
};

static char *ass_get_line(int readorder, int layer, const char *style,
                        const char *speaker, const char *effect, const char *text)
{
    return av_asprintf("%d,%d,%s,%s,0,0,0,%s,%s",
                       readorder, layer, style ? style : "Default",
                       speaker ? speaker : "", effect, text);
}

static char *process_dialog(StripStylesContext *s, const char *ass_line)
{
    DialogContext dlg_ctx = { .ss_ctx = s };
    ASSDialog *dialog = avpriv_ass_split_dialog(NULL, ass_line);
    char *result = NULL;

    if (!dialog)
        return NULL;

    if (s->select_layer >= 0 && dialog->layer != s->select_layer) {
        avpriv_ass_free_dialog(&dialog);
        return NULL;
    }

    dlg_ctx.ss_ctx = s;

    av_bprint_init(&dlg_ctx.buffer, 0, AV_BPRINT_SIZE_UNLIMITED);

    avpriv_ass_split_override_codes(&dialog_callbacks, &dlg_ctx, dialog->text);

    if (av_bprint_is_complete(&dlg_ctx.buffer)
        && dlg_ctx.buffer.len > 0)
        result = ass_get_line(dialog->readorder, dialog->layer, dialog->style, dialog->name, dialog->effect, dlg_ctx.buffer.str);

    av_bprint_finalize(&dlg_ctx.buffer, NULL);
    avpriv_ass_free_dialog(&dialog);

    return result;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->time_base = inlink->time_base;
    outlink->frame_rate = inlink->frame_rate;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    StripStylesContext *s = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int ret;

    outlink->format = inlink->format;

    ret = av_frame_make_writable(frame);
    if (ret <0 ) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    for (unsigned i = 0; i < frame->num_subtitle_areas; i++) {

        AVSubtitleArea *area = frame->subtitle_areas[i];

        if (area->ass) {
            char *tmp = area->ass;
            area->ass = process_dialog(s, area->ass);

            if (area->ass) {
                av_log(inlink->dst, AV_LOG_INFO, "original: %d %s\n", i, tmp);
                av_log(inlink->dst, AV_LOG_INFO, "stripped: %d %s\n", i, area->ass);
            }
            else
                area->ass = NULL;

            av_free(tmp);
        }
    }

    return ff_filter_frame(outlink, frame);
}

#define OFFSET(x) offsetof(StripStylesContext, x)
#define FLAGS (AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption stripstyles_options[] = {
    { "remove_animated", "remove animated text (default: yes)",   OFFSET(remove_animated),  AV_OPT_TYPE_BOOL, {.i64 = 1 },  0, 1, FLAGS, 0 },
    { "select_layer", "process a specific ass layer only",   OFFSET(remove_animated),  AV_OPT_TYPE_INT, {.i64 = -1 }, -1, INT_MAX, FLAGS, 0 },
    { NULL },
};

AVFILTER_DEFINE_CLASS(stripstyles);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_SUBTITLE,
        .config_props  = config_output,
    },
};

const AVFilter ff_sf_stripstyles = {
    .name          = "stripstyles",
    .description   = NULL_IF_CONFIG_SMALL("Strip subtitle inline styles"),
    .priv_size     = sizeof(StripStylesContext),
    .priv_class    = &stripstyles_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_SUBFMT(AV_SUBTITLE_FMT_ASS),
};

