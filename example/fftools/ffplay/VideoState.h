//
// Created by Johnny on 2020/12/6.
//

#pragma once

#include "Clock.h"
#include "FrameQueue.h"
#include "Decoder.h"

#include <SDL2/SDL_blendmode.h>
#include <SDL2/SDL_thread.h>
#include <SDL2/SDL_events.h>
#include <SDL.h>

extern "C" {
#include <libavutil/samplefmt.h> // for AVSampleFormat
#include <libavformat/avformat.h>
#include <libavcodec/avfft.h>
#include <libavfilter/avfilter.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <cmath>
#include <string>

/* NOTE: the size must be big enough to compensate the hardware audio buffersize
 * size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
constexpr int SAMPLE_ARRAY_SIZE = (8 * 65536);

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
constexpr int AUDIO_DIFF_AVG_NB = 20;

/* Minimum SDL audio buffer size, in samples. */
constexpr int SDL_AUDIO_MIN_BUFFER_SIZE = 512;

/* Calculate actual buffer size keeping in mind not cause too frequent audio
 * callbacks */
constexpr int SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30;

/* polls for possible required screen refresh at least this often, should be
 * less than 1/fps */
constexpr auto REFRESH_RATE = 0.01;

constexpr auto CURSOR_HIDE_DELAY = 1000000;

constexpr auto FF_QUIT_EVENT = (SDL_USEREVENT + 2);

/* Step size for volume control in dB */
constexpr auto SDL_VOLUME_STEP = (0.75);

constexpr auto EXTERNAL_CLOCK_MIN_FRAMES = 2;
constexpr auto EXTERNAL_CLOCK_MAX_FRAMES = 10;

/* external clock speed adjustment constants for realtime sources based on
 * buffer fullness */
constexpr auto EXTERNAL_CLOCK_SPEED_MIN = 0.900;
constexpr auto EXTERNAL_CLOCK_SPEED_MAX = 1.010;
constexpr auto EXTERNAL_CLOCK_SPEED_STEP = 0.001;

/* no AV sync correction is done if below the minimum AV sync threshold */
constexpr auto AV_SYNC_THRESHOLD_MIN = 0.04;
/* AV sync correction is done if above the maximum AV sync threshold */
constexpr auto AV_SYNC_THRESHOLD_MAX = 0.1;
/* If a frame duration is longer than this, it will not be duplicated to
 * compensate AV sync */
constexpr auto AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;

/* maximum audio speed change to get correct sync */
constexpr auto SAMPLE_CORRECTION_PERCENT_MAX = 10;

constexpr auto MAX_QUEUE_SIZE = (15 * 1024 * 1024);
constexpr auto MIN_FRAMES = 25;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
};

struct SDL_Renderer;
struct SDL_Texture;

enum ShowMode {
    SHOW_MODE_NONE = -1,
    SHOW_MODE_VIDEO = 0,
    SHOW_MODE_WAVES,
    SHOW_MODE_RDFT,
    SHOW_MODE_NB
};

struct VideoStateExtra {
    AVPacket flush_pkt;

    VideoStateExtra()
    {
        av_init_packet(&flush_pkt);
        flush_pkt.data = (uint8_t *)&flush_pkt;
    }
    /* options specified by the user */
    AVInputFormat *file_iformat { nullptr};
    int av_sync_type = AV_SYNC_AUDIO_MASTER;

    /* options specified by the user */
    const char *input_filename { nullptr};
    const char *window_title {nullptr};
    int default_width = 640;
    int default_height = 480;
    int screen_width = 0;
    int screen_height = 0;
    int screen_left = SDL_WINDOWPOS_CENTERED;
    int screen_top = SDL_WINDOWPOS_CENTERED;

    double rdftspeed = 0.02;
    int64_t cursor_last_shown {0LL};
    int cursor_hidden = 0;

