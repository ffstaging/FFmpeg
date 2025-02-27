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

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mem.h"
#include "../avassert.h"
#include "../avtextformat.h"
#include "../bprint.h"
#include "../error.h"
#include "../hash.h"
#include "../intreadwrite.h"
#include "../macros.h"
#include "../opt.h"

#define SECTION_ID_NONE -1

#define SHOW_OPTIONAL_FIELDS_AUTO       -1
#define SHOW_OPTIONAL_FIELDS_NEVER       0
#define SHOW_OPTIONAL_FIELDS_ALWAYS      1

static const struct {
    double bin_val;
    double dec_val;
    const char *bin_str;
    const char *dec_str;
} si_prefixes[] = {
    { 1.0, 1.0, "", "" },
    { 1.024e3, 1e3, "Ki", "K" },
    { 1.048576e6, 1e6, "Mi", "M" },
    { 1.073741824e9, 1e9, "Gi", "G" },
    { 1.099511627776e12, 1e12, "Ti", "T" },
    { 1.125899906842624e15, 1e15, "Pi", "P" },
};

static const char *avtext_context_get_formatter_name(void *p)
{
    AVTextFormatContext *wctx = p;
    return wctx->formatter->name;
}

#define OFFSET(x) offsetof(AVTextFormatContext, x)

static const AVOption textcontext_options[] = {
    { "string_validation", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, {.i64=AV_TEXTFORMAT_STRING_VALIDATION_REPLACE}, 0, AV_TEXTFORMAT_STRING_VALIDATION_NB-1, .unit = "sv" },
    { "sv", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, {.i64=AV_TEXTFORMAT_STRING_VALIDATION_REPLACE}, 0, AV_TEXTFORMAT_STRING_VALIDATION_NB-1, .unit = "sv" },
        { "ignore",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_TEXTFORMAT_STRING_VALIDATION_IGNORE},  .unit = "sv" },
        { "replace", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_TEXTFORMAT_STRING_VALIDATION_REPLACE}, .unit = "sv" },
        { "fail",    NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AV_TEXTFORMAT_STRING_VALIDATION_FAIL},    .unit = "sv" },
    { "string_validation_replacement", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, {.str=""}},
    { "svr", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, {.str="\xEF\xBF\xBD"}},
};

static void *trextcontext_child_next(void *obj, void *prev)
{
    AVTextFormatContext *ctx = obj;
    if (!prev && ctx->formatter && ctx->formatter->priv_class && ctx->priv)
        return ctx->priv;
    return NULL;
}

static const AVClass textcontext_class = {
    .class_name = "AVTextContext",
    .item_name  = avtext_context_get_formatter_name,
    .option     = textcontext_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .child_next = trextcontext_child_next,
};


static inline void textoutput_w8_avio(AVTextFormatContext *wctx, int b)
{
    avio_w8(wctx->avio, b);
}

static inline void textoutput_put_str_avio(AVTextFormatContext *wctx, const char *str)
{
    avio_write(wctx->avio, str, strlen(str));
}

static inline void textoutput_printf_avio(AVTextFormatContext *wctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    avio_vprintf(wctx->avio, fmt, ap);
    va_end(ap);
}

static inline void textoutput_w8_printf(AVTextFormatContext *wctx, int b)
{
    printf("%c", b);
}

static inline void textoutput_put_str_printf(AVTextFormatContext *wctx, const char *str)
{
    printf("%s", str);
}

