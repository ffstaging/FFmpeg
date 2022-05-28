/*
 * SSA/ASS common functions
 * Copyright (c) 2010  Aurelien Jacobs <aurel@gnuage.org>
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

#ifndef AVUTIL_ASS_INTERNAL_H
#define AVUTIL_ASS_INTERNAL_H

#include "subfmt.h"
#include "libavutil/bprint.h"

#define ASS_DEFAULT_PLAYRESX 384
#define ASS_DEFAULT_PLAYRESY 288

/**
 * @name Default values for ASS style
 * @{
 */
#define ASS_DEFAULT_FONT        "Arial"
#define ASS_DEFAULT_FONT_SIZE   16
#define ASS_DEFAULT_COLOR       0xffffff
#define ASS_DEFAULT_BACK_COLOR  0
#define ASS_DEFAULT_BOLD        0
#define ASS_DEFAULT_ITALIC      0
#define ASS_DEFAULT_UNDERLINE   0
#define ASS_DEFAULT_ALIGNMENT   2
#define ASS_DEFAULT_BORDERSTYLE 1
/** @} */

/**
 * Generate a suitable AVCodecContext.subtitle_header for SUBTITLE_ASS.
 * Can specify all fields explicitly
 *
 * @param play_res_x subtitle frame width
 * @param play_res_y subtitle frame height
 * @param font name of the default font face to use
 * @param font_size default font size to use
 * @param primary_color default text color to use (ABGR)
 * @param secondary_color default secondary text color to use (ABGR)
 * @param outline_color default outline color to use (ABGR)
 * @param back_color default background color to use (ABGR)
 * @param bold 1 for bold text, 0 for normal text
 * @param italic 1 for italic text, 0 for normal text
 * @param underline 1 for underline text, 0 for normal text
 * @param border_style 1 for outline, 3 for opaque box
 * @param alignment position of the text (left, center, top...), defined after
 *                  the layout of the numpad (1-3 sub, 4-6 mid, 7-9 top)
 * @param print_av_version include library version in header
 * @return a string containing the subtitle header that needs
 *         to be released via av_free()
 */
char* avpriv_ass_get_subtitle_header_full(int play_res_x, int play_res_y,
                                  const char *font, int font_size,
                                  int primary_color, int secondary_color,
                                  int outline_color, int back_color,
                                  int bold, int italic, int underline,
                                  int border_style, int alignment,
                                  int print_av_version);

/**
 * Generate a suitable AVCodecContext.subtitle_header for SUBTITLE_ASS.
 *
 * @param font name of the default font face to use
 * @param font_size default font size to use
 * @param color default text color to use (ABGR)
 * @param back_color default background color to use (ABGR)
 * @param bold 1 for bold text, 0 for normal text
 * @param italic 1 for italic text, 0 for normal text
 * @param underline 1 for underline text, 0 for normal text
 * @param border_style 1 for outline, 3 for opaque box
 * @param alignment position of the text (left, center, top...), defined after
 *                  the layout of the numpad (1-3 sub, 4-6 mid, 7-9 top)
 * @param print_av_version include library version in header
 * @return a string containing the subtitle header that needs
 *         to be released via av_free()
 */
char* avpriv_ass_get_subtitle_header(const char *font, int font_size,
                                int color, int back_color,
                                int bold, int italic, int underline,
                                int border_style, int alignment,
                                int print_av_version);

/**
 * Generate a suitable AVCodecContext.subtitle_header for SUBTITLE_ASS
 * with default style.
 *
 * @param print_av_version include library version in header
 * @return a string containing the subtitle header that needs
 *         to be released via av_free()
 */
char* avpriv_ass_get_subtitle_header_default(int print_av_version);

/**
 * Craft an ASS dialog string.
 */
char *avpriv_ass_get_dialog(int readorder, int layer, const char *style,
                        const char *speaker, const char *text);

/**
 * Craft an ASS dialog string.
 */
char *avpriv_ass_get_dialog_ex(int readorder, int layer, const char *style,
                        const char *speaker, int margin_l, int margin_r,
                        int margin_v, const char *text);

/**
 * Escape a text subtitle using ASS syntax into an AVBPrint buffer.
 * Newline characters will be escaped to \N.
 *
 * @param buf pointer to an initialized AVBPrint buffer
 * @param p source text
 * @param size size of the source text
 * @param linebreaks additional newline chars, which will be escaped to \N
 * @param keep_ass_markup braces and backslash will not be escaped if set
 */
void avpriv_ass_bprint_text_event(AVBPrint *buf, const char *p, int size,
                             const char *linebreaks, int keep_ass_markup);

#endif /* AVUTIL_ASS_INTERNAL_H */
