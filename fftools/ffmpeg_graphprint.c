/*
 * Copyright (c) 2018 - softworkz
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
 * output writers for filtergraph details
 */

#include "config.h"

#include <string.h>

#include "ffmpeg_graphprint.h"
#include "ffmpeg_filter.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/common.h"
#include "libavfilter/avfilter.h"
#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "textformat/avtextformat.h"

/* Text Format API Shortcuts */
#define print_int(k, v)         avtext_print_integer(w, k, v)
#define print_q(k, v, s)        avtext_print_rational(w, k, v, s)
#define print_str(k, v)         avtext_print_string(w, k, v, 0)

static void print_hwdevicecontext(AVTextFormatContext *w, const AVHWDeviceContext *hw_device_context)
{
    avtext_print_section_header(w, NULL, SECTION_ID_HWDEViCECONTEXT);

    print_int("HasHwDeviceContext", 1);
    print_str("DeviceType", av_hwdevice_get_type_name(hw_device_context->type));

    avtext_print_section_footer(w); // SECTION_ID_HWDEViCECONTEXT
}

static void print_hwframescontext(AVTextFormatContext *w, const AVHWFramesContext *hw_frames_context)
{
    const AVPixFmtDescriptor* pixdescHw;
    const AVPixFmtDescriptor* pixdescSw;

    avtext_print_section_header(w, NULL, SECTION_ID_HWFRAMESCONTEXT);

    print_int("HasHwFramesContext", 1);

    pixdescHw = av_pix_fmt_desc_get(hw_frames_context->format);
    if (pixdescHw) {
        print_str("HwPixelFormat", pixdescHw->name);
        print_str("HwPixelFormatAlias", pixdescHw->alias);
    }

    pixdescSw = av_pix_fmt_desc_get(hw_frames_context->sw_format);
    if (pixdescSw) {
        print_str("SwPixelFormat", pixdescSw->name);
        print_str("SwPixelFormatAlias", pixdescSw->alias);
    }

    print_int("Width", hw_frames_context->width);
    print_int("Height", hw_frames_context->height);

    print_hwdevicecontext(w, hw_frames_context->device_ctx);

    avtext_print_section_footer(w); // SECTION_ID_HWFRAMESCONTEXT
}

static void print_link(AVTextFormatContext *w, AVFilterLink *link)
{
    char layoutString[64];

    switch (link->type) {
        case AVMEDIA_TYPE_VIDEO:
            print_str("Format",  av_x_if_null(av_get_pix_fmt_name(link->format), "?"));
            print_int("Width", link->w);
            print_int("Height", link->h);
            print_q("SAR", link->sample_aspect_ratio, ':');
            print_q("TimeBase", link->time_base, '/');
            break;

        ////case AVMEDIA_TYPE_SUBTITLE:
        ////    print_str("Format",  av_x_if_null(av_get_subtitle_fmt_name(link->format), "?"));
        ////    print_int("Width", link->w);
        ////    print_int("Height", link->h);
        ////    print_q("TimeBase", link->time_base, '/');
        ////    break;

        case AVMEDIA_TYPE_AUDIO:
            av_channel_layout_describe(&link->ch_layout, layoutString, sizeof(layoutString));
            print_str("ChannelString", layoutString);
            print_int("Channels", link->ch_layout.nb_channels);
            ////print_int("ChannelLayout", link->ch_layout);
            print_int("SampleRate", link->sample_rate);
            break;
    }

    AVBufferRef *hw_frames_ctx = avfilter_link_get_hw_frames_ctx(link);

    if (hw_frames_ctx && hw_frames_ctx->buffer) {
      print_hwframescontext(w, (AVHWFramesContext *)hw_frames_ctx->data);
    }
}