    int autoexit{0};
    int exit_on_keydown{0};
    int exit_on_mousedown{0};
    int loop = 1;
    int framedrop = -1;
    int infinite_buffer = -1;


    //
    const char **vfilters_list{ nullptr};
    int nb_vfilters{0};
    char *afilters {nullptr};

    int autorotate = 1;
    int find_stream_info = 1;
    int filter_nbthreads = 0;

    /* current context */
    int is_full_screen{0};
    int64_t audio_callback_time{0LL};

    SDL_Window *window {nullptr};
    int show_status = -1;
    SDL_Renderer *renderer {nullptr};
    SDL_RendererInfo renderer_info = { 0 };

    ///////////

    int audio_disable {0};
    int video_disable {0};
    int subtitle_disable {0};

    const char *audio_codec_name{ nullptr};
    const char *subtitle_codec_name{ nullptr};
    const char *video_codec_name{ nullptr};

    ////////////
    unsigned sws_flags = SWS_BICUBIC;

    ShowMode show_mode_ = ShowMode::SHOW_MODE_NONE;
    ////////////

    int64_t start_time = AV_NOPTS_VALUE;
    int64_t duration = AV_NOPTS_VALUE;
    int fast = 0;
    int genpts = 0;
    int lowres = 0;





    ////////////
    const char wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
    int seek_by_bytes = -1;
    float seek_interval = 10.f;

    int display_disable {0};
    int borderless {0};
    int alwaysontop {0};

    int startup_volume = 100;

    /////////////////

    void ToggleFullScreen();

    void FillRectangle(int x, int y, int w, int h);
    void set_default_window_size(int width, int height, AVRational sar);
    void do_exit();

    int upload_texture(SDL_Texture **tex,
                              AVFrame *frame,
                              struct SwsContext **img_convert_ctx);
    int realloc_texture(SDL_Texture **texture,
                        Uint32 new_format,
                        int new_width,
                        int new_height,
                        SDL_BlendMode blendmode,
                        int init_texture);
};

struct FourierContext {
    RDFTContext *rdft{ nullptr };
    FFTSample *rdft_data{ nullptr };
    int rdft_bits{ 0 };

    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;

    void Close() {
        if (rdft) {
            av_rdft_end(rdft);
            av_freep(&rdft_data);
            rdft = nullptr;
            rdft_bits = 0;
        }
    }
};

struct VideoState {
    SDL_Thread *read_tid;
    AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;

    struct AudioParams audio_filter_src;

    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;


    ShowMode show_mode;
    // static ShowMode show_mode_;

    int last_i_start;
    FourierContext fourier;
    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    double max_frame_duration; // maximum duration of a frame - above this, we
                               // consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int eof;

    std::string filename;
    int width, height, xleft, ytop;
    int step;

    int vfilter_idx;
    AVFilterContext *in_video_filter;  // the first filter in the video chain
    AVFilterContext *out_video_filter; // the last filter in the video chain
    AVFilterContext *in_audio_filter;  // the first filter in the audio chain
    AVFilterContext *out_audio_filter; // the last filter in the audio chain
    AVFilterGraph *agraph{ nullptr};             // audio filter graph

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;

    // static SDL_Renderer *renderer;
    // static SDL_RendererInfo renderer_info;
    // static int64_t audio_callback_time;

    // static inline void FillRectangle(int x, int y, int w, int h);

    void VideoImageDisplay();
    void VideoAudioDisplay();

    static void do_exit();

    void StreamClose();
    bool StreamOpen();

    /* this thread gets the stream from the disk or the network */
    static int read_thread(void *arg);

    // static int genpts;

    // static int64_t start_time;
    // static int64_t duration;

    static int decode_interrupt_cb(void *ctx)
    {
        auto is = (VideoState *)ctx;
        assert(is);
        return is->abort_request;
    }

    /* open a given stream. Return 0 if OK */
    int StreamComponentOpen(int stream_index);

