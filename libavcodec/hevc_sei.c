/*
 * HEVC Supplementary Enhancement Information messages
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2013 Vittorio Giovara
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

#include "atsc_a53.h"
#include "bytestream.h"
#include "dynamic_hdr10_plus.h"
#include "dynamic_hdr_vivid.h"
#include "golomb.h"
#include "hevc_ps.h"
#include "hevc_sei.h"

#include "libavutil/display.h"
#include "libavutil/film_grain_params.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/stereo3d.h"
#include "libavutil/timecode.h"

static int decode_nal_sei_decoded_picture_hash(HEVCSEIPictureHash *s,
                                               GetByteContext *gb)
{
    int cIdx;
    uint8_t hash_type;
    //uint16_t picture_crc;
    //uint32_t picture_checksum;
    hash_type = bytestream2_get_byte(gb);

    for (cIdx = 0; cIdx < 3/*((s->sps->chroma_format_idc == 0) ? 1 : 3)*/; cIdx++) {
        if (hash_type == 0) {
            s->is_md5 = 1;
            bytestream2_get_buffer(gb, s->md5[cIdx], sizeof(s->md5[cIdx]));
        } else if (hash_type == 1) {
            // picture_crc = get_bits(gb, 16);
        } else if (hash_type == 2) {
            // picture_checksum = get_bits_long(gb, 32);
        }
    }
    return 0;
}

static int decode_nal_sei_mastering_display_info(HEVCSEIMasteringDisplay *s,
                                                 GetByteContext *gb)
{
    int i;

    if (bytestream2_get_bytes_left(gb) < 24)
        return AVERROR_INVALIDDATA;

    // Mastering primaries
    for (i = 0; i < 3; i++) {
        s->display_primaries[i][0] = bytestream2_get_be16u(gb);
        s->display_primaries[i][1] = bytestream2_get_be16u(gb);
    }
    // White point (x, y)
    s->white_point[0] = bytestream2_get_be16u(gb);
    s->white_point[1] = bytestream2_get_be16u(gb);

    // Max and min luminance of mastering display
    s->max_luminance = bytestream2_get_be32u(gb);
    s->min_luminance = bytestream2_get_be32u(gb);

    // As this SEI message comes before the first frame that references it,
    // initialize the flag to 2 and decrement on IRAP access unit so it
    // persists for the coded video sequence (e.g., between two IRAPs)
    s->present = 2;

    return 0;
}

static int decode_nal_sei_content_light_info(HEVCSEIContentLight *s,
                                             GetByteContext *gb)
{
    if (bytestream2_get_bytes_left(gb) < 4)
        return AVERROR_INVALIDDATA;

    // Max and average light levels
    s->max_content_light_level     = bytestream2_get_be16u(gb);
    s->max_pic_average_light_level = bytestream2_get_be16u(gb);
    // As this SEI message comes before the first frame that references it,
    // initialize the flag to 2 and decrement on IRAP access unit so it
    // persists for the coded video sequence (e.g., between two IRAPs)
    s->present = 2;

    return  0;
}

static int decode_nal_sei_frame_packing_arrangement(HEVCSEIFramePacking *s, GetBitContext *gb)
{
    get_ue_golomb_long(gb);             // frame_packing_arrangement_id
    s->present = !get_bits1(gb);

    if (s->present) {
        s->arrangement_type               = get_bits(gb, 7);
        s->quincunx_subsampling           = get_bits1(gb);
        s->content_interpretation_type    = get_bits(gb, 6);

        // spatial_flipping_flag, frame0_flipped_flag, field_views_flag
        skip_bits(gb, 3);
        s->current_frame_is_frame0_flag = get_bits1(gb);
    }
    return 0;
}

static int decode_nal_sei_display_orientation(HEVCSEIDisplayOrientation *s, GetBitContext *gb)
{
    s->present = !get_bits1(gb);

    if (s->present) {
        s->hflip = get_bits1(gb);     // hor_flip
        s->vflip = get_bits1(gb);     // ver_flip

        s->anticlockwise_rotation = get_bits(gb, 16);
        // skip_bits1(gb);     // display_orientation_persistence_flag
    }

    return 0;
}

