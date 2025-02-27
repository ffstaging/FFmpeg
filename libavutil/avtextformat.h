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
#include "libavutil/hash.h"

#define SECTION_MAX_NB_CHILDREN 11


struct AVTextFormatSection {
    int id;             ///< unique id identifying a section
    const char *name;

#define AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER      1 ///< the section only contains other sections, but has no data at its own level
#define AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY        2 ///< the section contains an array of elements of the same type
#define AV_TEXTFORMAT_SECTION_FLAG_HAS_VARIABLE_FIELDS 4 ///< the section may contain a variable number of fields with variable keys.
                                           ///  For these sections the element_name field is mandatory.
#define AV_TEXTFORMAT_SECTION_FLAG_HAS_TYPE        8 ///< the section contains a type to distinguish multiple nested elements
#define AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE 16 ///< the items in this array section should be numbered individually by type

    int flags;
    const int children_ids[SECTION_MAX_NB_CHILDREN+1]; ///< list of children section IDS, terminated by -1
    const char *element_name; ///< name of the contained element, if provided
    const char *unique_name;  ///< unique section name, in case the name is ambiguous
    AVDictionary *entries_to_show;
    const char *(* get_type)(const void *data); ///< function returning a type if defined, must be defined when SECTION_FLAG_HAS_TYPE is defined
    int show_all_entries;
};

typedef struct AVTextFormatContext AVTextFormatContext;

#define AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS 1
#define AV_TEXTFORMAT_FLAG_SUPPORTS_MIXED_ARRAY_CONTENT 2

typedef enum {
    AV_TEXTFORMAT_STRING_VALIDATION_FAIL,
    AV_TEXTFORMAT_STRING_VALIDATION_REPLACE,
    AV_TEXTFORMAT_STRING_VALIDATION_IGNORE,
    AV_TEXTFORMAT_STRING_VALIDATION_NB
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
    int flags;                  ///< a combination or AV_TEXTFORMAT__FLAG_*
} AVTextFormatter;

#define SECTION_MAX_NB_LEVELS    12
#define SECTION_MAX_NB_SECTIONS 100

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
    unsigned int nb_item_type[SECTION_MAX_NB_LEVELS][SECTION_MAX_NB_SECTIONS];

    /** section per each level */
    const struct AVTextFormatSection *section[SECTION_MAX_NB_LEVELS];
    AVBPrint section_pbuf[SECTION_MAX_NB_LEVELS]; ///< generic print buffer dedicated to each section,
                                                  ///  used by various writers

    int show_optional_fields;
    int show_value_unit;
    int use_value_prefix;
    int use_byte_value_binary_prefix;
    int use_value_sexagesimal_format;

    struct AVHashContext *hash;

    int string_validation;
    char *string_validation_replacement;
    unsigned int string_validation_utf8_flags;
};

#define SHOW_OPTIONAL_FIELDS_AUTO       -1
#define SHOW_OPTIONAL_FIELDS_NEVER       0
#define SHOW_OPTIONAL_FIELDS_ALWAYS      1

#define AV_TEXTFORMAT_PRINT_STRING_OPTIONAL 1
#define AV_TEXTFORMAT_PRINT_STRING_VALIDATE 2

int avtext_context_open(AVTextFormatContext **pwctx, const AVTextFormatter *writer, const char *args,
                        const struct AVTextFormatSection *sections, int nb_sections,
                        const char *output_filename,
                        int show_value_unit,
                        int use_value_prefix,
                        int use_byte_value_binary_prefix,
                        int use_value_sexagesimal_format,
                        int show_optional_fields,
                        char *show_data_hash);

int avtext_context_close(AVTextFormatContext **wctx);


void avtext_print_section_header(AVTextFormatContext *wctx, const void *data, int section_id);

void avtext_print_section_footer(AVTextFormatContext *wctx);

void avtext_print_integer(AVTextFormatContext *wctx, const char *key, int64_t val);

int avtext_print_string(AVTextFormatContext *wctx, const char *key, const char *val, int flags);

void avtext_print_unit_int(AVTextFormatContext *wctx, const char *key, int value, const char *unit);

void avtext_print_rational(AVTextFormatContext *wctx, const char *key, AVRational q, char sep);

void avtext_print_time(AVTextFormatContext *wctx, const char *key, int64_t ts, const AVRational *time_base, int is_duration);

void avtext_print_ts(AVTextFormatContext *wctx, const char *key, int64_t ts, int is_duration);

void avtext_print_data(AVTextFormatContext *wctx, const char *name, const uint8_t *data, int size);

void avtext_print_data_hash(AVTextFormatContext *wctx, const char *name, const uint8_t *data, int size);

void avtext_print_integers(AVTextFormatContext *wctx, const char *name, uint8_t *data, int size, 
                           const char *format, int columns, int bytes, int offset_add);

#endif /* AVUTIL_AVTEXTFORMAT_H */
