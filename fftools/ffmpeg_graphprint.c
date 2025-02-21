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
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/common.h"
#include "libavfilter/avfilter.h"
#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"

static const char *writer_get_name(void *p)
{
    WriterContext *wctx = p;
    return wctx->writer->name;
}

#define OFFSET(x) offsetof(WriterContext, x)

static const AVOption writer_options[] = {
    { "string_validation", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, {.i64=WRITER_STRING_VALIDATION_REPLACE}, 0, WRITER_STRING_VALIDATION_NB-1, .unit = "sv" },
    { "sv", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, {.i64=WRITER_STRING_VALIDATION_REPLACE}, 0, WRITER_STRING_VALIDATION_NB-1, .unit = "sv" },
    { "ignore",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = WRITER_STRING_VALIDATION_IGNORE},  .unit = "sv" },
    { "replace", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = WRITER_STRING_VALIDATION_REPLACE}, .unit = "sv" },
    { "fail",    NULL, 0, AV_OPT_TYPE_CONST, {.i64 = WRITER_STRING_VALIDATION_FAIL},    .unit = "sv" },
    { "string_validation_replacement", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, {.str=""}},
    { "svr", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, {.str="\xEF\xBF\xBD"}},
    { NULL }
};

static void *writer_child_next(void *obj, void *prev)
{
    WriterContext *ctx = obj;
    if (!prev && ctx->writer && ctx->writer->priv_class && ctx->priv)
        return ctx->priv;
    return NULL;
}

static const AVClass writer_class = {
    .class_name = "Writer",
    .item_name  = writer_get_name,
    .option     = writer_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .child_next = writer_child_next,
};

