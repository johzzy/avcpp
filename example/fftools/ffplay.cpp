/*
 * Copyright (c) 2003 Fabrice Bellard
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
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

extern "C" {
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
}
#include "cmdutils.h"
#include <SDL.h>
#include <SDL_thread.h>

#include <logger.h>
#include <assert.h>

#include "ffplay/FrameQueue.h"
#include "ffplay/Clock.h"
#include "ffplay/Decoder.h"
#include "ffplay/VideoState.h"
#include "ffplay/common.h"

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

#define USE_ONEPASS_SUBTITLE_RENDER 1

unsigned VideoState::sws_flags = SWS_BICUBIC;

/* options specified by the user */
static AVInputFormat *file_iformat;

const char *VideoState::input_filename;
const char *VideoState::window_title;
int VideoState::default_width = 640;
int VideoState::default_height = 480;
int VideoState::screen_width = 0;
int VideoState::screen_height = 0;
int VideoState::screen_left = SDL_WINDOWPOS_CENTERED;
int VideoState::screen_top = SDL_WINDOWPOS_CENTERED;

int VideoState::audio_disable;
int VideoState::video_disable;
int VideoState::subtitle_disable;
const char *VideoState::wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
int VideoState::seek_by_bytes = -1;
float VideoState::seek_interval = 10;
int VideoState::display_disable;
int VideoState::borderless;
int VideoState::alwaysontop;
int VideoState::startup_volume = 100;

int VideoState::show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
int64_t VideoState::start_time = AV_NOPTS_VALUE;
int64_t VideoState::duration = AV_NOPTS_VALUE;
int VideoState::fast = 0;
int VideoState::genpts = 0;
int VideoState::lowres = 0;
int Decoder::decoder_reorder_pts = -1;

int VideoState::autoexit;
int VideoState::exit_on_keydown;
int VideoState::exit_on_mousedown;
int VideoState::loop = 1;
int VideoState::framedrop = -1;
int VideoState::infinite_buffer = -1;

VideoState::ShowMode VideoState::show_mode_ = VideoState::SHOW_MODE_NONE;
const char *VideoState::audio_codec_name;
const char *VideoState::subtitle_codec_name;
const char *VideoState::video_codec_name;

double VideoState::rdftspeed = 0.02;
int64_t VideoState::cursor_last_shown;
int VideoState::cursor_hidden = 0;

const char **VideoState::vfilters_list = NULL;
int VideoState::nb_vfilters = 0;
char *VideoState::afilters = NULL;

int VideoState::autorotate = 1;
int VideoState::find_stream_info = 1;
int VideoState::filter_nbthreads = 0;

/* current context */
int VideoState::is_full_screen;
int64_t VideoState::audio_callback_time;

SDL_Window *VideoState::window;
SDL_Renderer *VideoState::renderer;
SDL_RendererInfo VideoState::renderer_info = { 0 };

struct OptionContext {
    void trace(const char *fn, int line, const char *file)
    {
        printf("%s %d %s\n", fn, line, file);
    }

    static void Trace(void *ctx, const char *fn, int line, const char *file)
    {
        if (!ctx)
            return;
        auto that = static_cast<OptionContext *>(ctx);
        that->trace(fn, line, file);
    }
};

static int opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    GROW_ARRAY(VideoState::vfilters_list, VideoState::nb_vfilters);
    VideoState::vfilters_list[VideoState::nb_vfilters - 1] = arg;
    return 0;
}

static void sigterm_handler(int sig)
{
    exit(123);
}

static int opt_frame_size(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    av_log(nullptr, AV_LOG_WARNING, "Option -s is deprecated, use -video_size.\n");
    return opt_default(NULL, "video_size", arg);
}