static int decode_nal_sei_pic_timing(HEVCSEI *s, GetBitContext *gb,
                                     const HEVCParamSets *ps, void *logctx)
{
    HEVCSEIPictureTiming *h = &s->picture_timing;
    HEVCSPS *sps;

    if (!ps->sps_list[s->active_seq_parameter_set_id])
        return(AVERROR(ENOMEM));
    sps = (HEVCSPS*)ps->sps_list[s->active_seq_parameter_set_id]->data;

    if (sps->vui.frame_field_info_present_flag) {
        int pic_struct = get_bits(gb, 4);
        h->picture_struct = AV_PICTURE_STRUCTURE_UNKNOWN;
        if (pic_struct == 2 || pic_struct == 10 || pic_struct == 12) {
            av_log(logctx, AV_LOG_DEBUG, "BOTTOM Field\n");
            h->picture_struct = AV_PICTURE_STRUCTURE_BOTTOM_FIELD;
        } else if (pic_struct == 1 || pic_struct == 9 || pic_struct == 11) {
            av_log(logctx, AV_LOG_DEBUG, "TOP Field\n");
            h->picture_struct = AV_PICTURE_STRUCTURE_TOP_FIELD;
        } else if (pic_struct == 7) {
            av_log(logctx, AV_LOG_DEBUG, "Frame/Field Doubling\n");
            h->picture_struct = HEVC_SEI_PIC_STRUCT_FRAME_DOUBLING;
        } else if (pic_struct == 8) {
            av_log(logctx, AV_LOG_DEBUG, "Frame/Field Tripling\n");
            h->picture_struct = HEVC_SEI_PIC_STRUCT_FRAME_TRIPLING;
        }
    }

    return 0;
}

static int decode_registered_user_data_closed_caption(HEVCSEIA53Caption *s,
                                                      GetByteContext *gb)
{
    int ret;

    ret = ff_parse_a53_cc(&s->buf_ref, gb->buffer,
                          bytestream2_get_bytes_left(gb));
    if (ret < 0)
        return ret;

    return 0;
}

static int decode_nal_sei_user_data_unregistered(HEVCSEIUnregistered *s,
                                                 GetByteContext *gb)
{
    AVBufferRef *buf_ref, **tmp;
    int size = bytestream2_get_bytes_left(gb);

    if (size < 16 || size >= INT_MAX - 1)
       return AVERROR_INVALIDDATA;

    tmp = av_realloc_array(s->buf_ref, s->nb_buf_ref + 1, sizeof(*s->buf_ref));
    if (!tmp)
        return AVERROR(ENOMEM);
    s->buf_ref = tmp;

    buf_ref = av_buffer_alloc(size + 1);
    if (!buf_ref)
        return AVERROR(ENOMEM);

    bytestream2_get_bufferu(gb, buf_ref->data, size);
    buf_ref->data[size] = 0;
    buf_ref->size = size;
    s->buf_ref[s->nb_buf_ref++] = buf_ref;

    return 0;
}

