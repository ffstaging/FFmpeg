/*
 * Copyright (c) 2007 Bobby Bingham
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

#include "libavutil/channel_layout.h"
#include "libavfilter/formats.c"

#undef printf

const int64_t avfilter_all_channel_layouts[] = {
    AV_CH_FRONT_CENTER,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY,
    AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_CENTER,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER,
    AV_CH_FRONT_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_CENTER|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_LOW_FREQUENCY|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT|AV_CH_FRONT_CENTER|AV_CH_BACK_CENTER|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT|AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT,
    -1
};

int main(void)
{
    AVChannelLayout layout = { 0 };
    const int64_t *cl;
    char buf[512];
    int i;
    const char *teststrings[] ={
        "blah",
        "1",
        "2",
        "-1",
        "60",
        "65",
        "1c",
        "2c",
        "-1c",
        "60c",
        "65c",
        "2C",
        "60C",
        "65C",
        "5.1",
        "stereo",
        "1+1+1+1",
        "1c+1c+1c+1c",
        "2c+1c",
        "0x3",
    };

    for (cl = avfilter_all_channel_layouts; *cl != -1; cl++) {
        av_channel_layout_from_mask(&layout, *cl);
        av_channel_layout_describe(&layout, buf, sizeof(buf));
        printf("%s\n", buf);
        av_channel_layout_uninit(&layout);
    }

    for ( i = 0; i<FF_ARRAY_ELEMS(teststrings); i++) {
        int count = -1;
        int ret;
        av_channel_layout_uninit(&layout);
        ret = ff_parse_channel_layout(&layout, &count, teststrings[i], NULL);

        printf ("%d = ff_parse_channel_layout(%016"PRIX64", %2d, %s);\n", ret ? -1 : 0, layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0, count, teststrings[i]);
    }

    return 0;
}
