/*
 * Copyright (c) The ffmpeg developers
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

#ifndef AVUTIL_AVTEXTFORMAT_H
#define AVUTIL_AVTEXTFORMAT_H

#include <stddef.h>
#include <stdint.h>
#include "attributes.h"
#include "dict.h"
#include <libavformat/avio.h>
#include "bprint.h"
#include "rational.h"

#define SECTION_MAX_NB_CHILDREN 11


struct AVTextFormatSection {
    int id;             ///< unique id identifying a section
    const char *name;

#define SECTION_FLAG_IS_WRAPPER      1 ///< the section only contains other sections, but has no data at its own level
#define SECTION_FLAG_IS_ARRAY        2 ///< the section contains an array of elements of the same type
#define SECTION_FLAG_HAS_VARIABLE_FIELDS 4 ///< the section may contain a variable number of fields with variable keys.
                                           ///  For these sections the element_name field is mandatory.
#define SECTION_FLAG_HAS_TYPE        8 ///< the section contains a type to distinguish multiple nested elements

    int flags;
    const int children_ids[SECTION_MAX_NB_CHILDREN+1]; ///< list of children section IDS, terminated by -1
    const char *element_name; ///< name of the contained element, if provided
    const char *unique_name;  ///< unique section name, in case the name is ambiguous
    AVDictionary *entries_to_show;
    const char *(* get_type)(const void *data); ///< function returning a type if defined, must be defined when SECTION_FLAG_HAS_TYPE is defined
    int show_all_entries;
};

typedef struct AVTextFormatContext AVTextFormatContext;

#define WRITER_FLAG_DISPLAY_OPTIONAL_FIELDS 1
#define WRITER_FLAG_PUT_PACKETS_AND_FRAMES_IN_SAME_CHAPTER 2

typedef enum {
    WRITER_STRING_VALIDATION_FAIL,
    WRITER_STRING_VALIDATION_REPLACE,
    WRITER_STRING_VALIDATION_IGNORE,
    WRITER_STRING_VALIDATION_NB
} StringValidation;

typedef struct AVTextFormatter {
    const AVClass *priv_class;      ///< private class of the writer, if any
    int priv_size;                  ///< private size for the writer context
    const char *name;

    int  (*init)  (AVTextFormatContext *wctx);
    void (*uninit)(AVTextFormatContext *wctx);

    void (*print_section_header)(AVTextFormatContext *wctx, const void *data);
    void (*print_section_footer)(AVTextFormatContext *wctx);
    void (*print_integer)       (AVTextFormatContext *wctx, const char *, int64_t);
    void (*print_rational)      (AVTextFormatContext *wctx, AVRational *q, char *sep);
    void (*print_string)        (AVTextFormatContext *wctx, const char *, const char *);
    int flags;                  ///< a combination or WRITER_FLAG_*
} AVTextFormatter;

#define SECTION_MAX_NB_LEVELS 12

struct AVTextFormatContext {
    const AVClass *class;           ///< class of the writer
    const AVTextFormatter *writer;           ///< the AVTextFormatter of which this is an instance
    AVIOContext *avio;              ///< the I/O context used to write

    void (* writer_w8)(AVTextFormatContext *wctx, int b);
    void (* writer_put_str)(AVTextFormatContext *wctx, const char *str);
    void (* writer_printf)(AVTextFormatContext *wctx, const char *fmt, ...);

    char *name;                     ///< name of this writer instance
    void *priv;                     ///< private data for use by the filter

    const struct AVTextFormatSection *sections; ///< array containing all sections
    int nb_sections;                ///< number of sections

    int level;                      ///< current level, starting from 0

    /** number of the item printed in the given section, starting from 0 */
    unsigned int nb_item[SECTION_MAX_NB_LEVELS];

    /** section per each level */
    const struct AVTextFormatSection *section[SECTION_MAX_NB_LEVELS];
    AVBPrint section_pbuf[SECTION_MAX_NB_LEVELS]; ///< generic print buffer dedicated to each section,
                                                  ///  used by various writers

    unsigned int nb_section_packet; ///< number of the packet section in case we are in "packets_and_frames" section
    unsigned int nb_section_frame;  ///< number of the frame  section in case we are in "packets_and_frames" section
    unsigned int nb_section_packet_frame; ///< nb_section_packet or nb_section_frame according if is_packets_and_frames

    int string_validation;
    char *string_validation_replacement;
    unsigned int string_validation_utf8_flags;
};



#endif /* AVUTIL_AVTEXTFORMAT_H */
