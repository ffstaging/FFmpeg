/*
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

#ifndef AVCODEC_DEFS_H
#define AVCODEC_DEFS_H

/**
 * @file
 * @ingroup libavc
 * Misc types and constants that do not belong anywhere else.
 */

#include <stdint.h>
#include <stdlib.h>

/**
 * @ingroup lavc_decoding
 * Required number of additionally allocated bytes at the end of the input bitstream for decoding.
 * This is mainly needed because some optimized bitstream readers read
 * 32 or 64 bit at once and could read over the end.<br>
 * Note: If the first 23 bits of the additional bytes are not 0, then damaged
 * MPEG bitstreams could cause overread and segfault.
 */
#define AV_INPUT_BUFFER_PADDING_SIZE 64

/**
 * Verify checksums embedded in the bitstream (could be of either encoded or
 * decoded data, depending on the format) and print an error message on mismatch.
 * If AV_EF_EXPLODE is also set, a mismatching checksum will result in the
 * decoder/demuxer returning an error.
 */
#define AV_EF_CRCCHECK       (1<<0)
#define AV_EF_BITSTREAM      (1<<1)   ///< detect bitstream specification deviations
#define AV_EF_BUFFER         (1<<2)   ///< detect improper bitstream length
#define AV_EF_EXPLODE        (1<<3)   ///< abort decoding on minor error detection

#define AV_EF_IGNORE_ERR     (1<<15)  ///< ignore errors and continue
#define AV_EF_CAREFUL        (1<<16)  ///< consider things that violate the spec, are fast to calculate and have not been seen in the wild as errors
#define AV_EF_COMPLIANT      (1<<17)  ///< consider all spec non compliances as errors
#define AV_EF_AGGRESSIVE     (1<<18)  ///< consider things that a sane encoder/muxer should not do as an error

#define FF_COMPLIANCE_VERY_STRICT   2 ///< Strictly conform to an older more strict version of the spec or reference software.
#define FF_COMPLIANCE_STRICT        1 ///< Strictly conform to all the things in the spec no matter what consequences.
#define FF_COMPLIANCE_NORMAL        0
#define FF_COMPLIANCE_UNOFFICIAL   -1 ///< Allow unofficial extensions
#define FF_COMPLIANCE_EXPERIMENTAL -2 ///< Allow nonstandardized experimental things.

/**
 * @ingroup lavc_decoding
 */
enum AVDiscard{
    /* We leave some space between them for extensions (drop some
     * keyframes for intra-only or drop just some bidir frames). */
    AVDISCARD_NONE    =-16, ///< discard nothing
    AVDISCARD_DEFAULT =  0, ///< discard useless packets like 0 size packets in avi
    AVDISCARD_NONREF  =  8, ///< discard all non reference
    AVDISCARD_BIDIR   = 16, ///< discard all bidirectional frames
    AVDISCARD_NONINTRA= 24, ///< discard all non intra frames
    AVDISCARD_NONKEY  = 32, ///< discard all frames except keyframes
    AVDISCARD_ALL     = 48, ///< discard all
};

enum AVAudioServiceType {
    AV_AUDIO_SERVICE_TYPE_MAIN              = 0,
    AV_AUDIO_SERVICE_TYPE_EFFECTS           = 1,
    AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED = 2,
    AV_AUDIO_SERVICE_TYPE_HEARING_IMPAIRED  = 3,
    AV_AUDIO_SERVICE_TYPE_DIALOGUE          = 4,
    AV_AUDIO_SERVICE_TYPE_COMMENTARY        = 5,
    AV_AUDIO_SERVICE_TYPE_EMERGENCY         = 6,
    AV_AUDIO_SERVICE_TYPE_VOICE_OVER        = 7,
    AV_AUDIO_SERVICE_TYPE_KARAOKE           = 8,
    AV_AUDIO_SERVICE_TYPE_NB                   , ///< Not part of ABI
};

/**
 * Pan Scan area.
 * This specifies the area which should be displayed.
 * Note there may be multiple such areas for one frame.
 */
typedef struct AVPanScan {
    /**
     * id
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     */
    int id;

    /**
     * width and height in 1/16 pel
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     */
    int width;
    int height;

    /**
     * position of the top left corner in 1/16 pel for up to 3 fields/frames
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     */
    int16_t position[3][2];
} AVPanScan;

/**
 * This structure describes the bitrate properties of an encoded bitstream. It
 * roughly corresponds to a subset the VBV parameters for MPEG-2 or HRD
 * parameters for H.264/HEVC.
 */
typedef struct AVCPBProperties {
    /**
     * Maximum bitrate of the stream, in bits per second.
     * Zero if unknown or unspecified.
     */
    int64_t max_bitrate;
    /**
     * Minimum bitrate of the stream, in bits per second.
     * Zero if unknown or unspecified.
     */
    int64_t min_bitrate;
    /**
     * Average bitrate of the stream, in bits per second.
     * Zero if unknown or unspecified.
     */
    int64_t avg_bitrate;

    /**
     * The size of the buffer to which the ratecontrol is applied, in bits.
     * Zero if unknown or unspecified.
     */
    int64_t buffer_size;

    /**
     * The delay between the time the packet this structure is associated with
     * is received and the time when it should be decoded, in periods of a 27MHz
     * clock.
     *
     * UINT64_MAX when unknown or unspecified.
     */
    uint64_t vbv_delay;
} AVCPBProperties;

/**
 * Allocate a CPB properties structure and initialize its fields to default
 * values.
 *
 * @param size if non-NULL, the size of the allocated struct will be written
 *             here. This is useful for embedding it in side data.
 *
 * @return the newly allocated struct or NULL on failure
 */
AVCPBProperties *av_cpb_properties_alloc(size_t *size);

/**
 * This structure supplies correlation between a packet timestamp and a wall clock
 * production time. The definition follows the Producer Reference Time ('prft')
 * as defined in ISO/IEC 14496-12
 */
typedef struct AVProducerReferenceTime {
    /**
     * A UTC timestamp, in microseconds, since Unix epoch (e.g, av_gettime()).
     */
    int64_t wallclock;
    int flags;
} AVProducerReferenceTime;

/**
 * Encode extradata length to a buffer. Used by xiph codecs.
 *
 * @param s buffer to write to; must be at least (v/255+1) bytes long
 * @param v size of extradata in bytes
 * @return number of bytes written to the buffer.
 */
unsigned int av_xiphlacing(unsigned char *s, unsigned int v);

#endif // AVCODEC_DEFS_H