static inline void textoutput_printf_printf(AVTextFormatContext *wctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void bprint_bytes(AVBPrint *bp, const uint8_t *ubuf, size_t ubuf_size)
{
    int i;
    av_bprintf(bp, "0X");
    for (i = 0; i < ubuf_size; i++)
        av_bprintf(bp, "%02X", ubuf[i]);
}

int avtext_context_close(AVTextFormatContext **pwctx)
{
    AVTextFormatContext *wctx = *pwctx;
    int i;
    int ret = 0;

    if (!wctx)
        return -1;

    av_hash_freep(&wctx->hash);

    if (wctx->formatter->uninit)
        wctx->formatter->uninit(wctx);
    for (i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_finalize(&wctx->section_pbuf[i], NULL);
    if (wctx->formatter->priv_class)
        av_opt_free(wctx->priv);
    av_freep(&wctx->priv);
    av_opt_free(wctx);
    if (wctx->avio) {
        avio_flush(wctx->avio);
        ret = avio_close(wctx->avio);
    }
    av_freep(pwctx);
    return ret;
}


int avtext_context_open(AVTextFormatContext **pwctx, const AVTextFormatter *formatter, const AVTextFormatter *writer, const char *args,
                        const struct AVTextFormatSection *sections, int nb_sections,
                        const char *output_filename,
                        int show_value_unit,
                        int use_value_prefix,
                        int use_byte_value_binary_prefix,
                        int use_value_sexagesimal_format,
                        int show_optional_fields,
                        char *show_data_hash)
{
    AVTextFormatContext *wctx;
    int i, ret = 0;

    if (!(wctx = av_mallocz(sizeof(AVTextFormatContext)))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (!(wctx->priv = av_mallocz(formatter->priv_size))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    wctx->show_value_unit = show_value_unit;
    wctx->use_value_prefix = use_value_prefix;
    wctx->use_byte_value_binary_prefix = use_byte_value_binary_prefix;
    wctx->use_value_sexagesimal_format = use_value_sexagesimal_format;
    wctx->show_optional_fields = show_optional_fields;
    
    if (nb_sections > SECTION_MAX_NB_SECTIONS) {
        av_log(wctx, AV_LOG_ERROR, "The number of section definitions (%d) is larger than the maximum allowed (%d)\n", nb_sections, SECTION_MAX_NB_SECTIONS);
        goto fail;
    }

    wctx->class = &textcontext_class;
    wctx->formatter = formatter;
    wctx->level = -1;
    wctx->sections = sections;
    wctx->nb_sections = nb_sections;

    av_opt_set_defaults(wctx);

    if (formatter->priv_class) {
        void *priv_ctx = wctx->priv;
        *(const AVClass **)priv_ctx = formatter->priv_class;
        av_opt_set_defaults(priv_ctx);
    }

    /* convert options to dictionary */
    if (args) {
        AVDictionary *opts = NULL;
        const AVDictionaryEntry *opt = NULL;

        if ((ret = av_dict_parse_string(&opts, args, "=", ":", 0)) < 0) {
            av_log(wctx, AV_LOG_ERROR, "Failed to parse option string '%s' provided to textformat context\n", args);
            av_dict_free(&opts);
            goto fail;
        }

        while ((opt = av_dict_iterate(opts, opt))) {
            if ((ret = av_opt_set(wctx, opt->key, opt->value, AV_OPT_SEARCH_CHILDREN)) < 0) {
                av_log(wctx, AV_LOG_ERROR, "Failed to set option '%s' with value '%s' provided to textformat context\n",
                       opt->key, opt->value);
                av_dict_free(&opts);
                goto fail;
            }
        }

        av_dict_free(&opts);
    }

    if (show_data_hash) {
        if ((ret = av_hash_alloc(&wctx->hash, show_data_hash)) < 0) {
            if (ret == AVERROR(EINVAL)) {
                const char *n;
                av_log(NULL, AV_LOG_ERROR, "Unknown hash algorithm '%s'\nKnown algorithms:", show_data_hash);
                for (i = 0; (n = av_hash_names(i)); i++)
                    av_log(NULL, AV_LOG_ERROR, " %s", n);
                av_log(NULL, AV_LOG_ERROR, "\n");
            }
            return ret;
        }
    }

    /* validate replace string */
    {
        const uint8_t *p = wctx->string_validation_replacement;
        const uint8_t *endp = p + strlen(p);
        while (*p) {
            const uint8_t *p0 = p;
            int32_t code;
            ret = av_utf8_decode(&code, &p, endp, wctx->string_validation_utf8_flags);
            if (ret < 0) {
                AVBPrint bp;
                av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                bprint_bytes(&bp, p0, p-p0),
                    av_log(wctx, AV_LOG_ERROR,
                           "Invalid UTF8 sequence %s found in string validation replace '%s'\n",
                           bp.str, wctx->string_validation_replacement);
                return ret;
            }
        }
    }

    if (!output_filename) {
        wctx->writer_w8 = textoutput_w8_printf;
        wctx->writer_put_str = textoutput_put_str_printf;
        wctx->writer_printf = textoutput_printf_printf;
    } else {
        if ((ret = avio_open(&wctx->avio, output_filename, AVIO_FLAG_WRITE)) < 0) {
            av_log(wctx, AV_LOG_ERROR,
                   "Failed to open output '%s' with error: %s\n", output_filename, av_err2str(ret));
            goto fail;
        }
        wctx->writer_w8 = textoutput_w8_avio;
        wctx->writer_put_str = textoutput_put_str_avio;
        wctx->writer_printf = textoutput_printf_avio;
    }

    for (i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_init(&wctx->section_pbuf[i], 1, AV_BPRINT_SIZE_UNLIMITED);

    if (wctx->formatter->init)
        ret = wctx->formatter->init(wctx);
    if (ret < 0)
        goto fail;

    *pwctx = wctx;

    return 0;

fail:
    avtext_context_close(&wctx);
    return ret;
}

/* Temporary definitions during refactoring */
static const char unit_second_str[]         = "s"    ;
static const char unit_hertz_str[]          = "Hz"   ;
static const char unit_byte_str[]           = "byte" ;
static const char unit_bit_per_second_str[] = "bit/s";


void avtext_print_section_header(AVTextFormatContext *wctx,
                                               const void *data,
                                               int section_id)
{
    wctx->level++;
    av_assert0(wctx->level < SECTION_MAX_NB_LEVELS);

    wctx->nb_item[wctx->level] = 0;
    memset(wctx->nb_item_type[wctx->level], 0, sizeof(wctx->nb_item_type[wctx->level]));
    wctx->section[wctx->level] = &wctx->sections[section_id];

    if (wctx->formatter->print_section_header)
        wctx->formatter->print_section_header(wctx, data);
}

void avtext_print_section_footer(AVTextFormatContext *wctx)
{
    int section_id = wctx->section[wctx->level]->id;
    int parent_section_id = wctx->level ?
        wctx->section[wctx->level-1]->id : SECTION_ID_NONE;

    if (parent_section_id != SECTION_ID_NONE) {
        wctx->nb_item[wctx->level - 1]++;
        wctx->nb_item_type[wctx->level - 1][section_id]++;
    }

    if (wctx->formatter->print_section_footer)
        wctx->formatter->print_section_footer(wctx);
    wctx->level--;
}

void avtext_print_integer(AVTextFormatContext *wctx,
                                        const char *key, int64_t val)
{
    const struct AVTextFormatSection *section = wctx->section[wctx->level];

    if (section->show_all_entries || av_dict_get(section->entries_to_show, key, NULL, 0)) {
        wctx->formatter->print_integer(wctx, key, val);
        wctx->nb_item[wctx->level]++;
    }
}

static inline int validate_string(AVTextFormatContext *wctx, char **dstp, const char *src)
{
    const uint8_t *p, *endp;
    AVBPrint dstbuf;
    int invalid_chars_nb = 0, ret = 0;

    av_bprint_init(&dstbuf, 0, AV_BPRINT_SIZE_UNLIMITED);

    endp = src + strlen(src);
    for (p = src; *p;) {
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
            case AV_TEXTFORMAT_STRING_VALIDATION_FAIL:
                av_log(wctx, AV_LOG_ERROR,
                       "Invalid UTF-8 sequence found in string '%s'\n", src);
                ret = AVERROR_INVALIDDATA;
                goto end;
                break;

            case AV_TEXTFORMAT_STRING_VALIDATION_REPLACE:
                av_bprintf(&dstbuf, "%s", wctx->string_validation_replacement);
                break;
            }
        }

        if (!invalid || wctx->string_validation == AV_TEXTFORMAT_STRING_VALIDATION_IGNORE)
            av_bprint_append_data(&dstbuf, p0, p-p0);
    }

    if (invalid_chars_nb && wctx->string_validation == AV_TEXTFORMAT_STRING_VALIDATION_REPLACE) {
        av_log(wctx, AV_LOG_WARNING,
               "%d invalid UTF-8 sequence(s) found in string '%s', replaced with '%s'\n",
               invalid_chars_nb, src, wctx->string_validation_replacement);
    }

end:
    av_bprint_finalize(&dstbuf, dstp);
    return ret;
}

struct unit_value {
    union { double d; int64_t i; } val;
    const char *unit;
};

static char *value_string(AVTextFormatContext *wctx, char *buf, int buf_size, struct unit_value uv)
{
    double vald;
    int64_t vali;
    int show_float = 0;

    if (uv.unit == unit_second_str) {
        vald = uv.val.d;
        show_float = 1;
    } else {
        vald = vali = uv.val.i;
    }

    if (uv.unit == unit_second_str && wctx->use_value_sexagesimal_format) {
        double secs;
        int hours, mins;
        secs  = vald;
        mins  = (int)secs / 60;
        secs  = secs - mins * 60;
        hours = mins / 60;
        mins %= 60;
        snprintf(buf, buf_size, "%d:%02d:%09.6f", hours, mins, secs);
    } else {
        const char *prefix_string = "";

        if (wctx->use_value_prefix && vald > 1) {
            int64_t index;

            if (uv.unit == unit_byte_str && wctx->use_byte_value_binary_prefix) {
                index = (int64_t) (log2(vald)) / 10;
                index = av_clip(index, 0, FF_ARRAY_ELEMS(si_prefixes) - 1);
                vald /= si_prefixes[index].bin_val;
                prefix_string = si_prefixes[index].bin_str;
            } else {
                index = (int64_t) (log10(vald)) / 3;
                index = av_clip(index, 0, FF_ARRAY_ELEMS(si_prefixes) - 1);
                vald /= si_prefixes[index].dec_val;
                prefix_string = si_prefixes[index].dec_str;
            }
            vali = vald;
        }

        if (show_float || (wctx->use_value_prefix && vald != (int64_t)vald))
            snprintf(buf, buf_size, "%f", vald);
        else
            snprintf(buf, buf_size, "%"PRId64, vali);
        av_strlcatf(buf, buf_size, "%s%s%s", *prefix_string || wctx->show_value_unit ? " " : "",
                 prefix_string, wctx->show_value_unit ? uv.unit : "");
    }

    return buf;
}


void avtext_print_unit_int(AVTextFormatContext *wctx, const char *key, int value, const char *unit)
{
    char val_str[128];
    struct unit_value uv;
    uv.val.i = value;
    uv.unit = unit;
    avtext_print_string(wctx, key, value_string(wctx, val_str, sizeof(val_str), uv), 0);
}


int avtext_print_string(AVTextFormatContext *wctx, const char *key, const char *val, int flags)
{
    const struct AVTextFormatSection *section = wctx->section[wctx->level];
    int ret = 0;

    if (wctx->show_optional_fields == SHOW_OPTIONAL_FIELDS_NEVER ||
        (wctx->show_optional_fields == SHOW_OPTIONAL_FIELDS_AUTO
        && (flags & AV_TEXTFORMAT_PRINT_STRING_OPTIONAL)
        && !(wctx->formatter->flags & AV_TEXTFORMAT_FLAG_SUPPORTS_OPTIONAL_FIELDS)))
        return 0;

    if (section->show_all_entries || av_dict_get(section->entries_to_show, key, NULL, 0)) {
        if (flags & AV_TEXTFORMAT_PRINT_STRING_VALIDATE) {
            char *key1 = NULL, *val1 = NULL;
            ret = validate_string(wctx, &key1, key);
            if (ret < 0) goto end;
            ret = validate_string(wctx, &val1, val);
            if (ret < 0) goto end;
            wctx->formatter->print_string(wctx, key1, val1);
        end:
            if (ret < 0) {
                av_log(wctx, AV_LOG_ERROR,
                       "Invalid key=value string combination %s=%s in section %s\n",
                       key, val, section->unique_name);
            }
            av_free(key1);
            av_free(val1);
        } else {
            wctx->formatter->print_string(wctx, key, val);
        }

        wctx->nb_item[wctx->level]++;
    }

    return ret;
}

void avtext_print_rational(AVTextFormatContext *wctx,
                                         const char *key, AVRational q, char sep)
{
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf, "%d%c%d", q.num, sep, q.den);
    avtext_print_string(wctx, key, buf.str, 0);
}

void avtext_print_time(AVTextFormatContext *wctx, const char *key,
                              int64_t ts, const AVRational *time_base, int is_duration)
{
    char buf[128];

    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
        avtext_print_string(wctx, key, "N/A", AV_TEXTFORMAT_PRINT_STRING_OPTIONAL);
    } else {
        double d = ts * av_q2d(*time_base);
        struct unit_value uv;
        uv.val.d = d;
        uv.unit = unit_second_str;
        value_string(wctx, buf, sizeof(buf), uv);
        avtext_print_string(wctx, key, buf, 0);
    }
}

void avtext_print_ts(AVTextFormatContext *wctx, const char *key, int64_t ts, int is_duration)
{
    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
        avtext_print_string(wctx, key, "N/A", AV_TEXTFORMAT_PRINT_STRING_OPTIONAL);
    } else {
        avtext_print_integer(wctx, key, ts);
    }
}