static void print_filter(AVTextFormatContext *w, const AVFilterContext* filter)
{
    avtext_print_section_header(w, NULL, SECTION_ID_FILTER);

    print_str("Name", filter->name);

    if (filter->filter) {
        print_str("Name2", filter->filter->name);
        print_str("Description", filter->filter->description);
    }

    if (filter->hw_device_ctx) {
        AVHWDeviceContext* decCtx = (AVHWDeviceContext*)filter->hw_device_ctx->data;
        print_hwdevicecontext(w, decCtx);
    }

    avtext_print_section_header(w, NULL, SECTION_ID_INPUTS);

    for (unsigned i = 0; i < filter->nb_inputs; i++) {
        AVFilterLink *link = filter->inputs[i];
        avtext_print_section_header(w, NULL, SECTION_ID_INPUT);

        print_str("SourceName", link->src->name);
        print_str("SourcePadName", avfilter_pad_get_name(link->srcpad, 0));
        print_str("DestPadName", avfilter_pad_get_name(link->dstpad, 0));

        print_link(w, link);

        avtext_print_section_footer(w); // SECTION_ID_INPUT
    }

    avtext_print_section_footer(w); // SECTION_ID_INPUTS

    avtext_print_section_header(w, NULL, SECTION_ID_OUTPUTS);

    for (unsigned i = 0; i < filter->nb_outputs; i++) {
        AVFilterLink *link = filter->outputs[i];
        avtext_print_section_header(w, NULL, SECTION_ID_OUTPUT);

        print_str("DestName", link->dst->name);
        print_str("DestPadName", avfilter_pad_get_name(link->dstpad, 0));
        print_str("SourceName", link->src->name);

        print_link(w, link);

        avtext_print_section_footer(w); // SECTION_ID_OUTPUT
    }

    avtext_print_section_footer(w); // SECTION_ID_OUTPUTS

    avtext_print_section_footer(w); // SECTION_ID_FILTER
}

static void init_sections(void)
{
    for (unsigned i = 0; i < FF_ARRAY_ELEMS(sections); i++)
        sections[i].show_all_entries = 1;
}

static void print_filtergraph_single(AVTextFormatContext *w, FilterGraph* fg, AVFilterGraph *graph)
{
    char layoutString[64];
    FilterGraphPriv *fgp = fgp_from_fg(fg);

    print_int("GraphIndex", fg->index);
    print_str("Description", fgp->graph_desc);

    avtext_print_section_header(w, NULL, SECTION_ID_INPUTS);

    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilterPriv* ifilter = ifp_from_ifilter(fg->inputs[i]);
        enum AVMediaType mediaType = ifilter->type;

        avtext_print_section_header(w, NULL, SECTION_ID_INPUT);

        print_str("Name1", (char*)ifilter->ifilter.name);

        if (ifilter->filter) {
            print_str("Name2", ifilter->filter->name);
            print_str("Name3", ifilter->filter->filter->name);
            print_str("Description", ifilter->filter->filter->description);
        }

        print_str("MediaType", av_get_media_type_string(mediaType));
        print_int("MediaTypeId", mediaType);

        switch (ifilter->type) {
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_SUBTITLE:
            print_str("Format",  av_x_if_null(av_get_pix_fmt_name(ifilter->format), "?"));
            print_int("Width", ifilter->width);
            print_int("Height", ifilter->height);
            print_q("SAR", ifilter->sample_aspect_ratio, ':');
            break;
        case AVMEDIA_TYPE_AUDIO:

            av_channel_layout_describe(&ifilter->ch_layout, layoutString, sizeof(layoutString));
            print_str("ChannelString", layoutString);
            print_int("SampleRate", ifilter->sample_rate);
            break;
        case AVMEDIA_TYPE_ATTACHMENT:
        case AVMEDIA_TYPE_DATA:
            break;
        }

        if (ifilter->hw_frames_ctx)
            print_hwframescontext(w, (AVHWFramesContext*)ifilter->hw_frames_ctx->data);
        else if (ifilter->filter && ifilter->filter->hw_device_ctx) {
            AVHWDeviceContext* devCtx = (AVHWDeviceContext*)ifilter->filter->hw_device_ctx->data;
            print_hwdevicecontext(w, devCtx);
        }

        avtext_print_section_footer(w); // SECTION_ID_INPUT
    }

    avtext_print_section_footer(w); // SECTION_ID_INPUTS


    avtext_print_section_header(w, NULL, SECTION_ID_OUTPUTS);

    for (int i = 0; i < fg->nb_outputs; i++) {
        OutputFilterPriv *ofilter = ofp_from_ofilter(fg->outputs[i]);
        enum AVMediaType mediaType = AVMEDIA_TYPE_UNKNOWN;

        avtext_print_section_header(w, NULL, SECTION_ID_OUTPUT);
        print_str("Name1", ofilter->name);

        if (ofilter->filter) {
            print_str("Name2", ofilter->filter->name);
            print_str("Name3", ofilter->filter->filter->name);
            print_str("Description", ofilter->filter->filter->description);

            if (ofilter->filter->nb_inputs > 0)
                mediaType = ofilter->filter->inputs[0]->type;
        }

        print_str("MediaType", av_get_media_type_string(mediaType));
        print_int("MediaTypeId", mediaType);

        switch (ofilter->ofilter.type) {
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_SUBTITLE:
            print_str("Format",  av_x_if_null(av_get_pix_fmt_name(ofilter->format), "?"));
            print_int("Width", ofilter->width);
            print_int("Height", ofilter->height);
            break;
        case AVMEDIA_TYPE_AUDIO:

            av_channel_layout_describe(&ofilter->ch_layout, layoutString, sizeof(layoutString));
            print_str("ChannelString", layoutString);
            print_int("SampleRate", ofilter->sample_rate);
            break;
        case AVMEDIA_TYPE_ATTACHMENT:
        case AVMEDIA_TYPE_DATA:
            break;
        }

        if (ofilter->filter && ofilter->filter->hw_device_ctx) {
            AVHWDeviceContext* devCtx = (AVHWDeviceContext*)ofilter->filter->hw_device_ctx->data;
            print_hwdevicecontext(w, devCtx);
        }

        avtext_print_section_footer(w); // SECTION_ID_OUTPUT
    }

    avtext_print_section_footer(w); // SECTION_ID_OUTPUTS


    avtext_print_section_header(w, NULL, SECTION_ID_FILTERS);

    if (graph) {
        for (unsigned i = 0; i < graph->nb_filters; i++) {
            AVFilterContext *filter = graph->filters[i];
            avtext_print_section_header(w, NULL, SECTION_ID_FILTER);

            print_filter(w, filter);

            avtext_print_section_footer(w); // SECTION_ID_FILTER
        }
    }

    avtext_print_section_footer(w); // SECTION_ID_FILTERS
}

