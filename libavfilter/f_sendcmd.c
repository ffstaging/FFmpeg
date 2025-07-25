/*
 * Copyright (c) 2012 Stefano Sabatini
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
 * send commands filter
 */

#include "config_components.h"

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/eval.h"
#include "libavutil/file.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "filters.h"
#include "audio.h"
#include "video.h"

#define COMMAND_FLAG_ENTER 1
#define COMMAND_FLAG_LEAVE 2
#define COMMAND_FLAG_EXPR  4

static const char *const var_names[] = {
    "N",     /* frame number */
    "T",     /* frame time in seconds */
    "PTS",   /* frame pts */
    "TS",    /* interval start time in seconds */
    "TE",    /* interval end time in seconds */
    "TI",    /* interval interpolated value: TI = (T - TS) / (TE - TS) */
    "W",     /* width for video frames */
    "H",     /* height for video frames */
    NULL
};

enum var_name {
    VAR_N,
    VAR_T,
    VAR_PTS,
    VAR_TS,
    VAR_TE,
    VAR_TI,
    VAR_W,
    VAR_H,
    VAR_VARS_NB
};

static inline char *make_command_flags_str(AVBPrint *pbuf, int flags)
{
    static const char * const flag_strings[] = { "enter", "leave", "expr" };
    int i, is_first = 1;

    av_bprint_init(pbuf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    for (i = 0; i < FF_ARRAY_ELEMS(flag_strings); i++) {
        if (flags & 1<<i) {
            if (!is_first)
                av_bprint_chars(pbuf, '+', 1);
            av_bprintf(pbuf, "%s", flag_strings[i]);
            is_first = 0;
        }
    }

    return pbuf->str;
}

typedef struct Command {
    int flags;
    char *target, *command, *arg;
    int index;
} Command;

typedef struct Interval {
    int64_t start_ts;          ///< start timestamp expressed as microseconds units
    int64_t end_ts;            ///< end   timestamp expressed as microseconds units
    int index;                 ///< unique index for these interval commands
    Command *commands;
    int   nb_commands;
    int enabled;               ///< current time detected inside this interval
} Interval;

typedef struct SendCmdContext {
    const AVClass *class;
    Interval *intervals;
    int   nb_intervals;

    char *commands_filename;
    char *commands_str;
} SendCmdContext;

#define OFFSET(x) offsetof(SendCmdContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "commands", "set commands", OFFSET(commands_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "c",        "set commands", OFFSET(commands_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "filename", "set commands file",  OFFSET(commands_filename), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "f",        "set commands file",  OFFSET(commands_filename), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { NULL }
};

#define SPACES " \f\t\n\r"

static void skip_comments(const char **buf)
{
    while (**buf) {
        /* skip leading spaces */
        *buf += strspn(*buf, SPACES);
        if (**buf != '#')
            break;

        (*buf)++;

        /* skip comment until the end of line */
        *buf += strcspn(*buf, "\n");
        if (**buf)
            (*buf)++;
    }
}

#define COMMAND_DELIMS " \f\t\n\r,;"

/**
 * Clears fields and frees the buffers used by @p cmd
 */
static void clear_command(Command *cmd)
{
    cmd->flags = 0;
    cmd->index = 0;
    av_freep(&cmd->target);
    av_freep(&cmd->command);
    av_freep(&cmd->arg);
}

