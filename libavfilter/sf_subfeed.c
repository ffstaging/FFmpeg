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
 * subtitle filter for feeding subtitle frames into a filtergraph in a contiguous way
 *
 *
 * also supports
 *   - duration fixup
 *     delaying a subtitle event with unknown duration and infer duration from the
 *     start time of the subsequent subtitle
 *   - scattering
 *     splitting a subtitle event with unknown duration into multiple ones with
 *     a short and fixed duration
 *
 */

#include "filters.h"
#include "libavutil/opt.h"
#include "subtitles.h"
#include "libavutil/avassert.h"

enum SubFeedMode {
    FM_REPEAT,
    FM_SCATTER,
    FM_FORWARD,
};

typedef struct SubFeedContext {
    const AVClass *class;
    enum AVSubtitleType format;
    enum SubFeedMode mode;

    AVRational frame_rate;
    int fix_durations;
    int fix_overlap;

    int current_frame_isnew;
    int eof;
    int got_first_input;
    int need_frame;
    int64_t next_pts_offset;
    int64_t recent_subtitle_pts;

    int64_t counter;

    /**
     * Queue of frames waiting to be filtered.
     */
    FFFrameQueue fifo;

} SubFeedContext;

static int64_t ms_to_avtb(int64_t ms)
{
    return av_rescale_q(ms, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
}

static int64_t avtb_to_ms(int64_t avtb)
{
    return av_rescale_q(avtb, AV_TIME_BASE_Q, (AVRational){ 1, 1000 });
}

static int init(AVFilterContext *ctx)
{
    SubFeedContext *s = ctx->priv;

    ff_framequeue_init(&s->fifo, NULL);

    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    SubFeedContext *s = ctx->priv;
    ff_framequeue_free(&s->fifo);
}

static int config_input(AVFilterLink *link)
{
    ////const subfeedContext *context = link->dst->priv;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterLink *inlink0 = ctx->inputs[0];
    AVFilterLink *outlink0 = ctx->outputs[0];
    static const enum AVSubtitleType subtitle_fmts[] = { AV_SUBTITLE_FMT_BITMAP, AV_SUBTITLE_FMT_ASS, AV_SUBTITLE_FMT_NB };
    int ret;

    formats = ff_make_format_list(subtitle_fmts);

    if ((ret = ff_formats_ref(formats, &inlink0->outcfg.formats)) < 0)
        return ret;

    if ((ret = ff_formats_ref(formats, &outlink0->incfg.formats)) < 0)
        return ret;

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    SubFeedContext *s = outlink->src->priv;
    const AVFilterLink *inlink = outlink->src->inputs[0];

    outlink->time_base = AV_TIME_BASE_Q;
    outlink->format = inlink->format;
    outlink->w = inlink->w;
    outlink->h = inlink->h;

    if (s->mode == FM_FORWARD)
        outlink->frame_rate = (AVRational) { 1, 0 };
    else
        outlink->frame_rate = s->frame_rate;

    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    SubFeedContext *s = outlink->src->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    int64_t last_pts = outlink->current_pts;
    int64_t next_pts;
    int64_t interval = ms_to_avtb((int64_t)(av_q2d(av_inv_q(outlink->frame_rate)) * 1000));
    AVFrame *out;
    int status;

    if (s->mode == FM_FORWARD)
        return ff_request_frame(inlink);

    s->counter++;
    if (interval == 0)
        interval =  ms_to_avtb(200);

    status = ff_outlink_get_status(inlink);
    if (status == AVERROR_EOF)
        s->eof = 1;

    if (s->eof)
        return AVERROR_EOF;

    if (!s->got_first_input && inlink->current_pts != AV_NOPTS_VALUE) {

        s->got_first_input = 1;
        next_pts = av_rescale_q(inlink->current_pts, inlink->time_base, AV_TIME_BASE_Q);
        if (next_pts < last_pts)
            next_pts = last_pts + interval;

    } else if (last_pts == AV_NOPTS_VALUE)
        next_pts = av_rescale_q(inlink->current_pts, inlink->time_base, AV_TIME_BASE_Q);
    else
        next_pts = last_pts + interval;

    if (next_pts == AV_NOPTS_VALUE)
        next_pts = 0;

    if (s->next_pts_offset) {
        av_log(outlink->src, AV_LOG_VERBOSE, "Subtracting next_pts_offset: %"PRId64" \n", s->next_pts_offset);
        next_pts -= s->next_pts_offset;
        s->next_pts_offset = 0;
    }

retry:
    if (ff_framequeue_queued_frames(&s->fifo) && !s->current_frame_isnew) {

        const AVFrame *current_frame = ff_framequeue_peek(&s->fifo, 0);
        const int64_t sub_end_time   = current_frame->subtitle_timing.start_pts + current_frame->subtitle_timing.duration;

        if (ff_framequeue_queued_frames(&s->fifo) > 1) {
            const AVFrame *next_frame = ff_framequeue_peek(&s->fifo, 1);
            if (next_pts + interval > next_frame->subtitle_timing.start_pts) {
                AVFrame *remove_frame = ff_framequeue_take(&s->fifo);
                av_frame_free(&remove_frame);
                s->current_frame_isnew = 1;
                goto retry;
            }
        }

        if (next_pts > sub_end_time) {
            AVFrame *remove_frame = ff_framequeue_take(&s->fifo);
            av_frame_free(&remove_frame);
            s->current_frame_isnew = 1;
            goto retry;
        }
    }

    if (ff_framequeue_queued_frames(&s->fifo)) {
        AVFrame *current_frame = ff_framequeue_peek(&s->fifo, 0);

        if (current_frame && current_frame->subtitle_timing.start_pts <= next_pts + interval) {
            if (!s->current_frame_isnew)
                current_frame->repeat_sub++;

            out = av_frame_clone(current_frame);

            if (!out)
                return AVERROR(ENOMEM);

            if (!s->current_frame_isnew) {
                out->pts = next_pts;
            } else {
                out->pts = out->subtitle_timing.start_pts;

                if (out->pts < next_pts)
                    out->pts = next_pts;

                s->next_pts_offset = (out->pts - next_pts) % interval;
            }

            if (s->mode == FM_SCATTER) {
                const int64_t sub_end_time  = current_frame->subtitle_timing.start_pts + current_frame->subtitle_timing.duration;

                if (s->current_frame_isnew == 1 && current_frame->subtitle_timing.start_pts < out->pts) {
                    const int64_t diff = out->pts - current_frame->subtitle_timing.start_pts;
                    current_frame->subtitle_timing.duration -= diff;
                }

                out->repeat_sub = 0;
                out->subtitle_timing.start_pts = out->pts;
                out->subtitle_timing.duration = interval;
                av_assert1(out->pts >= next_pts);
                av_assert1(out->pts < next_pts + interval);
                av_assert1(out->pts < sub_end_time);

                if (out->pts > next_pts)
                    out->subtitle_timing.duration -= out->pts - next_pts;

                if (sub_end_time < next_pts + interval) {
                    const int64_t diff = next_pts + interval - sub_end_time;
                    av_assert1(diff <= out->subtitle_timing.duration);
                    out->subtitle_timing.duration -= diff;
                }
            }

            s->current_frame_isnew = 0;
            s->recent_subtitle_pts = out->subtitle_timing.start_pts;

            av_log(outlink->src, AV_LOG_DEBUG, "Output1 frame pts: %"PRId64"  subtitle_pts: %"PRId64"  repeat_frame: %d\n",
                out->pts, out->subtitle_timing.start_pts, out->repeat_sub);

            return ff_filter_frame(outlink, out);
        }
    }

    if (ff_framequeue_queued_frames(&s->fifo) == 0) {
        status = ff_request_frame(inlink);
        if (status == AVERROR_EOF) {
            s->eof = 1;
            return status;
        }

        if (s->counter > 1 && s->counter % 2)
            return 0;
    }

    out = ff_get_subtitles_buffer(outlink, outlink->format);
    out->pts = next_pts;
    out->repeat_sub = 1;
    out->subtitle_timing.start_pts = s->recent_subtitle_pts;

    av_log(outlink->src, AV_LOG_DEBUG, "Output2 frame pts: %"PRId64"  subtitle_pts: %"PRId64"  repeat_frame: %d\n",
        out->pts, out->subtitle_timing.start_pts, out->repeat_sub);

    return ff_filter_frame(outlink, out);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx        = inlink->dst;
    SubFeedContext *s           = inlink->dst->priv;
    AVFilterLink *outlink       = inlink->dst->outputs[0];
    const int64_t index         = (int64_t)ff_framequeue_queued_frames(&s->fifo) - 1;
    size_t nb_queued_frames;
    int ret = 0;

    av_log(ctx, AV_LOG_VERBOSE, "frame.pts: %"PRId64" (AVTB: %"PRId64") -  subtitle_timing.start_pts: %"PRId64" subtitle_timing.duration: %"PRId64" - format: %d\n",
        frame->pts, av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q), frame->subtitle_timing.start_pts, frame->subtitle_timing.duration, frame->format);

    frame->pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

    if (index < 0) {
        s->current_frame_isnew = 1;
    } else if (s->fix_durations || s->fix_overlap) {
        AVFrame *previous_frame = ff_framequeue_peek(&s->fifo, index);
        const int64_t pts_diff = frame->subtitle_timing.start_pts - previous_frame->subtitle_timing.start_pts;
        nb_queued_frames = ff_framequeue_queued_frames(&s->fifo);

        if (s->fix_durations && pts_diff > 0 && previous_frame->subtitle_timing.duration > ms_to_avtb(29000)) {
            av_log(ctx, AV_LOG_VERBOSE, "Previous frame (index #%"PRId64") has a duration of %"PRId64" ms, setting to  %"PRId64" ms\n",
                index, avtb_to_ms(previous_frame->subtitle_timing.duration), avtb_to_ms(frame->subtitle_timing.start_pts - previous_frame->subtitle_timing.start_pts));
            previous_frame->subtitle_timing.duration = frame->subtitle_timing.start_pts - previous_frame->subtitle_timing.start_pts;
        }

        if (s->fix_overlap && pts_diff > 0 && previous_frame->subtitle_timing.duration > pts_diff) {
            av_log(ctx, AV_LOG_VERBOSE, "Detected overlap from previous frame (index #%"PRId64") which had a duration of %"PRId64" ms, setting to the pts_diff which is %"PRId64" ms\n",
                index, avtb_to_ms(previous_frame->subtitle_timing.duration), avtb_to_ms(pts_diff));
            previous_frame->subtitle_timing.duration = pts_diff;
        }

        if (pts_diff <= 0) {
            av_log(ctx, AV_LOG_WARNING, "The pts_diff to the previous frame (index #%"PRId64")  is <= 0: %"PRId64" ms. The previous frame duration is %"PRId64" ms.\n",
                index, avtb_to_ms(pts_diff),  avtb_to_ms(previous_frame->subtitle_timing.duration));
        }
    }

    ff_framequeue_add(&s->fifo, frame);

    nb_queued_frames = ff_framequeue_queued_frames(&s->fifo);

    if (nb_queued_frames > 3)
        av_log(ctx, AV_LOG_WARNING, "frame queue count: %zu\n", nb_queued_frames);

    if (s->mode == FM_FORWARD && nb_queued_frames) {

        AVFrame *first_frame = ff_framequeue_peek(&s->fifo, 0);

        if (s->fix_overlap && nb_queued_frames < 2) {
          av_log(ctx, AV_LOG_VERBOSE, "Return no frame since we have less than 2\n");
          return 0;
        }

        if (s->fix_durations && first_frame->subtitle_timing.duration > ms_to_avtb(29000)) {
            av_log(ctx, AV_LOG_VERBOSE, "Return no frame because first frame duration is %"PRId64" ms\n", avtb_to_ms(first_frame->subtitle_timing.duration));
            return 0;
        }

        first_frame = ff_framequeue_take(&s->fifo);
        return ff_filter_frame(outlink, first_frame);
    }

    return ret;
}

