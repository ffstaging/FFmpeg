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
 * subtitle filter to convert graphical subs to text subs via OCR
 */

#include <tesseract/capi.h>
#include <libavutil/ass_internal.h>

#include "drawutils.h"
#include "libavutil/opt.h"
#include "subtitles.h"

#include "libavcodec/elbg.h"

enum {
    RFLAGS_NONE         = 0,
    RFLAGS_HALIGN       = 1 << 0,
    RFLAGS_VALIGN       = 1 << 1,
    RFLAGS_FBOLD        = 1 << 2,
    RFLAGS_FITALIC      = 1 << 3,
    RFLAGS_FUNDERLINE   = 1 << 4,
    RFLAGS_FONT         = 1 << 5,
    RFLAGS_FONTSIZE     = 1 << 6,
    RFLAGS_COLOR        = 1 << 7,
    RFLAGS_OUTLINECOLOR = 1 << 8,
    RFLAGS_ALL = RFLAGS_HALIGN | RFLAGS_VALIGN | RFLAGS_FBOLD | RFLAGS_FITALIC | RFLAGS_FUNDERLINE |
                RFLAGS_FONT | RFLAGS_FONTSIZE | RFLAGS_COLOR | RFLAGS_OUTLINECOLOR,
};

typedef struct SubOcrContext {
    const AVClass *class;
    int w, h;

    TessBaseAPI *tapi;
    TessOcrEngineMode ocr_mode;
    char *tessdata_path;
    char *language;
    int preprocess_images;
    int dump_bitmaps;
    int delay_when_no_duration;
    int recognize;
    double font_size_factor;

    int readorder_counter;

    AVFrame *pending_frame;
    AVBufferRef *subtitle_header;
    AVBPrint buffer;

    // Color Quantization Fields
    struct ELBGContext *ctx;
    AVLFG lfg;
    int *codeword;
    int *codeword_closest_codebook_idxs;
    int *codebook;
    int r_idx, g_idx, b_idx, a_idx;
    int64_t last_subtitle_pts;
} SubOcrContext;

typedef struct OcrImageProps {
    int background_color_index;
    int fill_color_index;

} OcrImageProps;

static int64_t ms_to_avtb(int64_t ms)
{
    return av_rescale_q(ms, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
}

static int create_ass_header(AVFilterContext* ctx)
{
    SubOcrContext* s = ctx->priv;

    if (!(s->w && s->h)) {
        av_log(ctx, AV_LOG_WARNING, "create_ass_header: no width and height specified!\n");
        s->w = ASS_DEFAULT_PLAYRESX;
        s->h = ASS_DEFAULT_PLAYRESY;
    }

    char* subtitle_header_text = avpriv_ass_get_subtitle_header_full(s->w, s->h, ASS_DEFAULT_FONT, ASS_DEFAULT_FONT_SIZE,
        ASS_DEFAULT_COLOR, ASS_DEFAULT_COLOR, ASS_DEFAULT_BACK_COLOR, ASS_DEFAULT_BACK_COLOR, ASS_DEFAULT_BOLD,
        ASS_DEFAULT_ITALIC, ASS_DEFAULT_UNDERLINE, ASS_DEFAULT_BORDERSTYLE, ASS_DEFAULT_ALIGNMENT, 0);

    if (!subtitle_header_text)
        return AVERROR(ENOMEM);

    s->subtitle_header = av_buffer_create((uint8_t*)subtitle_header_text, strlen(subtitle_header_text) + 1, NULL, NULL, 0);

    if (!s->subtitle_header)
        return AVERROR(ENOMEM);

    return 0;
}

static int init(AVFilterContext *ctx)
{
    SubOcrContext *s = ctx->priv;
    const char* tver = TessVersion();
    uint8_t rgba_map[4];
    int ret;

    s->tapi = TessBaseAPICreate();

    if (!s->tapi || !tver || !strlen(tver)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to access libtesseract\n");
        return AVERROR(ENOSYS);
    }

    av_log(ctx, AV_LOG_VERBOSE, "Initializing libtesseract, version: %s\n", tver);

    ret = TessBaseAPIInit4(s->tapi, s->tessdata_path, s->language, s->ocr_mode, NULL, 0, NULL, NULL, 0, 1);
    if (ret < 0 ) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialize libtesseract. Error: %d\n", ret);
        return AVERROR(ENOSYS);
    }

    ret = TessBaseAPISetVariable(s->tapi, "tessedit_char_blacklist", "|");
    if (ret < 0 ) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set 'tessedit_char_blacklist'. Error: %d\n", ret);
        return AVERROR(EINVAL);
    }

    av_bprint_init(&s->buffer, 0, AV_BPRINT_SIZE_UNLIMITED);

    ff_fill_rgba_map(&rgba_map[0], AV_PIX_FMT_RGB32);

    s->r_idx = rgba_map[0]; // R
    s->g_idx = rgba_map[1]; // G
    s->b_idx = rgba_map[2]; // B
    s->a_idx = rgba_map[3]; // A

    av_lfg_init(&s->lfg, 123456789);

    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    SubOcrContext *s = ctx->priv;

    av_buffer_unref(&s->subtitle_header);
    av_bprint_finalize(&s->buffer, NULL);

    if (s->tapi) {
        TessBaseAPIEnd(s->tapi);
        TessBaseAPIDelete(s->tapi);
    }

    avpriv_elbg_free(&s->ctx);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats, *formats2;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSubtitleType in_fmts[] = { AV_SUBTITLE_FMT_BITMAP, AV_SUBTITLE_FMT_NONE };
    static const enum AVSubtitleType out_fmts[] = { AV_SUBTITLE_FMT_ASS, AV_SUBTITLE_FMT_NONE };
    int ret;

    /* set input format */
    formats = ff_make_format_list(in_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->outcfg.formats)) < 0)
        return ret;

    /* set output format */
    formats2 = ff_make_format_list(out_fmts);
    if ((ret = ff_formats_ref(formats2, &outlink->incfg.formats)) < 0)
        return ret;

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    SubOcrContext *s = ctx->priv;

    if (s->w <= 0 || s->h <= 0) {
        s->w = inlink->w;
        s->h = inlink->h;
    }

    return create_ass_header(ctx);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    const AVFilterContext *ctx  = outlink->src;
    SubOcrContext *s = ctx->priv;

    outlink->format = AV_SUBTITLE_FMT_ASS;
    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = inlink->time_base;
    outlink->frame_rate = inlink->frame_rate;

    return 0;
}