    void StreamComponentClose(int stream_index);

    /* display the current picture, if any */
    void VideoDisplay();

    int VideoOpen();

    int AudioOpen(int64_t wanted_channel_layout,
                  int wanted_nb_channels,
                  int wanted_sample_rate);

    void StreamCycleChannel(int codec_type);

    void toggle_audio_display();

    void RefreshLoopWaitEvent(SDL_Event *event);

    void SeekChapter(int incr);

    /* handle an event sent by the GUI */
    void EventLoop();

    int GetMasterSyncType();

    /* get the current master clock value */
    double GetMasterClock();

    void CheckExternalClockSpeed();

    /* seek in the stream */
    void StreamSeek(int64_t pos, int64_t rel, int seek_by_bytes);

    /* pause or resume the video */
    void StreamTogglePause();

    void TogglePause();

    void ToggleMute();

    void UpdateVolume(int sign, double step);

    void StepToNextFrame();

    double TargetDelay(double delay);

    double vp_duration(Frame *vp, Frame *nextvp);

    void UpdateVideoPTS(double pts, int64_t pos, int serial);

    /* called to display each frame */
    void VideoRefresh(double *remaining_time);

    int queue_picture(AVFrame *src_frame,
                      double pts,
                      double duration,
                      int64_t pos,
                      int serial);

    int GetVideoFrame(AVFrame *frame);

    int ConfigureVideoFilters(AVFilterGraph *graph,
                              const char *vfilters,
                              AVFrame *frame);

    int ConfigureAudioFilters(const char *afilters, int force_output_format);

    /**
     * Decode one audio frame and return its uncompressed size.
     *
     * The processed audio frame is decoded, converted if required, and
     * stored in is->audio_buf, with size in bytes given by the return
     * value.
     */
    int AudioDecodeFrame();

    /* copy samples for viewing in editor window */
    void UpdateSampleDisplay(short *samples, int samples_size);

    /* return the wanted number of samples to get better sync if sync_type is
     * video or external master clock */
    int SynchronizeAudio(int nb_samples);

    //////////

    // static unsigned sws_flags;
    // static int upload_texture(SDL_Texture **tex,
    //                           AVFrame *frame,
    //                           struct SwsContext **img_convert_ctx);

    /* prepare a new audio buffer */
    static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);

    static int audio_thread(void *arg);

    static int video_thread(void *arg);

    static int subtitle_thread(void *arg);

    // void set_default_window_size(int width, int height, AVRational sar);

    // static SDL_Window *window;
    // static int show_status;

    // static int nb_vfilters;
    // static const char **vfilters_list;

    SDL_AudioDeviceID audio_dev;
    // static int lowres;

    // static const char *audio_codec_name;
    // static const char *subtitle_codec_name;
    // static const char *video_codec_name;

    // static int fast;

    // static char *afilters;

    // static int autorotate;
    // static int find_stream_info;
    // static int filter_nbthreads;

    // static int is_full_screen;

    /* options specified by the user */
    // static const char *input_filename;
    // static const char *window_title;
    // static int default_width;
    // static int default_height;
    // static int screen_width;
    // static int screen_height;
    // static int screen_left;
    // static int screen_top;

    // static double rdftspeed;
    // static int64_t cursor_last_shown;
    // static int cursor_hidden;

    // static int autoexit;
    // static int exit_on_keydown;
    // static int exit_on_mousedown;
    // static int loop;
    // static int framedrop;
    // static int infinite_buffer;



    // static int audio_disable;
    // static int video_disable;
    // static int subtitle_disable;

    // static const char *wanted_stream_spec[AVMEDIA_TYPE_NB];
    // static int seek_by_bytes;
    // static float seek_interval;
    // static int display_disable;
    // static int borderless;
    // static int alwaysontop;
    // static int startup_volume;

    VideoStateExtra& extra;
    VideoState(VideoStateExtra& e): extra(e) {}
};