int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph)
{
    const AVTextFormatter *text_formatter;
    AVTextFormatContext *tctx;
    AVTextWriterContext *wctx;
    char *w_name, *w_args;
    int ret;
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    AVBPrint *target_buf = &fgp->graph_print_buf;

    init_sections();

    if (target_buf->len)
        av_bprint_finalize(target_buf, NULL);

    av_bprint_init(target_buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (!print_graphs_format)
        print_graphs_format = av_strdup("default");
    if (!print_graphs_format)
        return AVERROR(ENOMEM);

    w_name = av_strtok(print_graphs_format, "=", &w_args);
    if (!w_name) {
        av_log(NULL, AV_LOG_ERROR, "No name specified for the filter graph output format\n");
        return AVERROR(EINVAL);
    }

    text_formatter = avtext_get_formatter_by_name(w_name);
    if (text_formatter == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph  output format with name '%s'\n", w_name);
        return AVERROR(EINVAL);
    }

    ret = avtextwriter_create_buffer(&wctx, target_buf);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "avtextwriter_create_buffer failed. Error code %d\n", ret);
        return AVERROR(EINVAL);
    }

    if ((ret = avtext_context_open(&tctx, text_formatter, wctx, w_args, sections, FF_ARRAY_ELEMS(sections), 0, 0, 0, 0, -1, NULL)) >= 0) {
        avtext_print_section_header(tctx, NULL, SECTION_ID_ROOT);
        avtext_print_section_header(tctx, NULL, SECTION_ID_FILTERGRAPHS);
        avtext_print_section_header(tctx, NULL, SECTION_ID_FILTERGRAPH);

        av_bprint_clear(target_buf);

        print_filtergraph_single(tctx, fg, graph);

        avtext_context_close(&tctx);
        avtextwriter_context_close(&wctx);
    } else
        return ret;

    return 0;
}