static void free_subtitle_area(AVSubtitleArea *area)
{
    for (unsigned n = 0; n < FF_ARRAY_ELEMS(area->buf); n++)
        av_buffer_unref(&area->buf[n]);

    av_freep(&area->text);
    av_freep(&area->ass);
    av_free(area);

}

static AVSubtitleArea *copy_subtitle_area(const AVSubtitleArea *src)
{
    AVSubtitleArea *dst = av_mallocz(sizeof(AVSubtitleArea));

    if (!dst)
        return NULL;

    dst->x         =  src->x;
    dst->y         =  src->y;
    dst->w         =  src->w;
    dst->h         =  src->h;
    dst->nb_colors =  src->nb_colors;
    dst->type      =  src->type;
    dst->flags     =  src->flags;

    for (unsigned i = 0; i < AV_NUM_BUFFER_POINTERS; i++) {
        if (src->h > 0 && src->w > 0 && src->buf[i]) {
            dst->buf[0] = av_buffer_ref(src->buf[i]);
            if (!dst->buf[i])
                return NULL;

            const int ret = av_buffer_make_writable(&dst->buf[i]);
            if (ret < 0)
                return NULL;

            dst->linesize[i] = src->linesize[i];
        }
    }

    memcpy(&dst->pal[0], &src->pal[0], sizeof(src->pal[0]) * 256);


    return dst;
}

static int quantize_image_colors(SubOcrContext *const s, AVSubtitleArea *subtitle_area)
{
    const int num_quantized_colors = 3;
    int k, ret;
    const int codeword_length = subtitle_area->w * subtitle_area->h;
    uint8_t *src_data = subtitle_area->buf[0]->data;

    if (subtitle_area->nb_colors <= num_quantized_colors) {
        av_log(s, AV_LOG_DEBUG, "No need to quantize colors. Color count: %d\n", subtitle_area->nb_colors);
        return 0;
    }

    // Convert palette to grayscale
    for (int i = 0; i < subtitle_area->nb_colors; i++) {
        uint8_t *color        = (uint8_t *)&subtitle_area->pal[i];
        const uint8_t average = (uint8_t)(((int)color[s->r_idx] + color[s->g_idx] + color[s->b_idx]) / 3);
        color[s->b_idx]       = average;
        color[s->g_idx]       = average;
        color[s->r_idx]       = average;
    }

    /* Re-Initialize */
    s->codeword = av_realloc_f(s->codeword, codeword_length, 4 * sizeof(*s->codeword));
    if (!s->codeword)
        return AVERROR(ENOMEM);

    s->codeword_closest_codebook_idxs = av_realloc_f(s->codeword_closest_codebook_idxs,
        codeword_length, sizeof(*s->codeword_closest_codebook_idxs));
    if (!s->codeword_closest_codebook_idxs)
        return AVERROR(ENOMEM);

    s->codebook = av_realloc_f(s->codebook, num_quantized_colors, 4 * sizeof(*s->codebook));
    if (!s->codebook)
        return AVERROR(ENOMEM);

    /* build the codeword */
    k = 0;
    for (int i = 0; i < subtitle_area->h; i++) {
        uint8_t *p = src_data;
        for (int j = 0; j < subtitle_area->w; j++) {
            const uint8_t *color = (uint8_t *)&subtitle_area->pal[*p];
            s->codeword[k++] = color[s->b_idx];
            s->codeword[k++] = color[s->g_idx];
            s->codeword[k++] = color[s->r_idx];
            s->codeword[k++] = color[s->a_idx];
            p++;
        }
        src_data += subtitle_area->linesize[0];
    }

    /* compute the codebook */
    ret = avpriv_elbg_do(&s->ctx, s->codeword, 4, codeword_length, s->codebook,
        num_quantized_colors, 1, s->codeword_closest_codebook_idxs, &s->lfg, 0);
    if (ret < 0)
        return ret;

    /* Write Palette */
    for (int i = 0; i < num_quantized_colors; i++) {
        subtitle_area->pal[i] = s->codebook[i*4+3] << 24  |
                    (s->codebook[i*4+2] << 16) |
                    (s->codebook[i*4+1] <<  8) |
                    (s->codebook[i*4  ] <<  0);
    }


    av_log(s, AV_LOG_DEBUG, "Quantized colors from %d to %d\n", subtitle_area->nb_colors, num_quantized_colors);

    subtitle_area->nb_colors = num_quantized_colors;
    src_data = subtitle_area->buf[0]->data;

    /* Write Image */
    k = 0;
    for (int i = 0; i < subtitle_area->h; i++) {
        uint8_t *p = src_data;
        for (int j = 0; j < subtitle_area->w; j++, p++) {
            p[0] = (uint8_t)s->codeword_closest_codebook_idxs[k++];
        }

        src_data += subtitle_area->linesize[0];
    }

    return ret;
}