void writer_close(WriterContext **wctx)
{
    int i;

    if (!*wctx)
        return;

    if ((*wctx)->writer->uninit)
        (*wctx)->writer->uninit(*wctx);
    for (i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_finalize(&(*wctx)->section_pbuf[i], NULL);
    av_bprint_finalize(&(*wctx)->bpBuf, NULL);
    if ((*wctx)->writer->priv_class)
        av_opt_free((*wctx)->priv);
    av_freep(&((*wctx)->priv));
    av_opt_free(*wctx);
    av_freep(wctx);
}

static void bprint_bytes(AVBPrint *bp, const uint8_t *ubuf, size_t ubuf_size)
{
    av_bprintf(bp, "0X");
    for (size_t i = 0; i < ubuf_size; i++)
        av_bprintf(bp, "%02X", ubuf[i]);
}

int writer_open(WriterContext **wctx, const Writer *writer, const char *args,
                       const struct section *sections1, int nb_sections)
{
    int i, ret = 0;

    if (!(*wctx = av_mallocz(sizeof(WriterContext)))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (!((*wctx)->priv = av_mallocz(writer->priv_size))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    (*wctx)->class = &writer_class;
    (*wctx)->writer = writer;
    (*wctx)->level = -1;
    (*wctx)->sections = sections;
    (*wctx)->nb_sections = nb_sections;

    av_opt_set_defaults(*wctx);

    if (writer->priv_class) {
        void *priv_ctx = (*wctx)->priv;
        *((const AVClass **)priv_ctx) = writer->priv_class;
        av_opt_set_defaults(priv_ctx);
    }

    /* convert options to dictionary */
    if (args) {
        AVDictionary *opts = NULL;
        AVDictionaryEntry *opt = NULL;

        if ((ret = av_dict_parse_string(&opts, args, "=", ":", 0)) < 0) {
            av_log(*wctx, AV_LOG_ERROR, "Failed to parse option string '%s' provided to writer context\n", args);
            av_dict_free(&opts);
            goto fail;
        }

        while ((opt = av_dict_get(opts, "", opt, AV_DICT_IGNORE_SUFFIX))) {
            if ((ret = av_opt_set(*wctx, opt->key, opt->value, AV_OPT_SEARCH_CHILDREN)) < 0) {
                av_log(*wctx, AV_LOG_ERROR, "Failed to set option '%s' with value '%s' provided to writer context\n",
                       opt->key, opt->value);
                av_dict_free(&opts);
                goto fail;
            }
        }

        av_dict_free(&opts);
    }

    /* validate replace string */
    {
        const uint8_t *p = (*wctx)->string_validation_replacement;
        const uint8_t *endp = p + strlen(p);
        while (*p) {
            const uint8_t *p0 = p;
            int32_t code;
            ret = av_utf8_decode(&code, &p, endp, (*wctx)->string_validation_utf8_flags);
            if (ret < 0) {
                AVBPrint bp;
                av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                bprint_bytes(&bp, p0, p-p0),
                    av_log(wctx, AV_LOG_ERROR,
                           "Invalid UTF8 sequence %s found in string validation replace '%s'\n",
                           bp.str, (*wctx)->string_validation_replacement);
                return ret;
            }
        }
    }

    for (i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_init(&(*wctx)->section_pbuf[i], 1, AV_BPRINT_SIZE_UNLIMITED);

    av_bprint_init(&(*wctx)->bpBuf, 500000, AV_BPRINT_SIZE_UNLIMITED);

    if ((*wctx)->writer->init)
        ret = (*wctx)->writer->init(*wctx);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    writer_close(wctx);
    return ret;
}

void writer_print_section_header(WriterContext *wctx, int section_id)
{
    //int parent_section_id;
    wctx->level++;
    av_assert0(wctx->level < SECTION_MAX_NB_LEVELS);
    //parent_section_id = wctx->level ?
    //    (wctx->section[wctx->level-1])->id : SECTION_ID_NONE;

    wctx->nb_item[wctx->level] = 0;
    wctx->section[wctx->level] = &wctx->sections[section_id];

    if (wctx->writer->print_section_header)
        wctx->writer->print_section_header(wctx);
}

void writer_print_section_footer(WriterContext *wctx)
{
    //int section_id = wctx->section[wctx->level]->id;
    int parent_section_id = wctx->level ?
        wctx->section[wctx->level-1]->id : SECTION_ID_NONE;

    if (parent_section_id != SECTION_ID_NONE)
        wctx->nb_item[wctx->level-1]++;

    if (wctx->writer->print_section_footer)
        wctx->writer->print_section_footer(wctx);
    wctx->level--;
}

static inline int validate_string(WriterContext *wctx, char **dstp, const char *src)
{
    const uint8_t *p, *endp;
    AVBPrint dstbuf;
    int invalid_chars_nb = 0, ret = 0;

    av_bprint_init(&dstbuf, 0, AV_BPRINT_SIZE_UNLIMITED);

    endp = src + strlen(src);
    for (p = (uint8_t *)src; *p;) {
        uint32_t code;
        int invalid = 0;
        const uint8_t *p0 = p;

        if (av_utf8_decode(&code, &p, endp, wctx->string_validation_utf8_flags) < 0) {
            AVBPrint bp;
            av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
            bprint_bytes(&bp, p0, p-p0);
            av_log(wctx, AV_LOG_DEBUG,
                   "Invalid UTF-8 sequence %s found in string '%s'\n", bp.str, src);
            invalid = 1;
        }

        if (invalid) {
            invalid_chars_nb++;

            switch (wctx->string_validation) {
            case WRITER_STRING_VALIDATION_FAIL:
                av_log(wctx, AV_LOG_ERROR,
                       "Invalid UTF-8 sequence found in string '%s'\n", src);
                ret = AVERROR_INVALIDDATA;
                goto end;

            case WRITER_STRING_VALIDATION_REPLACE:
                av_bprintf(&dstbuf, "%s", wctx->string_validation_replacement);
                break;
            }
        }

        if (!invalid || wctx->string_validation == WRITER_STRING_VALIDATION_IGNORE)
            av_bprint_append_data(&dstbuf, p0, p-p0);
    }

    if (invalid_chars_nb && wctx->string_validation == WRITER_STRING_VALIDATION_REPLACE) {
        av_log(wctx, AV_LOG_WARNING,
               "%d invalid UTF-8 sequence(s) found in string '%s', replaced with '%s'\n",
               invalid_chars_nb, src, wctx->string_validation_replacement);
    }

end:
    av_bprint_finalize(&dstbuf, dstp);
    return ret;
}

int writer_print_string(WriterContext *wctx, const char *key, const char *val, int flags)
{
    const struct section *section = wctx->section[wctx->level];
    int ret = 0;

    if ((flags & PRINT_STRING_OPT)
        && !(wctx->writer->flags & WRITER_FLAG_DISPLAY_OPTIONAL_FIELDS))
        return 0;

    if (val == NULL)
        return 0;

    if (flags & PRINT_STRING_VALIDATE) {
        char *key1 = NULL, *val1 = NULL;
        ret = validate_string(wctx, &key1, key);
        if (ret < 0) goto end;
        ret = validate_string(wctx, &val1, val);
        if (ret < 0) goto end;
        wctx->writer->print_string(wctx, key1, val1);
    end:
        if (ret < 0) {
            av_log(wctx, AV_LOG_ERROR,
                    "Invalid key=value string combination %s=%s in section %s\n",
                    key, val, section->unique_name);
        }
        av_free(key1);
        av_free(val1);
    } else {
        wctx->writer->print_string(wctx, key, val);
    }

    wctx->nb_item[wctx->level]++;

    return ret;
}

void writer_print_integer(WriterContext *wctx, const char *key, long long int val)
{
    //const struct section *section = wctx->section[wctx->level];

    wctx->writer->print_integer(wctx, key, val);
    wctx->nb_item[wctx->level]++;
}

void writer_print_rational(WriterContext *wctx, const char *key, AVRational q, char sep)
{
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf, "%d%c%d", q.num, sep, q.den);
    writer_print_string(wctx, key, buf.str, 0);
}

void writer_print_guid(WriterContext *wctx, const char *key, GUID *guid)
{
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf, "{%8.8x-%4.4x-%4.4x-%2.2x%2.2x-%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x}",
             (unsigned) guid->Data1, guid->Data2, guid->Data3,
             guid->Data4[0], guid->Data4[1],
             guid->Data4[2], guid->Data4[3],
             guid->Data4[4], guid->Data4[5],
             guid->Data4[6], guid->Data4[7]);

    writer_print_string(wctx, key, buf.str, 0);
}

//writer_print_time(WriterContext *wctx, const char *key, int64_t ts, const AVRational *time_base, int is_duration)
//{
//    char buf[128];
//
//    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
//        writer_print_string(wctx, key, "N/A", PRINT_STRING_OPT);
//    } else {
//        double d = ts * av_q2d(*time_base);
//        struct unit_value uv;
//        uv.val.d = d;
//        uv.unit = unit_second_str;
//        value_string(buf, sizeof(buf), uv);
//        writer_print_string(wctx, key, buf, 0);
//    }
//}

void writer_print_ts(WriterContext *wctx, const char *key, int64_t ts, int is_duration)
{
    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
        writer_print_string(wctx, key, "N/A", PRINT_STRING_OPT);
    } else {
        writer_print_integer(wctx, key, ts);
    }
}


static const Writer *registered_writers[MAX_REGISTERED_WRITERS_NB + 1];

static int writer_register(const Writer *writer)
{
    static int next_registered_writer_idx = 0;

    if (next_registered_writer_idx == MAX_REGISTERED_WRITERS_NB)
        return AVERROR(ENOMEM);

    registered_writers[next_registered_writer_idx++] = writer;
    return 0;
}

const Writer *writer_get_by_name(const char *name)
{
    int i;

    for (i = 0; registered_writers[i]; i++)
        if (!strcmp(registered_writers[i]->name, name))
            return registered_writers[i];

    return NULL;
}

/* WRITERS */

#define DEFINE_WRITER_CLASS(name)                   \
static const char *name##_get_name(void *ctx)       \
{                                                   \
    return #name ;                                  \
}                                                   \
static const AVClass name##_class = {               \
    .class_name = #name,                            \
    .item_name  = name##_get_name,                  \
    .option     = name##_options                    \
}