void avtext_print_data(AVTextFormatContext *wctx, const char *name,
                              const uint8_t *data, int size)
{
    AVBPrint bp;
    int offset = 0, l, i;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "\n");
    while (size) {
        av_bprintf(&bp, "%08x: ", offset);
        l = FFMIN(size, 16);
        for (i = 0; i < l; i++) {
            av_bprintf(&bp, "%02x", data[i]);
            if (i & 1)
                av_bprintf(&bp, " ");
        }
        av_bprint_chars(&bp, ' ', 41 - 2 * i - i / 2);
        for (i = 0; i < l; i++)
            av_bprint_chars(&bp, data[i] - 32U < 95 ? data[i] : '.', 1);
        av_bprintf(&bp, "\n");
        offset += l;
        data   += l;
        size   -= l;
    }
    avtext_print_string(wctx, name, bp.str, 0);
    av_bprint_finalize(&bp, NULL);
}

void avtext_print_data_hash(AVTextFormatContext *wctx, const char *name,
                                   const uint8_t *data, int size)
{
    char *p, buf[AV_HASH_MAX_SIZE * 2 + 64] = { 0 };

    if (!wctx->hash)
        return;
    av_hash_init(wctx->hash);
    av_hash_update(wctx->hash, data, size);
    snprintf(buf, sizeof(buf), "%s:", av_hash_get_name(wctx->hash));
    p = buf + strlen(buf);
    av_hash_final_hex(wctx->hash, p, buf + sizeof(buf) - p);
    avtext_print_string(wctx, name, buf, 0);
}

void avtext_print_integers(AVTextFormatContext *wctx, const char *name,
                                  uint8_t *data, int size, const char *format,
                                  int columns, int bytes, int offset_add)
{
    AVBPrint bp;
    int offset = 0, l, i;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "\n");
    while (size) {
        av_bprintf(&bp, "%08x: ", offset);
        l = FFMIN(size, columns);
        for (i = 0; i < l; i++) {
            if      (bytes == 1) av_bprintf(&bp, format, *data);
            else if (bytes == 2) av_bprintf(&bp, format, AV_RN16(data));
            else if (bytes == 4) av_bprintf(&bp, format, AV_RN32(data));
            data += bytes;
            size --;
        }
        av_bprintf(&bp, "\n");
        offset += offset_add;
    }
    avtext_print_string(wctx, name, bp.str, 0);
    av_bprint_finalize(&bp, NULL);
}
