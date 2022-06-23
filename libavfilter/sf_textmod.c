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
 * text subtitle filter which allows to modify subtitle text in several ways
 */

#include <libavutil/ass_internal.h>

#include "libavutil/opt.h"
#include "internal.h"
#include "libavutil/ass_split_internal.h"
#include "libavutil/bprint.h"
#include "libavutil/file.h"

static const char* leet_src = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char* leet_dst = "abcd3f6#1jklmn0pq257uvwxyzAB(D3F6#1JKLMN0PQ257UVWXYZ";

enum TextModFilterType {
    TM_TEXTMOD,
    TM_CENSOR,
    TM_SHOW_SPEAKER,
};

enum TextModOperation {
    OP_LEET,
    OP_TO_UPPER,
    OP_TO_LOWER,
    OP_REPLACE_CHARS,
    OP_REMOVE_CHARS,
    OP_REPLACE_WORDS,
    OP_REMOVE_WORDS,
    NB_OPS,
};

enum CensorMode {
    CM_KEEP_FIRST_LAST,
    CM_KEEP_FIRST,
    CM_ALL,
};

enum ShowSpeakerMode {
    SM_SQUARE_BRACKETS,
    SM_ROUND_BRACKETS,
    SM_COLON,
    SM_PLAIN,
};

typedef struct TextModContext {
    const AVClass *class;
    enum AVSubtitleType format;
    enum TextModFilterType filter_type;
    enum TextModOperation operation;
    enum CensorMode censor_mode;
    enum ShowSpeakerMode speaker_mode;
    char *find;
    char *find_file;
    char *style;
    char *replace;
    char *replace_file;
    char *separator;
    char *censor_char;
    char **find_list;
    int  line_break;
    int  nb_find_list;
    char **replace_list;
    int  nb_replace_list;
} TextModContext;

static char **split_string(char *source, int *nb_elems, const char *delim)
{
    char **list = NULL;
    char *temp = NULL;
    char *ptr = av_strtok(source, delim, &temp);

    while (ptr) {
        if (strlen(ptr)) {
            av_dynarray_add(&list, nb_elems, ptr);
            if (!list)
                return NULL;
        }

        ptr = av_strtok(NULL, delim, &temp);
    }

    if (!list)
        return NULL;

    for (int i = 0; i < *nb_elems; i++) {
        list[i] = av_strdup(list[i]);
        if (!list[i]) {
            for (int n = 0; n < i; n++)
                av_free(list[n]);
            av_free(list);
            return NULL;
        }
    }

    return list;
}

static int load_text_from_file(AVFilterContext *ctx, const char *file_name, char **text, char separator)
{
    int err;
    uint8_t *textbuf;
    char *tmp;
    size_t textbuf_size;
    int offset = 0;

    if ((err = av_file_map(file_name, &textbuf, &textbuf_size, 0, ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "The text file '%s' could not be read or is empty\n", file_name);
        return err;
    }

    if (textbuf_size > 1 &&
        (textbuf[0] == 0xFF && textbuf[1] == 0xFE
        || textbuf[0] == 0xFE && textbuf[1] == 0xFF)) {
        av_log(ctx, AV_LOG_ERROR, "UTF text files are not supported. File: %s\n", file_name);
        return AVERROR(EINVAL);
    }

    if (textbuf_size > 2 && textbuf[0] == 0xEF && textbuf[1] == 0xBB && textbuf[2] == 0xBF)
        offset = 3; // UTF-8

    if (textbuf_size > SIZE_MAX - 1 || !((tmp = av_strndup((char *)textbuf + offset, textbuf_size - offset)))) {
        av_file_unmap(textbuf, textbuf_size);
        return AVERROR(ENOMEM);
    }

    av_file_unmap(textbuf, textbuf_size);

    for (size_t i = 0; i < strlen(tmp); i++) {
        switch (tmp[i]) {
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            tmp[i] = separator;
        }
    }

    *text = tmp;

    return 0;
}

static int load_files(AVFilterContext *ctx)
{
    TextModContext *s = ctx->priv;
    int ret;

    if (!s->separator || strlen(s->separator) != 1) {
        av_log(ctx, AV_LOG_ERROR, "A single character needs to be specified for the separator parameter.\n");
        return AVERROR(EINVAL);
    }

    if (s->find_file && strlen(s->find_file)) {
        ret = load_text_from_file(ctx, s->find_file, &s->find, s->separator[0]);
        if (ret < 0 )
            return ret;
    }

    if (s->replace_file && strlen(s->replace_file)) {
        ret = load_text_from_file(ctx, s->replace_file, &s->replace, s->separator[0]);
        if (ret < 0 )
            return ret;
    }

    return 0;
}

static int init_censor(AVFilterContext *ctx)
{
    TextModContext *s = ctx->priv;
    int ret;

    s->filter_type = TM_CENSOR;
    s->operation = OP_REPLACE_WORDS;

    ret = load_files(ctx);
    if (ret < 0 )
        return ret;

    if (!s->find || !strlen(s->find)) {
        av_log(ctx, AV_LOG_ERROR, "Either the 'words' or the 'words_file' parameter needs to be specified\n");
        return AVERROR(EINVAL);
    }

    if (!s->censor_char || strlen(s->censor_char) != 1) {
        av_log(ctx, AV_LOG_ERROR, "A single character needs to be specified for the censor_char parameter\n");
        return AVERROR(EINVAL);
    }

    s->find_list = split_string(s->find, &s->nb_find_list, s->separator);
    if (!s->find_list)
        return AVERROR(ENOMEM);

    s->replace_list = av_calloc(s->nb_find_list, sizeof(char *));
    if (!s->replace_list)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->nb_find_list; i++) {
        size_t len, start = 0, end;
        char *item = av_strdup(s->find_list[i]);
        if (!item)
            return AVERROR(ENOMEM);

        len = end = strlen(item);

        switch (s->censor_mode) {
        case CM_KEEP_FIRST_LAST:

            if (len > 2)
                start = 1;
            if (len > 3)
                end--;

            break;
        case CM_KEEP_FIRST:

            if (len > 2)
                start = 1;

            break;
        }

        for (size_t n = start; n < end; n++)
            item[n] = s->censor_char[0];

        s->replace_list[i] = item;
    }

    return 0;
}

