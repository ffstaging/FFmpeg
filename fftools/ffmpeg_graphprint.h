/*
 * Copyright (c) 2018 - softworkz
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

#ifndef FFTOOLS_FFMPEG_GRAPHPRINT_H
#define FFTOOLS_FFMPEG_GRAPHPRINT_H

#include <stdint.h>

#include "config.h"
#include "ffmpeg.h"
#include "libavutil/avutil.h"
#include "libavutil/bprint.h"
#include "textformat/avtextformat.h"

int print_filtergraphs(FilterGraph **graphs, int nb_graphs, OutputFile **output_files, int nb_output_files);
int print_filtergraph(FilterGraph *fg, AVFilterGraph *graph);

#endif /* FFTOOLS_FFMPEG_GRAPHPRINT_H */
