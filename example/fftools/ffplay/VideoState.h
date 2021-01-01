//
// Created by Johnny on 2020/12/6.
//

#pragma once

#include "Clock.h"
#include "Decoder.h"
#include "FrameQueue.h"

#include <SDL.h>
#include <SDL2/SDL_blendmode.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_thread.h>

extern "C" {
#include <libavcodec/avfft.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h> // for AVSampleFormat
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <cstdint>
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

enum class ShowMode : int { None = -1, Video = 0, Waves, RDFT, NB };

struct VideoStateExtra {
    AVPacket flush_pkt;

    VideoStateExtra() {
        av_init_packet(&flush_pkt);
        flush_pkt.data = (uint8_t*)&flush_pkt;
    }
    /* options specified by the user */
    AVInputFormat* file_iformat{ nullptr };
    int av_sync_type = AV_SYNC_AUDIO_MASTER;

    /* options specified by the user */
    const char* input_filename{ nullptr };
    const char* window_title{ nullptr };
    int default_width = 640;
    int default_height = 480;
    int screen_width = 0;
    int screen_height = 0;
    int screen_left = SDL_WINDOWPOS_CENTERED;
    int screen_top = SDL_WINDOWPOS_CENTERED;

    double rdftspeed = 0.02;
    int64_t cursor_last_shown{ 0LL };
    int cursor_hidden = 0;

    int autoexit{ 0 };
    int exit_on_keydown{ 0 };
    int exit_on_mousedown{ 0 };
    int loop = 1;
    int framedrop = -1;
    int infinite_buffer = -1;

    ///////
    const char** vfilters_list{ nullptr };
    int nb_vfilters{ 0 };
    char* afilters{ nullptr };

    int autorotate = 1;
    int find_stream_info = 1;
    int filter_nbthreads = 0;

    /* current context */
    int is_full_screen{ 0 };
    int64_t audio_callback_time{ 0LL };

    SDL_Window* window{ nullptr };
    int show_status = -1;
    SDL_Renderer* renderer{ nullptr };
    SDL_RendererInfo renderer_info = { 0 };

    ///////////

    int audio_disable{ 0 };
    int video_disable{ 0 };
    int subtitle_disable{ 0 };

    const char* audio_codec_name{ nullptr };
    const char* subtitle_codec_name{ nullptr };
    const char* video_codec_name{ nullptr };

    ////////////
    unsigned sws_flags = SWS_BICUBIC;

    ShowMode show_mode_ = ShowMode::None;
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

    int display_disable{ 0 };
    int borderless{ 0 };
    int alwaysontop{ 0 };

    int startup_volume = 100;

    /////////////////

    void ToggleFullScreen();

    void RenderFillRect(int x, int y, int w, int h);
    void RenderYdX(int x, int y1, int delta);
    void set_default_window_size(int width, int height, AVRational sar);
    void do_exit();

    int upload_texture(SDL_Texture** tex, AVFrame* frame, struct SwsContext** img_convert_ctx);
    int ReallocTexture(SDL_Texture** texture,
                       Uint32 new_format,
                       int new_width,
                       int new_height,
                       SDL_BlendMode blendmode,
                       int init_texture);
};

struct FourierContext {
    RDFTContext* rdft{ nullptr };
    FFTSample* rdft_data{ nullptr };
    int rdft_bits{ 0 };

    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;

    int SampleZero(int ii, int channels) const {
        if (ii >= SAMPLE_ARRAY_SIZE) {
            return -1;
        }
        if (channels < 1) {
            return -1;
        }
        for (int i = ii; i < SAMPLE_ARRAY_SIZE; i += channels) {
            if (sample_array[i]) {
                return 1;
            }
        }
        return 0;
    }

    int sample_fixed_interal(int ii, int channels) const {
        auto left = std::max(0, ii - channels);
        // [left, ii)
        // 1 1 0 0 1 1 0 0 1 1 0 0
        int last_zero{ 0 };
        int max_zero{ 0 };
        for (int i = ii - 1; i >= left; --i) {
            if (sample_array[i]) {
                ++last_zero;
            } else {
                max_zero = std::max(max_zero, last_zero);
                last_zero = 0;
            }
        }
        return std::max(max_zero, last_zero);
    }

    int SampleFixed2(int ii, int channels) const {
        int max_zero = sample_fixed_interal(ii, channels);
        const int try_count {10};
        for (int i=1; i<try_count; ++i) {
            int iii = ii + channels * try_count;
            max_zero = std::max(max_zero, sample_fixed_interal(iii, channels));
        }
        return max_zero;
    }

    int SampleFixed(int ii, int channels) const {
        return sample_fixed_interal(ii, channels);
    }

    /* copy samples for viewing in editor window */
    void SampleUpdate(const short* samples, int samples_size) {
        int len;
        int size = samples_size / sizeof(short);
        while (size > 0) {
            len = SAMPLE_ARRAY_SIZE - sample_array_index;
            if (len > size)
                len = size;
            memcpy(sample_array + sample_array_index, samples, len * sizeof(short));
            samples += len;
            sample_array_index += len;
            if (sample_array_index >= SAMPLE_ARRAY_SIZE)
                sample_array_index = 0;
            size -= len;
        }
    }

    int SampleDeltaY(int h2, int i) const { return (sample_array[i] * h2) >> 15; }

    int SamplePulse(int channel_index, int sample_index, int h2, int y) const {
        static int pulse_offset{ SAMPLE_ARRAY_SIZE };
        constexpr auto k = 3;
        constexpr auto speed = 1;
        constexpr auto size = 1;
        if (sample_index == 0) {
            // pulse_offset += speed;
            pulse_offset += y;
        }
        int offset = pulse_offset - (channel_index+1) * sample_index/k;
        h2 *= size;
        return offset % (h2 * 2) - h2;
    }

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
    SDL_Thread* read_tid;
    AVInputFormat* iformat;
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
    AVFormatContext* ic;
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
    AVStream* audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t* audio_buf;
    uint8_t* audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;

    struct AudioParams audio_filter_src;

    struct AudioParams audio_tgt;
    struct SwrContext* swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    ShowMode show_mode;

    int last_i_start;
    FourierContext fourier;
    int xpos;
    double last_vis_time;
    SDL_Texture* vis_texture{ nullptr };
    SDL_Texture* sub_texture{ nullptr };
    SDL_Texture* vid_texture{ nullptr };

    int subtitle_stream;
    AVStream* subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream* video_st;
    PacketQueue videoq;
    double max_frame_duration; // maximum duration of a frame - above this, we
                               // consider the jump a timestamp discontinuity
    struct SwsContext* img_convert_ctx{ nullptr };
    struct SwsContext* sub_convert_ctx{ nullptr };
    int eof;

    std::string filename;
    int width, height, xleft, ytop;
    int step;

    int vfilter_idx;
    AVFilterContext* in_video_filter;  // the first filter in the video chain
    AVFilterContext* out_video_filter; // the last filter in the video chain
    AVFilterContext* in_audio_filter;  // the first filter in the audio chain
    AVFilterContext* out_audio_filter; // the last filter in the audio chain
    AVFilterGraph* agraph{ nullptr };  // audio filter graph

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond* continue_read_thread;

    void VideoImageDisplay();
    void VideoAudioDisplay();

    // static void do_exit();

    void StreamClose();
    bool StreamOpen();

    /* this thread gets the stream from the disk or the network */
    static int ReadThread(void* arg);

    static int decode_interrupt_cb(void* ctx) {
        auto is = (VideoState*)ctx;
        assert(is);
        return is->abort_request;
    }

    /* open a given stream. Return 0 if OK */
    int StreamComponentOpen(int stream_index);

    void StreamComponentClose(int stream_index);

    /* display the current picture, if any */
    void VideoDisplay();

    int VideoOpen();

    int AudioOpen(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate);

    void StreamCycleChannel(int codec_type);

    void ToggleAudioDisplay();

    void RefreshLoopWaitEvent(SDL_Event* event);

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

    double vp_duration(Frame* vp, Frame* nextvp);

    void UpdateVideoPTS(double pts, int64_t pos, int serial);

    /* called to display each frame */
    void VideoRefresh(double* remaining_time);

    int queue_picture(AVFrame* src_frame, double pts, double duration, int64_t pos, int serial);

    int GetVideoFrame(AVFrame* frame);

    int ConfigureVideoFilters(AVFilterGraph* graph, const char* vfilters, AVFrame* frame);

    int ConfigureAudioFilters(const char* afilters, int force_output_format);

    /**
     * Decode one audio frame and return its uncompressed size.
     *
     * The processed audio frame is decoded, converted if required, and
     * stored in is->audio_buf, with size in bytes given by the return
     * value.
     */
    int AudioDecodeFrame();

    /* return the wanted number of samples to get better sync if sync_type is
     * video or external master clock */
    int SynchronizeAudio(int nb_samples);

    /* prepare a new audio buffer */
    static void sdl_audio_callback(void* opaque, Uint8* stream, int len);

    static int audio_thread(void* arg);

    static int video_thread(void* arg);

    static int subtitle_thread(void* arg);

    SDL_AudioDeviceID audio_device_id{ 0 };

    VideoStateExtra& extra;
    VideoState(VideoStateExtra& e) : extra(e) {}
    void DrawWaves(int i_start, int nb_display_channels, int channels);
    void HackDrawWaves(int i_start, int nb_display_channels, int channels);
    void DrawRDFT(int i_start, int nb_display_channels, int channels, int rdft_bits, int nb_freq);
    int UpdateAudioDisplay(int nb_freq, int channels);
    void DoSeek(double incr);
    void DoKeyDown(SDL_Event const& event) {
        if (extra.exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
            StreamClose();
            extra.do_exit();
            return;
        }
        // If we don't yet have a window, skip all key events, because
        // ReadThread might still be initializing...
        if (!width)
            return;
        switch (event.key.keysym.sym) {
            case SDLK_f:
                extra.ToggleFullScreen();
                force_refresh = 1;
                break;
            case SDLK_p:
            case SDLK_SPACE:
                TogglePause();
                break;
            case SDLK_m:
                ToggleMute();
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                UpdateVolume(1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                UpdateVolume(-1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // S: Step to next frame
                StepToNextFrame();
                break;
            case SDLK_a:
                StreamCycleChannel(AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                StreamCycleChannel(AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                StreamCycleChannel(AVMEDIA_TYPE_VIDEO);
                StreamCycleChannel(AVMEDIA_TYPE_AUDIO);
                StreamCycleChannel(AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                StreamCycleChannel(AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
                if (show_mode == ShowMode::Video && vfilter_idx < extra.nb_vfilters - 1) {
                    if (++vfilter_idx >= extra.nb_vfilters)
                        vfilter_idx = 0;
                } else {
                    vfilter_idx = 0;
                    ToggleAudioDisplay();
                }
                break;
            case SDLK_PAGEUP:
                if (ic->nb_chapters <= 1) {
                    DoSeek(600.0);
                } else {
                    SeekChapter(1);
                }
                break;
            case SDLK_PAGEDOWN:
                if (ic->nb_chapters <= 1) {
                    DoSeek(-600.0);
                } else {
                    SeekChapter(-1);
                }
                break;
            case SDLK_LEFT:
                DoSeek(extra.seek_interval ? -extra.seek_interval : -10.0);
                break;
            case SDLK_RIGHT:
                DoSeek(extra.seek_interval ? extra.seek_interval : 10.0);
                break;
            case SDLK_UP:
                DoSeek(60.0);
                break;
            case SDLK_DOWN:
                DoSeek(-60.0);
                break;
            case SDLK_r:
                StreamSeek(extra.start_time != AV_NOPTS_VALUE ? extra.start_time : 0, 0, 0);
                break;
            default:
                break;
        }
    }
    void DoMouseButtonDown(SDL_Event const& event) {
        if (extra.exit_on_mousedown) {
            StreamClose();
            extra.do_exit();
            return;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            static int64_t last_mouse_left_click = 0;
            if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                extra.ToggleFullScreen();
                force_refresh = 1;
                last_mouse_left_click = 0;
            } else {
                last_mouse_left_click = av_gettime_relative();
            }
        }
    }
    void DoMouseMotton(SDL_Event const& event) {
        if (extra.cursor_hidden) {
            SDL_ShowCursor(1);
            extra.cursor_hidden = 0;
        }
        extra.cursor_last_shown = av_gettime_relative();
        double x;
        if (event.type == SDL_MOUSEBUTTONDOWN) {
            if (event.button.button != SDL_BUTTON_RIGHT)
                return;
            x = event.button.x;
        } else {
            if (!(event.motion.state & SDL_BUTTON_RMASK))
                return;
            x = event.motion.x;
        }
        if (extra.seek_by_bytes || ic->duration <= 0) {
            uint64_t size = avio_size(ic->pb);
            StreamSeek(size * x / width, 0, 1);
        } else {
            double frac;
            int64_t ts;
            int ns, hh, mm, ss;
            int tns, thh, tmm, tss;
            tns = ic->duration / 1000000LL;
            thh = tns / 3600;
            tmm = (tns % 3600) / 60;
            tss = (tns % 60);
            frac = x / width;
            ns = frac * tns;
            hh = ns / 3600;
            mm = (ns % 3600) / 60;
            ss = (ns % 60);
            av_log(NULL, AV_LOG_INFO,
                   "Seek to %2.0f%% (%2d:%02d:%02d) of total duration "
                   "(%2d:%02d:%02d)       \n",
                   frac * 100, hh, mm, ss, thh, tmm, tss);
            ts = frac * ic->duration;
            if (ic->start_time != AV_NOPTS_VALUE)
                ts += ic->start_time;
            StreamSeek(ts, 0, 0);
        }
    }
    void DoWindowEvent(SDL_Event const& event) {
        switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                extra.screen_width = width = event.window.data1;
                extra.screen_height = height = event.window.data2;
                if (vis_texture) {
                    SDL_DestroyTexture(vis_texture);
                    vis_texture = nullptr;
                }
            case SDL_WINDOWEVENT_EXPOSED:
                force_refresh = 1;
        }
    }
};