static int init_showspeaker(AVFilterContext *ctx)
{
    TextModContext *s = ctx->priv;
    s->filter_type = TM_SHOW_SPEAKER;

    return 0;
}

static int init(AVFilterContext *ctx)
{
    TextModContext *s = ctx->priv;
    int ret;

    ret = load_files(ctx);
    if (ret < 0 )
        return ret;

    switch (s->operation) {
    case OP_REPLACE_CHARS:
    case OP_REMOVE_CHARS:
    case OP_REPLACE_WORDS:
    case OP_REMOVE_WORDS:
        if (!s->find || !strlen(s->find)) {
            av_log(ctx, AV_LOG_ERROR, "Selected mode requires the 'find' parameter to be specified\n");
            return AVERROR(EINVAL);
        }
        break;
    }

    switch (s->operation) {
    case OP_REPLACE_CHARS:
    case OP_REPLACE_WORDS:
        if (!s->replace || !strlen(s->replace)) {
            av_log(ctx, AV_LOG_ERROR, "Selected mode requires the 'replace' parameter to be specified\n");
            return AVERROR(EINVAL);
        }
        break;
    }

    if (s->operation == OP_REPLACE_CHARS && strlen(s->find) != strlen(s->replace)) {
        av_log(ctx, AV_LOG_ERROR, "Selected mode requires the 'find' and 'replace' parameters to have the same length\n");
        return AVERROR(EINVAL);
    }

    if (s->operation == OP_REPLACE_WORDS || s->operation == OP_REMOVE_WORDS) {
        if (!s->separator || strlen(s->separator) != 1) {
            av_log(ctx, AV_LOG_ERROR, "Selected mode requires a single separator char to be specified\n");
            return AVERROR(EINVAL);
        }

        s->find_list = split_string(s->find, &s->nb_find_list, s->separator);
        if (!s->find_list)
            return AVERROR(ENOMEM);

        if (s->operation == OP_REPLACE_WORDS) {

            s->replace_list = split_string(s->replace, &s->nb_replace_list, s->separator);
            if (!s->replace_list)
                return AVERROR(ENOMEM);

            if (s->nb_find_list != s->nb_replace_list) {
                av_log(ctx, AV_LOG_ERROR, "The number of words in 'find' and 'replace' needs to be equal\n");
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    TextModContext *s = ctx->priv;

    for (int i = 0; i < s->nb_find_list; i++)
        av_freep(&s->find_list[i]);

    s->nb_find_list = 0;
    av_freep(&s->find_list);

    for (int i = 0; i < s->nb_replace_list; i++)
        av_freep(&s->replace_list[i]);

    s->nb_replace_list = 0;
    av_freep(&s->replace_list);
}

static char *process_text(TextModContext *s, char *text)
{
    const char *char_src = s->find;
    const char *char_dst = s->replace;
    char *result         = NULL;
    int escape_level     = 0, k = 0;

    switch (s->operation) {
    case OP_LEET:
    case OP_REPLACE_CHARS:

        if (s->operation == OP_LEET) {
            char_src = leet_src;
            char_dst = leet_dst;
        }

        result = av_strdup(text);
        if (!result)
            return NULL;

        for (size_t n = 0; n < strlen(result); n++) {
            if (result[n] == '{')
                escape_level++;

            if (!escape_level) {
                size_t len = strlen(char_src);
                for (size_t t = 0; t < len; t++) {
                    if (result[n] == char_src[t]) {
                        result[n] = char_dst[t];
                        break;
                    }
                }
            }

            if (result[n] == '}')
                escape_level--;
        }

        break;
    case OP_TO_UPPER:
    case OP_TO_LOWER:

        result = av_strdup(text);
        if (!result)
            return NULL;

        for (size_t n = 0; n < strlen(result); n++) {
            if (result[n] == '{')
                escape_level++;
            if (!escape_level)
                result[n] = s->operation == OP_TO_LOWER ? av_tolower(result[n]) : av_toupper(result[n]);
            if (result[n] == '}')
                escape_level--;
        }

        break;
    case OP_REMOVE_CHARS:

        result = av_strdup(text);
        if (!result)
            return NULL;

        for (size_t n = 0; n < strlen(result); n++) {
            int skip_char = 0;

            if (result[n] == '{')
                escape_level++;

            if (!escape_level) {
                size_t len = strlen(char_src);
                for (size_t t = 0; t < len; t++) {
                    if (result[n] == char_src[t]) {
                        skip_char = 1;
                        break;
                    }
                }
            }

            if (!skip_char)
                result[k++] = result[n];

            if (result[n] == '}')
                escape_level--;
        }

        result[k] = 0;

        break;
    case OP_REPLACE_WORDS:
    case OP_REMOVE_WORDS:

        result = av_strdup(text);
        if (!result)
            return NULL;

        for (int n = 0; n < s->nb_find_list; n++) {
            char *tmp           = result;
            const char *replace = (s->operation == OP_REPLACE_WORDS) ? s->replace_list[n] : "";

            result = av_strireplace(result, s->find_list[n], replace);
            if (!result)
                return NULL;

            av_free(tmp);
        }

        break;
    }

    return result;
}

static char *process_dialog_show_speaker(TextModContext *s, char *ass_line)
{
    ASSDialog *dialog = avpriv_ass_split_dialog(NULL, ass_line);
    int escape_level = 0;
    unsigned pos = 0, len;
    char *result, *text;
    AVBPrint pbuf;

    if (!dialog)
        return NULL;

    text = process_text(s, dialog->text);
    if (!text)
        return NULL;

    if (!dialog->name || !strlen(dialog->name) || !dialog->text || !strlen(dialog->text))
        return av_strdup(ass_line);

    // Find insertion point in case the line starts with style codes
    len = (unsigned)strlen(dialog->text);
    for (unsigned i = 0; i < len; i++) {

        if (dialog->text[i] == '{')
            escape_level++;

        if (dialog->text[i] == '}')
            escape_level--;

        if (escape_level == 0) {
            pos = i;
            break;
        }
    }

    if (s->style && strlen(s->style))
        // When a style is specified reset the insertion point
        // (always add speaker plus style at the start in that case)
        pos = 0;

    if (pos >= len - 1)
        return av_strdup(ass_line);

    av_bprint_init(&pbuf, 1, AV_BPRINT_SIZE_UNLIMITED);

    if (pos > 0) {
        av_bprint_append_data(&pbuf, dialog->text, pos);
    }

    if (s->style && strlen(s->style)) {
        if (s->style[0] == '{')
            // Assume complete and valid style code, e.g. {\c&HFF0000&}
            av_bprintf(&pbuf, "%s", s->style);
        else
            // Otherwise it must be a style name
            av_bprintf(&pbuf, "{\\r%s}", s->style);
    }

    switch (s->speaker_mode) {
    case SM_SQUARE_BRACKETS:
        av_bprintf(&pbuf, "[%s]", dialog->name);
        break;
    case SM_ROUND_BRACKETS:
        av_bprintf(&pbuf, "(%s)", dialog->name);
        break;
    case SM_COLON:
        av_bprintf(&pbuf, "%s:", dialog->name);
        break;
    case SM_PLAIN:
        av_bprintf(&pbuf, "%s", dialog->name);
        break;
    }

    if (s->style && strlen(s->style)) {
        // Reset line style
        if (dialog->style && strlen(dialog->style) && !av_strcasecmp(dialog->style, "default"))
            av_bprintf(&pbuf, "{\\r%s}", dialog->style);
        else
            av_bprintf(&pbuf, "{\\r}");
    }

    if (s->line_break)
        av_bprintf(&pbuf, "\\N");
    else
        av_bprintf(&pbuf, " ");

    av_bprint_append_data(&pbuf, dialog->text + pos, len - pos);

    av_bprint_finalize(&pbuf, &text);

    result = avpriv_ass_get_dialog_ex(dialog->readorder, dialog->layer, dialog->style, dialog->name, dialog->margin_l, dialog->margin_r, dialog->margin_v, dialog->effect, text);

    av_free(text);
    avpriv_ass_free_dialog(&dialog);
    return result;
}

static char *process_dialog(TextModContext *s, char *ass_line)
{
    ASSDialog *dialog;
    char *result, *text;

    if (s->filter_type == TM_SHOW_SPEAKER)
        return process_dialog_show_speaker(s, ass_line);

    dialog = avpriv_ass_split_dialog(NULL, ass_line);
    if (!dialog)
        return NULL;

    text = process_text(s, dialog->text);
    if (!text)
        return NULL;

    result = avpriv_ass_get_dialog_ex(dialog->readorder, dialog->layer, dialog->style, dialog->name, dialog->margin_l, dialog->margin_r, dialog->margin_v, dialog->effect, text);

    av_free(text);
    avpriv_ass_free_dialog(&dialog);
    return result;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->time_base = inlink->time_base;
    outlink->frame_rate = inlink->frame_rate;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    TextModContext *s = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int ret;

    outlink->format = inlink->format;

    ret = av_frame_make_writable(frame);
    if (ret < 0)
        return ret;

    for (unsigned i = 0; i < frame->num_subtitle_areas; i++) {

        AVSubtitleArea *area = frame->subtitle_areas[i];

        if (area->ass) {
            char *tmp = area->ass;
            area->ass = process_dialog(s, area->ass);
            av_free(tmp);
            if (!area->ass)
                return AVERROR(ENOMEM);
        }
    }

    return ff_filter_frame(outlink, frame);
}

#define OFFSET(x) offsetof(TextModContext, x)
#define FLAGS (AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption textmod_options[] = {
    { "mode",             "set operation mode",              OFFSET(operation),    AV_OPT_TYPE_INT,    {.i64=OP_LEET},          OP_LEET, NB_OPS-1, FLAGS, "mode" },
    {   "leet",           "convert text to 'leet speak'",    0,                    AV_OPT_TYPE_CONST,  {.i64=OP_LEET},          0,       0,        FLAGS, "mode" },
    {   "to_upper",       "change to upper case",            0,                    AV_OPT_TYPE_CONST,  {.i64=OP_TO_UPPER},      0,       0,        FLAGS, "mode" },
    {   "to_lower",       "change to lower case",            0,                    AV_OPT_TYPE_CONST,  {.i64=OP_TO_LOWER},      0,       0,        FLAGS, "mode" },
    {   "replace_chars",  "replace characters",              0,                    AV_OPT_TYPE_CONST,  {.i64=OP_REPLACE_CHARS}, 0,       0,        FLAGS, "mode" },
    {   "remove_chars",   "remove characters",               0,                    AV_OPT_TYPE_CONST,  {.i64=OP_REMOVE_CHARS},  0,       0,        FLAGS, "mode" },
    {   "replace_words",  "replace words",                   0,                    AV_OPT_TYPE_CONST,  {.i64=OP_REPLACE_WORDS}, 0,       0,        FLAGS, "mode" },
    {   "remove_words",   "remove words",                    0,                    AV_OPT_TYPE_CONST,  {.i64=OP_REMOVE_WORDS},  0,       0,        FLAGS, "mode" },
    { "find",             "chars/words to find or remove",   OFFSET(find),         AV_OPT_TYPE_STRING, {.str = NULL},           0,       0,        FLAGS, NULL   },
    { "find_file",        "load find param from file",       OFFSET(find_file),    AV_OPT_TYPE_STRING, {.str = NULL},           0,       0,        FLAGS, NULL   },
    { "replace",          "chars/words to replace",          OFFSET(replace),      AV_OPT_TYPE_STRING, {.str = NULL},           0,       0,        FLAGS, NULL   },
    { "replace_file",     "load replace param from file",    OFFSET(replace_file), AV_OPT_TYPE_STRING, {.str = NULL},           0,       0,        FLAGS, NULL   },
    { "separator",        "word separator",                  OFFSET(separator),    AV_OPT_TYPE_STRING, {.str = ","},            0,       0,        FLAGS, NULL   },
    { NULL },
};


static const AVOption censor_options[] = {
    { "mode",               "set censoring mode",        OFFSET(censor_mode), AV_OPT_TYPE_INT,    {.i64=CM_KEEP_FIRST_LAST}, 0, 2, FLAGS, "mode" },
    {   "keep_first_last",  "censor inner chars",        0,                   AV_OPT_TYPE_CONST,  {.i64=CM_KEEP_FIRST_LAST}, 0, 0, FLAGS, "mode" },
    {   "keep_first",       "censor all but first char", 0,                   AV_OPT_TYPE_CONST,  {.i64=CM_KEEP_FIRST},      0, 0, FLAGS, "mode" },
    {   "all",              "censor all chars",          0,                   AV_OPT_TYPE_CONST,  {.i64=CM_ALL},             0, 0, FLAGS, "mode" },
    { "words",              "list of words to censor",   OFFSET(find),        AV_OPT_TYPE_STRING, {.str = NULL},             0, 0, FLAGS, NULL   },
    { "words_file",         "path to word list file",    OFFSET(find_file),   AV_OPT_TYPE_STRING, {.str = NULL},             0, 0, FLAGS, NULL   },
    { "separator",          "word separator",            OFFSET(separator),   AV_OPT_TYPE_STRING, {.str = ","},              0, 0, FLAGS, NULL   },
    { "censor_char",        "replacement character",     OFFSET(censor_char), AV_OPT_TYPE_STRING, {.str = "*"},              0, 0, FLAGS, NULL   },
    { NULL },
};

static const AVOption showspeaker_options[] = {
    { "format",             "speaker name formatting",        OFFSET(speaker_mode), AV_OPT_TYPE_INT,    {.i64=SM_SQUARE_BRACKETS}, 0, 2, FLAGS, "format" },
    {   "square_brackets",  "[speaker] text",                 0,                    AV_OPT_TYPE_CONST,  {.i64=SM_SQUARE_BRACKETS}, 0, 0, FLAGS, "format" },
    {   "round_brackets",   "(speaker) text",                 0,                    AV_OPT_TYPE_CONST,  {.i64=SM_ROUND_BRACKETS},  0, 0, FLAGS, "format" },
    {   "colon",            "speaker: text",                  0,                    AV_OPT_TYPE_CONST,  {.i64=SM_COLON},           0, 0, FLAGS, "format" },
    {   "plain",            "speaker text",                   0,                    AV_OPT_TYPE_CONST,  {.i64=SM_PLAIN},           0, 0, FLAGS, "format" },
    { "line_break",         "insert line break",              OFFSET(line_break),   AV_OPT_TYPE_BOOL,   {.i64=0},                  0, 1, FLAGS, NULL     },
    { "style",              "ass type name or style code",    OFFSET(style),        AV_OPT_TYPE_STRING, {.str = NULL},             0, 0, FLAGS, NULL     },
    { NULL },
};

AVFILTER_DEFINE_CLASS(textmod);
AVFILTER_DEFINE_CLASS(censor);
AVFILTER_DEFINE_CLASS(showspeaker);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_SUBTITLE,
        .config_props  = config_output,
    },
};

const AVFilter ff_sf_textmod = {
    .name          = "textmod",
    .description   = NULL_IF_CONFIG_SMALL("Modify subtitle text in several ways"),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(TextModContext),
    .priv_class    = &textmod_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_SUBFMT(AV_SUBTITLE_FMT_ASS),
};

const AVFilter ff_sf_censor = {
    .name          = "censor",
    .description   = NULL_IF_CONFIG_SMALL("Censor words in subtitle text"),
    .init          = init_censor,
    .uninit        = uninit,
    .priv_size     = sizeof(TextModContext),
    .priv_class    = &censor_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_SUBFMT(AV_SUBTITLE_FMT_ASS),
};

const AVFilter ff_sf_showspeaker = {
    .name          = "showspeaker",
    .description   = NULL_IF_CONFIG_SMALL("Prepend speaker names to text subtitles (when available)"),
    .init          = init_showspeaker,
    .uninit        = uninit,
    .priv_size     = sizeof(TextModContext),
    .priv_class    = &showspeaker_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_SUBFMT(AV_SUBTITLE_FMT_ASS),
};