/* Default output */

typedef struct DefaultContext {
    const AVClass *class;
    int nokey;
    int noprint_wrappers;
    int nested_section[SECTION_MAX_NB_LEVELS];
} DefaultContext;

#undef OFFSET
#define OFFSET(x) offsetof(DefaultContext, x)

static const AVOption default_options[] = {
    { "noprint_wrappers", "do not print headers and footers", OFFSET(noprint_wrappers), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { "nw",               "do not print headers and footers", OFFSET(noprint_wrappers), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { "nokey",          "force no key printing",     OFFSET(nokey),          AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { "nk",             "force no key printing",     OFFSET(nokey),          AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    {NULL},
};

DEFINE_WRITER_CLASS(default);

/* lame uppercasing routine, assumes the string is lower case ASCII */
static inline char *upcase_string(char *dst, size_t dst_size, const char *src)
{
    size_t i;
    for (i = 0; src[i] && i < dst_size-1; i++)
        dst[i] = av_toupper(src[i]);
    dst[i] = 0;
    return dst;
}

static void default_print_section_header(WriterContext *wctx)
{
    DefaultContext *def = wctx->priv;
    char buf[32];
    const struct section *section = wctx->section[wctx->level];
    const struct section *parent_section = wctx->level ?
        wctx->section[wctx->level-1] : NULL;

    av_bprint_clear(&wctx->section_pbuf[wctx->level]);
    if (parent_section &&
        !(parent_section->flags & (SECTION_FLAG_IS_WRAPPER|SECTION_FLAG_IS_ARRAY))) {
        def->nested_section[wctx->level] = 1;
        av_bprintf(&wctx->section_pbuf[wctx->level], "%s%s:",
                   wctx->section_pbuf[wctx->level-1].str,
                   upcase_string(buf, sizeof(buf),
                                 av_x_if_null(section->element_name, section->name)));
    }

    if (def->noprint_wrappers || def->nested_section[wctx->level])
        return;

    if (!(section->flags & (SECTION_FLAG_IS_WRAPPER|SECTION_FLAG_IS_ARRAY)))
        av_bprintf(&wctx->bpBuf, "[%s]\n", upcase_string(buf, sizeof(buf), section->name));
}

static void default_print_section_footer(WriterContext *wctx)
{
    DefaultContext *def = wctx->priv;
    const struct section *section = wctx->section[wctx->level];
    char buf[32];

    if (def->noprint_wrappers || def->nested_section[wctx->level])
        return;

    if (!(section->flags & (SECTION_FLAG_IS_WRAPPER|SECTION_FLAG_IS_ARRAY)))
        av_bprintf(&wctx->bpBuf, "[/%s]\n", upcase_string(buf, sizeof(buf), section->name));
}

static void default_print_str(WriterContext *wctx, const char *key, const char *value)
{
    DefaultContext *def = wctx->priv;

    if (!def->nokey)
        av_bprintf(&wctx->bpBuf, "%s%s=", wctx->section_pbuf[wctx->level].str, key);
    av_bprintf(&wctx->bpBuf, "%s\n", value);
}

static void default_print_int(WriterContext *wctx, const char *key, long long int value)
{
    DefaultContext *def = wctx->priv;

    if (!def->nokey)
        av_bprintf(&wctx->bpBuf, "%s%s=", wctx->section_pbuf[wctx->level].str, key);
    av_bprintf(&wctx->bpBuf, "%lld\n", value);
}

static const Writer default_writer = {
    .name                  = "default",
    .priv_size             = sizeof(DefaultContext),
    .print_section_header  = default_print_section_header,
    .print_section_footer  = default_print_section_footer,
    .print_integer         = default_print_int,
    .print_string          = default_print_str,
    .flags = WRITER_FLAG_DISPLAY_OPTIONAL_FIELDS,
    .priv_class            = &default_class,
};

/* JSON output */

typedef struct JSONContext {
    const AVClass *class;
    int indent_level;
    int compact;
    const char *item_sep, *item_start_end;
} JSONContext;

#undef OFFSET
#define OFFSET(x) offsetof(JSONContext, x)

static const AVOption json_options[]= {
    { "compact", "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { "c",       "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { NULL }
};

DEFINE_WRITER_CLASS(json);

static av_cold int json_init(WriterContext *wctx)
{
    JSONContext *json = wctx->priv;

    json->item_sep       = json->compact ? ", " : ",\n";
    json->item_start_end = json->compact ? " "  : "\n";

    return 0;
}

static const char *json_escape_str(AVBPrint *dst, const char *src, void *log_ctx)
{
    static const char json_escape[] = {'"', '\\', '\b', '\f', '\n', '\r', '\t', 0};
    static const char json_subst[]  = {'"', '\\',  'b',  'f',  'n',  'r',  't', 0};
    const char *p;

    for (p = src; *p; p++) {
        char *s = strchr(json_escape, *p);
        if (s) {
            av_bprint_chars(dst, '\\', 1);
            av_bprint_chars(dst, json_subst[s - json_escape], 1);
        } else if ((unsigned char)*p < 32) {
            av_bprintf(dst, "\\u00%02x", *p & 0xff);
        } else {
            av_bprint_chars(dst, *p, 1);
        }
    }
    return dst->str;
}

#define JSON_INDENT() av_bprintf(&wctx->bpBuf, "%*c", json->indent_level * 4, ' ')

static void json_print_section_header(WriterContext *wctx)
{
    JSONContext *json = wctx->priv;
    AVBPrint buf;
    const struct section *section = wctx->section[wctx->level];
    const struct section *parent_section = wctx->level ?
        wctx->section[wctx->level-1] : NULL;

    if (wctx->level && wctx->nb_item[wctx->level-1])
        av_bprintf(&wctx->bpBuf, ",\n");

    if (section->flags & SECTION_FLAG_IS_WRAPPER) {
        av_bprintf(&wctx->bpBuf, "{\n");
        json->indent_level++;
    } else {
        av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
        json_escape_str(&buf, section->name, wctx);
        JSON_INDENT();

        json->indent_level++;
        if (section->flags & SECTION_FLAG_IS_ARRAY) {
            av_bprintf(&wctx->bpBuf, "\"%s\": [\n", buf.str);
        } else if (parent_section && !(parent_section->flags & SECTION_FLAG_IS_ARRAY)) {
            av_bprintf(&wctx->bpBuf, "\"%s\": {%s", buf.str, json->item_start_end);
        } else {
            av_bprintf(&wctx->bpBuf, "{%s", json->item_start_end);
        }
        av_bprint_finalize(&buf, NULL);
    }
}

static void json_print_section_footer(WriterContext *wctx)
{
    JSONContext *json = wctx->priv;
    const struct section *section = wctx->section[wctx->level];

    if (wctx->level == 0) {
        json->indent_level--;
        av_bprintf(&wctx->bpBuf, "\n}\n");
    } else if (section->flags & SECTION_FLAG_IS_ARRAY) {
        av_bprintf(&wctx->bpBuf, "\n");
        json->indent_level--;
        JSON_INDENT();
        av_bprintf(&wctx->bpBuf, "]");
    } else {
        av_bprintf(&wctx->bpBuf, "%s", json->item_start_end);
        json->indent_level--;
        if (!json->compact)
            JSON_INDENT();
        av_bprintf(&wctx->bpBuf, "}");
    }
}

static inline void json_print_item_str(WriterContext *wctx,
                                       const char *key, const char *value)
{
    AVBPrint buf;

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&wctx->bpBuf, "\"%s\":", json_escape_str(&buf, key,   wctx));
    av_bprint_clear(&buf);
    av_bprintf(&wctx->bpBuf, " \"%s\"", json_escape_str(&buf, value, wctx));
    av_bprint_finalize(&buf, NULL);
}

static void json_print_str(WriterContext *wctx, const char *key, const char *value)
{
    JSONContext *json = wctx->priv;

    if (wctx->nb_item[wctx->level])
        av_bprintf(&wctx->bpBuf, "%s", json->item_sep);
    if (!json->compact)
        JSON_INDENT();
    json_print_item_str(wctx, key, value);
}

static void json_print_int(WriterContext *wctx, const char *key, long long int value)
{
    JSONContext *json = wctx->priv;
    AVBPrint buf;

    if (wctx->nb_item[wctx->level])
        av_bprintf(&wctx->bpBuf, "%s", json->item_sep);
    if (!json->compact)
        JSON_INDENT();

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&wctx->bpBuf, "\"%s\": %lld", json_escape_str(&buf, key, wctx), value);
    av_bprint_finalize(&buf, NULL);
}

static const Writer json_writer = {
    .name                 = "json",
    .priv_size            = sizeof(JSONContext),
    .init                 = json_init,
    .print_section_header = json_print_section_header,
    .print_section_footer = json_print_section_footer,
    .print_integer        = json_print_int,
    .print_string         = json_print_str,
    .flags = WRITER_FLAG_PUT_PACKETS_AND_FRAMES_IN_SAME_CHAPTER,
    .priv_class           = &json_class,
};

static void print_hwdevicecontext(WriterContext *w, const AVHWDeviceContext *hw_device_context)
{
    writer_print_section_header(w, SECTION_ID_HWDEViCECONTEXT);

    print_int("HasHwDeviceContext", 1);
    print_str("DeviceType", av_hwdevice_get_type_name(hw_device_context->type));

    writer_print_section_footer(w); // SECTION_ID_HWDEViCECONTEXT
}

static void print_hwframescontext(WriterContext *w, const AVHWFramesContext *hw_frames_context)
{
    const AVPixFmtDescriptor* pixdescHw;
    const AVPixFmtDescriptor* pixdescSw;

    writer_print_section_header(w, SECTION_ID_HWFRAMESCONTEXT);

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

    writer_print_section_footer(w); // SECTION_ID_HWFRAMESCONTEXT
}

static void print_link(WriterContext *w, AVFilterLink *link)
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

static void print_filter(WriterContext *w, const AVFilterContext* filter)
{
    writer_print_section_header(w, SECTION_ID_FILTER);

    print_str("Name", filter->name);

    if (filter->filter) {
        print_str("Name2", filter->filter->name);
        print_str("Description", filter->filter->description);
    }

    if (filter->hw_device_ctx) {
        AVHWDeviceContext* decCtx = (AVHWDeviceContext*)filter->hw_device_ctx->data;
        print_hwdevicecontext(w, decCtx);
    }

    writer_print_section_header(w, SECTION_ID_INPUTS);

    for (unsigned i = 0; i < filter->nb_inputs; i++) {
        AVFilterLink *link = filter->inputs[i];
        writer_print_section_header(w, SECTION_ID_INPUT);

        print_str("SourceName", link->src->name);
        print_str("SourcePadName", avfilter_pad_get_name(link->srcpad, 0));
        print_str("DestPadName", avfilter_pad_get_name(link->dstpad, 0));

        print_link(w, link);

        writer_print_section_footer(w); // SECTION_ID_INPUT
    }

    writer_print_section_footer(w); // SECTION_ID_INPUTS

    // --------------------------------------------------

    writer_print_section_header(w, SECTION_ID_OUTPUTS);

    for (unsigned i = 0; i < filter->nb_outputs; i++) {
        AVFilterLink *link = filter->outputs[i];
        writer_print_section_header(w, SECTION_ID_OUTPUT);

        print_str("DestName", link->dst->name);
        print_str("DestPadName", avfilter_pad_get_name(link->dstpad, 0));
        print_str("SourceName", link->src->name);

        print_link(w, link);

        writer_print_section_footer(w); // SECTION_ID_OUTPUT
    }

    writer_print_section_footer(w); // SECTION_ID_OUTPUTS

    writer_print_section_footer(w); // SECTION_ID_FILTER
}

static void print_filtergraph_single(WriterContext *w, FilterGraph* fg, AVFilterGraph *graph)
{
    char layoutString[64];
    FilterGraphPriv *fgp = fgp_from_fg(fg);

    print_int("GraphIndex", fg->index);
    print_str("Description", fgp->graph_desc);

    writer_print_section_header(w, SECTION_ID_INPUTS);

    for (int i = 0; i < fg->nb_inputs; i++) {
        InputFilterPriv* ifilter = ifp_from_ifilter(fg->inputs[i]);
        enum AVMediaType mediaType = ifilter->type;

        writer_print_section_header(w, SECTION_ID_INPUT);

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
            ////print_int("Channels", ifilter->channels);
            ////print_int("ChannelLayout", ifilter->channel_layout);
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

        writer_print_section_footer(w); // SECTION_ID_INPUT
    }

    writer_print_section_footer(w); // SECTION_ID_INPUTS


    writer_print_section_header(w, SECTION_ID_OUTPUTS);

    for (int i = 0; i < fg->nb_outputs; i++) {
        OutputFilterPriv *ofilter = ofp_from_ofilter(fg->outputs[i]);
        enum AVMediaType mediaType = AVMEDIA_TYPE_UNKNOWN;

        writer_print_section_header(w, SECTION_ID_OUTPUT);
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
            ////print_int("Channels", ofilter->channels);
            ////print_int("ChannelLayout", ofilter->channel_layout);
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

        writer_print_section_footer(w); // SECTION_ID_OUTPUT
    }

    writer_print_section_footer(w); // SECTION_ID_OUTPUTS


    writer_print_section_header(w, SECTION_ID_FILTERS);

    if (graph) {
        for (unsigned i = 0; i < graph->nb_filters; i++) {
            AVFilterContext *filter = graph->filters[i];
            writer_print_section_header(w, SECTION_ID_FILTER);

            print_filter(w, filter);

            writer_print_section_footer(w); // SECTION_ID_FILTER
        }
    }

    writer_print_section_footer(w); // SECTION_ID_FILTERS
}

int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph)
{
    const Writer *writer;
    WriterContext *w;
    char *buf, *w_name, *w_args;
    int ret;
    FilterGraphPriv *fgp = fgp_from_fg(fg);
    AVBPrint *targetBuf = &fgp->graph_print_buf;

    writer_register_all();

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

    writer = writer_get_by_name(w_name);
    if (writer == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph  output format with name '%s'\n", w_name);
        return AVERROR(EINVAL);
    }

    if (targetBuf->len) {
        av_bprint_finalize(targetBuf, NULL);
    }

    if ((ret = writer_open(&w, writer, w_args, sections, FF_ARRAY_ELEMS(sections))) >= 0) {
        writer_print_section_header(w, SECTION_ID_ROOT);
        writer_print_section_header(w, SECTION_ID_FILTERGRAPHS);
        writer_print_section_header(w, SECTION_ID_FILTERGRAPH);

        av_bprint_clear(&w->bpBuf);

        print_filtergraph_single(w, fg, graph);

        av_bprint_finalize(&w->bpBuf, &targetBuf->str);
        targetBuf->len = w->bpBuf.len;
        targetBuf->size = w->bpBuf.len + 1;

        writer_close(&w);
    } else
        return ret;

    return 0;
}

int print_filtergraphs(FilterGraph **graphs, int nb_graphs, OutputFile **ofiles, int nb_ofiles)
{
    const Writer *writer;
    WriterContext *w;
    char *buf, *w_name, *w_args;
    int ret;

    writer_register_all();

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

    writer = writer_get_by_name(w_name);
    if (writer == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter graph  output format with name '%s'\n", w_name);
        return AVERROR(EINVAL);
    }

    if ((ret = writer_open(&w, writer, w_args, sections, FF_ARRAY_ELEMS(sections))) >= 0) {
        writer_print_section_header(w, SECTION_ID_ROOT);

        writer_print_section_header(w, SECTION_ID_FILTERGRAPHS);

        for (int i = 0; i < nb_graphs; i++) {

            FilterGraphPriv *fgp = fgp_from_fg(graphs[i]);
            AVBPrint *graph_buf = &fgp->graph_print_buf;

            if (graph_buf->len > 0) {
                writer_print_section_header(w, SECTION_ID_FILTERGRAPH);

                av_bprint_append_data(&w->bpBuf, graph_buf->str, graph_buf->len);
                av_bprint_finalize(graph_buf, NULL);

                writer_print_section_footer(w); // SECTION_ID_FILTERGRAPH
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
                        writer_print_section_header(w, SECTION_ID_FILTERGRAPH);

                        av_bprint_append_data(&w->bpBuf, graph_buf->str,
                                            graph_buf->len);
                        av_bprint_finalize(graph_buf, NULL);

                        writer_print_section_footer(w); // SECTION_ID_FILTERGRAPH
                    }
                }
            }
        }

        writer_print_section_footer(w); // SECTION_ID_FILTERGRAPHS

        writer_print_section_footer(w); // SECTION_ID_ROOT

        if (print_graphs_file) {

            AVIOContext *avio = NULL;

            ret = avio_open2(&avio, print_graphs_file, AVIO_FLAG_WRITE, NULL, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open graph output file, \"%s\": %s\n",
                       print_graphs_file, av_err2str(ret));
                return ret;
            }

            avio_write(avio, (const unsigned char*)w->bpBuf.str, FFMIN(w->bpBuf.len, w->bpBuf.size - 1));
            avio_flush(avio);

            if ((ret = avio_closep(&avio)) < 0)
                av_log(NULL, AV_LOG_ERROR, "Error closing graph output file, loss of information possible: %s\n", av_err2str(ret));
        }

        if (print_graphs)
            av_log(NULL, AV_LOG_INFO, "%s    %c", w->bpBuf.str, '\n');

        writer_close(&w);
    }

    return 0;
}