#define MEASURE_LINE_COUNT 6

static uint8_t get_background_color_index(SubOcrContext *const s, const AVSubtitleArea *subtitle_area)
{
    const int linesize = subtitle_area->linesize[0];
    int index_counts[256] = {0};
    const unsigned int line_offsets[MEASURE_LINE_COUNT] = {
        0,
        linesize,
        2 * linesize,
        (subtitle_area->h - 3) * linesize,
        (subtitle_area->h - 2) * linesize,
        (subtitle_area->h - 1) * linesize
    };

    const uint8_t *src_data = subtitle_area->buf[0]->data;
    const uint8_t tl = src_data[0];
    const uint8_t tr = src_data[subtitle_area->w - 1];
    const uint8_t bl = src_data[(subtitle_area->h - 1) * linesize + 0];
    const uint8_t br = src_data[(subtitle_area->h - 1) * linesize + subtitle_area->w - 1];
    uint8_t max_index = 0;
    int max_count;

    // When all corner pixels are equal, assume that as background color
    if (tl == tr == bl == br || subtitle_area->h < 6)
        return tl;

    for (unsigned int i = 0; i < MEASURE_LINE_COUNT; i++) {
        uint8_t *p = subtitle_area->buf[0]->data + line_offsets[i];
        for (int k = 0; k < subtitle_area->w; k++)
            index_counts[p[k]]++;
    }

    max_count = index_counts[0];

    for (uint8_t i = 1; i < subtitle_area->nb_colors; i++) {
        if (index_counts[i] > max_count) {
            max_count = index_counts[i];
            max_index = i;
        }
    }

    return max_index;
}

static uint8_t get_text_color_index(SubOcrContext *const s, const AVSubtitleArea *subtitle_area, const uint8_t bg_color_index, uint8_t *outline_color_index)
{
    const int linesize = subtitle_area->linesize[0];
    int index_counts[256] = {0};
    uint8_t last_index = bg_color_index;
    int max_count, min_req_count;
    uint8_t max_index = 0;

    for (int i = 3; i < subtitle_area->h - 3; i += 5) {
        const uint8_t *p = subtitle_area->buf[0]->data + ((ptrdiff_t)linesize * i);
        for (int k = 0; k < subtitle_area->w; k++) {
            const uint8_t cur_index = p[k];

            // When color hasn't changed, continue
            if (cur_index == last_index)
                continue;

            if (cur_index != bg_color_index)
                index_counts[cur_index]++;

            last_index = cur_index;
        }
    }

    max_count = index_counts[0];

    for (uint8_t i = 1; i < subtitle_area->nb_colors; i++) {
        if (index_counts[i] > max_count) {
            max_count = index_counts[i];
            max_index = i;
        }
    }

    min_req_count = max_count / 3;

    for (uint8_t i = 1; i < subtitle_area->nb_colors; i++) {
        if (index_counts[i] < min_req_count)
            index_counts[i] = 0;
    }

    *outline_color_index = max_index;

    index_counts[max_index] = 0;
    max_count = 0;

    for (uint8_t i = 0; i < subtitle_area->nb_colors; i++) {
        if (index_counts[i] > max_count) {
            max_count = index_counts[i];
            max_index = i;
        }
    }

    if (*outline_color_index == max_index)
        *outline_color_index = 255;

    return max_index;
}

static void make_image_binary(SubOcrContext *const s, AVSubtitleArea *subtitle_area, const uint8_t text_color_index)
{
    for (int i = 0; i < subtitle_area->nb_colors; i++) {

        if (i != text_color_index)
            subtitle_area->pal[i] = 0xffffffff;
        else
            subtitle_area->pal[i] = 0xff000000;
    }
}

