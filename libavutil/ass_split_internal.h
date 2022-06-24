/*
 * SSA/ASS spliting functions
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

#ifndef AVUTIL_ASS_SPLIT_INTERNAL_H
#define AVUTIL_ASS_SPLIT_INTERNAL_H

#include "bprint.h"

enum ASSSplitComponents
{
    ASS_SPLIT_ANY = 0,
    ASS_SPLIT_TEXT           = (1 << 0),
    ASS_SPLIT_TEXT2          = (1 << 1), // Same semantics as ASS_SPLIT_TEXT. To work around help output default display.
    ASS_SPLIT_COLOR          = (1 << 2),
    ASS_SPLIT_ALPHA          = (1 << 3),
    ASS_SPLIT_FONT_NAME      = (1 << 4),
    ASS_SPLIT_FONT_SIZE      = (1 << 5),
    ASS_SPLIT_FONT_SCALE     = (1 << 6),
    ASS_SPLIT_FONT_SPACING   = (1 << 7),
    ASS_SPLIT_FONT_CHARSET   = (1 << 8),
    ASS_SPLIT_FONT_BOLD      = (1 << 9),
    ASS_SPLIT_FONT_ITALIC    = (1 << 10),
    ASS_SPLIT_FONT_UNDERLINE = (1 << 11),
    ASS_SPLIT_FONT_STRIKEOUT = (1 << 12),
    ASS_SPLIT_TEXT_BORDER    = (1 << 13),
    ASS_SPLIT_TEXT_SHADOW    = (1 << 14),
    ASS_SPLIT_TEXT_ROTATE    = (1 << 15),
    ASS_SPLIT_TEXT_BLUR      = (1 << 16),
    ASS_SPLIT_TEXT_WRAP      = (1 << 17),
    ASS_SPLIT_TEXT_ALIGNMENT = (1 << 18),
    ASS_SPLIT_CANCELLING     = (1 << 19),
    ASS_SPLIT_MOVE           = (1 << 20),
    ASS_SPLIT_POS            = (1 << 21),
    ASS_SPLIT_ORIGIN         = (1 << 22),
    ASS_SPLIT_DRAW           = (1 << 23),
    ASS_SPLIT_ANIMATE        = (1 << 24),
    ASS_SPLIT_FADE           = (1 << 25),
    ASS_SPLIT_CLIP           = (1 << 26),
    ASS_SPLIT_UNKNOWN        = (1 << 27),

    ASS_SPLIT_BASIC =  ASS_SPLIT_TEXT2 | ASS_SPLIT_COLOR | ASS_SPLIT_ALPHA | ASS_SPLIT_FONT_NAME | ASS_SPLIT_FONT_SIZE | ASS_SPLIT_FONT_SCALE | ASS_SPLIT_FONT_SPACING | ASS_SPLIT_FONT_CHARSET | ASS_SPLIT_FONT_BOLD | ASS_SPLIT_FONT_ITALIC | ASS_SPLIT_FONT_UNDERLINE | ASS_SPLIT_FONT_STRIKEOUT | ASS_SPLIT_TEXT_BORDER | ASS_SPLIT_TEXT_SHADOW | ASS_SPLIT_TEXT_WRAP | ASS_SPLIT_TEXT_ALIGNMENT | ASS_SPLIT_POS | ASS_SPLIT_CANCELLING,
    ASS_SPLIT_ALL_KNOWN =  ASS_SPLIT_TEXT2 | ASS_SPLIT_COLOR | ASS_SPLIT_ALPHA | ASS_SPLIT_FONT_NAME | ASS_SPLIT_FONT_SIZE | ASS_SPLIT_FONT_SCALE | ASS_SPLIT_FONT_SPACING | ASS_SPLIT_FONT_CHARSET | ASS_SPLIT_FONT_BOLD | ASS_SPLIT_FONT_ITALIC | ASS_SPLIT_FONT_UNDERLINE | ASS_SPLIT_FONT_STRIKEOUT | ASS_SPLIT_TEXT_BORDER | ASS_SPLIT_TEXT_SHADOW | ASS_SPLIT_TEXT_ROTATE | ASS_SPLIT_TEXT_BLUR | ASS_SPLIT_TEXT_WRAP | ASS_SPLIT_TEXT_ALIGNMENT | ASS_SPLIT_CANCELLING | ASS_SPLIT_POS | ASS_SPLIT_MOVE | ASS_SPLIT_ORIGIN | ASS_SPLIT_DRAW | ASS_SPLIT_ANIMATE | ASS_SPLIT_FADE | ASS_SPLIT_CLIP,
};

    /**
     * fields extracted from the [Script Info] section
     */
    typedef struct {
      char *script_type; /**< SSA script format version (eg. v4.00) */
  char *collisions;  /**< how subtitles are moved to prevent collisions */
  int play_res_x;    /**< video width that ASS coords are referring to */
  int play_res_y;    /**< video height that ASS coords are referring to */
  float timer;       /**< time multiplier to apply to SSA clock (in %) */
} ASSScriptInfo;

/**
 * fields extracted from the [V4(+) Styles] section
 */
typedef struct {
  char *name;        /**< name of the tyle (case sensitive) */
  char *font_name;   /**< font face (case sensitive) */
  int font_size;     /**< font height */
  int primary_color; /**< color that a subtitle will normally appear in */
  int secondary_color;
  int outline_color; /**< color for outline in ASS, called tertiary in SSA */
  int back_color;    /**< color of the subtitle outline or shadow */
  int bold;          /**< whether text is bold (1) or not (0) */
  int italic;        /**< whether text is italic (1) or not (0) */
  int underline;     /**< whether text is underlined (1) or not (0) */
  int strikeout;
  float scalex;
  float scaley;
  float spacing;
  float angle;
  int border_style;
  float outline;
  float shadow;
  int alignment; /**< position of the text (left, center, top...),
                      defined after the layout of the numpad
                      (1-3 sub, 4-6 mid, 7-9 top) */
  int margin_l;
  int margin_r;
  int margin_v;
  int alpha_level;
  int encoding;
} ASSStyle;