int print_filtergraphs(FilterGraph **graphs, int nb_graphs, OutputFile **ofiles, int nb_ofiles)
{
    const AVTextFormatter *text_formatter;
    AVTextFormatContext *tctx;
    AVTextWriterContext *wctx;
    AVBPrint target_buf;
    char *buf, *w_name, *w_args;
    int ret;

    init_sections();

    if (!print_graphs_format)
        print_graphs_format = av_strdup("default");
    if (!print_graphs_format) {
        return AVERROR(ENOMEM);
    }

    w_name = av_strtok(print_graphs_format, "=", &buf);
    if (!w_name) {
        av_log(NULL, AV_LOG_ERROR, "No name specified for the filter graph output format\n");
        return AVERROR(EINVAL);
    }
    w_args = buf;

    text_formatter = avtext_get_formatter_by_name(w_name);
    if (text_formatter == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph  output format with name '%s'\n", w_name);
        return AVERROR(EINVAL);
    }

    av_bprint_init(&target_buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    ret = avtextwriter_create_buffer(&wctx, &target_buf);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "avtextwriter_create_buffer failed. Error code %d\n", ret);
        return AVERROR(EINVAL);
    }

    if ((ret = avtext_context_open(&tctx, text_formatter, wctx, w_args, sections, FF_ARRAY_ELEMS(sections), 0, 0, 0, 0, -1, NULL)) >= 0) {
        avtext_print_section_header(tctx, NULL, SECTION_ID_ROOT);

        avtext_print_section_header(tctx, NULL, SECTION_ID_FILTERGRAPHS);

        for (int i = 0; i < nb_graphs; i++) {

            FilterGraphPriv *fgp = fgp_from_fg(graphs[i]);
            AVBPrint *graph_buf = &fgp->graph_print_buf;

            if (graph_buf->len > 0) {
                avtext_print_section_header(tctx, NULL, SECTION_ID_FILTERGRAPH);

                av_bprint_append_data(&target_buf, graph_buf->str, graph_buf->len);
                av_bprint_finalize(graph_buf, NULL);

                avtext_print_section_footer(tctx); // SECTION_ID_FILTERGRAPH
            }
       }

        for (int n = 0; n < nb_ofiles; n++) {
            OutputFile *of = ofiles[n];

            for (int i = 0; i < of->nb_streams; i++) {
                OutputStream *ost = of->streams[i];

                if (ost->fg_simple) {
                    FilterGraphPriv *fgp = fgp_from_fg(ost->fg_simple);
                    AVBPrint *graph_buf = &fgp->graph_print_buf;

                    if (graph_buf->len > 0) {
                        avtext_print_section_header(tctx, NULL, SECTION_ID_FILTERGRAPH);

                        av_bprint_append_data(&target_buf, graph_buf->str, graph_buf->len);
                        av_bprint_finalize(graph_buf, NULL);

                        avtext_print_section_footer(tctx); // SECTION_ID_FILTERGRAPH
                    }
                }
            }
        }

        avtext_print_section_footer(tctx); // SECTION_ID_FILTERGRAPHS
        avtext_print_section_footer(tctx); // SECTION_ID_ROOT

        if (print_graphs_file) {
            AVIOContext *avio = NULL;

            ret = avio_open2(&avio, print_graphs_file, AVIO_FLAG_WRITE, NULL, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open graph output file, \"%s\": %s\n",
                       print_graphs_file, av_err2str(ret));
                return ret;
            }

            avio_write(avio, (const unsigned char*)target_buf.str, FFMIN(target_buf.len, target_buf.size - 1));
            avio_flush(avio);

            if ((ret = avio_closep(&avio)) < 0)
                av_log(NULL, AV_LOG_ERROR, "Error closing graph output file, loss of information possible: %s\n", av_err2str(ret));
        }

        if (print_graphs) {
            printf("%s", target_buf.str);
            av_log(NULL, AV_LOG_INFO, "%s    %c", target_buf.str, '\n');
        }

        avtext_context_close(&tctx);
        avtextwriter_context_close(&wctx);
    }

    return 0;
}