static int get_crop_region(SubOcrContext *const s, const AVSubtitleArea *subtitle_area, uint8_t text_color_index, int *x, int *y, int *w, int *h)
{
    const int linesize = subtitle_area->linesize[0];
    int max_y = 0, max_x = 0;
    int min_y = subtitle_area->h - 1, min_x = subtitle_area->w - 1;

    for (int i = 0; i < subtitle_area->h; i += 3) {
        const uint8_t *p = subtitle_area->buf[0]->data + ((ptrdiff_t)linesize * i);
        for (int k = 0; k < subtitle_area->w; k += 2) {
            if (p[k] == text_color_index) {
                min_y = FFMIN(min_y, i);
                min_x = FFMIN(min_x, k);
                max_y = FFMAX(max_y, i);
                max_x = FFMAX(max_x, k);
            }
        }
    }

    if (max_y <= min_y || max_x <= min_x) {
        av_log(s, AV_LOG_WARNING, "Unable to detect crop region\n");
        *x = 0;
        *y = 0;
        *w = subtitle_area->w;
        *h = subtitle_area->h;
    }    else {
        *x = FFMAX(min_x - 10, 0);
        *y = FFMAX(min_y - 10, 0);
        *w = FFMIN(max_x + 10 - *x, (subtitle_area->w - *x));
        *h = FFMIN(max_y + 10 - *y, (subtitle_area->h - *y));
    }

    return 0;
}

static int crop_area_bitmap(SubOcrContext *const s, AVSubtitleArea *subtitle_area, int x, int y, int w, int h)
{
    const int linesize = subtitle_area->linesize[0];
    AVBufferRef *dst = av_buffer_allocz(h * w);
    uint8_t *d;

    if (!dst)
        return AVERROR(ENOMEM);

    d = dst->data;

    for (int i = y; i < y + h; i++) {
        const uint8_t *p = subtitle_area->buf[0]->data + ((ptrdiff_t)linesize * i);
        for (int k = x; k < x + w; k++) {
            *d = p[k];
            d++;
        }
    }

    subtitle_area->w = w;
    subtitle_area->h = h;
    subtitle_area->x += x;
    subtitle_area->y += y;
    subtitle_area->linesize[0] = w;
    av_buffer_replace(&subtitle_area->buf[0], dst);

    av_buffer_unref(&dst);
    return 0;
}

#define R 0
#define G 1
#define B 2
#define A 3

static int print_code(AVBPrint *buf, int in_code, const char *fmt, ...)
{
    va_list vl;

    if (!in_code)
        av_bprint_chars(buf, '{', 1);

    va_start(vl, fmt);
    av_vbprintf(buf, fmt, vl);
    va_end(vl);

    return 1;
}

static int end_code(AVBPrint *buf, int in_code)
{
    if (in_code)
        av_bprint_chars(buf, '}', 1);
    return 0;
}

static uint8_t* create_grayscale_image(AVFilterContext *ctx, AVSubtitleArea *area, int invert)
{
    uint8_t gray_pal[256];
    const size_t img_size = area->buf[0]->size;
    const uint8_t* img    = area->buf[0]->data;
    uint8_t* gs_img       = av_malloc(img_size);

    if (!gs_img)
        return NULL;

    for (unsigned i = 0; i < 256; i++) {
        const uint8_t *col = (uint8_t*)&area->pal[i];
        const int val      = (int)col[3] * FFMAX3(col[0], col[1], col[2]);
        gray_pal[i]        = (uint8_t)(val >> 8);
    }

    if (invert)
        for (unsigned i = 0; i < img_size; i++)
            gs_img[i]   = 255 - gray_pal[img[i]];
    else
        for (unsigned i = 0; i < img_size; i++)
            gs_img[i]   = gray_pal[img[i]];

    return gs_img;
}

static uint8_t* create_bitmap_image(AVFilterContext *ctx, AVSubtitleArea *area, const uint8_t text_color_index)
{
    const size_t img_size = area->buf[0]->size;
    const uint8_t* img    = area->buf[0]->data;
    uint8_t* gs_img       = av_malloc(img_size);

    if (!gs_img)
        return NULL;

    for (unsigned i = 0; i < img_size; i++) {
        if (img[i] == text_color_index)
            gs_img[i]   = 0;
        else
            gs_img[i]   = 255;
    }

    return gs_img;
}

static void png_save(AVFilterContext *ctx, const char *filename, AVSubtitleArea *area)
{
    int x, y;
    int v;
    FILE *f;
    char fname[40];
    const uint8_t *data = area->buf[0]->data;

    snprintf(fname, sizeof(fname), "%s.ppm", filename);

    f = fopen(fname, "wb");
    if (!f) {
        perror(fname);
        return;
    }
    fprintf(f, "P6\n"
            "%d %d\n"
            "%d\n",
            area->w, area->h, 255);
    for(y = 0; y < area->h; y++) {
        for(x = 0; x < area->w; x++) {
            const uint8_t index = data[y * area->linesize[0] + x];
            v = (int)area->pal[index];
            putc(v >> 16 & 0xff, f);
            putc(v >> 8 & 0xff, f);
            putc(v >> 0 & 0xff, f);
        }
    }

    fclose(f);
}

static int get_max_index(int score[256])
{
    int max_val = 0, max_index = 0;

    for (int i = 0; i < 256; i++) {
        if (score[i] > max_val) {
            max_val = score[i];
            max_index = i;
        }
    }

    return max_index;
}

