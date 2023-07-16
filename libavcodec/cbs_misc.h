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

#ifndef AVCODEC_CBS_MISC_H
#define AVCODEC_CBS_MISC_H

#include <stddef.h>
#include <stdint.h>

#include "libavutil/common.h"


enum {
    A53_USER_IDENTIFIER_ATSC = MKBETAG('G', 'A', '9', '4'),
    A53_USER_IDENTIFIER_AFD  = MKBETAG('D', 'T', 'G', '1'),
};

enum {
    A53_USER_DATA_TYPE_CODE_CC_DATA  = 0x03,
    A53_USER_DATA_TYPE_CODE_BAR_DATA = 0x06,
};

typedef struct A53BarData {
    uint8_t top_bar_flag;
    uint8_t bottom_bar_flag;
    uint8_t left_bar_flag;
    uint8_t right_bar_flag;

    uint16_t line_number_end_of_top_bar;
    uint16_t line_number_end_of_bottom_bar;
    uint16_t line_number_end_of_left_bar;
    uint16_t line_number_end_of_right_bar;
} A53BarData;

typedef struct CEA708CCDataPacket {
    uint8_t cc_valid;
    uint8_t cc_type;
    uint8_t cc_data_1;
    uint8_t cc_data_2;
} CEA708CCDataPacket;

typedef struct CEA708CCData {
    uint8_t process_em_data_flag;
    uint8_t process_cc_data_flag;
    uint8_t additional_data_flag;

    uint8_t em_data;

    uint8_t cc_count;
    CEA708CCDataPacket cc_data_pkts[31];
} CEA708CCData;

typedef struct A53ATSCUserData {
    uint8_t user_data_type_code;
    union {
        CEA708CCData cc_data;
        A53BarData bar_data;
    };
} A53ATSCUserData;

typedef struct A53AFDData {
    uint8_t active_format_flag;
    uint8_t active_format;
} A53AFDData;

typedef struct A53UserData {
    uint32_t user_identifier;
    union {
        A53ATSCUserData atsc;
        A53AFDData afd;
    };
} A53UserData;


int ff_cbs_read_a53_user_data(CodedBitstreamContext *ctx,
                              A53UserData *data,
                              const uint8_t *read_buffer, size_t length);

int ff_cbs_write_a53_user_data(CodedBitstreamContext *ctx,
                               uint8_t *write_buffer, size_t *length,
                               A53UserData *data);

int ff_cbs_read_a53_cc_side_data(CodedBitstreamContext *ctx,
                                 A53UserData *data,
                                 const uint8_t *side_data,
                                 size_t side_data_size);

int ff_cbs_write_a53_cc_side_data(CodedBitstreamContext *ctx,
                                  uint8_t **side_data,
                                  size_t *side_data_length,
                                  A53UserData *data);


#endif /* AVCODEC_CBS_MISC_H */
