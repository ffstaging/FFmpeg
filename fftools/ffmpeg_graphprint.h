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

#define SECTION_MAX_NB_CHILDREN 11
#define PRINT_STRING_OPT      1
#define PRINT_STRING_VALIDATE 2
#define MAX_REGISTERED_WRITERS_NB 64

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[ 8 ];
} GUID;

#define IsEqualGUID(rguid1, rguid2) (!memcmp(rguid1, rguid2, sizeof(GUID)))

#endif

struct section {
    int id;             ///< unique id identifying a section
    const char *name;

#define SECTION_FLAG_IS_WRAPPER      1 ///< the section only contains other sections, but has no data at its own level
#define SECTION_FLAG_IS_ARRAY        2 ///< the section contains an array of elements of the same type
#define SECTION_FLAG_HAS_VARIABLE_FIELDS 4 ///< the section may contain a variable number of fields with variable keys.
                                           ///  For these sections the element_name field is mandatory.
    int flags;
    int children_ids[SECTION_MAX_NB_CHILDREN+1]; ///< list of children section IDS, terminated by -1
    const char *element_name; ///< name of the contained element, if provided
    const char *unique_name;  ///< unique section name, in case the name is ambiguous
};

typedef enum {
    SECTION_ID_NONE = -1,
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

static const struct section sections[] = {
    [SECTION_ID_ROOT] =               { SECTION_ID_ROOT, "GraphDescription", SECTION_FLAG_IS_WRAPPER,
                                      { SECTION_ID_ERROR, SECTION_ID_PROGRAM_VERSION, SECTION_ID_FILTERGRAPHS, SECTION_ID_LOGS, -1} },
    [SECTION_ID_PROGRAM_VERSION] =    { SECTION_ID_PROGRAM_VERSION, "ProgramVersion", 0, { -1 } },

    [SECTION_ID_FILTERGRAPHS] =       { SECTION_ID_FILTERGRAPHS, "Graphs", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTERGRAPH, -1 } },
    [SECTION_ID_FILTERGRAPH] =        { SECTION_ID_FILTERGRAPH, "Graph", 0, { SECTION_ID_INPUTS, SECTION_ID_OUTPUTS, SECTION_ID_FILTERS, -1 },  },

    [SECTION_ID_INPUTS] =             { SECTION_ID_INPUTS, "Inputs", SECTION_FLAG_IS_ARRAY, { SECTION_ID_INPUT, SECTION_ID_ERROR, -1 } },
    [SECTION_ID_INPUT] =              { SECTION_ID_INPUT, "Input", 0, { SECTION_ID_HWFRAMESCONTEXT, SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_OUTPUTS] =            { SECTION_ID_OUTPUTS, "Outputs", SECTION_FLAG_IS_ARRAY, { SECTION_ID_OUTPUT, SECTION_ID_ERROR, -1 } },
    [SECTION_ID_OUTPUT] =             { SECTION_ID_OUTPUT, "Output", 0, { SECTION_ID_HWFRAMESCONTEXT, SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_FILTERS] =            { SECTION_ID_FILTERS, "Filters", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FILTER, SECTION_ID_ERROR, -1 } },
    [SECTION_ID_FILTER] =             { SECTION_ID_FILTER, "Filter", 0, { SECTION_ID_HWDEViCECONTEXT, SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_HWDEViCECONTEXT] =    { SECTION_ID_HWDEViCECONTEXT, "HwDeviceContext", 0, { SECTION_ID_ERROR, -1 },  },
    [SECTION_ID_HWFRAMESCONTEXT] =    { SECTION_ID_HWFRAMESCONTEXT, "HwFramesContext", 0, { SECTION_ID_ERROR, -1 },  },

    [SECTION_ID_ERROR] =              { SECTION_ID_ERROR, "Error", 0, { -1 } },
    [SECTION_ID_LOGS] =               { SECTION_ID_LOGS, "Log", SECTION_FLAG_IS_ARRAY, { SECTION_ID_LOG, -1 } },
    [SECTION_ID_LOG] =                { SECTION_ID_LOG, "LogEntry", 0, { -1 },  },
};

struct unit_value {
    union { double d; long long int i; } val;
    const char *unit;
};

static const char unit_second_str[]         = "s"    ;
static const char unit_hertz_str[]          = "Hz"   ;
static const char unit_byte_str[]           = "byte" ;
static const char unit_bit_per_second_str[] = "bit/s";

/* WRITERS API */

typedef struct WriterContext WriterContext;

#define WRITER_FLAG_DISPLAY_OPTIONAL_FIELDS 1
#define WRITER_FLAG_PUT_PACKETS_AND_FRAMES_IN_SAME_CHAPTER 2

typedef enum {
    WRITER_STRING_VALIDATION_FAIL,
    WRITER_STRING_VALIDATION_REPLACE,
    WRITER_STRING_VALIDATION_IGNORE,
    WRITER_STRING_VALIDATION_NB
} StringValidation;

typedef struct Writer {
    const AVClass *priv_class;      ///< private class of the writer, if any
    int priv_size;                  ///< private size for the writer context
    const char *name;

    int  (*init)  (WriterContext *wctx);
    void (*uninit)(WriterContext *wctx);

    void (*print_section_header)(WriterContext *wctx);
    void (*print_section_footer)(WriterContext *wctx);
    void (*print_integer)       (WriterContext *wctx, const char *, long long int);
    void (*print_rational)      (WriterContext *wctx, AVRational *q, char *sep);
    void (*print_string)        (WriterContext *wctx, const char *, const char *);
    int flags;                  ///< a combination or WRITER_FLAG_*
} Writer;

#define SECTION_MAX_NB_LEVELS 10

struct WriterContext {
    const AVClass *class;           ///< class of the writer
    const Writer *writer;           ///< the Writer of which this is an instance
    char *name;                     ///< name of this writer instance
    void *priv;                     ///< private data for use by the filter

    const struct section *sections; ///< array containing all sections
    int nb_sections;                ///< number of sections

    int level;                      ///< current level, starting from 0

    /** number of the item printed in the given section, starting from 0 */
    unsigned int nb_item[SECTION_MAX_NB_LEVELS];

    /** section per each level */
    const struct section *section[SECTION_MAX_NB_LEVELS];
    AVBPrint section_pbuf[SECTION_MAX_NB_LEVELS]; ///< generic print buffer dedicated to each section,
                                                  ///  used by various writers
    AVBPrint bpBuf;
    unsigned int nb_section_packet; ///< number of the packet section in case we are in "packets_and_frames" section
    unsigned int nb_section_frame;  ///< number of the frame  section in case we are in "packets_and_frames" section
    unsigned int nb_section_packet_frame; ///< nb_section_packet or nb_section_frame according if is_packets_and_frames

    int string_validation;
    char *string_validation_replacement;
    unsigned int string_validation_utf8_flags;
};

#define print_fmt(k, f, ...) do {              \
    av_bprint_clear(&pbuf);                    \
    av_bprintf(&pbuf, f, __VA_ARGS__);         \
    writer_print_string(w, k, pbuf.str, 0);    \
} while (0)

#define print_int(k, v)         writer_print_integer(w, k, v)
#define print_q(k, v, s)        writer_print_rational(w, k, v, s)
#define print_guid(k, v)        writer_print_guid(w, k, v)
#define print_str(k, v)         writer_print_string(w, k, v, 0)
#define print_str_opt(k, v)     writer_print_string(w, k, v, PRINT_STRING_OPT)
#define print_str_validate(k, v) writer_print_string(w, k, v, PRINT_STRING_VALIDATE)
#define print_time(k, v, tb)    writer_print_time(w, k, v, tb, 0)
#define print_ts(k, v)          writer_print_ts(w, k, v, 0)
#define print_duration_time(k, v, tb) writer_print_time(w, k, v, tb, 1)
#define print_duration_ts(k, v)       writer_print_ts(w, k, v, 1)
#define print_val(k, v, u) do {                                     \
    struct unit_value uv;                                           \
    uv.val.i = v;                                                   \
    uv.unit = u;                                                    \
    writer_print_string(w, k, value_string(val_str, sizeof(val_str), uv), 0); \
} while (0)

void writer_register_all(void);
int print_filtergraphs(FilterGraph **graphs, int nb_graphs, OutputFile **output_files, int nb_output_files);
int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph);

const Writer *writer_get_by_name(const char *name);

int writer_open(WriterContext **wctx, const Writer *writer, const char *args,
                       const struct section *sections, int nb_sections);

void writer_print_section_header(WriterContext *wctx, int section_id);
void writer_print_section_footer(WriterContext *wctx);

int writer_print_string(WriterContext *wctx, const char *key, const char *val, int flags);
void writer_print_integer(WriterContext *wctx, const char *key, long long int val);
void writer_print_rational(WriterContext *wctx, const char *key, AVRational q, char sep);
void writer_print_guid(WriterContext *wctx, const char *key, GUID *guid);
void writer_print_ts(WriterContext *wctx, const char *key, int64_t ts, int is_duration);

void writer_close(WriterContext **wctx);
void write_error(WriterContext *w, int err);
void write_error_msg(WriterContext *w, int err, const char *msg);
void write_error_fmt(WriterContext *w, int err, const char *fmt,...);

#endif /* FFTOOLS_FFMPEG_GRAPHPRINT_H */