/**
 * fields extracted from the [Events] section
 */
typedef struct {
  int readorder;
  int layer;   /**< higher numbered layers are drawn over lower numbered */
  int start;   /**< start time of the dialog in centiseconds */
  int end;     /**< end time of the dialog in centiseconds */
  char *style; /**< name of the ASSStyle to use with this dialog */
  char *name;
  int margin_l;
  int margin_r;
  int margin_v;
  char *effect;
  char *text; /**< actual text which will be displayed as a subtitle,
                   can include style override control codes (see
                   avpriv_ass_split_override_codes()) */
} ASSDialog;

/**
 * structure containing the whole split ASS data
 */
typedef struct {
  ASSScriptInfo script_info; /**< general information about the SSA script*/
  ASSStyle *styles;          /**< array of split out styles */
  int styles_count;          /**< number of ASSStyle in the styles array */
  ASSDialog *dialogs;        /**< array of split out dialogs */
  int dialogs_count;         /**< number of ASSDialog in the dialogs array*/
} ASS;

/**
 * This struct can be casted to ASS to access to the split data.
 */
typedef struct ASSSplitContext ASSSplitContext;

/**
 * Split a full ASS file or a ASS header from a string buffer and store
 * the split structure in a newly allocated context.
 *
 * @param buf String containing the ASS formatted data.
 * @return Newly allocated struct containing split data.
 */
ASSSplitContext *avpriv_ass_split(const char *buf);

/**
 * Free a dialogue obtained from avpriv_ass_split_dialog().
 */
void avpriv_ass_free_dialog(ASSDialog **dialogp);

/**
 * Split one ASS Dialogue line from a string buffer.
 *
 * @param ctx Context previously initialized by ff_ass_split().
 * @param buf String containing the ASS "Dialogue" line.
 * @return Pointer to the split ASSDialog. Must be freed with
 * ff_ass_free_dialog()
 */
ASSDialog *avpriv_ass_split_dialog(ASSSplitContext *ctx, const char *buf);

/**
 * Free all the memory allocated for an ASSSplitContext.
 *
 * @param ctx Context previously initialized by ff_ass_split().
 */
void avpriv_ass_split_free(ASSSplitContext *ctx);

/**
 * Set of callback functions corresponding to each override codes that can
 * be encountered in a "Dialogue" Text field.
 */
typedef struct {
  /**
   * @defgroup ass_styles    ASS styles
   * @{
   */
  void (*text)(void *priv, const char *text, int len);
  void (*hard_space)(void *priv);
  void (*new_line)(void *priv, int forced);
  void (*style)(void *priv, char style, int close);
  void (*color)(void *priv, unsigned int /* color */, unsigned int color_id);
  void (*alpha)(void *priv, int alpha, int alpha_id);
  void (*font_name)(void *priv, const char *name);
  void (*font_size)(void *priv, int size);
  void (*alignment)(void *priv, int alignment);
  void (*cancel_overrides)(void *priv, const char *style);
  /** @} */

  /**
   * @defgroup ass_functions    ASS functions
   * @{
   */
  void (*move)(void *priv, int x1, int y1, int x2, int y2, int t1, int t2);
  void (*animate)(void *priv, int t1, int t2, int accel, char *style);
  void (*origin)(void *priv, int x, int y);
  void (*drawing_mode)(void *priv, int scale);
  /** @} */

  /**
   * @defgroup ass_ext    ASS extensible parsing callback
   * @{
   */
  void (*ext)(void *priv, int ext_id, const char *text, int p1, int p2);
  /** @} */

  /**
   * @defgroup ass_end    end of Dialogue Event
   * @{
   */
  void (*end)(void *priv);
  /** @} */
} ASSCodesCallbacks;

/**
 * Split override codes out of a ASS "Dialogue" Text field.
 *
 * @param callbacks Set of callback functions called for each override code
 *                  encountered.
 * @param priv Opaque pointer passed to the callback functions.
 * @param buf The ASS "Dialogue" Text field to split.
 * @param outbuffer The output buffer.
 * @param keep_flags Flags for filtering ass codes.
 * @return >= 0 on success otherwise an error code <0
 */
int avpriv_ass_filter_override_codes(const ASSCodesCallbacks *callbacks,
                                     void *priv, const char *buf,
                                     AVBPrint *outbuffer, enum ASSSplitComponents keep_flags);

/**
 * Split override codes out of a ASS "Dialogue" Text field.
 *
 * @param callbacks Set of callback functions called for each override code
 *                  encountered.
 * @param priv Opaque pointer passed to the callback functions.
 * @param buf The ASS "Dialogue" Text field to split.
 * @return >= 0 on success otherwise an error code <0
 */
int avpriv_ass_split_override_codes(const ASSCodesCallbacks *callbacks,
                                    void *priv, const char *buf);

/**
 * Find an ASSStyle structure by its name.
 *
 * @param ctx Context previously initialized by ff_ass_split().
 * @param style name of the style to search for.
 * @return the ASSStyle corresponding to style, or NULL if style can't be found
 */
ASSStyle *avpriv_ass_style_get(ASSSplitContext *ctx, const char *style);

#endif /* AVUTIL_ASS_SPLIT_INTERNAL_H */
