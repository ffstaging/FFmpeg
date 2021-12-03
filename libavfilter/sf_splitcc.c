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
 * subtitle filter for splitting out closed-caption/A53 subtitles from video frame side data
 */

#include "filters.h"
#include "libavutil/opt.h"
#include "subtitles.h"
#include "libavcodec/avcodec.h"

static const AVRational ms_tb = {1, 1000};

typedef struct SplitCaptionsContext {
    const AVClass *class;
    enum AVSubtitleType format;
    AVCodecContext *cc_dec;
    int eof;
    AVFrame *next_sub_frame;
    AVFrame *empty_sub_frame;
    int new_frame;
    int64_t next_repetition_pts;
    int had_keyframe;
    AVBufferRef *subtitle_header;
    int use_cc_styles;
    int real_time;
    int real_time_latency_msec;
    int data_field;
    int scatter_realtime_output;
} SplitCaptionsContext;

static int64_t ms_to_avtb(int64_t ms)
{
    return av_rescale_q(ms, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
}

static int init(AVFilterContext *ctx)
{
    SplitCaptionsContext *s = ctx->priv;
    AVDictionary *options = NULL;

    int ret;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    if (!codec) {
        av_log(ctx, AV_LOG_ERROR, "failed to find EIA-608/708 decoder\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    if (!((s->cc_dec = avcodec_alloc_context3(codec)))) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate EIA-608/708 decoder\n");
        return AVERROR(ENOMEM);
    }

    av_dict_set_int(&options, "real_time", s->real_time, 0);
    av_dict_set_int(&options, "real_time_latency_msec", s->real_time_latency_msec, 0);
    av_dict_set_int(&options, "data_field", s->data_field, 0);

    if ((ret = avcodec_open2(s->cc_dec, codec, &options)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "failed to open EIA-608/708 decoder: %i\n", ret);
        return ret;
    }

    if (s->use_cc_styles && s->cc_dec->subtitle_header && s->cc_dec->subtitle_header[0] != 0) {
        char* subtitle_header =  av_strdup((char *)s->cc_dec->subtitle_header);
        if (!subtitle_header)
            return AVERROR(ENOMEM);
        s->subtitle_header = av_buffer_create((uint8_t *)subtitle_header, strlen(subtitle_header) + 1, NULL, NULL, 0);
        if (!s->subtitle_header) {
            av_free(subtitle_header);
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    SplitCaptionsContext *s = ctx->priv;
    av_frame_free(&s->next_sub_frame);
    av_frame_free(&s->empty_sub_frame);
    av_buffer_unref(&s->subtitle_header);
}

static int config_input(AVFilterLink *link)
{
    const SplitCaptionsContext *context = link->dst->priv;

    if (context->cc_dec)
        context->cc_dec->pkt_timebase = link->time_base;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterLink *inlink0 = ctx->inputs[0];
    AVFilterLink *outlink0 = ctx->outputs[0];
    AVFilterLink *outlink1 = ctx->outputs[1];
    static const enum AVSubtitleType subtitle_fmts[] = { AV_SUBTITLE_FMT_ASS, AV_SUBTITLE_FMT_NONE };
    int ret;

    /* set input0 video formats */
    formats = ff_all_formats(AVMEDIA_TYPE_VIDEO);
    if ((ret = ff_formats_ref(formats, &inlink0->outcfg.formats)) < 0)
        return ret;

    /* set output0 video formats */
    if ((ret = ff_formats_ref(formats, &outlink0->incfg.formats)) < 0)
        return ret;

    /* set output1 subtitle formats */
    formats = ff_make_format_list(subtitle_fmts);
    if ((ret = ff_formats_ref(formats, &outlink1->incfg.formats)) < 0)
        return ret;

    return 0;
}

static int config_video_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    const AVFilterLink *inlink = outlink->src->inputs[0];

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->frame_rate = ctx->inputs[0]->frame_rate;
    outlink->time_base = ctx->inputs[0]->time_base;
    outlink->sample_aspect_ratio = ctx->inputs[0]->sample_aspect_ratio;

    if (inlink->hw_frames_ctx)
        outlink->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);

    return 0;
}

static int config_sub_output(AVFilterLink *outlink)
{
    SplitCaptionsContext *s = outlink->src->priv;
    const AVFilterLink *inlink = outlink->src->inputs[0];

    outlink->time_base = inlink->time_base;
    outlink->format = AV_SUBTITLE_FMT_ASS;
    outlink->frame_rate = (AVRational){1000, s->real_time_latency_msec};

    return 0;
}

static int request_sub_frame(AVFilterLink *outlink)
{
    SplitCaptionsContext *s = outlink->src->priv;
    int status;
    int64_t pts;

    if (!s->empty_sub_frame) {
        s->empty_sub_frame = ff_get_subtitles_buffer(outlink, outlink->format);
        if (!s->empty_sub_frame)
            return AVERROR(ENOMEM);
    }

    if (!s->eof && ff_inlink_acknowledge_status(outlink->src->inputs[0], &status, &pts)) {
        if (status == AVERROR_EOF)
            s->eof = 1;
    }

    if (s->eof)
        return AVERROR_EOF;

    if (s->next_sub_frame) {

        AVFrame *out = NULL;
        s->next_sub_frame->pts++;

        if (s->new_frame)
            out = av_frame_clone(s->next_sub_frame);
        else if (s->empty_sub_frame) {
            s->empty_sub_frame->pts = s->next_sub_frame->pts;
            out = av_frame_clone(s->empty_sub_frame);
            av_frame_copy_props(out, s->next_sub_frame);
            out->repeat_sub = 1;
        }

        if (!out)
            return AVERROR(ENOMEM);

        out->subtitle_timing.start_pts = av_rescale_q(s->next_sub_frame->pts, outlink->time_base, AV_TIME_BASE_Q);
        s->new_frame = 0;

        return ff_filter_frame(outlink, out);
    }

    return 0;
}

static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;

    *got_frame = 0;

    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFrameSideData *sd;
    SplitCaptionsContext *s = inlink->dst->priv;
    AVFilterLink *outlink0 = inlink->dst->outputs[0];
    AVFilterLink *outlink1 = inlink->dst->outputs[1];
    AVPacket *pkt = NULL;
    AVFrame *sub_out = NULL;

    int ret;

    outlink0->format = inlink->format;

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);

    if (sd && (s->had_keyframe || frame->key_frame)) {
        int got_output = 0;

        s->had_keyframe = 1;
        pkt = av_packet_alloc();
        pkt->buf = av_buffer_ref(sd->buf);
        if (!pkt->buf) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        pkt->data = pkt->buf->data;
        pkt->size = pkt->buf->size;
        pkt->pts  = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

        sub_out = ff_get_subtitles_buffer(outlink1, AV_SUBTITLE_FMT_ASS);
        if (!sub_out) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        if ((ret = av_buffer_replace(&sub_out->subtitle_header, s->subtitle_header)) < 0)
            goto fail;

        ret = decode(s->cc_dec, sub_out, &got_output, pkt);

        ////if (got_output) {
        ////    av_log(inlink->dst, AV_LOG_INFO, "CC Packet PTS: %"PRId64" got_output: %d  out_frame_pts: %"PRId64"  out_sub_pts: %"PRId64"\n", pkt->pts, got_output, sub_out->pts, sub_out->subtitle_pts);
        ////}

        av_packet_free(&pkt);

        if (ret < 0) {
            av_log(inlink->dst, AV_LOG_ERROR, "Decode error: %d \n", ret);
            goto fail;
        }

        if (got_output) {
            sub_out->pts = frame->pts;
            av_frame_free(&s->next_sub_frame);
            s->next_sub_frame = sub_out;
            sub_out = NULL;
            s->new_frame = 1;
            s->next_sub_frame->pts = frame->pts;

            if ((ret = av_buffer_replace(&s->next_sub_frame->subtitle_header, s->subtitle_header)) < 0)
                goto fail;

            if (s->real_time && s->scatter_realtime_output) {
                if (s->next_repetition_pts)
                    s->next_sub_frame->pts = s->next_repetition_pts;

                s->next_sub_frame->subtitle_timing.duration = ms_to_avtb(s->real_time_latency_msec);
                s->next_repetition_pts = s->next_sub_frame->pts + av_rescale_q(s->real_time_latency_msec, ms_tb, inlink->time_base);
            }

            ret = request_sub_frame(outlink1);
            if (ret < 0)
                goto fail;
        }
    }

    if (s->real_time && s->scatter_realtime_output && !s->new_frame && s->next_repetition_pts > 0 && frame->pts > s->next_repetition_pts) {
        s->new_frame = 1;
        s->next_sub_frame->pts = s->next_repetition_pts;
        s->next_repetition_pts = s->next_sub_frame->pts + av_rescale_q(s->real_time_latency_msec, ms_tb, inlink->time_base);
    }

    if (!s->next_sub_frame) {
        s->next_sub_frame = ff_get_subtitles_buffer(outlink1, outlink1->format);
        if (!s->next_sub_frame) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        s->next_sub_frame->subtitle_timing.duration = ms_to_avtb(s->real_time_latency_msec);
        s->next_sub_frame->pts = frame->pts;
        s->new_frame = 1;

        if ((ret = av_buffer_replace(&s->next_sub_frame->subtitle_header, s->subtitle_header)) < 0)
            goto fail;
    }

    ret = ff_filter_frame(outlink0, frame);

