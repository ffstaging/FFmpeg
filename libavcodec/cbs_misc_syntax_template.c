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

static int FUNC(a53_bar_data)(CodedBitstreamContext *ctx, RWContext *rw,
                              A53BarData *current)
{
    int err;

    ui(1, top_bar_flag);
    ui(1, bottom_bar_flag);
    ui(1, left_bar_flag);
    ui(1, right_bar_flag);
    fixed(4, reserved, 0xf);

    if (current->top_bar_flag) {
        fixed(2, one_bits, 3);
        ui(14, line_number_end_of_top_bar);
    }
    if (current->bottom_bar_flag) {
        fixed(2, one_bits, 3);
        ui(14, line_number_end_of_bottom_bar);
    }
    if (current->left_bar_flag) {
        fixed(2, one_bits, 3);
        ui(14, line_number_end_of_left_bar);
    }
    if (current->right_bar_flag) {
        fixed(2, one_bits, 3);
        ui(14, line_number_end_of_right_bar);
    }

    return 0;
}

static int FUNC(cea708_cc_data_packet)(CodedBitstreamContext *ctx,
                                       RWContext *rw,
                                       CEA708CCDataPacket *current)
{
    int err;

    fixed(5, marker_bits, 0x1f);
    ui(1, cc_valid);
    ui(2, cc_type);

    ui(8, cc_data_1);
    ui(8, cc_data_2);

    return 0;
}

static int FUNC(cea708_cc_data)(CodedBitstreamContext *ctx, RWContext *rw,
                                CEA708CCData *current)
{
    int err, i;

    ui(1, process_em_data_flag);
    ui(1, process_cc_data_flag);
    ui(1, additional_data_flag);

    ui(5, cc_count);

    ui(8, em_data);

    for (i = 0; i < current->cc_count; i++) {
        CHECK(FUNC(cea708_cc_data_packet)(ctx, rw,
                                          &current->cc_data_pkts[i]));
    }

    fixed(8, marker_bits, 0xff);

    if (current->additional_data_flag) {
        // Ignored.
    }

    return 0;
}

static int FUNC(a53_atsc_user_data)(CodedBitstreamContext *ctx, RWContext *rw,
                                    A53ATSCUserData *current)
{
    int err;

    ui(8, user_data_type_code);

    switch (current->user_data_type_code) {
    case A53_USER_DATA_TYPE_CODE_CC_DATA:
        return FUNC(cea708_cc_data)(ctx, rw, &current->cc_data);
    case A53_USER_DATA_TYPE_CODE_BAR_DATA:
        return FUNC(a53_bar_data)(ctx, rw, &current->bar_data);
    default:
        av_log(ctx->log_ctx, AV_LOG_WARNING,
               "Unknown ATSC user data found: type code %#02x.\n",
               current->user_data_type_code);
    }

    return 0;
}

static int FUNC(a53_afd_data)(CodedBitstreamContext *ctx, RWContext *rw,
                              A53AFDData *current)
{
    int err;

    fixed(1, zero_bit, 0);
    ui(1, active_format_flag);
    fixed(6, alignment_bits, 1);

    if (current->active_format_flag) {
        fixed(4, reserved, 0xf);
        ui(4, active_format);
    }

    return 0;
}

static int FUNC(a53_user_data)(CodedBitstreamContext *ctx, RWContext *rw,
                               A53UserData *current)
{
    int err;

    ui(32, user_identifier);

    switch (current->user_identifier) {
    case A53_USER_IDENTIFIER_ATSC:
        return FUNC(a53_atsc_user_data)(ctx, rw, &current->atsc);
    case A53_USER_IDENTIFIER_AFD:
        return FUNC(a53_afd_data)(ctx, rw, &current->afd);
    default:
        av_log(ctx->log_ctx, AV_LOG_WARNING,
               "Unknown registered user data found: identifier %#08x.\n",
               current->user_identifier);
    }

    return 0;
}
