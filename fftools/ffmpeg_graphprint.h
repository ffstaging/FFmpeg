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

#ifndef FFTOOLS_FFMPEG_GRAPHPRINT_H
#define FFTOOLS_FFMPEG_GRAPHPRINT_H

#include <stdint.h>

#include "config.h"
#include "ffmpeg.h"
#include "libavutil/avutil.h"
#include "libavutil/bprint.h"
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
    SECTION_ID_HWDEViCECONTEXT,
    SECTION_ID_HWFRAMESCONTEXT,
    SECTION_ID_ERROR,
    SECTION_ID_LOG,
    SECTION_ID_LOGS,

} SectionID;

static struct AVTextFormatSection sections[] = {
    [SECTION_ID_ROOT] =               { SECTION_ID_ROOT, "GraphDescription", AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER,
                                      { SECTION_ID_ERROR, SECTION_ID_PROGRAM_VERSION, SECTION_ID_FILTERGRAPHS, SECTION_ID_LOGS, -1} },
    [SECTION_ID_PROGRAM_VERSION] =    { SECTION_ID_PROGRAM_VERSION, "ProgramVersion", 0, { -1 } },

    [SECTION_ID_FILTERGRAPHS] =       { SECTION_ID_FILTERGRAPHS, "Graphs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTERGRAPH, -1 } },
    [SECTION_ID_FILTERGRAPH] =        { SECTION_ID_FILTERGRAPH, "Graph", 0, { SECTION_ID_INPUTS, SECTION_ID_OUTPUTS, SECTION_ID_FILTERS, -1 },  },

    [SECTION_ID_INPUTS] =             { SECTION_ID_INPUTS, "Inputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_INPUT, SECTION_ID_ERROR, -1 } },
    [SECTION_ID_INPUT] =              { SECTION_ID_INPUT, "Input", 0, { SECTION_ID_HWFRAMESCONTEXT, SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_OUTPUTS] =            { SECTION_ID_OUTPUTS, "Outputs", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_OUTPUT, SECTION_ID_ERROR, -1 } },
    [SECTION_ID_OUTPUT] =             { SECTION_ID_OUTPUT, "Output", 0, { SECTION_ID_HWFRAMESCONTEXT, SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_FILTERS] =            { SECTION_ID_FILTERS, "Filters", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTER, SECTION_ID_ERROR, -1 } },
    [SECTION_ID_FILTER] =             { SECTION_ID_FILTER, "Filter", 0, { SECTION_ID_HWDEViCECONTEXT, SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_HWDEViCECONTEXT] =    { SECTION_ID_HWDEViCECONTEXT, "HwDeviceContext", 0, { SECTION_ID_ERROR, -1 },  },
    [SECTION_ID_HWFRAMESCONTEXT] =    { SECTION_ID_HWFRAMESCONTEXT, "HwFramesContext", 0, { SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_ERROR] =              { SECTION_ID_ERROR, "Error", 0, { -1 } },
    [SECTION_ID_LOGS] =               { SECTION_ID_LOGS, "Log", AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY, { SECTION_ID_LOG, -1 } },
    [SECTION_ID_LOG] =                { SECTION_ID_LOG, "LogEntry", 0, { -1 },  },
};

int print_filtergraphs(FilterGraph **graphs, int nb_graphs, OutputFile **output_files, int nb_output_files);
int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph);

#endif /* FFTOOLS_FFMPEG_GRAPHPRINT_H */