static int parse_command(Command *cmd, int cmd_count, int interval_count,
                         const char **buf, void *log_ctx)
{
    int ret;

    memset(cmd, 0, sizeof(Command));
    cmd->index = cmd_count;

    /* format: [FLAGS] target command arg */
    *buf += strspn(*buf, SPACES);

    /* parse flags */
    if (**buf == '[') {
        (*buf)++; /* skip "[" */

        while (**buf) {
            int len = strcspn(*buf, "|+]");

            if      (!strncmp(*buf, "enter", strlen("enter"))) cmd->flags |= COMMAND_FLAG_ENTER;
            else if (!strncmp(*buf, "leave", strlen("leave"))) cmd->flags |= COMMAND_FLAG_LEAVE;
            else if (!strncmp(*buf, "expr",  strlen("expr")))  cmd->flags |= COMMAND_FLAG_EXPR;
            else {
                char flag_buf[64];
                av_strlcpy(flag_buf, *buf, sizeof(flag_buf));
                av_log(log_ctx, AV_LOG_ERROR,
                       "Unknown flag '%s' in interval #%d, command #%d\n",
                       flag_buf, interval_count, cmd_count);
                return AVERROR(EINVAL);
            }
            *buf += len;
            if (**buf == ']')
                break;
            if (!strspn(*buf, "+|")) {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Invalid flags char '%c' in interval #%d, command #%d\n",
                       **buf, interval_count, cmd_count);
                return AVERROR(EINVAL);
            }
            if (**buf)
                (*buf)++;
        }

        if (**buf != ']') {
            av_log(log_ctx, AV_LOG_ERROR,
                   "Missing flag terminator or extraneous data found at the end of flags "
                   "in interval #%d, command #%d\n", interval_count, cmd_count);
            return AVERROR(EINVAL);
        }
        (*buf)++; /* skip "]" */
    } else {
        cmd->flags = COMMAND_FLAG_ENTER;
    }

    *buf += strspn(*buf, SPACES);
    cmd->target = av_get_token(buf, COMMAND_DELIMS);
    if (!cmd->target || !cmd->target[0]) {
        av_log(log_ctx, AV_LOG_ERROR,
               "No target specified in interval #%d, command #%d\n",
               interval_count, cmd_count);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    *buf += strspn(*buf, SPACES);
    cmd->command = av_get_token(buf, COMMAND_DELIMS);
    if (!cmd->command || !cmd->command[0]) {
        av_log(log_ctx, AV_LOG_ERROR,
               "No command specified in interval #%d, command #%d\n",
               interval_count, cmd_count);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    *buf += strspn(*buf, SPACES);
    cmd->arg = av_get_token(buf, COMMAND_DELIMS);

    return 1;

fail:
    clear_command(cmd);
    return ret;
}

static int parse_commands(Command **cmds, int *nb_cmds, int interval_count,
                          const char **buf, void *log_ctx)
{
    int cmd_count = 0;
    int ret, n = 0;
    AVBPrint pbuf;

    *cmds = NULL;
    *nb_cmds = 0;

    while (**buf) {
        Command cmd;

        if ((ret = parse_command(&cmd, cmd_count, interval_count, buf, log_ctx)) < 0)
            return ret;
        cmd_count++;

        /* (re)allocate commands array if required */
        if (*nb_cmds == n) {
            n = FFMAX(16, 2*n); /* first allocation = 16, or double the number */
            *cmds = av_realloc_f(*cmds, n, 2*sizeof(Command));
            if (!*cmds) {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Could not (re)allocate command array\n");
                clear_command(&cmd);
                return AVERROR(ENOMEM);
            }
        }

        (*cmds)[(*nb_cmds)++] = cmd;

        *buf += strspn(*buf, SPACES);
        if (**buf && **buf != ';' && **buf != ',') {
            av_log(log_ctx, AV_LOG_ERROR,
                   "Missing separator or extraneous data found at the end of "
                   "interval #%d, in command #%d\n",
                   interval_count, cmd_count);
            av_log(log_ctx, AV_LOG_ERROR,
                   "Command was parsed as: flags:[%s] target:%s command:%s arg:%s\n",
                   make_command_flags_str(&pbuf, cmd.flags), cmd.target, cmd.command, cmd.arg);
            return AVERROR(EINVAL);
        }
        if (**buf == ';')
            break;
        if (**buf == ',')
            (*buf)++;
    }

    return 0;
}

#define DELIMS " \f\t\n\r,;"

static int parse_interval(Interval *interval, int interval_count,
                          const char **buf, void *log_ctx)
{
    char *intervalstr;
    int ret;

    *buf += strspn(*buf, SPACES);
    if (!**buf)
        return 0;

    /* reset data */
    memset(interval, 0, sizeof(Interval));
    interval->index = interval_count;

    /* format: INTERVAL COMMANDS */

    /* parse interval */
    intervalstr = av_get_token(buf, DELIMS);
    if (intervalstr && intervalstr[0]) {
        char *start, *end;

        start = av_strtok(intervalstr, "-", &end);
        if (!start) {
            ret = AVERROR(EINVAL);
            av_log(log_ctx, AV_LOG_ERROR,
                   "Invalid interval specification '%s' in interval #%d\n",
                   intervalstr, interval_count);
            goto end;
        }
        if ((ret = av_parse_time(&interval->start_ts, start, 1)) < 0) {
            av_log(log_ctx, AV_LOG_ERROR,
                   "Invalid start time specification '%s' in interval #%d\n",
                   start, interval_count);
            goto end;
        }

        if (end) {
            if ((ret = av_parse_time(&interval->end_ts, end, 1)) < 0) {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Invalid end time specification '%s' in interval #%d\n",
                       end, interval_count);
                goto end;
            }
        } else {
            interval->end_ts = INT64_MAX;
        }
        if (interval->end_ts < interval->start_ts) {
            av_log(log_ctx, AV_LOG_ERROR,
                   "Invalid end time '%s' in interval #%d: "
                   "cannot be lesser than start time '%s'\n",
                   end, interval_count, start);
            ret = AVERROR(EINVAL);
            goto end;
        }
    } else {
        av_log(log_ctx, AV_LOG_ERROR,
               "No interval specified for interval #%d\n", interval_count);
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* parse commands */
    ret = parse_commands(&interval->commands, &interval->nb_commands,
                         interval_count, buf, log_ctx);

end:
    av_free(intervalstr);
    return ret;
}

static int parse_intervals(Interval **intervals, int *nb_intervals,
                           const char *buf, void *log_ctx)
{
    int interval_count = 0;
    int ret, n = 0;

    *intervals = NULL;
    *nb_intervals = 0;

    if (!buf)
        return 0;

    while (1) {
        Interval interval;

        skip_comments(&buf);
        if (!(*buf))
            break;

        if ((ret = parse_interval(&interval, interval_count, &buf, log_ctx)) < 0)
            return ret;

        buf += strspn(buf, SPACES);
        if (*buf) {
            if (*buf != ';') {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Missing terminator or extraneous data found at the end of interval #%d\n",
                       interval_count);
                return AVERROR(EINVAL);
            }
            buf++; /* skip ';' */
        }
        interval_count++;

        /* (re)allocate commands array if required */
        if (*nb_intervals == n) {
            n = FFMAX(16, 2*n); /* first allocation = 16, or double the number */
            *intervals = av_realloc_f(*intervals, n, 2*sizeof(Interval));
            if (!*intervals) {
                av_log(log_ctx, AV_LOG_ERROR,
                       "Could not (re)allocate intervals array\n");
                return AVERROR(ENOMEM);
            }
        }

        (*intervals)[(*nb_intervals)++] = interval;
    }

    return 0;
}