static int opt_width(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    VideoState::screen_width =
        parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    VideoState::screen_height =
        parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(nullptr, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_frame_pix_fmt(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    av_log(nullptr, AV_LOG_WARNING,
           "Option -pix_fmt is deprecated, use -pixel_format.\n");
    return opt_default(NULL, "pixel_format", arg);
}

static int opt_sync(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    if (!strcmp(arg, "audio"))
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    else {
        av_log(nullptr, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

static int opt_seek(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    VideoState::start_time = parse_time_or_die(opt, arg, 1);
    return 0;
}

static int opt_duration(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    VideoState::duration = parse_time_or_die(opt, arg, 1);
    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    VideoState::show_mode_ =
        !strcmp(arg, "video")   ? VideoState::SHOW_MODE_VIDEO
        : !strcmp(arg, "waves") ? VideoState::SHOW_MODE_WAVES
        : !strcmp(arg, "rdft")
            ? VideoState::SHOW_MODE_RDFT
            : (VideoState::ShowMode)parse_number_or_die(
                  opt, arg, OPT_INT, 0, VideoState::SHOW_MODE_NB - 1);
    return 0;
}

static void opt_input_file(void *optctx, const char *filename)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    if (VideoState::input_filename) {
        av_log(nullptr, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already "
               "specified.\n",
               filename, VideoState::input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    VideoState::input_filename = filename;
}

static int opt_codec(void *optctx, const char *opt, const char *arg)
{
    OptionContext::Trace(optctx, __FUNCTION__, __LINE__, __FILE__);
    const char *spec = strchr(opt, ':');
    if (!spec) {
        av_log(nullptr, AV_LOG_ERROR,
               "No media specifier was specified in '%s' in option '%s'\n", arg,
               opt);
        return AVERROR(EINVAL);
    }
    spec++;
    switch (spec[0]) {
    case 'a':
        VideoState::audio_codec_name = arg;
        break;
    case 's':
        VideoState::subtitle_codec_name = arg;
        break;
    case 'v':
        VideoState::video_codec_name = arg;
        break;
    default:
        av_log(nullptr, AV_LOG_ERROR,
               "Invalid media specifier '%s' in option '%s'\n", spec, opt);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int dummy;

// clang-format off
static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "x", HAS_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
    { "y", HAS_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
    { "s", HAS_ARG | OPT_VIDEO, { .func_arg = opt_frame_size }, "set frame size (WxH or abbreviation)", "size" },
    { "fs", OPT_BOOL, { &VideoState::is_full_screen }, "force full screen" },
    { "an", OPT_BOOL, { &VideoState::audio_disable }, "disable audio" },
    { "vn", OPT_BOOL, { &VideoState::video_disable }, "disable video" },
    { "sn", OPT_BOOL, { &VideoState::subtitle_disable }, "disable subtitling" },
    { "ast", OPT_STRING | HAS_ARG | OPT_EXPERT, { &VideoState::wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },
    { "vst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &VideoState::wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },
    { "sst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &VideoState::wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },
    { "ss", HAS_ARG, { .func_arg = opt_seek }, "seek to a given position in seconds", "pos" },
    { "t", HAS_ARG, { .func_arg = opt_duration }, "play  \"duration\" seconds of audio/video", "duration" },
    { "bytes", OPT_INT | HAS_ARG, { &VideoState::seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },
    { "seek_interval", OPT_FLOAT | HAS_ARG, { &VideoState::seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },
    { "nodisp", OPT_BOOL, { &VideoState::display_disable }, "disable graphical display" },
    { "noborder", OPT_BOOL, { &VideoState::borderless }, "borderless window" },
    { "alwaysontop", OPT_BOOL, { &VideoState::alwaysontop }, "window always on top" },
    { "volume", OPT_INT | HAS_ARG, { &VideoState::startup_volume}, "set startup volume 0=min 100=max", "volume" },
    { "f", HAS_ARG, { .func_arg = opt_format }, "force format", "fmt" },
    { "pix_fmt", HAS_ARG | OPT_EXPERT | OPT_VIDEO, { .func_arg = opt_frame_pix_fmt }, "set pixel format", "format" },
    { "stats", OPT_BOOL | OPT_EXPERT, { &VideoState::show_status }, "show status", "" },
    { "fast", OPT_BOOL | OPT_EXPERT, { &VideoState::fast }, "non spec compliant optimizations", "" },
    { "genpts", OPT_BOOL | OPT_EXPERT, { &VideoState::genpts }, "generate pts", "" },
    { "drp", OPT_INT | HAS_ARG | OPT_EXPERT, { &Decoder::decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    { "lowres", OPT_INT | HAS_ARG | OPT_EXPERT, { &VideoState::lowres }, "", "" },
    { "sync", HAS_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },
    { "autoexit", OPT_BOOL | OPT_EXPERT, { &VideoState::autoexit }, "exit at the end", "" },
    { "exitonkeydown", OPT_BOOL | OPT_EXPERT, { &VideoState::exit_on_keydown }, "exit on key down", "" },
    { "exitonmousedown", OPT_BOOL | OPT_EXPERT, { &VideoState::exit_on_mousedown }, "exit on mouse down", "" },
    { "loop", OPT_INT | HAS_ARG | OPT_EXPERT, { &VideoState::loop }, "set number of times the playback shall be looped", "loop count" },
    { "framedrop", OPT_BOOL | OPT_EXPERT, { &VideoState::framedrop }, "drop frames when cpu is too slow", "" },
    { "infbuf", OPT_BOOL | OPT_EXPERT, { &VideoState::infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
    { "window_title", OPT_STRING | HAS_ARG, { &VideoState::window_title }, "set window title", "window title" },
    { "left", OPT_INT | HAS_ARG | OPT_EXPERT, { &VideoState::screen_left }, "set the x position for the left of the window", "x pos" },
    { "top", OPT_INT | HAS_ARG | OPT_EXPERT, { &VideoState::screen_top }, "set the y position for the top of the window", "y pos" },
    { "vf", OPT_EXPERT | HAS_ARG, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
    { "af", OPT_STRING | HAS_ARG, { &VideoState::afilters }, "set audio filters", "filter_graph" },
    { "rdftspeed", OPT_INT | HAS_ARG| OPT_AUDIO | OPT_EXPERT, { &VideoState::rdftspeed }, "rdft speed", "msecs" },
    { "showmode", HAS_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
    { "default", HAS_ARG | OPT_AUDIO | OPT_VIDEO | OPT_EXPERT, { .func_arg = opt_default }, "generic catch all option", "" },
    { "i", OPT_BOOL, { &dummy}, "read specified file", "input_file"},
    { "codec", HAS_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },
    { "acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &VideoState::audio_codec_name }, "force audio decoder",    "decoder_name" },
    { "scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, { &VideoState::subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
    { "vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &VideoState::video_codec_name }, "force video decoder",    "decoder_name" },
    { "autorotate", OPT_BOOL, { &VideoState::autorotate }, "automatically rotate video", "" },
    { "find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, { &VideoState::find_stream_info },
        "read and decode the streams to fill missing information with heuristics" },
    { "filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, { &VideoState::filter_nbthreads }, "number of filter threads per graph" },
    { NULL, },
};
// clang-format on

static void show_usage(void)
{
    av_log(nullptr, AV_LOG_INFO, "Simple media player\n");
    av_log(nullptr, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(nullptr, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds or to custom "
           "interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to "
           "fraction of width\n"
           "left double-click   toggle full screen\n");
}

enum BadEnum : uint8_t {
    BigNumA = 0,
    BigNumB = 1 + BigNumA,
    BigNumC = 1 + BigNumB
};

/* Called from the main */
int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_TRACE);
    SPDLOG_INFO("SDL_COMPILEDVERSION={}, SDL_VERSIONNUM(2,0,5)={}\n",
                SDL_COMPILEDVERSION, SDL_VERSIONNUM(2, 0, 5));
    SPDLOG_INFO("AVSampleFormat size: {}", sizeof(AVSampleFormat));
    SPDLOG_INFO("BadEnum size: {}", sizeof(BadEnum));
    // assert(false);
    int flags;

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    /* register all codecs, demux and protocols */
    avdevice_register_all();
    avformat_network_init();

    init_opts();

    signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    show_banner(argc, argv, options);
    OptionContext optctx;
    parse_options(&optctx, argc, argv, options, opt_input_file);
    // assert(false);

    if (!VideoState::input_filename) {
        show_usage();
        av_log(nullptr, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(nullptr, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n",
               program_name);
        exit(1);
    }

    if (VideoState::display_disable) {
        VideoState::video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (VideoState::audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer
         * size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }
    if (VideoState::display_disable)
        flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init(flags)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n",
               SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    if (!VideoState::display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (VideoState::alwaysontop)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        if (VideoState::borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        VideoState::window = SDL_CreateWindow(
            program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            VideoState::default_width, VideoState::default_height, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (VideoState::window) {
            VideoState::renderer = SDL_CreateRenderer(
                VideoState::window, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!VideoState::renderer) {
                av_log(nullptr, AV_LOG_WARNING,
                       "Failed to initialize a hardware accelerated renderer: "
                       "%s\n",
                       SDL_GetError());
                VideoState::renderer =
                    SDL_CreateRenderer(VideoState::window, -1, 0);
            }
            if (VideoState::renderer) {
                if (!SDL_GetRendererInfo(VideoState::renderer,
                                         &VideoState::renderer_info))
                    av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n",
                           VideoState::renderer_info.name);
            }
        }
        if (!VideoState::window || !VideoState::renderer ||
            !VideoState::renderer_info.num_texture_formats) {
            av_log(nullptr, AV_LOG_FATAL,
                   "Failed to create window or renderer: %s", SDL_GetError());
            VideoState::do_exit();
        }
    }

    auto is = VideoState{};

    auto state =
        is.StreamOpen(VideoState::input_filename, file_iformat, av_sync_type);
    if (!state) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        VideoState::do_exit();
    }

    is.EventLoop();

    return 0;
}