static int decode_registered_user_data_dynamic_hdr_plus(HEVCSEIDynamicHDRPlus *s,
                                                        GetByteContext *gb)
{
    size_t meta_size;
    int err;
    AVDynamicHDRPlus *metadata = av_dynamic_hdr_plus_alloc(&meta_size);
    if (!metadata)
        return AVERROR(ENOMEM);

    err = ff_parse_itu_t_t35_to_dynamic_hdr10_plus(metadata, gb->buffer,
                                                   bytestream2_get_bytes_left(gb));
    if (err < 0) {
        av_free(metadata);
        return err;
    }

    av_buffer_unref(&s->info);
    s->info = av_buffer_create((uint8_t *)metadata, meta_size, NULL, NULL, 0);
    if (!s->info) {
        av_free(metadata);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int decode_registered_user_data_dynamic_hdr_vivid(HEVCSEIDynamicHDRVivid *s,
                                                         GetByteContext *gb)
{
    size_t meta_size;
    int err;
    AVDynamicHDRVivid *metadata = av_dynamic_hdr_vivid_alloc(&meta_size);
    if (!metadata)
        return AVERROR(ENOMEM);

    err = ff_parse_itu_t_t35_to_dynamic_hdr_vivid(metadata,
                                                  gb->buffer, bytestream2_get_bytes_left(gb));
    if (err < 0) {
        av_free(metadata);
        return err;
    }

    av_buffer_unref(&s->info);
    s->info = av_buffer_create((uint8_t *)metadata, meta_size, NULL, NULL, 0);
    if (!s->info) {
        av_free(metadata);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int decode_nal_sei_user_data_registered_itu_t_t35(HEVCSEI *s, GetByteContext *gb,
                                                         void *logctx)
{
    int country_code, provider_code;

    if (bytestream2_get_bytes_left(gb) < 3)
        return AVERROR_INVALIDDATA;

    country_code = bytestream2_get_byteu(gb);
    if (country_code == 0xFF) {
        if (bytestream2_get_bytes_left(gb) < 3)
            return AVERROR_INVALIDDATA;

        bytestream2_skipu(gb, 1);
    }

    if (country_code != 0xB5 && country_code != 0x26) { // usa_country_code and cn_country_code
        av_log(logctx, AV_LOG_VERBOSE,
               "Unsupported User Data Registered ITU-T T35 SEI message (country_code = 0x%x)\n",
               country_code);
        return 0;
    }

    provider_code = bytestream2_get_be16u(gb);

    switch (provider_code) {
    case 0x04: { // cuva_provider_code
        const uint16_t cuva_provider_oriented_code = 0x0005;
        uint16_t provider_oriented_code;

        if (bytestream2_get_bytes_left(gb) < 2)
            return AVERROR_INVALIDDATA;

        provider_oriented_code = bytestream2_get_be16u(gb);
        if (provider_oriented_code == cuva_provider_oriented_code) {
            return decode_registered_user_data_dynamic_hdr_vivid(&s->dynamic_hdr_vivid, gb);
        }
        break;
    }
    case 0x3C: { // smpte_provider_code
        // A/341 Amendment - 2094-40
        const uint16_t smpte2094_40_provider_oriented_code = 0x0001;
        const uint8_t smpte2094_40_application_identifier = 0x04;
        uint16_t provider_oriented_code;
        uint8_t application_identifier;

        if (bytestream2_get_bytes_left(gb) < 3)
            return AVERROR_INVALIDDATA;

        provider_oriented_code = bytestream2_get_be16u(gb);
        application_identifier = bytestream2_get_byteu(gb);
        if (provider_oriented_code == smpte2094_40_provider_oriented_code &&
            application_identifier == smpte2094_40_application_identifier) {
            return decode_registered_user_data_dynamic_hdr_plus(&s->dynamic_hdr_plus, gb);
        }
        break;
    }
    case 0x31: { // atsc_provider_code
        uint32_t user_identifier;

        if (bytestream2_get_bytes_left(gb) < 4)
            return AVERROR_INVALIDDATA;

        user_identifier = bytestream2_get_be32u(gb);
        switch (user_identifier) {
        case MKBETAG('G', 'A', '9', '4'):
            return decode_registered_user_data_closed_caption(&s->a53_caption, gb);
        default:
            av_log(logctx, AV_LOG_VERBOSE,
                   "Unsupported User Data Registered ITU-T T35 SEI message (atsc user_identifier = 0x%04x)\n",
                   user_identifier);
            break;
        }
        break;
    }
    default:
        av_log(logctx, AV_LOG_VERBOSE,
               "Unsupported User Data Registered ITU-T T35 SEI message (provider_code = %d)\n",
               provider_code);
        break;
    }

    return 0;
}

static int decode_nal_sei_active_parameter_sets(HEVCSEI *s, GetBitContext *gb, void *logctx)
{
    int num_sps_ids_minus1;
    unsigned active_seq_parameter_set_id;

    get_bits(gb, 4); // active_video_parameter_set_id
    get_bits(gb, 1); // self_contained_cvs_flag
    get_bits(gb, 1); // num_sps_ids_minus1
    num_sps_ids_minus1 = get_ue_golomb_long(gb); // num_sps_ids_minus1

    if (num_sps_ids_minus1 < 0 || num_sps_ids_minus1 > 15) {
        av_log(logctx, AV_LOG_ERROR, "num_sps_ids_minus1 %d invalid\n", num_sps_ids_minus1);
        return AVERROR_INVALIDDATA;
    }

    active_seq_parameter_set_id = get_ue_golomb_long(gb);
    if (active_seq_parameter_set_id >= HEVC_MAX_SPS_COUNT) {
        av_log(logctx, AV_LOG_ERROR, "active_parameter_set_id %d invalid\n", active_seq_parameter_set_id);
        return AVERROR_INVALIDDATA;
    }
    s->active_seq_parameter_set_id = active_seq_parameter_set_id;

    return 0;
}

static int decode_nal_sei_alternative_transfer(HEVCSEIAlternativeTransfer *s,
                                               GetByteContext *gb)
{
    if (bytestream2_get_bytes_left(gb) < 1)
        return AVERROR_INVALIDDATA;

    s->present = 1;
    s->preferred_transfer_characteristics = bytestream2_get_byteu(gb);

    return 0;
}

static int decode_nal_sei_timecode(HEVCSEITimeCode *s, GetBitContext *gb)
{
    s->num_clock_ts = get_bits(gb, 2);

    for (int i = 0; i < s->num_clock_ts; i++) {
        s->clock_timestamp_flag[i] =  get_bits(gb, 1);

        if (s->clock_timestamp_flag[i]) {
            s->units_field_based_flag[i] = get_bits(gb, 1);
            s->counting_type[i]          = get_bits(gb, 5);
            s->full_timestamp_flag[i]    = get_bits(gb, 1);
            s->discontinuity_flag[i]     = get_bits(gb, 1);
            s->cnt_dropped_flag[i]       = get_bits(gb, 1);

            s->n_frames[i]               = get_bits(gb, 9);

            if (s->full_timestamp_flag[i]) {
                s->seconds_value[i]      = av_clip(get_bits(gb, 6), 0, 59);
                s->minutes_value[i]      = av_clip(get_bits(gb, 6), 0, 59);
                s->hours_value[i]        = av_clip(get_bits(gb, 5), 0, 23);
            } else {
                s->seconds_flag[i] = get_bits(gb, 1);
                if (s->seconds_flag[i]) {
                    s->seconds_value[i] = av_clip(get_bits(gb, 6), 0, 59);
                    s->minutes_flag[i]  = get_bits(gb, 1);
                    if (s->minutes_flag[i]) {
                        s->minutes_value[i] = av_clip(get_bits(gb, 6), 0, 59);
                        s->hours_flag[i] =  get_bits(gb, 1);
                        if (s->hours_flag[i]) {
                            s->hours_value[i] = av_clip(get_bits(gb, 5), 0, 23);
                        }
                    }
                }
            }

            s->time_offset_length[i] = get_bits(gb, 5);
            if (s->time_offset_length[i] > 0) {
                s->time_offset_value[i] = get_bits_long(gb, s->time_offset_length[i]);
            }
        }
    }

    s->present = 1;
    return 0;
}

static int decode_film_grain_characteristics(HEVCSEIFilmGrainCharacteristics *h,
                                             GetBitContext *gb)
{
    h->present = !get_bits1(gb); // film_grain_characteristics_cancel_flag

    if (h->present) {
        memset(h, 0, sizeof(*h));
        h->model_id = get_bits(gb, 2);
        h->separate_colour_description_present_flag = get_bits1(gb);
        if (h->separate_colour_description_present_flag) {
            h->bit_depth_luma = get_bits(gb, 3) + 8;
            h->bit_depth_chroma = get_bits(gb, 3) + 8;
            h->full_range = get_bits1(gb);
            h->color_primaries = get_bits(gb, 8);
            h->transfer_characteristics = get_bits(gb, 8);
            h->matrix_coeffs = get_bits(gb, 8);
        }
        h->blending_mode_id = get_bits(gb, 2);
        h->log2_scale_factor = get_bits(gb, 4);
        for (int c = 0; c < 3; c++)
            h->comp_model_present_flag[c] = get_bits1(gb);
        for (int c = 0; c < 3; c++) {
            if (h->comp_model_present_flag[c]) {
                h->num_intensity_intervals[c] = get_bits(gb, 8) + 1;
                h->num_model_values[c] = get_bits(gb, 3) + 1;
                if (h->num_model_values[c] > 6)
                    return AVERROR_INVALIDDATA;
                for (int i = 0; i < h->num_intensity_intervals[c]; i++) {
                    h->intensity_interval_lower_bound[c][i] = get_bits(gb, 8);
                    h->intensity_interval_upper_bound[c][i] = get_bits(gb, 8);
                    for (int j = 0; j < h->num_model_values[c]; j++)
                        h->comp_model_value[c][i][j] = get_se_golomb_long(gb);
                }
            }
        }
        h->persistence_flag = get_bits1(gb);

        h->present = 1;
    }

    return 0;
}

static int decode_nal_sei_prefix(GetBitContext *gb, GetByteContext *gbyte,
                                 void *logctx, HEVCSEI *s,
                                 const HEVCParamSets *ps, int type)
{
    switch (type) {
    case 256:  // Mismatched value from HM 8.1
        return decode_nal_sei_decoded_picture_hash(&s->picture_hash, gbyte);
    case SEI_TYPE_FRAME_PACKING_ARRANGEMENT:
        return decode_nal_sei_frame_packing_arrangement(&s->frame_packing, gb);
    case SEI_TYPE_DISPLAY_ORIENTATION:
        return decode_nal_sei_display_orientation(&s->display_orientation, gb);
    case SEI_TYPE_PIC_TIMING:
        return decode_nal_sei_pic_timing(s, gb, ps, logctx);
    case SEI_TYPE_MASTERING_DISPLAY_COLOUR_VOLUME:
        return decode_nal_sei_mastering_display_info(&s->mastering_display, gbyte);
    case SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO:
        return decode_nal_sei_content_light_info(&s->content_light, gbyte);
    case SEI_TYPE_ACTIVE_PARAMETER_SETS:
        return decode_nal_sei_active_parameter_sets(s, gb, logctx);
    case SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35:
        return decode_nal_sei_user_data_registered_itu_t_t35(s, gbyte, logctx);
    case SEI_TYPE_USER_DATA_UNREGISTERED:
        return decode_nal_sei_user_data_unregistered(&s->unregistered, gbyte);
    case SEI_TYPE_ALTERNATIVE_TRANSFER_CHARACTERISTICS:
        return decode_nal_sei_alternative_transfer(&s->alternative_transfer, gbyte);
    case SEI_TYPE_TIME_CODE:
        return decode_nal_sei_timecode(&s->timecode, gb);
    case SEI_TYPE_FILM_GRAIN_CHARACTERISTICS:
        return decode_film_grain_characteristics(&s->film_grain_characteristics, gb);
    default:
        av_log(logctx, AV_LOG_DEBUG, "Skipped PREFIX SEI %d\n", type);
        return 0;
    }
}

static int decode_nal_sei_suffix(GetBitContext *gb, GetByteContext *gbyte,
                                 void *logctx, HEVCSEI *s, int type)
{
    switch (type) {
    case SEI_TYPE_DECODED_PICTURE_HASH:
        return decode_nal_sei_decoded_picture_hash(&s->picture_hash, gbyte);
    default:
        av_log(logctx, AV_LOG_DEBUG, "Skipped SUFFIX SEI %d\n", type);
        return 0;
    }
}

static int decode_nal_sei_message(GetByteContext *gb, void *logctx, HEVCSEI *s,
                                  const HEVCParamSets *ps, int nal_unit_type)
{
    GetByteContext message_gbyte;
    GetBitContext message_gb;
    int payload_type = 0;
    int payload_size = 0;
    int byte = 0xFF;
    av_unused int ret;
    av_log(logctx, AV_LOG_DEBUG, "Decoding SEI\n");

    while (byte == 0xFF) {
        if (bytestream2_get_bytes_left(gb) < 2 || payload_type > INT_MAX - 255)
            return AVERROR_INVALIDDATA;
        byte          = bytestream2_get_byteu(gb);
        payload_type += byte;
    }
    byte = 0xFF;
    while (byte == 0xFF) {
        if (bytestream2_get_bytes_left(gb) < 1 + payload_size)
            return AVERROR_INVALIDDATA;
        byte          = bytestream2_get_byteu(gb);
        payload_size += byte;
    }
    if (bytestream2_get_bytes_left(gb) < payload_size)
        return AVERROR_INVALIDDATA;
    bytestream2_init(&message_gbyte, gb->buffer, payload_size);
    ret = init_get_bits8(&message_gb, gb->buffer, payload_size);
    av_assert1(ret >= 0);
    bytestream2_skipu(gb, payload_size);
    if (nal_unit_type == HEVC_NAL_SEI_PREFIX) {
        return decode_nal_sei_prefix(&message_gb, &message_gbyte,
                                     logctx, s, ps, payload_type);
    } else { /* nal_unit_type == NAL_SEI_SUFFIX */
        return decode_nal_sei_suffix(&message_gb, &message_gbyte,
                                     logctx, s, payload_type);
    }
}

int ff_hevc_decode_nal_sei(GetBitContext *gb, void *logctx, HEVCSEI *s,
                           const HEVCParamSets *ps, enum HEVCNALUnitType type)
{
    GetByteContext gbyte;
    int ret;

    av_assert1((get_bits_count(gb) % 8) == 0);
    bytestream2_init(&gbyte, gb->buffer + get_bits_count(gb) / 8,
                     get_bits_left(gb) / 8);

    do {
        ret = decode_nal_sei_message(&gbyte, logctx, s, ps, type);
        if (ret < 0)
            return ret;
    } while (bytestream2_get_bytes_left(&gbyte) > 0);
    return 1;
}

void ff_hevc_reset_sei(HEVCSEI *s)
{
    av_buffer_unref(&s->a53_caption.buf_ref);

    for (int i = 0; i < s->unregistered.nb_buf_ref; i++)
        av_buffer_unref(&s->unregistered.buf_ref[i]);
    s->unregistered.nb_buf_ref = 0;
    av_freep(&s->unregistered.buf_ref);
    av_buffer_unref(&s->dynamic_hdr_plus.info);
    av_buffer_unref(&s->dynamic_hdr_vivid.info);
}

int ff_hevc_set_sei_to_frame(AVCodecContext *logctx, HEVCSEI *sei, AVFrame *out, AVRational framerate, uint64_t seed, const VUI *vui, int bit_depth_luma, int bit_depth_chroma)
{
    if (sei->frame_packing.present &&
        sei->frame_packing.arrangement_type >= 3 &&
        sei->frame_packing.arrangement_type <= 5 &&
        sei->frame_packing.content_interpretation_type > 0 &&
        sei->frame_packing.content_interpretation_type < 3) {
        AVStereo3D *stereo = av_stereo3d_create_side_data(out);
        if (!stereo)
            return AVERROR(ENOMEM);

        switch (sei->frame_packing.arrangement_type) {
        case 3:
            if (sei->frame_packing.quincunx_subsampling)
                stereo->type = AV_STEREO3D_SIDEBYSIDE_QUINCUNX;
            else
                stereo->type = AV_STEREO3D_SIDEBYSIDE;
            break;
        case 4:
            stereo->type = AV_STEREO3D_TOPBOTTOM;
            break;
        case 5:
            stereo->type = AV_STEREO3D_FRAMESEQUENCE;
            break;
        }

        if (sei->frame_packing.content_interpretation_type == 2)
            stereo->flags = AV_STEREO3D_FLAG_INVERT;

        if (sei->frame_packing.arrangement_type == 5) {
            if (sei->frame_packing.current_frame_is_frame0_flag)
                stereo->view = AV_STEREO3D_VIEW_LEFT;
            else
                stereo->view = AV_STEREO3D_VIEW_RIGHT;
        }
    }

    if (sei->display_orientation.present &&
        (sei->display_orientation.anticlockwise_rotation ||
         sei->display_orientation.hflip || sei->display_orientation.vflip)) {
        double angle = sei->display_orientation.anticlockwise_rotation * 360 / (double) (1 << 16);
        AVFrameSideData *rotation = av_frame_new_side_data(out,
                                                           AV_FRAME_DATA_DISPLAYMATRIX,
                                                           sizeof(int32_t) * 9);
        if (!rotation)
            return AVERROR(ENOMEM);

        /* av_display_rotation_set() expects the angle in the clockwise
         * direction, hence the first minus.
         * The below code applies the flips after the rotation, yet
         * the H.2645 specs require flipping to be applied first.
         * Because of R O(phi) = O(-phi) R (where R is flipping around
         * an arbitatry axis and O(phi) is the proper rotation by phi)
         * we can create display matrices as desired by negating
         * the degree once for every flip applied. */
        angle = -angle * (1 - 2 * !!sei->display_orientation.hflip)
                       * (1 - 2 * !!sei->display_orientation.vflip);
        av_display_rotation_set((int32_t *)rotation->data, angle);
        av_display_matrix_flip((int32_t *)rotation->data,
                               sei->display_orientation.hflip,
                               sei->display_orientation.vflip);
    }

    if (sei->mastering_display.present) {
        // HEVC uses a g,b,r ordering, which we convert to a more natural r,g,b
        const int mapping[3] = {2, 0, 1};
        const int chroma_den = 50000;
        const int luma_den = 10000;
        int i;
        AVMasteringDisplayMetadata *metadata =
            av_mastering_display_metadata_create_side_data(out);
        if (!metadata)
            return AVERROR(ENOMEM);

        for (i = 0; i < 3; i++) {
            const int j = mapping[i];
            metadata->display_primaries[i][0].num = sei->mastering_display.display_primaries[j][0];
            metadata->display_primaries[i][0].den = chroma_den;
            metadata->display_primaries[i][1].num = sei->mastering_display.display_primaries[j][1];
            metadata->display_primaries[i][1].den = chroma_den;
        }
        metadata->white_point[0].num = sei->mastering_display.white_point[0];
        metadata->white_point[0].den = chroma_den;
        metadata->white_point[1].num = sei->mastering_display.white_point[1];
        metadata->white_point[1].den = chroma_den;

        metadata->max_luminance.num = sei->mastering_display.max_luminance;
        metadata->max_luminance.den = luma_den;
        metadata->min_luminance.num = sei->mastering_display.min_luminance;
        metadata->min_luminance.den = luma_den;
        metadata->has_luminance = 1;
        metadata->has_primaries = 1;

        av_log(logctx, AV_LOG_DEBUG, "Mastering Display Metadata:\n");
        av_log(logctx, AV_LOG_DEBUG,
               "r(%5.4f,%5.4f) g(%5.4f,%5.4f) b(%5.4f %5.4f) wp(%5.4f, %5.4f)\n",
               av_q2d(metadata->display_primaries[0][0]),
               av_q2d(metadata->display_primaries[0][1]),
               av_q2d(metadata->display_primaries[1][0]),
               av_q2d(metadata->display_primaries[1][1]),
               av_q2d(metadata->display_primaries[2][0]),
               av_q2d(metadata->display_primaries[2][1]),
               av_q2d(metadata->white_point[0]), av_q2d(metadata->white_point[1]));
        av_log(logctx, AV_LOG_DEBUG,
               "min_luminance=%f, max_luminance=%f\n",
               av_q2d(metadata->min_luminance), av_q2d(metadata->max_luminance));
    }
    if (sei->content_light.present) {
        AVContentLightMetadata *metadata =
            av_content_light_metadata_create_side_data(out);
        if (!metadata)
            return AVERROR(ENOMEM);
        metadata->MaxCLL  = sei->content_light.max_content_light_level;
        metadata->MaxFALL = sei->content_light.max_pic_average_light_level;

        av_log(logctx, AV_LOG_DEBUG, "Content Light Level Metadata:\n");
        av_log(logctx, AV_LOG_DEBUG, "MaxCLL=%d, MaxFALL=%d\n",
               metadata->MaxCLL, metadata->MaxFALL);
    }

    if (sei->a53_caption.buf_ref) {
        HEVCSEIA53Caption *a53 = &sei->a53_caption;

        AVFrameSideData *sd = av_frame_new_side_data_from_buf(out, AV_FRAME_DATA_A53_CC, a53->buf_ref);
        if (!sd)
            av_buffer_unref(&a53->buf_ref);
        a53->buf_ref = NULL;
    }

    for (int i = 0; i < sei->unregistered.nb_buf_ref; i++) {
        HEVCSEIUnregistered *unreg = &sei->unregistered;

        if (unreg->buf_ref[i]) {
            AVFrameSideData *sd = av_frame_new_side_data_from_buf(out,
                    AV_FRAME_DATA_SEI_UNREGISTERED,
                    unreg->buf_ref[i]);
            if (!sd)
                av_buffer_unref(&unreg->buf_ref[i]);
            unreg->buf_ref[i] = NULL;
        }
    }
    sei->unregistered.nb_buf_ref = 0;

    if (sei->timecode.present) {
        uint32_t *tc_sd;
        char tcbuf[AV_TIMECODE_STR_SIZE];
        AVFrameSideData *tcside = av_frame_new_side_data(out, AV_FRAME_DATA_S12M_TIMECODE,
                                                         sizeof(uint32_t) * 4);
        if (!tcside)
            return AVERROR(ENOMEM);

        tc_sd = (uint32_t*)tcside->data;
        tc_sd[0] = sei->timecode.num_clock_ts;

        for (int i = 0; i < tc_sd[0]; i++) {
            int drop = sei->timecode.cnt_dropped_flag[i];
            int   hh = sei->timecode.hours_value[i];
            int   mm = sei->timecode.minutes_value[i];
            int   ss = sei->timecode.seconds_value[i];
            int   ff = sei->timecode.n_frames[i];

            tc_sd[i + 1] = av_timecode_get_smpte(framerate, drop, hh, mm, ss, ff);
            av_timecode_make_smpte_tc_string2(tcbuf, framerate, tc_sd[i + 1], 0, 0);
            av_dict_set(&out->metadata, "timecode", tcbuf, 0);
        }

        sei->timecode.num_clock_ts = 0;
    }

    if (sei->film_grain_characteristics.present) {
        HEVCSEIFilmGrainCharacteristics *fgc = &sei->film_grain_characteristics;
        AVFilmGrainParams *fgp = av_film_grain_params_create_side_data(out);
        if (!fgp)
            return AVERROR(ENOMEM);

        fgp->type = AV_FILM_GRAIN_PARAMS_H274;
        fgp->seed = seed; /* no poc_offset in HEVC */
        fgp->codec.h274.model_id = fgc->model_id;
        if (fgc->separate_colour_description_present_flag) {
            fgp->codec.h274.bit_depth_luma = fgc->bit_depth_luma;
            fgp->codec.h274.bit_depth_chroma = fgc->bit_depth_chroma;
            fgp->codec.h274.color_range = fgc->full_range + 1;
            fgp->codec.h274.color_primaries = fgc->color_primaries;
            fgp->codec.h274.color_trc = fgc->transfer_characteristics;
            fgp->codec.h274.color_space = fgc->matrix_coeffs;
        } else {
            fgp->codec.h274.bit_depth_luma = bit_depth_luma;
            fgp->codec.h274.bit_depth_chroma = bit_depth_chroma;
            if (vui->video_signal_type_present_flag)
                fgp->codec.h274.color_range = vui->video_full_range_flag + 1;
            else
                fgp->codec.h274.color_range = AVCOL_RANGE_UNSPECIFIED;
            if (vui->colour_description_present_flag) {
                fgp->codec.h274.color_primaries = vui->colour_primaries;
                fgp->codec.h274.color_trc = vui->transfer_characteristic;
                fgp->codec.h274.color_space = vui->matrix_coeffs;
            } else {
                fgp->codec.h274.color_primaries = AVCOL_PRI_UNSPECIFIED;
                fgp->codec.h274.color_trc = AVCOL_TRC_UNSPECIFIED;
                fgp->codec.h274.color_space = AVCOL_SPC_UNSPECIFIED;
            }
        }
        fgp->codec.h274.blending_mode_id = fgc->blending_mode_id;
        fgp->codec.h274.log2_scale_factor = fgc->log2_scale_factor;

        memcpy(&fgp->codec.h274.component_model_present, &fgc->comp_model_present_flag,
               sizeof(fgp->codec.h274.component_model_present));
        memcpy(&fgp->codec.h274.num_intensity_intervals, &fgc->num_intensity_intervals,
               sizeof(fgp->codec.h274.num_intensity_intervals));
        memcpy(&fgp->codec.h274.num_model_values, &fgc->num_model_values,
               sizeof(fgp->codec.h274.num_model_values));
        memcpy(&fgp->codec.h274.intensity_interval_lower_bound, &fgc->intensity_interval_lower_bound,
               sizeof(fgp->codec.h274.intensity_interval_lower_bound));
        memcpy(&fgp->codec.h274.intensity_interval_upper_bound, &fgc->intensity_interval_upper_bound,
               sizeof(fgp->codec.h274.intensity_interval_upper_bound));
        memcpy(&fgp->codec.h274.comp_model_value, &fgc->comp_model_value,
               sizeof(fgp->codec.h274.comp_model_value));

        fgc->present = fgc->persistence_flag;
    }

    if (sei->dynamic_hdr_plus.info) {
        AVBufferRef *info_ref = av_buffer_ref(sei->dynamic_hdr_plus.info);
        if (!info_ref)
            return AVERROR(ENOMEM);

        if (!av_frame_new_side_data_from_buf(out, AV_FRAME_DATA_DYNAMIC_HDR_PLUS, info_ref)) {
            av_buffer_unref(&info_ref);
            return AVERROR(ENOMEM);
        }
    }

    if (sei->dynamic_hdr_vivid.info) {
        AVBufferRef *info_ref = av_buffer_ref(sei->dynamic_hdr_vivid.info);
        if (!info_ref)
            return AVERROR(ENOMEM);

        if (!av_frame_new_side_data_from_buf(out, AV_FRAME_DATA_DYNAMIC_HDR_VIVID, info_ref)) {
            av_buffer_unref(&info_ref);
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}