static int cmp_intervals(const void *a, const void *b)
{
    const Interval *i1 = a;
    const Interval *i2 = b;
    return 2 * FFDIFFSIGN(i1->start_ts, i2->start_ts) + FFDIFFSIGN(i1->index, i2->index);
}

static av_cold int init(AVFilterContext *ctx)
{
    SendCmdContext *s = ctx->priv;
    int ret, i, j;

    if ((!!s->commands_filename + !!s->commands_str) != 1) {
        av_log(ctx, AV_LOG_ERROR,
               "One and only one of the filename or commands options must be specified\n");
        return AVERROR(EINVAL);
    }

    if (s->commands_filename) {
        uint8_t *file_buf, *buf;
        size_t file_bufsize;
        ret = av_file_map(s->commands_filename,
                          &file_buf, &file_bufsize, 0, ctx);
        if (ret < 0)
            return ret;

        /* create a 0-terminated string based on the read file */
        buf = av_malloc(file_bufsize + 1);
        if (!buf) {
            av_file_unmap(file_buf, file_bufsize);
            return AVERROR(ENOMEM);
        }
        memcpy(buf, file_buf, file_bufsize);
        buf[file_bufsize] = 0;
        av_file_unmap(file_buf, file_bufsize);
        s->commands_str = buf;
    }

    if ((ret = parse_intervals(&s->intervals, &s->nb_intervals,
                               s->commands_str, ctx)) < 0)
        return ret;

    if (s->nb_intervals == 0) {
        av_log(ctx, AV_LOG_ERROR, "No commands were specified\n");
        return AVERROR(EINVAL);
    }

    qsort(s->intervals, s->nb_intervals, sizeof(Interval), cmp_intervals);

    av_log(ctx, AV_LOG_DEBUG, "Parsed commands:\n");
    for (i = 0; i < s->nb_intervals; i++) {
        AVBPrint pbuf;
        Interval *interval = &s->intervals[i];
        av_log(ctx, AV_LOG_VERBOSE, "start_time:%f end_time:%f index:%d\n",
               (double)interval->start_ts/1000000, (double)interval->end_ts/1000000, interval->index);
        for (j = 0; j < interval->nb_commands; j++) {
            Command *cmd = &interval->commands[j];
            av_log(ctx, AV_LOG_VERBOSE,
                   "    [%s] target:%s command:%s arg:%s index:%d\n",
                   make_command_flags_str(&pbuf, cmd->flags), cmd->target, cmd->command, cmd->arg, cmd->index);
        }
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SendCmdContext *s = ctx->priv;
    int i, j;

    for (i = 0; i < s->nb_intervals; i++) {
        Interval *interval = &s->intervals[i];
        for (j = 0; j < interval->nb_commands; j++) {
            Command *cmd = &interval->commands[j];
            clear_command(cmd);
        }
        av_freep(&interval->commands);
    }
    av_freep(&s->intervals);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *ref)
{
    FilterLink *inl = ff_filter_link(inlink);
    AVFilterContext *ctx = inlink->dst;
    SendCmdContext *s = ctx->priv;
    int64_t ts;
    int i, j, ret;

    if (ref->pts == AV_NOPTS_VALUE)
        goto end;

    ts = av_rescale_q(ref->pts, inlink->time_base, AV_TIME_BASE_Q);

#define WITHIN_INTERVAL(ts, start_ts, end_ts) ((ts) >= (start_ts) && (ts) < (end_ts))

    for (i = 0; i < s->nb_intervals; i++) {
        Interval *interval = &s->intervals[i];
        int flags = 0;

        if (!interval->enabled && WITHIN_INTERVAL(ts, interval->start_ts, interval->end_ts)) {
            flags += COMMAND_FLAG_ENTER;
            interval->enabled = 1;
        }
        if (interval->enabled && !WITHIN_INTERVAL(ts, interval->start_ts, interval->end_ts)) {
            flags += COMMAND_FLAG_LEAVE;
            interval->enabled = 0;
        }
        if (interval->enabled)
            flags += COMMAND_FLAG_EXPR;

        if (flags) {
            AVBPrint pbuf;
            av_log(ctx, AV_LOG_VERBOSE,
                   "[%s] interval #%d start_ts:%f end_ts:%f ts:%f\n",
                   make_command_flags_str(&pbuf, flags), interval->index,
                   (double)interval->start_ts/1000000, (double)interval->end_ts/1000000,
                   (double)ts/1000000);

            for (j = 0; flags && j < interval->nb_commands; j++) {
                Command *cmd = &interval->commands[j];
                char *cmd_arg = cmd->arg;
                char buf[1024];

                if (cmd->flags & flags) {
                    if (cmd->flags & COMMAND_FLAG_EXPR) {
                        double var_values[VAR_VARS_NB], res;
                        double start = TS2T(interval->start_ts, AV_TIME_BASE_Q);
                        double end = TS2T(interval->end_ts, AV_TIME_BASE_Q);
                        double current = TS2T(ref->pts, inlink->time_base);

                        var_values[VAR_N]   = inl->frame_count_in;
                        var_values[VAR_PTS] = TS2D(ref->pts);
                        var_values[VAR_T]   = current;
                        var_values[VAR_TS]  = start;
                        var_values[VAR_TE]  = end;
                        var_values[VAR_TI]  = (current - start) / (end - start);
                        var_values[VAR_W]   = ref->width;
                        var_values[VAR_H]   = ref->height;

                        if ((ret = av_expr_parse_and_eval(&res, cmd->arg, var_names, var_values,
                                                          NULL, NULL, NULL, NULL, NULL, 0, NULL)) < 0) {
                            av_log(ctx, AV_LOG_ERROR, "Invalid expression '%s' for command argument.\n", cmd->arg);
                            av_frame_free(&ref);
                            return AVERROR(EINVAL);
                        }

                        cmd_arg = av_asprintf("%g", res);
                        if (!cmd_arg) {
                            av_frame_free(&ref);
                            return AVERROR(ENOMEM);
                        }
                    }
                    av_log(ctx, AV_LOG_VERBOSE,
                           "Processing command #%d target:%s command:%s arg:%s\n",
                           cmd->index, cmd->target, cmd->command, cmd_arg);
                    ret = avfilter_graph_send_command(inl->graph,
                                                      cmd->target, cmd->command, cmd_arg,
                                                      buf, sizeof(buf),
                                                      AVFILTER_CMD_FLAG_ONE);
                    av_log(ctx, AV_LOG_VERBOSE,
                           "Command reply for command #%d: ret:%s res:%s\n",
                           cmd->index, av_err2str(ret), buf);
                    if (cmd->flags & COMMAND_FLAG_EXPR)
                        av_freep(&cmd_arg);
                }
            }
        }
    }

end:
    switch (inlink->type) {
    case AVMEDIA_TYPE_VIDEO:
    case AVMEDIA_TYPE_AUDIO:
        return ff_filter_frame(inlink->dst->outputs[0], ref);
    }

    return AVERROR(ENOSYS);
}

AVFILTER_DEFINE_CLASS_EXT(sendcmd, "(a)sendcmd", options);

#if CONFIG_SENDCMD_FILTER

static const AVFilterPad sendcmd_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_sendcmd = {
    .p.name        = "sendcmd",
    .p.description = NULL_IF_CONFIG_SMALL("Send commands to filters."),
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .p.priv_class  = &sendcmd_class,
    .init        = init,
    .uninit      = uninit,
    .priv_size   = sizeof(SendCmdContext),
    FILTER_INPUTS(sendcmd_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
};

#endif

#if CONFIG_ASENDCMD_FILTER

static const AVFilterPad asendcmd_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_af_asendcmd = {
    .p.name        = "asendcmd",
    .p.description = NULL_IF_CONFIG_SMALL("Send commands to filters."),
    .p.priv_class  = &sendcmd_class,
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .init        = init,
    .uninit      = uninit,
    .priv_size   = sizeof(SendCmdContext),
    FILTER_INPUTS(asendcmd_inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
};

#endif