static int get_word_colors(AVFilterContext *ctx, TessResultIterator* ri, const AVSubtitleArea* area, const AVSubtitleArea* original_area,
                           uint8_t bg_color_index, uint8_t text_color_index, uint8_t outline_color_index,
                           uint32_t* bg_color, uint32_t* text_color, uint32_t* outline_color)
{
    int left = 0, top = 0, right = 0, bottom = 0, ret;
    int bg_score[256] = {0}, text_score[256] = {0}, outline_score[256] = {0};
    int max_index;

    ret = TessPageIteratorBoundingBox((TessPageIterator*)ri, RIL_WORD, &left, &top, &right, &bottom);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "get_word_colors: IteratorBoundingBox failed: %d\n", ret);
        return  ret;
    }

    if (left >= area->w || right >= area->w || top >= area->h || bottom >= area->h) {
        av_log(ctx, AV_LOG_WARNING, "get_word_colors: word bounding box (l: %d, t: %d r: %d, b: %d) out of image bounds (%dx%d)\n", left,top, right, bottom, area->w, area->h);
        return  AVERROR(EINVAL);
    }

    for (int y = top; y < bottom; y += 3) {
        uint8_t *p = area->buf[0]->data + (y * area->linesize[0]) + left;
        uint8_t *porig = original_area->buf[0]->data + (y * original_area->linesize[0]) + left;
        uint8_t current_index = 255;

        for (int x = left; x < right; x++, p++, porig++) {

            if (*p == current_index) {
                if (*p == bg_color_index)
                    bg_score[*porig]++;
                if (*p == text_color_index)
                    text_score[*porig]++;
                if (*p == outline_color_index)
                    outline_score[*porig]++;
            }

            current_index = *p;
        }
    }

    max_index = get_max_index(bg_score);
    if (bg_score[max_index] > 0)
        *bg_color = original_area->pal[max_index];

    max_index = get_max_index(text_score);
    if (text_score[max_index] > 0)
        *text_color = original_area->pal[max_index];

    max_index = get_max_index(outline_score);
    if (outline_score[max_index] > 0)
        *outline_color = original_area->pal[max_index];

    return 0;
}