fail:
    av_packet_free(&pkt);
    av_frame_free(&sub_out);
    return ret;
}

#define OFFSET(x) offsetof(SplitCaptionsContext, x)
#define FLAGS (AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption split_cc_options[] = {
    { "use_cc_styles",    "Emit closed caption style header", OFFSET(use_cc_styles),  AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS, NULL },
    { "real_time", "emit subtitle events as they are decoded for real-time display", OFFSET(real_time), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "real_time_latency_msec", "minimum elapsed time between emitting real-time subtitle events", OFFSET(real_time_latency_msec), AV_OPT_TYPE_INT, { .i64 = 200 }, 0, 500, FLAGS },
    { "scatter_realtime_output", "scatter output events to a duration of real_time_latency_msec", OFFSET(scatter_realtime_output), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "data_field", "select data field", OFFSET(data_field), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 1, FLAGS, "data_field" },
    { "auto",   "pick first one that appears", 0, AV_OPT_TYPE_CONST, { .i64 =-1 }, 0, 0, FLAGS, "data_field" },
    { "first",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, "data_field" },
    { "second", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGS, "data_field" },
    { NULL },
};

AVFILTER_DEFINE_CLASS(split_cc);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name          = "video_passthrough",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_video_output,
    },
    {
        .name          = "subtitles",
        .type          = AVMEDIA_TYPE_SUBTITLE,
        .request_frame = request_sub_frame,
        .config_props  = config_sub_output,
    },
};

const AVFilter ff_sf_splitcc = {
    .name           = "splitcc",
    .description    = NULL_IF_CONFIG_SMALL("Extract closed-caption (A53) data from video as subtitle stream."),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(SplitCaptionsContext),
    .priv_class     = &split_cc_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
};
