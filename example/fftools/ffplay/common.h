//
// Created by Johnny on 2020/12/6.
//

#pragma once

extern "C" {
#include <libavutil/samplefmt.h> // for AVSampleFormat
}

#include <cstdint>

int compute_mod(int a, int b);

struct AVFormatContext;
int is_realtime(AVFormatContext *s);

struct AVFrame;
void set_sdl_yuv_conversion_mode(AVFrame *frame);

struct AVFilterGraph;
struct AVFilterContext;
int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                          AVFilterContext *source_ctx, AVFilterContext *sink_ctx);

struct SDL_Rect;
struct AVRational;
void calculate_display_rect(SDL_Rect *rect,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar);

int64_t get_valid_channel_layout(int64_t channel_layout, int channels);

// enum AVSampleFormat;
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2);