static int convert_area(AVFilterContext *ctx, AVSubtitleArea *area, const AVFrame *frame, const unsigned area_index, int *margin_v)
{
    SubOcrContext *s = ctx->priv;
    char *ocr_text = NULL;
    int ret = 0;
    uint8_t *gs_img;
    uint8_t bg_color_index;
    uint8_t text_color_index = 255;
    uint8_t outline_color_index = 255;
    char filename[32];
    AVSubtitleArea *original_area = copy_subtitle_area(area);

    if (!original_area)
        return AVERROR(ENOMEM);

    if (area->w < 6 || area->h < 6) {
        area->ass = NULL;
        goto exit;
    }

    if (s->dump_bitmaps) {
        snprintf(filename, sizeof(filename), "graphicsub2text_%"PRId64"_%d_original", frame->subtitle_timing.start_pts, area_index);
        png_save(ctx, filename, area);
    }

    if (s->preprocess_images) {
        ret = quantize_image_colors(s, area);
        if (ret < 0)
            goto exit;
        if (s->dump_bitmaps && original_area->nb_colors != area->nb_colors) {
            snprintf(filename, sizeof(filename), "graphicsub2text_%"PRId64"_%d_quantized", frame->subtitle_timing.start_pts, area_index);
            png_save(ctx, filename, area);
        }
    }

    bg_color_index = get_background_color_index(s, area);

    if (s->preprocess_images) {
        int x, y, w, h;

        for (int i = 0; i < area->nb_colors; ++i) {
            av_log(s, AV_LOG_DEBUG, "Color #%d: %0.8X\n", i, area->pal[i]);
        }

        text_color_index = get_text_color_index(s, area, bg_color_index, &outline_color_index);

        get_crop_region(s, area, text_color_index, &x, &y, &w, &h);

        if ((ret = crop_area_bitmap(s, area, x, y, w, h) < 0))
            goto exit;

        if ((ret = crop_area_bitmap(s, original_area, x, y, w, h) < 0))
            goto exit;

        make_image_binary(s, area, text_color_index);

        if (s->dump_bitmaps) {
            snprintf(filename, sizeof(filename), "graphicsub2text_%"PRId64"_%d_preprocessed", frame->subtitle_timing.start_pts, area_index);
            png_save(ctx, filename, area);
        }

        gs_img = create_bitmap_image(ctx, area, text_color_index);
    } else
        gs_img = create_grayscale_image(ctx, area, 1);

    if (!gs_img) {
        ret = AVERROR(ENOMEM);
        goto exit;
    }

    area->type = AV_SUBTITLE_FMT_ASS;
    TessBaseAPISetImage(s->tapi, gs_img, area->w, area->h, 1, area->linesize[0]);

    TessBaseAPISetSourceResolution(s->tapi, 72);

    ret = TessBaseAPIRecognize(s->tapi, NULL);
    if (ret == 0)
        ocr_text = TessBaseAPIGetUTF8Text(s->tapi);

    if (!ocr_text || !strlen(ocr_text)) {
        av_log(ctx, AV_LOG_WARNING, "OCR didn't return a text. ret=%d\n", ret);
        area->ass = NULL;

        goto exit;
    }

    const size_t len = strlen(ocr_text);
    if (len > 0 && ocr_text[len - 1] == '\n')
        ocr_text[len - 1] = 0;

    av_log(ctx, AV_LOG_VERBOSE, "OCR Result: %s\n", ocr_text);

    area->ass = av_strdup(ocr_text);
    TessDeleteText(ocr_text);

    // End of simple recognition

    if (s->recognize != RFLAGS_NONE) {
        TessResultIterator* ri = 0;
        const TessPageIteratorLevel level = RIL_WORD;
        int cur_is_bold = 0, cur_is_italic = 0, cur_is_underlined = 0, cur_pointsize = 0;
        uint32_t cur_text_color = 0, cur_bg_color = 0, cur_outline_color = 0;

        char *cur_font_name = NULL;
        int valign = 0; // 0: bottom, 4: top, 8 middle
        int halign = 2; // 1: left, 2: center, 3: right
        int in_code = 0;
        double font_factor = (0.000666 * (s->h - 480) + 1) * s->font_size_factor;

        av_freep(&area->ass);
        av_bprint_clear(&s->buffer);

        ri = TessBaseAPIGetIterator(s->tapi);

        // Horizontal Alignment
        if (s->w && s->recognize & RFLAGS_HALIGN) {
            int left_margin = area->x;
            int right_margin = s->w - area->x - area->w;
            double relative_diff = ((double)left_margin - right_margin) / s->w;

            if (FFABS(relative_diff) < 0.1)
                halign = 2; // center
            else if (relative_diff > 0)
                halign = 3; // right
            else
                halign = 1; // left
        }

        // Vertical Alignment
        if (s->h && frame->height && s->recognize & RFLAGS_VALIGN) {
            int left = 0, top = 0, right = 0, bottom = 0;

            TessPageIteratorBoundingBox((TessPageIterator*)ri, RIL_TEXTLINE, &left, &top, &right, &bottom);
            av_log(s, AV_LOG_DEBUG, "RIL_TEXTLINE - TOP: %d  BOTTOM: %d HEIGHT: %d\n", top, bottom, bottom - top);

            TessPageIteratorBoundingBox((TessPageIterator*)ri, RIL_BLOCK, &left, &top, &right, &bottom);

            const int vertical_pos = area->y + area->h / 2;
            if (vertical_pos < s->h / 3) {
                *margin_v = area->y + top;
                valign = 4;
            }
            else if (vertical_pos < s->h / 3 * 2) {
                *margin_v = 0;
                valign = 8;
            } else {
                *margin_v = frame->height - area->y - area->h;
                valign = 0;
            }
        }

        if (*margin_v < 0)
            *margin_v = 0;

        // Set alignment when not default (2)
        if ((valign | halign) != 2)
            in_code = print_code(&s->buffer, in_code, "\\a%d", valign | halign);

        do {
            int is_bold, is_italic, is_underlined, is_monospace, is_serif, is_smallcaps, pointsize, font_id;
            char* word;
            const char *font_name = TessResultIteratorWordFontAttributes(ri, &is_bold, &is_italic, &is_underlined, &is_monospace, &is_serif, &is_smallcaps, &pointsize, &font_id);
            uint32_t text_color = 0, bg_color = 0, outline_color = 0;

            if (cur_is_underlined && !is_underlined && s->recognize & RFLAGS_FUNDERLINE)
                in_code = print_code(&s->buffer, in_code, "\\u0");

            if (cur_is_bold && !is_bold && s->recognize & RFLAGS_FBOLD)
                in_code = print_code(&s->buffer, in_code, "\\b0");

            if (cur_is_italic && !is_italic && s->recognize & RFLAGS_FITALIC)
                in_code = print_code(&s->buffer, in_code, "\\i0");


            if (TessPageIteratorIsAtBeginningOf((TessPageIterator*)ri, RIL_TEXTLINE) && !TessPageIteratorIsAtBeginningOf((TessPageIterator*)ri, RIL_BLOCK)) {
                in_code = end_code(&s->buffer, in_code);
                av_bprintf(&s->buffer, "\\N");
            }

            if (get_word_colors(ctx, ri, area, original_area, bg_color_index, text_color_index, outline_color_index, &bg_color, &text_color, &outline_color) == 0) {

                if (text_color > 0 && cur_text_color != text_color && s->recognize & RFLAGS_COLOR) {
                    const uint8_t* tval = (uint8_t*)&text_color;
                    const int color = (int)tval[R] << 16 | (int)tval[G] << 8 | tval[B];

                    in_code = print_code(&s->buffer, in_code, "\\1c&H%0.6X&", color);
                    if (tval[A] != 255)
                        in_code = print_code(&s->buffer, in_code, "\\1a&H%0.2X&", 255 - tval[A]);
                }

                if (outline_color > 0 && cur_outline_color != outline_color && s->recognize & RFLAGS_OUTLINECOLOR) {
                    const uint8_t* tval = (uint8_t*)&outline_color;
                    const int color = (int)tval[R] << 16 | (int)tval[G] << 8 | tval[B];

                    in_code = print_code(&s->buffer, in_code, "\\3c&H%0.6X&\\bord2", color);
                    in_code = print_code(&s->buffer, in_code, "\\3a&H%0.2X&", FFMIN(255 - tval[A], 30));
                }

                cur_text_color = text_color;
                cur_outline_color = outline_color;
            }

            if (font_name && strlen(font_name) && s->recognize & RFLAGS_FONT) {
                if (!cur_font_name || !strlen(cur_font_name) || strcmp(cur_font_name, font_name) != 0) {
                    char *sanitized_font_name = av_strireplace(font_name, "_", " ");
                    if (!sanitized_font_name) {
                        ret = AVERROR(ENOMEM);
                        goto exit;
                    }

                    in_code = print_code(&s->buffer, in_code, "\\fn%s", sanitized_font_name);
                    av_freep(&sanitized_font_name);

                    if (cur_font_name)
                        av_freep(&cur_font_name);
                    cur_font_name = av_strdup(font_name);
                    if (!cur_font_name) {
                        ret = AVERROR(ENOMEM);
                        goto exit;
                    }
                }
            }

            if (pointsize > 0 && pointsize != cur_pointsize && s->recognize & RFLAGS_FONTSIZE) {
                float change_factor = (float)(FFABS(pointsize - cur_pointsize)) / FFMAX(pointsize, cur_pointsize);

                // Avoid small changes due to recognition variance
                if (change_factor > 0.12f) {
                    av_log(s, AV_LOG_DEBUG, "pointsize - pointsize: %d\n", pointsize);
                    in_code = print_code(&s->buffer, in_code, "\\fs%d", (int)(pointsize * font_factor));
                    cur_pointsize = pointsize;
                }
            }

            if (is_italic && !cur_is_italic && s->recognize & RFLAGS_FITALIC)
                in_code = print_code(&s->buffer, in_code, "\\i1");

            if (is_bold && !cur_is_bold && s->recognize & RFLAGS_FBOLD)
                in_code = print_code(&s->buffer, in_code, "\\b1");

            if (is_underlined && !cur_is_underlined && s->recognize & RFLAGS_FUNDERLINE)
                in_code = print_code(&s->buffer, in_code, "\\u1");

            in_code = end_code(&s->buffer, in_code);

            cur_is_underlined = is_underlined;
            cur_is_bold = is_bold;
            cur_is_italic = is_italic;

            if (!TessPageIteratorIsAtBeginningOf((TessPageIterator*)ri, RIL_TEXTLINE))
                av_bprint_chars(&s->buffer, ' ', 1);

            word = TessResultIteratorGetUTF8Text(ri, level);
            av_bprint_append_data(&s->buffer, word, strlen(word));
            TessDeleteText(word);

        } while (TessResultIteratorNext(ri, level));

        if (!av_bprint_is_complete(&s->buffer))
            ret = AVERROR(ENOMEM);
        else {
            av_log(ctx, AV_LOG_VERBOSE, "ASS Result: %s\n", s->buffer.str);
            area->ass = av_strdup(s->buffer.str);
        }

        TessResultIteratorDelete(ri);
        av_freep(&cur_font_name);
    }

exit:
    free_subtitle_area(original_area);
    av_freep(&gs_img);
    av_buffer_unref(&area->buf[0]);
    area->type = AV_SUBTITLE_FMT_ASS;

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    SubOcrContext *s = ctx->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int ret, frame_sent = 0;

    if (s->pending_frame && !frame->repeat_sub) {
        const int64_t pts_diff = frame->subtitle_timing.start_pts - s->pending_frame->subtitle_timing.start_pts;

        if (pts_diff == 0) {
            // This is just a repetition of the previous frame, ignore it
            av_frame_free(&frame);
            return 0;
        }

        s->pending_frame->subtitle_timing.duration = pts_diff;

        if ((ret = av_buffer_replace(&s->pending_frame->subtitle_header, s->subtitle_header)) < 0)
            return ret;

        ret = ff_filter_frame(outlink, s->pending_frame);
        s->pending_frame = NULL;
        if (ret < 0)
            return  ret;

        frame_sent = 1;
        s->last_subtitle_pts = frame->subtitle_timing.start_pts;
    }

    if (frame->repeat_sub) {
        // Ignore repeated frame
        av_frame_free(&frame);
        return 0;
    }

    s->last_subtitle_pts = frame->subtitle_timing.start_pts;

    ret = av_frame_make_writable(frame);

    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    frame->format = AV_SUBTITLE_FMT_ASS;

    av_log(ctx, AV_LOG_VERBOSE, "filter_frame sub_pts: %"PRIu64", duration: %"PRIu64", num_areas: %d\n",
        frame->subtitle_timing.start_pts, frame->subtitle_timing.duration, frame->num_subtitle_areas);

    if (frame->num_subtitle_areas > 1 &&
        frame->subtitle_areas[0]->y > frame->subtitle_areas[frame->num_subtitle_areas - 1]->y) {

        for (unsigned i = 0; i < frame->num_subtitle_areas / 2; i++)
            FFSWAP(AVSubtitleArea*, frame->subtitle_areas[i], frame->subtitle_areas[frame->num_subtitle_areas - i - 1]);
    }

    for (int i = 0; i < frame->num_subtitle_areas; i++) {
        AVSubtitleArea *area = frame->subtitle_areas[i];
        int margin_v = 0;

        ret = convert_area(ctx, area, frame, i, &margin_v);
        if (ret < 0)
            return ret;

        if (area->ass && area->ass[0] != '\0') {

            const int layer = s->recognize ? i : 0;
            char *tmp = area->ass;
            area->ass = avpriv_ass_get_dialog_ex(s->readorder_counter++, layer, "Default", NULL, 0, 0, margin_v, NULL, tmp);
            av_free(tmp);
        }
    }

    // When decoders can't determine the end time, they are setting it either to UINT32_NAX
    // or 30s (dvbsub).
    if (s->delay_when_no_duration && frame->subtitle_timing.duration >= ms_to_avtb(29000)) {
        // Can't send it without end time, wait for the next frame to determine the end_display time
        s->pending_frame = frame;

        if (frame_sent)
            return 0;

        // To keep all going, send an empty frame instead
        frame = ff_get_subtitles_buffer(outlink, AV_SUBTITLE_FMT_ASS);
        if (!frame)
            return AVERROR(ENOMEM);

        av_frame_copy_props(frame, s->pending_frame);
        frame->subtitle_timing.start_pts = 0;
        frame->subtitle_timing.duration = 1;
        frame->repeat_sub = 1;
    }

    if ((ret = av_buffer_replace(&frame->subtitle_header, s->subtitle_header)) < 0)
        return ret;

    return ff_filter_frame(outlink, frame);
}