#define OFFSET(x) offsetof(SubFeedContext, x)
#define FLAGS (AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption subfeed_options[] = {
    { "fix_durations", "delay output and determine duration from next frame", OFFSET(fix_durations),   AV_OPT_TYPE_BOOL,   { .i64=1 },                  0, 1, FLAGS, NULL     },
    { "fix_overlap", "delay output and adjust durations to prevent overlap", OFFSET(fix_overlap),   AV_OPT_TYPE_BOOL,   { .i64=0 },                  0, 1, FLAGS, NULL     },
    { "mode",       "set feed mode",         OFFSET(mode),      AV_OPT_TYPE_INT,                 { .i64=FM_REPEAT },  FM_REPEAT, FM_FORWARD,  FLAGS, "mode" },
    {   "repeat",     "repeat recent while valid, send empty otherwise",   0, AV_OPT_TYPE_CONST, { .i64=FM_REPEAT },  0,                  0,  FLAGS, "mode" },
    {   "scatter",    "subdivide subtitles into 1/framerate segments",     0, AV_OPT_TYPE_CONST, { .i64=FM_SCATTER }, 0,                  0,  FLAGS, "mode" },
    {   "forward",    "forward only (clears output framerate)",            0, AV_OPT_TYPE_CONST, { .i64=FM_FORWARD }, 0,                  0,  FLAGS, "mode" },
    { "rate",       "output frame rate",     OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE,         { .str = "5"},       0,         INT_MAX,     FLAGS, NULL },\
    { "r",          "output frame rate",     OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE,         { .str = "5"},       0,         INT_MAX,     FLAGS, NULL },\
    { NULL },
};

AVFILTER_DEFINE_CLASS(subfeed);

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
        .name          = "default",
        .type          = AVMEDIA_TYPE_SUBTITLE,
        .request_frame = request_frame,
        .config_props  = config_output,
    },
};

const AVFilter ff_sf_subfeed = {
    .name           = "subfeed",
    .description    = NULL_IF_CONFIG_SMALL("Control subtitle frame timing and flow in a filtergraph"),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(SubFeedContext),
    .priv_class     = &subfeed_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
};
