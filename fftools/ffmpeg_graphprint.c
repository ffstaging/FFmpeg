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

typedef enum {
    SECTION_ID_ROOT,
    SECTION_ID_PROGRAM_VERSION,
    SECTION_ID_FILTERGRAPHS,
    SECTION_ID_FILTERGRAPH,
    SECTION_ID_INPUTS,
    SECTION_ID_INPUT,
    SECTION_ID_OUTPUTS,
    SECTION_ID_OUTPUT,
    SECTION_ID_FILTERS,
    SECTION_ID_FILTER,
    SECTION_ID_HWFRAMESCONTEXT,
} SectionID;

static struct AVTextFormatSection sections[] = {
    [SECTION_ID_ROOT] =               { SECTION_ID_ROOT, "graph_description", AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER,
                                        { SECTION_ID_PROGRAM_VERSION, SECTION_ID_FILTERGRAPHS, -1} },
    [SECTION_ID_PROGRAM_VERSION] =    { SECTION_ID_PROGRAM_VERSION, "program_version", 0, { -1 } },

    [SECTION_ID_FILTERGRAPHS] =       { SECTION_ID_FILTERGRAPHS, "graphs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTERGRAPH, -1 } },
    [SECTION_ID_FILTERGRAPH] =        { SECTION_ID_FILTERGRAPH, "graph", 0, { SECTION_ID_INPUTS, SECTION_ID_OUTPUTS, SECTION_ID_FILTERS, -1 },  },

    [SECTION_ID_INPUTS] =             { SECTION_ID_INPUTS, "inputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_INPUT, -1 } },
    [SECTION_ID_INPUT] =              { SECTION_ID_INPUT, "input", 0, { SECTION_ID_HWFRAMESCONTEXT, -1 },  },

    [SECTION_ID_OUTPUTS] =            { SECTION_ID_OUTPUTS, "outputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_OUTPUT, -1 } },
    [SECTION_ID_OUTPUT] =             { SECTION_ID_OUTPUT, "output", 0, { SECTION_ID_HWFRAMESCONTEXT, -1 },  },

    [SECTION_ID_FILTERS] =            { SECTION_ID_FILTERS, "filters", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTER, -1 } },
    [SECTION_ID_FILTER] =             { SECTION_ID_FILTER, "filter", 0, { -1 },  },

    [SECTION_ID_HWFRAMESCONTEXT] =    { SECTION_ID_HWFRAMESCONTEXT, "hw_frames_context", 0, { -1 },  },
};

/* Text Format API Shortcuts */
#define print_int(k, v)         avtext_print_integer(tfc, k, v)
#define print_q(k, v, s)        avtext_print_rational(tfc, k, v, s)
#define print_str(k, v)         avtext_print_string(tfc, k, v, 0)

static void print_hwdevicecontext(AVTextFormatContext *tfc, const AVHWDeviceContext *hw_device_context)
{
    print_int("has_hw_device_context", 1);
    print_str("hw_device_type", av_hwdevice_get_type_name(hw_device_context->type));
}

static void print_hwframescontext(AVTextFormatContext *tfc, const AVHWFramesContext *hw_frames_context)
{
    const AVPixFmtDescriptor* pix_desc_hw;
    const AVPixFmtDescriptor* pix_desc_sw;

    avtext_print_section_header(tfc, NULL, SECTION_ID_HWFRAMESCONTEXT);

    print_int("has_hw_frames_context", 1);
    print_str("hw_device_type", av_hwdevice_get_type_name(hw_frames_context->device_ctx->type));

    pix_desc_hw = av_pix_fmt_desc_get(hw_frames_context->format);
    if (pix_desc_hw) {
        print_str("hw_pixel_format", pix_desc_hw->name);
        if (pix_desc_hw->alias)
            print_str("hw_pixel_format_alias", pix_desc_hw->alias);
    }

    pix_desc_sw = av_pix_fmt_desc_get(hw_frames_context->sw_format);
    if (pix_desc_sw) {
        print_str("sw_pixel_format", pix_desc_sw->name);
        if (pix_desc_sw->alias)
            print_str("sw_pixel_format_alias", pix_desc_sw->alias);
    }

    print_int("width", hw_frames_context->width);
    print_int("height", hw_frames_context->height);
    print_int("initial_pool_size", hw_frames_context->initial_pool_size);

    avtext_print_section_footer(tfc); // SECTION_ID_HWFRAMESCONTEXT
}

static void print_link(AVTextFormatContext *tfc, AVFilterLink *link)
{
    AVBufferRef *hw_frames_ctx;
    char layout_string[64];

    print_str("media_type", av_get_media_type_string(link->type));

    switch (link->type) {
        case AVMEDIA_TYPE_VIDEO:
            print_str("format", av_x_if_null(av_get_pix_fmt_name(link->format), "?"));
            print_int("width", link->w);
            print_int("height", link->h);
            print_q("sar", link->sample_aspect_ratio, ':');
            print_str("color_range", av_color_range_name(link->color_range));
            print_str("color_space", av_color_space_name(link->colorspace));
            break;

        case AVMEDIA_TYPE_SUBTITLE:
            ////print_str("format", av_x_if_null(av_get_subtitle_fmt_name(link->format), "?"));
            print_int("width", link->w);
            print_int("height", link->h);
            break;

        case AVMEDIA_TYPE_AUDIO:
            av_channel_layout_describe(&link->ch_layout, layout_string, sizeof(layout_string));
            print_str("channel_layout", layout_string);
            print_int("channels", link->ch_layout.nb_channels);
            print_int("sample_rate", link->sample_rate);
            break;
    }

    print_q("time_base", link->time_base, '/');

    hw_frames_ctx = avfilter_link_get_hw_frames_ctx(link);

    if (hw_frames_ctx && hw_frames_ctx->data)
        print_hwframescontext(tfc, (AVHWFramesContext *)hw_frames_ctx->data);
}

static void print_filter(AVTextFormatContext *tfc, const AVFilterContext* filter)
{
    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTER);

    print_str("filter_id", filter->name);

    if (filter->filter) {
        print_str("filter_name", filter->filter->name);
        print_str("description", filter->filter->description);
    }

    if (filter->hw_device_ctx) {
        AVHWDeviceContext* device_context = (AVHWDeviceContext*)filter->hw_device_ctx->data;
        print_hwdevicecontext(tfc, device_context);
        if (filter->extra_hw_frames > 0)
            print_int("extra_hw_frames", filter->extra_hw_frames);
    }

    avtext_print_section_header(tfc, NULL, SECTION_ID_INPUTS);

    for (unsigned i = 0; i < filter->nb_inputs; i++) {
        AVFilterLink *link = filter->inputs[i];
        avtext_print_section_header(tfc, NULL, SECTION_ID_INPUT);

        print_int("input_index", i);
        print_str("pad_name", avfilter_pad_get_name(link->dstpad, 0));;
        print_str("source_filter_id", link->src->name);
        print_str("source_pad_name", avfilter_pad_get_name(link->srcpad, 0));

        print_link(tfc, link);

        avtext_print_section_footer(tfc); // SECTION_ID_INPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_INPUTS

    avtext_print_section_header(tfc, NULL, SECTION_ID_OUTPUTS);

    for (unsigned i = 0; i < filter->nb_outputs; i++) {
        AVFilterLink *link = filter->outputs[i];
        avtext_print_section_header(tfc, NULL, SECTION_ID_OUTPUT);

        print_int("output_index", i);
        print_str("pad_name", avfilter_pad_get_name(link->srcpad, 0));
        print_str("dest_filter_id", link->dst->name);
        print_str("dest_pad_name", avfilter_pad_get_name(link->dstpad, 0));

        print_link(tfc, link);

        avtext_print_section_footer(tfc); // SECTION_ID_OUTPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_OUTPUTS

    avtext_print_section_footer(tfc); // SECTION_ID_FILTER
}

static void init_sections(void)
{
    for (unsigned i = 0; i < FF_ARRAY_ELEMS(sections); i++)
        sections[i].show_all_entries = 1;
}

static void print_filtergraph_single(AVTextFormatContext *tfc, FilterGraph *fg, AVFilterGraph *graph)
{
    FilterGraphPriv *fgp = fgp_from_fg(fg);

    print_int("graph_index", fg->index);
    print_str("description", fgp->graph_desc);

    avtext_print_section_header(tfc, NULL, SECTION_ID_INPUTS);

    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilterPriv* ifilter = ifp_from_ifilter(fg->inputs[i]);
        enum AVMediaType media_type = ifilter->type;

        avtext_print_section_header(tfc, NULL, SECTION_ID_INPUT);

        print_int("input_index", ifilter->index);

        if (ifilter->linklabel)
            print_str("link_label", (const char*)ifilter->linklabel);

        if (ifilter->filter) {
            print_str("filter_id", ifilter->filter->name);
            print_str("filter_name", ifilter->filter->filter->name);
        }

        print_str("media_type", av_get_media_type_string(media_type));

        avtext_print_section_footer(tfc); // SECTION_ID_INPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_INPUTS

    avtext_print_section_header(tfc, NULL, SECTION_ID_OUTPUTS);

    for (int i = 0; i < fg->nb_outputs; i++) {
        OutputFilterPriv *ofilter = ofp_from_ofilter(fg->outputs[i]);

        avtext_print_section_header(tfc, NULL, SECTION_ID_OUTPUT);

        print_int("output_index", ofilter->index);

        print_str("name", ofilter->name);

        if (fg->outputs[i]->linklabel)
            print_str("link_label", (const char*)fg->outputs[i]->linklabel);

        if (ofilter->filter) {
            print_str("filter_id", ofilter->filter->name);
            print_str("filter_name", ofilter->filter->filter->name);
        }

        print_str("media_type", av_get_media_type_string(fg->outputs[i]->type));

        avtext_print_section_footer(tfc); // SECTION_ID_OUTPUT
    }

    avtext_print_section_footer(tfc); // SECTION_ID_OUTPUTS

    avtext_print_section_header(tfc, NULL, SECTION_ID_FILTERS);

    if (graph) {
        for (unsigned i = 0; i < graph->nb_filters; i++) {
            AVFilterContext *filter = graph->filters[i];
            avtext_print_section_header(tfc, NULL, SECTION_ID_FILTER);

            print_filter(tfc, filter);

            avtext_print_section_footer(tfc); // SECTION_ID_FILTER
        }
    }

    avtext_print_section_footer(tfc); // SECTION_ID_FILTERS
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
    if (!text_formatter ) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph output format with name '%s'\n", w_name);
        return AVERROR(EINVAL);
    }

    ret = avtextwriter_create_buffer(&wctx, target_buf);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "avtextwriter_create_buffer failed. Error code %d\n", ret);
        return AVERROR(EINVAL);
    }

    if ((ret = avtext_context_open(&tctx, text_formatter, wctx, w_args, sections, FF_ARRAY_ELEMS(sections), 0, 0, 0, 0, -1, NULL)) >= 0) {

        // Due to the threading model each graph needs to print itself into a buffer
        // from its own thread. The actual printing happens short before cleanup in ffmpeg.c
        // where all grahps are assembled together. To make this work, we need to put the
        // formatting context into the same state like it would be when printing all at once,
        // so here we print the section headers and clear the buffer to get into the right state.
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
    if (!text_formatter) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph output format with name '%s'\n", w_name);
        return AVERROR(EINVAL);
    }

    av_bprint_init(&target_buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    ret = avtextwriter_create_buffer(&wctx, &target_buf);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "avtextwriter_create_buffer failed. Error code %d\n", ret);
        return ret;
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

            if (!strcmp(print_graphs_file, "-")) {
                printf("%s", target_buf.str);
            } else {
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
        }

        if (print_graphs)
            av_log(NULL, AV_LOG_INFO, "%s    %c", target_buf.str, '\n');

        avtext_context_close(&tctx);
        avtextwriter_context_close(&wctx);
    }

    return 0;
}
