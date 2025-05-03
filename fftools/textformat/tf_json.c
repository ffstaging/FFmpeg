/*
 * Copyright (c) The FFmpeg developers
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

#include "avtextformat.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "tf_internal.h"

/* JSON output */

typedef struct JSONContext {
    const AVClass *class;
    int indent_level;
    int compact;
    const char *item_sep, *item_start_end;
} JSONContext;

#undef OFFSET
#define OFFSET(x) offsetof(JSONContext, x)

static const AVOption json_options[] = {
    { "compact", "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { "c",       "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1 },
    { NULL }
};

DEFINE_FORMATTER_CLASS(json);

static av_cold int json_init(AVTextFormatContext *tctx)
{
    JSONContext *json = tctx->priv;

    json->item_sep       = json->compact ? ", " : ",\n";
    json->item_start_end = json->compact ? " "  : "\n";

    return 0;
}

static const char *json_escape_str(AVBPrint *dst, const char *src, void *log_ctx)
{
    static const char json_escape[] = { '"', '\\', '\b', '\f', '\n', '\r', '\t', 0 };
    static const char json_subst[]  = { '"', '\\',  'b',  'f',  'n',  'r',  't', 0 };
    const char *p;

    if (!src) {
        av_log(log_ctx, AV_LOG_WARNING, "Cannot escape NULL string, returning NULL\n");
        return NULL;
    }

    for (p = src; *p; p++) {
        char *s = strchr(json_escape, *p);
        if (s) {
            av_bprint_chars(dst, '\\', 1);
            av_bprint_chars(dst, json_subst[s - json_escape], 1);
        } else if ((unsigned char)*p < 32) {
            av_bprintf(dst, "\\u00%02x", (unsigned char)*p);
        } else {
            av_bprint_chars(dst, *p, 1);
        }
    }
    return dst->str;
}

#define JSON_INDENT() writer_printf(tctx, "%*c", json->indent_level * 4, ' ')

static void json_print_section_header(AVTextFormatContext *tctx, const void *data)
{
    const AVTextFormatSection *section = tf_get_section(tctx, tctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(tctx, tctx->level);
    JSONContext *json = tctx->priv;
    AVBPrint buf;

    if (!section)
        return;

    if (tctx->level && tctx->nb_item[tctx->level - 1])
        writer_put_str(tctx, ",\n");

    if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_WRAPPER) {
        writer_put_str(tctx, "{\n");
        json->indent_level++;
    } else {
        av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
        json_escape_str(&buf, section->name, tctx);
        JSON_INDENT();

        json->indent_level++;
        if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) {
            writer_printf(tctx, "\"%s\": [\n", buf.str);
        } else if (parent_section && !(parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY)) {
            writer_printf(tctx, "\"%s\": {%s", buf.str, json->item_start_end);
        } else {
            writer_printf(tctx, "{%s", json->item_start_end);

            /* this is required so the parser can distinguish between packets and frames */
            if (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE) {
                if (!json->compact)
                    JSON_INDENT();
                writer_printf(tctx, "\"type\": \"%s\"", section->name);
                tctx->nb_item[tctx->level]++;
            }
        }
        av_bprint_finalize(&buf, NULL);
    }
}

static void json_print_section_footer(AVTextFormatContext *tctx)
{
    const AVTextFormatSection *section = tf_get_section(tctx, tctx->level);
    JSONContext *json = tctx->priv;

    if (!section)
        return;

    if (tctx->level == 0) {
        json->indent_level--;
        writer_put_str(tctx, "\n}\n");
    } else if (section->flags & AV_TEXTFORMAT_SECTION_FLAG_IS_ARRAY) {
        writer_w8(tctx, '\n');
        json->indent_level--;
        JSON_INDENT();
        writer_w8(tctx, ']');
    } else {
        writer_put_str(tctx, json->item_start_end);
        json->indent_level--;
        if (!json->compact)
            JSON_INDENT();
        writer_w8(tctx, '}');
    }
}

static inline void json_print_item_str(AVTextFormatContext *tctx,
                                       const char *key, const char *value)
{
    AVBPrint buf;

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(tctx, "\"%s\":", json_escape_str(&buf, key,   tctx));
    av_bprint_clear(&buf);
    writer_printf(tctx, " \"%s\"", json_escape_str(&buf, value, tctx));
    av_bprint_finalize(&buf, NULL);
}

static void json_print_str(AVTextFormatContext *tctx, const char *key, const char *value)
{
    const AVTextFormatSection *section = tf_get_section(tctx, tctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(tctx, tctx->level);
    JSONContext *json = tctx->priv;

    if (!section)
        return;

    if (tctx->nb_item[tctx->level] || (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE))
        writer_put_str(tctx, json->item_sep);
    if (!json->compact)
        JSON_INDENT();
    json_print_item_str(tctx, key, value);
}

static void json_print_int(AVTextFormatContext *tctx, const char *key, int64_t value)
{
    const AVTextFormatSection *section = tf_get_section(tctx, tctx->level);
    const AVTextFormatSection *parent_section = tf_get_parent_section(tctx, tctx->level);
    JSONContext *json = tctx->priv;
    AVBPrint buf;

    if (!section)
        return;

    if (tctx->nb_item[tctx->level] || (parent_section && parent_section->flags & AV_TEXTFORMAT_SECTION_FLAG_NUMBERING_BY_TYPE))
        writer_put_str(tctx, json->item_sep);
    if (!json->compact)
        JSON_INDENT();

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(tctx, "\"%s\": %"PRId64, json_escape_str(&buf, key, tctx), value);
    av_bprint_finalize(&buf, NULL);
}

const AVTextFormatter avtextformatter_json = {
    .name                 = "json",
    .priv_size            = sizeof(JSONContext),
    .init                 = json_init,
    .print_section_header = json_print_section_header,
    .print_section_footer = json_print_section_footer,
    .print_integer        = json_print_int,
    .print_string         = json_print_str,
    .flags = AV_TEXTFORMAT_FLAG_SUPPORTS_MIXED_ARRAY_CONTENT,
    .priv_class           = &json_class,
};