void writer_register_all(void)
{
    static int initialized;

    if (initialized)
        return;
    initialized = 1;

    writer_register(&default_writer);
    //writer_register(&compact_writer);
    //writer_register(&csv_writer);
    //writer_register(&flat_writer);
    //writer_register(&ini_writer);
    writer_register(&json_writer);
    //writer_register(&xml_writer);
}

void write_error(WriterContext *w, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));

    writer_print_section_header(w, SECTION_ID_ERROR);
    print_int("Number", err);
    print_str("Message", errbuf_ptr);
    writer_print_section_footer(w);
}

void write_error_msg(WriterContext *w, int err, const char *msg)
{
    writer_print_section_header(w, SECTION_ID_ERROR);
    print_int("Number", err);
    print_str("Message", msg);
    writer_print_section_footer(w);
}

void write_error_fmt(WriterContext *w, int err, const char *fmt,...)
{
    AVBPrint pbuf;
    va_list vl;
    va_start(vl, fmt);

    writer_print_section_header(w, SECTION_ID_ERROR);
    print_int("Number", err);

    av_bprint_init(&pbuf, 1, AV_BPRINT_SIZE_UNLIMITED);

    av_bprint_clear(&pbuf);

    av_vbprintf(&pbuf, fmt, vl);
    va_end(vl);

    print_str("Message", pbuf.str);

    av_bprint_finalize(&pbuf, NULL);

    writer_print_section_footer(w);
}