#define OFFSET(x) offsetof(SubOcrContext, x)
#define FLAGS (AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption graphicsub2text_options[] = {
    { "delay_when_no_duration", "delay output when duration is unknown", OFFSET(delay_when_no_duration), AV_OPT_TYPE_BOOL,   { .i64 = 0 },                         0,                  1,       FLAGS, NULL },
    { "dump_bitmaps",           "save processed bitmaps as .ppm",        OFFSET(dump_bitmaps),           AV_OPT_TYPE_BOOL,   { .i64 = 0 },                         0,                  1,       FLAGS, NULL },
    { "font_size_factor",       "font size adjustment factor",           OFFSET(font_size_factor),       AV_OPT_TYPE_DOUBLE, { .dbl = 1.0 },                       0.2,                5,       FLAGS, NULL },
    { "language",               "ocr language",                          OFFSET(language),               AV_OPT_TYPE_STRING, { .str = "eng" },                     0,                  0,       FLAGS, NULL },
    { "ocr_mode",               "set ocr mode",                          OFFSET(ocr_mode),               AV_OPT_TYPE_INT,    { .i64=OEM_TESSERACT_ONLY },          OEM_TESSERACT_ONLY, 2,       FLAGS, "ocr_mode" },
    {   "tesseract",            "classic tesseract ocr",                 0,                              AV_OPT_TYPE_CONST,  { .i64=OEM_TESSERACT_ONLY },          0,                  0,       FLAGS, "ocr_mode" },
    {   "lstm",                 "lstm (ML based)",                       0,                              AV_OPT_TYPE_CONST,  { .i64=OEM_LSTM_ONLY},                0,                  0,       FLAGS, "ocr_mode" },
    {   "both",                 "use both models combined",              0,                              AV_OPT_TYPE_CONST,  { .i64=OEM_TESSERACT_LSTM_COMBINED }, 0,                  0,       FLAGS, "ocr_mode" },
    { "preprocess_images",      "reduce colors, remove outlines",        OFFSET(preprocess_images),      AV_OPT_TYPE_BOOL,   { .i64 = 1 },                         0,                  1,       FLAGS, NULL },
    { "recognize",              "detect fonts, styles and colors",       OFFSET(recognize),              AV_OPT_TYPE_FLAGS,  { .i64 = RFLAGS_ALL},                  0,                  INT_MAX, FLAGS, "reco_flags" },
        { "none",         "no format detection",  0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_NONE         }, 0, 0, FLAGS, "reco_flags" },
        { "halign",       "horizontal alignment", 0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_HALIGN       }, 0, 0, FLAGS, "reco_flags" },
        { "valign",       "vertical alignment",   0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_VALIGN       }, 0, 0, FLAGS, "reco_flags" },
        { "bold",         "font bold",            0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_FBOLD        }, 0, 0, FLAGS, "reco_flags" },
        { "italic",       "font italic",          0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_FITALIC      }, 0, 0, FLAGS, "reco_flags" },
        { "underline",    "font underline",       0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_FUNDERLINE   }, 0, 0, FLAGS, "reco_flags" },
        { "font",         "font name",            0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_FONT         }, 0, 0, FLAGS, "reco_flags" },
        { "fontsize",     "font size",            0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_FONTSIZE     }, 0, 0, FLAGS, "reco_flags" },
        { "color",        "font color",           0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_COLOR        }, 0, 0, FLAGS, "reco_flags" },
        { "outlinecolor", "outline color",        0, AV_OPT_TYPE_CONST, { .i64 = RFLAGS_OUTLINECOLOR }, 0, 0, FLAGS, "reco_flags" },
    { "tessdata_path",          "path to tesseract data",                OFFSET(tessdata_path),          AV_OPT_TYPE_STRING, { .str = NULL },                      0,                  0,       FLAGS, NULL },
    { NULL },
};

AVFILTER_DEFINE_CLASS(graphicsub2text);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_SUBTITLE,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_SUBTITLE,
        .config_props  = config_output,
    },
};

const AVFilter ff_sf_graphicsub2text = {
    .name          = "graphicsub2text",
    .description   = NULL_IF_CONFIG_SMALL("Convert graphical subtitles to text subtitles via OCR"),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(SubOcrContext),
    .priv_class    = &graphicsub2text_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
};
