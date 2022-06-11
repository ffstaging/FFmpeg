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
 * null subtitle filter
 */

#include "avfilter.h"
#include "internal.h"
#include "libavutil/internal.h"

static const AVFilterPad avfilter_sf_snull_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_SUBTITLE,
    },
};

static const AVFilterPad avfilter_sf_snull_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_SUBTITLE,
    },
};

const AVFilter ff_sf_snull = {
    .name          = "snull",
    .description   = NULL_IF_CONFIG_SMALL("Pass the source unchanged to the output."),
    .flags         = AVFILTER_FLAG_METADATA_ONLY,
    FILTER_INPUTS(avfilter_sf_snull_inputs),
    FILTER_OUTPUTS(avfilter_sf_snull_outputs),
};
