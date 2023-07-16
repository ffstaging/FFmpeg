/*
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

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"

#include "cbs.h"
#include "cbs_internal.h"
#include "cbs_misc.h"

#define CHECK(call) do { \
        err = (call); \
        if (err < 0) \
            return err; \
    } while (0)

#define FUNC_NAME(rw, codec, name) cbs_ ## codec ## _ ## rw ## _ ## name
#define FUNC_MISC(rw, name) FUNC_NAME(rw, misc, name)
#define FUNC(name) FUNC_MISC(READWRITE, name)


#define READWRITE read
#define RWContext GetBitContext

#define xui(width, name, var) do { \
        uint32_t value = 0; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, width, #name, NULL, \
                                   &value, 0, MAX_UINT_BITS(width))); \
        var = value; \
    } while (0)

#define ui(width, name) \
        xui(width, name, current->name)

#define fixed(width, name, expected) do { \
        av_unused uint32_t value; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, width, #name, NULL, \
                                   &value, expected, expected)); \
    } while (0)

#include "cbs_misc_syntax_template.c"

#undef READWRITE
#undef RWContext
#undef xui
#undef ui
#undef fixed


#define READWRITE write
#define RWContext PutBitContext

#define xui(width, name, var) do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, width, #name, NULL, \
                                    var, 0, MAX_UINT_BITS(width))); \
    } while (0)

#define ui(width, name) \
        xui(width, name, current->name)

#define fixed(width, name, value) do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, width, #name, NULL, \
                                    value, value, value)); \
    } while (0)

#include "cbs_misc_syntax_template.c"

#undef READWRITE
#undef RWContext
#undef xui
#undef ui
#undef fixed


int ff_cbs_read_a53_user_data(CodedBitstreamContext *ctx,
                              A53UserData *data,
                              const uint8_t *read_buffer, size_t length)
{
    GetBitContext gbc;
    int err;

    err = init_get_bits(&gbc, read_buffer, 8 * length);
    if (err < 0)
        return err;

    return cbs_misc_read_a53_user_data(ctx, &gbc, data);
}

int ff_cbs_write_a53_user_data(CodedBitstreamContext *ctx,
                               uint8_t *write_buffer, size_t *length,
                               A53UserData *data)
{
    PutBitContext pbc;
    int err;

    init_put_bits(&pbc, write_buffer, *length);

    err = cbs_misc_write_a53_user_data(ctx, &pbc, data);
    if (err < 0) {
        // Includes AVERROR(ENOSPC).
        return err;
    }

    // That output must be aligned.
    av_assert0(put_bits_count(&pbc) % 8 == 0);

    *length = put_bits_count(&pbc) / 8;

    flush_put_bits(&pbc);

    return 0;
}

int ff_cbs_read_a53_cc_side_data(CodedBitstreamContext *ctx,
                                 A53UserData *data,
                                 const uint8_t *side_data,
                                 size_t side_data_size)
{
    GetBitContext gbc;
    CEA708CCData *cc;
    int err, i, cc_count;

    if (side_data_size % 3) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "A53 CC side data length must "
               "be a multiple of 3 (got %zu).\n", side_data_size);
        return AVERROR(EINVAL);
    }
    cc_count = side_data_size / 3;
    if (cc_count > 31) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "A53 CC can only fit 31 packets "
               "in a single user data block (got %d).\n", cc_count);
        return AVERROR(EINVAL);
    }

    *data = (A53UserData) {
        .user_identifier = A53_USER_IDENTIFIER_ATSC,

        .atsc = {
            .user_data_type_code = A53_USER_DATA_TYPE_CODE_CC_DATA,

            .cc_data = {
                .process_em_data_flag = 0,
                .process_cc_data_flag = 1,
                .additional_data_flag = 0,

                .em_data = 0,

                .cc_count = cc_count,
            },
        },
    };
    cc = &data->atsc.cc_data;

    err = init_get_bits(&gbc, side_data, 8 * side_data_size);
    if (err < 0)
        return err;

    for (i = 0; i < cc->cc_count; i++) {
        err = cbs_misc_read_cea708_cc_data_packet(ctx, &gbc,
                                                  &cc->cc_data_pkts[i]);
        if (err < 0)
            return err;
    }

    return 0;
}

int ff_cbs_write_a53_cc_side_data(CodedBitstreamContext *ctx,
                                  uint8_t **side_data,
                                  size_t *side_data_size,
                                  A53UserData *data)
{
    PutBitContext pbc;
    CEA708CCData *cc;
    int err, i;

    if (data->user_identifier != A53_USER_IDENTIFIER_ATSC ||
        data->atsc.user_data_type_code != A53_USER_DATA_TYPE_CODE_CC_DATA)
        return AVERROR(EINVAL);

    cc = &data->atsc.cc_data;

    err = av_reallocp(side_data, *side_data_size + 3 * cc->cc_count);
    if (err < 0)
        return err;

    init_put_bits(&pbc, *side_data + *side_data_size, 3 * cc->cc_count);

    for (i = 0; i < cc->cc_count; i++) {
        err = cbs_misc_write_cea708_cc_data_packet(ctx, &pbc,
                                                   &cc->cc_data_pkts[i]);
        if (err < 0) {
            av_freep(side_data);
            return err;
        }
    }

    flush_put_bits(&pbc);
    *side_data_size += 3 * cc->cc_count;

    return 0;
}
