//
// Created by Johnny on 2020/12/6.
//

#include "VideoState.h"
#include "common.h"

#include "cmdutils.h"

#include <SDL.h>
#include <SDL_thread.h>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/bprint.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN },
};

int realloc_texture(SDL_Texture **texture,
                    Uint32 new_format,
                    int new_width,
                    int new_height,
                    SDL_BlendMode blendmode,
                    int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 ||
        new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(VideoState::renderer, new_format,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n",
               new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void get_sdl_pix_fmt_and_blendmode(int format,
                                          Uint32 *sdl_pix_fmt,
                                          SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32 || format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32 || format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int stream_has_enough_packets(AVStream *st,
                                     int stream_id,
                                     PacketQueue *queue)
{
    return stream_id < 0 || queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES &&
               (!queue->duration ||
                av_q2d(st->time_base) * queue->duration > 1.0);
}

int VideoState::upload_texture(SDL_Texture **tex,
                               AVFrame *frame,
                               struct SwsContext **img_convert_ctx)
{
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex,
                        sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN
                            ? SDL_PIXELFORMAT_ARGB8888
                            : sdl_pix_fmt,
                        frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_UNKNOWN:
        /* This should only happen if we are not using avfilter... */
        *img_convert_ctx = sws_getCachedContext(
            *img_convert_ctx, frame->width, frame->height,
            (AVPixelFormat)frame->format, frame->width, frame->height,
            AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
        if (*img_convert_ctx != NULL) {
            uint8_t *pixels[4];
            int pitch[4];
            if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                sws_scale(*img_convert_ctx, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, frame->height, pixels, pitch);
                SDL_UnlockTexture(*tex);
            }
        } else {
            av_log(NULL, AV_LOG_FATAL,
                   "Cannot initialize the conversion context\n");
            ret = -1;
        }
        break;
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 &&
            frame->linesize[2] > 0) {
            ret = SDL_UpdateYUVTexture(
                *tex, NULL, frame->data[0], frame->linesize[0], frame->data[1],
                frame->linesize[1], frame->data[2], frame->linesize[2]);
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 &&
                   frame->linesize[2] < 0) {
            ret = SDL_UpdateYUVTexture(
                *tex, NULL,
                frame->data[0] + frame->linesize[0] * (frame->height - 1),
                -frame->linesize[0],
                frame->data[1] +
                    frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                -frame->linesize[1],
                frame->data[2] +
                    frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                -frame->linesize[2]);
        } else {
            av_log(
                NULL, AV_LOG_ERROR,
                "Mixed negative and positive linesizes are not supported.\n");
            return -1;
        }
        break;
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(*tex, NULL,
                                    frame->data[0] + frame->linesize[0] *
                                                         (frame->height - 1),
                                    -frame->linesize[0]);
        } else {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0],
                                    frame->linesize[0]);
        }
        break;
    }
    return ret;
}

void VideoState::VideoImageDisplay()
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    vp = pictq.PeekLast();
    if (subtitle_st) {
        if (subpq.NbRemaining() > 0) {
            sp = subpq.Peek();

            if (vp->pts >=
                sp->pts + ((float)sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t *pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(&sub_texture, SDL_PIXELFORMAT_ARGB8888,
                                        sp->width, sp->height,
                                        SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w =
                            av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h =
                            av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        sub_convert_ctx = sws_getCachedContext(
                            sub_convert_ctx, sub_rect->w, sub_rect->h,
                            AV_PIX_FMT_PAL8, sub_rect->w, sub_rect->h,
                            AV_PIX_FMT_BGRA, 0, NULL, NULL, NULL);
                        if (!sub_convert_ctx) {
                            av_log(
                                NULL, AV_LOG_FATAL,
                                "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(sub_texture, (SDL_Rect *)sub_rect,
                                             (void **)pixels, pitch)) {
                            sws_scale(sub_convert_ctx,
                                      (const uint8_t *const *)sub_rect->data,
                                      sub_rect->linesize, 0, sub_rect->h,
                                      pixels, pitch);
                            SDL_UnlockTexture(sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }

    calculate_display_rect(&rect, xleft, ytop, width, height, vp->width,
                           vp->height, vp->sar);

    if (!vp->uploaded) {
        if (upload_texture(&vid_texture, vp->frame, &img_convert_ctx) < 0)
            return;
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    set_sdl_yuv_conversion_mode(vp->frame);
    SDL_RenderCopyEx(renderer, vid_texture, NULL, &rect, 0, NULL,
                     vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    set_sdl_yuv_conversion_mode(NULL);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer, sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect *)sp->sub.rects[i];
            SDL_Rect target = { .x = int(rect.x + sub_rect->x * xratio),
                                .y = int(rect.y + sub_rect->y * yratio),
                                .w = int(sub_rect->w * xratio),
                                .h = int(sub_rect->h * yratio) };
            SDL_RenderCopy(renderer, sub_texture, sub_rect, &target);
        }
#endif
    }
}
void VideoState::fill_rectangle(int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}
void VideoState::VideoAudioDisplay()
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * this->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = audio_tgt.channels;
    nb_display_channels = channels;
    if (!paused) {
        int data_used = show_mode == VideoState::SHOW_MODE_WAVES
                            ? this->width
                            : (2 * nb_freq);
        n = 2 * channels;
        delay = audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
           the last buffer computation */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * this->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start = x = compute_mod(this->sample_array_index - delay * channels,
                                  SAMPLE_ARRAY_SIZE);
        if (this->show_mode == VideoState::SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = this->sample_array[idx];
                int b = this->sample_array[(idx + 4 * channels) %
                                           SAMPLE_ARRAY_SIZE];
                int c = this->sample_array[(idx + 5 * channels) %
                                           SAMPLE_ARRAY_SIZE];
                int d = this->sample_array[(idx + 9 * channels) %
                                           SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        this->last_i_start = i_start;
    } else {
        i_start = this->last_i_start;
    }

    if (this->show_mode == VideoState::SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        /* total height for one channel */
        h = this->height / nb_display_channels;
        /* graph height / 2 */
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = this->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < this->width; x++) {
                y = (this->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(this->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = this->ytop + ch * h;
            fill_rectangle(this->xleft, y, this->width, 1);
        }
    } else {
        if (realloc_texture(&this->vis_texture, SDL_PIXELFORMAT_ARGB8888,
                            this->width, this->height, SDL_BLENDMODE_NONE,
                            1) < 0)
            return;

        nb_display_channels = FFMIN(nb_display_channels, 2);
        if (rdft_bits != this->rdft_bits) {
            av_rdft_end(this->rdft);
            av_free(this->rdft_data);
            this->rdft = av_rdft_init(rdft_bits, DFT_R2C);
            this->rdft_bits = rdft_bits;
            this->rdft_data = (FFTSample *)av_malloc_array(
                nb_freq, 4 * sizeof(*this->rdft_data));
        }
        if (!this->rdft || !this->rdft_data) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to allocate buffers for RDFT, switching to waves "
                   "display\n");
            this->show_mode = VideoState::SHOW_MODE_WAVES;
        } else {
            FFTSample *data[2];
            SDL_Rect rect = {
                .x = this->xpos, .y = 0, .w = 1, .h = this->height
            };
            uint32_t *pixels;
            int pitch;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data[ch] = this->rdft_data + 2 * nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x - nb_freq) * (1.0 / nb_freq);
                    data[ch][x] = this->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                av_rdft_calc(this->rdft, data[ch]);
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. */
            if (!SDL_LockTexture(this->vis_texture, &rect, (void **)&pixels,
                                 &pitch)) {
                pitch >>= 2;
                pixels += pitch * this->height;
                for (y = 0; y < this->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a =
                        sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] +
                                      data[0][2 * y + 1] * data[0][2 * y + 1]));
                    int b = (nb_display_channels == 2)
                                ? sqrt(w * hypot(data[1][2 * y + 0],
                                                 data[1][2 * y + 1]))
                                : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
                }
                SDL_UnlockTexture(this->vis_texture);
            }
            SDL_RenderCopy(renderer, this->vis_texture, NULL, NULL);
        }
        if (!this->paused)
            this->xpos++;
        if (this->xpos >= this->width)
            this->xpos = this->xleft;
    }
}
void VideoState::do_exit()
{
    do_exit(this);
}

VideoState *VideoState::StreamOpen(const char *filename,
                                   AVInputFormat *iformat,
                                   int av_sync_type)
{
    auto is = (VideoState *)av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = iformat;
    is->ytop = 0;
    is->xleft = 0;

    /* start video display */
    if (is->pictq.Init(&is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (is->subpq.Init(&is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (is->sampq.Init(&is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (is->videoq.Init() < 0 || is->audioq.Init() < 0 ||
        is->subtitleq.Init() < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    is->vidclk.init_clock(&is->videoq.serial);
    is->audclk.init_clock(&is->audioq.serial);
    is->extclk.init_clock(&is->extclk.serial);
    is->audio_clock_serial = -1;
    if (VideoState::startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n",
               VideoState::startup_volume);
    if (VideoState::startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n",
               VideoState::startup_volume);
    VideoState::startup_volume = av_clip(VideoState::startup_volume, 0, 100);
    VideoState::startup_volume =
        av_clip(SDL_MIX_MAXVOLUME * VideoState::startup_volume / 100, 0,
                SDL_MIX_MAXVOLUME);
    is->audio_volume = VideoState::startup_volume;
    is->muted = 0;
    is->av_sync_type = av_sync_type;
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    fail:
        VideoState::StreamClose(is);
        return NULL;
    }
    return is;
}

void VideoState::StreamClose(VideoState *&that)
{
    if (that) {
        that->StreamClose();
        av_free(that);
        that = nullptr;
    }
}
void VideoState::StreamClose()
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    abort_request = 1;
    SDL_WaitThread(read_tid, NULL);

    /* close each stream */
    if (audio_stream >= 0)
        StreamComponentClose(audio_stream);
    if (video_stream >= 0)
        StreamComponentClose(video_stream);
    if (subtitle_stream >= 0)
        StreamComponentClose(subtitle_stream);

    avformat_close_input(&ic);

    videoq.Destroy();
    audioq.Destroy();
    subtitleq.Destroy();

    /* free all pictures */
    pictq.Destory();
    sampq.Destory();
    subpq.Destory();
    SDL_DestroyCond(continue_read_thread);
    sws_freeContext(img_convert_ctx);
    sws_freeContext(sub_convert_ctx);
    av_free(filename);
    if (vis_texture)
        SDL_DestroyTexture(vis_texture);
    if (vid_texture)
        SDL_DestroyTexture(vid_texture);
    if (sub_texture)
        SDL_DestroyTexture(sub_texture);
    // av_free(this); // todo free
}
void VideoState::video_display()
{
    if (!width)
        video_open();

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (audio_st && show_mode != VideoState::SHOW_MODE_VIDEO)
        VideoAudioDisplay();
    else if (video_st)
        VideoImageDisplay();
    SDL_RenderPresent(renderer);
}
void VideoState::do_exit(VideoState *that)
{
    StreamClose(that);

    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();
    av_freep(&vfilters_list);
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}
void VideoState::StreamComponentClose(int stream_index)
{
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        auddec.decoder_abort(&sampq);
        SDL_CloseAudioDevice(audio_dev);
        auddec.decoder_destroy();
        swr_free(&swr_ctx);
        av_freep(&audio_buf1);
        audio_buf1_size = 0;
        audio_buf = NULL;

        if (this->rdft) {
            av_rdft_end(this->rdft);
            av_freep(&this->rdft_data);
            this->rdft = NULL;
            this->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        this->viddec.decoder_abort(&this->pictq);
        this->viddec.decoder_destroy();
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        this->subdec.decoder_abort(&this->subpq);
        this->subdec.decoder_destroy();
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        this->audio_st = NULL;
        this->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        this->video_st = NULL;
        this->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        this->subtitle_st = NULL;
        this->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

int VideoState::audio_open(int64_t wanted_channel_layout,
                           int wanted_nb_channels,
                           int wanted_sample_rate)
{
    struct AudioParams *audio_hw_params = &audio_tgt;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
    static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout =
            av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout ||
        wanted_nb_channels !=
            av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout =
            av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels =
        av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx &&
           next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples =
        FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
              2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = this;
    while (!(audio_dev =
                 SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec,
                                     SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                         SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout =
            av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n",
                   spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(
        NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(
        NULL, audio_hw_params->channels, audio_hw_params->freq,
        audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 ||
        audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}

int VideoState::StreamComponentOpen(int stream_index)
{
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx,
                                        ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        this->last_audio_stream = stream_index;
        forced_codec_name = audio_codec_name;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        this->last_subtitle_stream = stream_index;
        forced_codec_name = subtitle_codec_name;
        break;
    case AVMEDIA_TYPE_VIDEO:
        this->last_video_stream = stream_index;
        forced_codec_name = video_codec_name;
        break;
    default:
        assert(false);
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            av_log(NULL, AV_LOG_WARNING,
                   "No codec could be found with name '%s'\n",
                   forced_codec_name);
        else
            av_log(NULL, AV_LOG_WARNING,
                   "No decoder could be found for codec %s\n",
                   avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING,
               "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic,
                             ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
        avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    this->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
    {
        AVFilterContext *sink;

        this->audio_filter_src.freq = avctx->sample_rate;
        this->audio_filter_src.channels = avctx->channels;
        this->audio_filter_src.channel_layout =
            get_valid_channel_layout(avctx->channel_layout, avctx->channels);
        this->audio_filter_src.fmt = avctx->sample_fmt;
        if ((ret = configure_audio_filters(afilters, 0)) < 0)
            goto fail;
        sink = this->out_audio_filter;
        sample_rate = av_buffersink_get_sample_rate(sink);
        nb_channels = av_buffersink_get_channels(sink);
        channel_layout = av_buffersink_get_channel_layout(sink);
    }
#else
        sample_rate = avctx->sample_rate;
        nb_channels = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output */
        if ((ret = audio_open(channel_layout, nb_channels, sample_rate)) < 0)
            goto fail;
        this->audio_hw_buf_size = ret;
        this->audio_src = this->audio_tgt;
        this->audio_buf_size = 0;
        this->audio_buf_index = 0;

        /* init averaging filter */
        this->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        this->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        this->audio_diff_threshold =
            (double)(this->audio_hw_buf_size) / this->audio_tgt.bytes_per_sec;

        this->audio_stream = stream_index;
        this->audio_st = ic->streams[stream_index];

        this->auddec.Init(avctx, &audioq, continue_read_thread);
        if ((this->ic->iformat->flags &
             (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) &&
            !this->ic->iformat->read_seek) {
            this->auddec.start_pts = this->audio_st->start_time;
            this->auddec.start_pts_tb = this->audio_st->time_base;
        }
        if ((ret = this->auddec.Start(flush_pkt, audio_thread, "audio_decoder",
                                      this)) < 0)
            goto out;
        SDL_PauseAudioDevice(audio_dev, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        this->video_stream = stream_index;
        this->video_st = ic->streams[stream_index];

        this->viddec.Init(avctx, &videoq, continue_read_thread);
        if ((ret = this->viddec.Start(flush_pkt, video_thread, "video_decoder",
                                      this)) < 0)
            goto out;
        this->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        this->subtitle_stream = stream_index;
        this->subtitle_st = ic->streams[stream_index];

        this->subdec.Init(avctx, &subtitleq, continue_read_thread);
        if ((ret = this->subdec.Start(flush_pkt, subtitle_thread,
                                      "subtitle_decoder", this)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}
int VideoState::video_open()
{
    int w, h;

    w = screen_width ? screen_width : default_width;
    h = screen_height ? screen_height : default_height;

    if (!window_title)
        window_title = input_filename;
    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);

    this->width = w;
    this->height = h;

    return 0;
}
void VideoState::stream_cycle_channel(int codec_type)
{
    AVFormatContext *ic = this->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = this->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = this->last_video_stream;
        old_index = this->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = this->last_audio_stream;
        old_index = this->audio_stream;
    } else {
        start_index = this->last_subtitle_stream;
        old_index = this->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && this->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, this->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                this->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st =
            this->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string((AVMediaType)codec_type), old_index,
           stream_index);

    this->StreamComponentClose(old_index);
    this->StreamComponentOpen(stream_index);
}
void VideoState::toggle_full_screen()
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window,
                            is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
void VideoState::toggle_audio_display()
{
    int next = this->show_mode;
    do {
        next = (next + 1) % VideoState::SHOW_MODE_NB;
    } while (next != this->show_mode &&
             (next == VideoState::SHOW_MODE_VIDEO && !this->video_st ||
              next != VideoState::SHOW_MODE_VIDEO && !this->audio_st));
    if (this->show_mode != next) {
        this->force_refresh = 1;
        this->show_mode = (VideoState::ShowMode)next;
    }
}
void VideoState::refresh_loop_wait_event(SDL_Event *event)
{
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT,
                           SDL_LASTEVENT)) {
        if (!cursor_hidden &&
            av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (this->show_mode != VideoState::SHOW_MODE_NONE &&
            (!this->paused || this->force_refresh))
            video_refresh(this, &remaining_time);
        SDL_PumpEvents();
    }
}
void VideoState::seek_chapter(int incr)
{
    int64_t pos = get_master_clock() * AV_TIME_BASE;
    int i;

    if (!this->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < this->ic->nb_chapters; i++) {
        AVChapter *ch = this->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= this->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    StreamSeek(av_rescale_q(this->ic->chapters[i]->start,
                            this->ic->chapters[i]->time_base, AV_TIME_BASE_Q),
               0, 0);
}
void VideoState::EventLoop()
{
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        refresh_loop_wait_event(&event);
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE ||
                event.key.keysym.sym == SDLK_q) {
                // todo av_free(that);
                StreamClose();
                do_exit(nullptr);
                break;
            }
            // If we don't yet have a window, skip all key events, because
            // read_thread might still be initializing...
            if (!width)
                continue;
            switch (event.key.keysym.sym) {
            case SDLK_f:
                toggle_full_screen();
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
                update_volume(1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(-1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // S: Step to next frame
                step_to_next_frame();
                break;
            case SDLK_a:
                stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
                if (show_mode == VideoState::SHOW_MODE_VIDEO &&
                    vfilter_idx < nb_vfilters - 1) {
                    if (++vfilter_idx >= nb_vfilters)
                        vfilter_idx = 0;
                } else {
                    vfilter_idx = 0;
                    toggle_audio_display();
                }
                break;
            case SDLK_PAGEUP:
                if (ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(1);
                break;
            case SDLK_PAGEDOWN:
                if (this->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(-1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                if (seek_by_bytes) {
                    pos = -1;
                    if (pos < 0 && this->video_stream >= 0)
                        pos = this->pictq.LastPosition();
                    if (pos < 0 && this->audio_stream >= 0)
                        pos = this->sampq.LastPosition();
                    if (pos < 0)
                        pos = avio_tell(this->ic->pb);
                    if (this->ic->bit_rate)
                        incr *= this->ic->bit_rate / 8.0;
                    else
                        incr *= 180000.0;
                    pos += incr;
                    StreamSeek(pos, incr, 1);
                } else {
                    pos = get_master_clock();
                    if (isnan(pos))
                        pos = (double)this->seek_pos / AV_TIME_BASE;
                    pos += incr;
                    if (this->ic->start_time != AV_NOPTS_VALUE &&
                        pos < this->ic->start_time / (double)AV_TIME_BASE)
                        pos = this->ic->start_time / (double)AV_TIME_BASE;
                    StreamSeek((int64_t)(pos * AV_TIME_BASE),
                               (int64_t)(incr * AV_TIME_BASE), 0);
                }
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (exit_on_mousedown) {
                // todo av_free(that);
                StreamClose();
                do_exit(nullptr);
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    toggle_full_screen();
                    this->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
        case SDL_MOUSEMOTION:
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT)
                    break;
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK))
                    break;
                x = event.motion.x;
            }
            if (seek_by_bytes || this->ic->duration <= 0) {
                uint64_t size = avio_size(this->ic->pb);
                StreamSeek(size * x / this->width, 0, 1);
            } else {
                int64_t ts;
                int ns, hh, mm, ss;
                int tns, thh, tmm, tss;
                tns = this->ic->duration / 1000000LL;
                thh = tns / 3600;
                tmm = (tns % 3600) / 60;
                tss = (tns % 60);
                frac = x / this->width;
                ns = frac * tns;
                hh = ns / 3600;
                mm = (ns % 3600) / 60;
                ss = (ns % 60);
                av_log(NULL, AV_LOG_INFO,
                       "Seek to %2.0f%% (%2d:%02d:%02d) of total duration "
                       "(%2d:%02d:%02d)       \n",
                       frac * 100, hh, mm, ss, thh, tmm, tss);
                ts = frac * this->ic->duration;
                if (this->ic->start_time != AV_NOPTS_VALUE)
                    ts += this->ic->start_time;
                StreamSeek(ts, 0, 0);
            }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                screen_width = width = event.window.data1;
                screen_height = height = event.window.data2;
                if (vis_texture) {
                    SDL_DestroyTexture(vis_texture);
                    vis_texture = NULL;
                }
            case SDL_WINDOWEVENT_EXPOSED:
                force_refresh = 1;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            // todo av_free(that);
            StreamClose();
            do_exit(nullptr);
            break;
        default:
            break;
        }
    }
}
int VideoState::get_master_sync_type()
{
    if (this->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (this->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (this->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (this->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}
double VideoState::get_master_clock()
{
    double val;

    switch (get_master_sync_type()) {
    case AV_SYNC_VIDEO_MASTER:
        val = this->vidclk.get_clock();
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = this->audclk.get_clock();
        break;
    default:
        val = this->extclk.get_clock();
        break;
    }
    return val;
}
void VideoState::check_external_clock_speed()
{
    if (this->video_stream >= 0 &&
            this->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        this->audio_stream >= 0 &&
            this->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        this->extclk.set_clock_speed(
            FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                  this->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((this->video_stream < 0 ||
                this->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (this->audio_stream < 0 ||
                this->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        this->extclk.set_clock_speed(
            FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                  this->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = this->extclk.speed;
        if (speed != 1.0)
            this->extclk.set_clock_speed(speed + EXTERNAL_CLOCK_SPEED_STEP *
                                                     (1.0 - speed) /
                                                     fabs(1.0 - speed));
    }
}
void VideoState::StreamSeek(int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!this->seek_req) {
        this->seek_pos = pos;
        this->seek_rel = rel;
        this->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            this->seek_flags |= AVSEEK_FLAG_BYTE;
        this->seek_req = 1;
        SDL_CondSignal(this->continue_read_thread);
    }
}
void VideoState::StreamTogglePause()
{
    if (this->paused) {
        this->frame_timer +=
            av_gettime_relative() / 1000000.0 - this->vidclk.last_updated;
        if (this->read_pause_return != AVERROR(ENOSYS)) {
            this->vidclk.paused = 0;
        }
        this->vidclk.set_clock(this->vidclk.get_clock(), this->vidclk.serial);
    }
    this->extclk.set_clock(this->extclk.get_clock(), this->extclk.serial);
    this->paused = this->audclk.paused = this->vidclk.paused =
        this->extclk.paused = !this->paused;
}
void VideoState::TogglePause()
{
    StreamTogglePause();
    this->step = 0;
}
void VideoState::ToggleMute()
{
    this->muted = !this->muted;
}
void VideoState::update_volume(int sign, double step)
{
    double volume_level =
        this->audio_volume
            ? (20 * log(this->audio_volume / (double)SDL_MIX_MAXVOLUME) /
               log(10))
            : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME *
                           pow(10.0, (volume_level + sign * step) / 20.0));
    this->audio_volume =
        av_clip(this->audio_volume == new_volume ? (this->audio_volume + sign)
                                                 : new_volume,
                0, SDL_MIX_MAXVOLUME);
}
void VideoState::step_to_next_frame()
{
    /* if the stream is paused unpause it, then step */
    if (this->paused)
        StreamTogglePause();
    this->step = 1;
}
double VideoState::compute_target_delay(double delay)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type() != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = this->vidclk.get_clock() - get_master_clock();

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold =
            FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < this->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold &&
                     delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}
double VideoState::vp_duration(Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 ||
            duration > this->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}
void VideoState::update_video_pts(double pts, int64_t pos, int serial)
{
    /* update current video pts */
    this->vidclk.set_clock(pts, serial);
    this->extclk.sync_clock_to_slave(&this->vidclk);
}
void VideoState::video_refresh(void *opaque, double *remaining_time)
{
    auto is = (VideoState *)opaque;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && is->get_master_sync_type() == AV_SYNC_EXTERNAL_CLOCK &&
        is->realtime)
        is->check_external_clock_speed();

    if (!display_disable && is->show_mode != VideoState::SHOW_MODE_VIDEO &&
        is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            is->video_display();
            is->last_vis_time = time;
        }
        *remaining_time =
            FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st) {
    retry:
        if (is->pictq.NbRemaining() == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = is->pictq.PeekLast();
            vp = is->pictq.Peek();

            if (vp->serial != is->videoq.serial) {
                is->pictq.Next();
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = is->vp_duration(lastvp, vp);
            delay = is->compute_target_delay(last_duration);

            time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time =
                    FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                is->update_video_pts(vp->pts, vp->pos, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);

            if (is->pictq.NbRemaining() > 1) {
                Frame *nextvp = is->pictq.PeekNext();
                duration = is->vp_duration(vp, nextvp);
                if (!is->step &&
                    (framedrop > 0 ||
                     (framedrop &&
                      is->get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) &&
                    time > is->frame_timer + duration) {
                    is->frame_drops_late++;
                    is->pictq.Next();
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (is->subpq.NbRemaining() > 0) {
                    sp = is->subpq.Peek();

                    if (is->subpq.NbRemaining() > 1)
                        sp2 = is->subpq.PeekNext();
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial ||
                        (is->vidclk.pts >
                         (sp->pts +
                          ((float)sp->sub.end_display_time / 1000))) ||
                        (sp2 &&
                         is->vidclk.pts >
                             (sp2->pts +
                              ((float)sp2->sub.start_display_time / 1000)))) {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (!SDL_LockTexture(
                                        is->sub_texture, (SDL_Rect *)sub_rect,
                                        (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h;
                                         j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
                        is->subpq.Next();
                    } else {
                        break;
                    }
                }
            }

            is->pictq.Next();
            is->force_refresh = 1;

            if (is->step && !is->paused)
                is->StreamTogglePause();
        }
    display:
        /* display picture */
        if (!display_disable && is->force_refresh &&
            is->show_mode == VideoState::SHOW_MODE_VIDEO &&
            is->pictq.rindex_shown)
            is->video_display();
    }
    is->force_refresh = 0;
    if (show_status) {
        AVBPrint buf;
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = is->audclk.get_clock() - is->vidclk.get_clock();
            else if (is->video_st)
                av_diff = is->get_master_clock() - is->vidclk.get_clock();
            else if (is->audio_st)
                av_diff = is->get_master_clock() - is->audclk.get_clock();

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(
                &buf,
                "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%" PRId64
                "/%" PRId64 "   \r",
                is->get_master_clock(),
                (is->audio_st && is->video_st)
                    ? "A-V"
                    : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                av_diff, is->frame_drops_early + is->frame_drops_late,
                aqsize / 1024, vqsize / 1024, sqsize,
                is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts
                             : 0,
                is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts
                             : 0);

            if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
                fprintf(stderr, "%s", buf.str);
            else
                av_log(NULL, AV_LOG_INFO, "%s", buf.str);

            fflush(stderr);
            av_bprint_finalize(&buf, NULL);

            last_time = cur_time;
        }
    }
}
int VideoState::queue_picture(AVFrame *src_frame,
                              double pts,
                              double duration,
                              int64_t pos,
                              int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = this->pictq.PeekWritable()))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    this->pictq.Push();
    return 0;
}
int VideoState::get_video_frame(AVFrame *frame)
{
    int got_picture;

    if ((got_picture = this->viddec.decoder_decode_frame(frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(this->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio =
            av_guess_sample_aspect_ratio(this->ic, this->video_st, frame);

        if (framedrop > 0 ||
            (framedrop && get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock();
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - this->frame_last_filter_delay < 0 &&
                    this->viddec.pkt_serial == this->vidclk.serial &&
                    this->videoq.nb_packets) {
                    this->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

// extern int autorotate;
// extern int find_stream_info;
// extern int filter_nbthreads;

int VideoState::ConfigureVideoFilters(AVFilterGraph *graph,
                                      const char *vfilters,
                                      AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = this->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(this->ic, this->video_st, NULL);
    AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;

    for (i = 0; i < renderer_info.num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
            if (renderer_info.texture_formats[i] ==
                sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags",
                        e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key,
                        e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             this->video_st->time_base.num, this->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num,
             FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d",
                    fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(
             &filt_src, avfilter_get_by_name("buffer"), "ffplay_buffer",
             buffersrc_args, NULL, graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,
                                   AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) <
        0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg)                                                 \
    do {                                                                       \
        AVFilterContext *filt_ctx;                                             \
                                                                               \
        ret = avfilter_graph_create_filter(&filt_ctx,                          \
                                           avfilter_get_by_name(name),         \
                                           "ffplay_" name, arg, NULL, graph);  \
        if (ret < 0)                                                           \
            goto fail;                                                         \
                                                                               \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                      \
        if (ret < 0)                                                           \
            goto fail;                                                         \
                                                                               \
        last_filter = filt_ctx;                                                \
    } while (0)

    if (autorotate) {
        double theta = get_rotation(this->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);
            INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) <
        0)
        goto fail;

    this->in_video_filter = filt_src;
    this->out_video_filter = filt_out;

fail:
    return ret;
}
int VideoState::configure_audio_filters(const char *afilters,
                                        int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16,
                                                       AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&this->agraph);
    if (!(this->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    this->agraph->nb_threads = filter_nbthreads;

    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts),
                    "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
    av_opt_set(this->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   this->audio_filter_src.freq,
                   av_get_sample_fmt_name(this->audio_filter_src.fmt),
                   this->audio_filter_src.channels, 1,
                   this->audio_filter_src.freq);
    if (this->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%" PRIx64,
                 this->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(
        &filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer",
        asrc_args, NULL, this->agraph);
    if (ret < 0)
        goto end;

    ret = avfilter_graph_create_filter(
        &filt_asink, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
        NULL, NULL, this->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,
                                   AV_SAMPLE_FMT_NONE,
                                   AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1,
                              AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = this->audio_tgt.channel_layout;
        channels[0] =
            this->audio_tgt.channel_layout ? -1 : this->audio_tgt.channels;
        sample_rates[0] = this->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0,
                                  AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts",
                                       channel_layouts, -1,
                                       AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels,
                                       -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates,
                                       -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    if ((ret = configure_filtergraph(this->agraph, afilters, filt_asrc,
                                     filt_asink)) < 0)
        goto end;

    this->in_audio_filter = filt_asrc;
    this->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&this->agraph);
    return ret;
}

int VideoState::audio_decode_frame()
{
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (this->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) >
                1000000LL * is->audio_hw_buf_size /
                    is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep(1000);
        }
#endif
        if (!(af = this->sampq.PeekReadable()))
            return -1;
        this->sampq.Next();
    } while (af->serial != this->audioq.serial);

    data_size = av_samples_get_buffer_size(
        NULL, af->frame->channels, af->frame->nb_samples,
        (AVSampleFormat)af->frame->format, 1);

    dec_channel_layout =
        (af->frame->channel_layout &&
         af->frame->channels ==
             av_get_channel_layout_nb_channels(af->frame->channel_layout))
            ? af->frame->channel_layout
            : av_get_default_channel_layout(af->frame->channels);
    wanted_nb_samples = synchronize_audio(af->frame->nb_samples);

    if (af->frame->format != this->audio_src.fmt ||
        dec_channel_layout != this->audio_src.channel_layout ||
        af->frame->sample_rate != this->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !this->swr_ctx)) {
        swr_free(&this->swr_ctx);
        this->swr_ctx = swr_alloc_set_opts(
            NULL, this->audio_tgt.channel_layout, this->audio_tgt.fmt,
            this->audio_tgt.freq, dec_channel_layout,
            (AVSampleFormat)af->frame->format, af->frame->sample_rate, 0, NULL);
        if (!this->swr_ctx || swr_init(this->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d "
                   "Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate,
                   av_get_sample_fmt_name((AVSampleFormat)af->frame->format),
                   af->frame->channels, this->audio_tgt.freq,
                   av_get_sample_fmt_name(this->audio_tgt.fmt),
                   this->audio_tgt.channels);
            swr_free(&this->swr_ctx);
            return -1;
        }
        this->audio_src.channel_layout = dec_channel_layout;
        this->audio_src.channels = af->frame->channels;
        this->audio_src.freq = af->frame->sample_rate;
        this->audio_src.fmt = (AVSampleFormat)af->frame->format;
    }

    if (this->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &this->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * this->audio_tgt.freq /
                            af->frame->sample_rate +
                        256;
        int out_size = av_samples_get_buffer_size(
            NULL, this->audio_tgt.channels, out_count, this->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(
                    this->swr_ctx,
                    (wanted_nb_samples - af->frame->nb_samples) *
                        this->audio_tgt.freq / af->frame->sample_rate,
                    wanted_nb_samples * this->audio_tgt.freq /
                        af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&this->audio_buf1, &this->audio_buf1_size, out_size);
        if (!this->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(this->swr_ctx, out, out_count, in,
                           af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING,
                   "audio buffer is probably too small\n");
            if (swr_init(this->swr_ctx) < 0)
                swr_free(&this->swr_ctx);
        }
        this->audio_buf = this->audio_buf1;
        resampled_data_size = len2 * this->audio_tgt.channels *
                              av_get_bytes_per_sample(this->audio_tgt.fmt);
    } else {
        this->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = this->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        this->audio_clock =
            af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    else
        this->audio_clock = NAN;
    this->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               this->audio_clock - last_clock, this->audio_clock, audio_clock0);
        last_clock = this->audio_clock;
    }
#endif
    return resampled_data_size;
}
void VideoState::update_sample_display(short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - this->sample_array_index;
        if (len > size)
            len = size;
        memcpy(this->sample_array + this->sample_array_index, samples,
               len * sizeof(short));
        samples += len;
        this->sample_array_index += len;
        if (this->sample_array_index >= SAMPLE_ARRAY_SIZE)
            this->sample_array_index = 0;
        size -= len;
    }
}
int VideoState::synchronize_audio(int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock
     */
    if (this->get_master_sync_type() != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = this->audclk.get_clock() - this->get_master_clock();

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            this->audio_diff_cum =
                diff + this->audio_diff_avg_coef * this->audio_diff_cum;
            if (this->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                this->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff =
                    this->audio_diff_cum * (1.0 - this->audio_diff_avg_coef);

                if (fabs(avg_diff) >= this->audio_diff_threshold) {
                    wanted_nb_samples =
                        nb_samples + (int)(diff * this->audio_src.freq);
                    min_nb_samples =
                        ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) /
                          100));
                    max_nb_samples =
                        ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) /
                          100));
                    wanted_nb_samples = av_clip(wanted_nb_samples,
                                                min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE,
                       "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff,
                       avg_diff, wanted_nb_samples - nb_samples,
                       this->audio_clock, this->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            this->audio_diff_avg_count = 0;
            this->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

void VideoState::sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    auto is = (VideoState *)opaque;
    int audio_size, len1;

    is->audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = is->audio_decode_frame();
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE /
                                     is->audio_tgt.frame_size *
                                     is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != VideoState::SHOW_MODE_VIDEO)
                    is->update_sample_display((int16_t *)is->audio_buf,
                                              audio_size);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf &&
            is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index,
                   len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(
                    stream, (uint8_t *)is->audio_buf + is->audio_buf_index,
                    AUDIO_S16SYS, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        is->audclk.set_clock_at(
            is->audio_clock -
                (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) /
                    is->audio_tgt.bytes_per_sec,
            is->audio_clock_serial, is->audio_callback_time / 1000000.0);
        is->extclk.sync_clock_to_slave(&is->audclk);
    }
}
int VideoState::audio_thread(void *arg)
{
    auto is = (VideoState *)arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = is->auddec.decoder_decode_frame(frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
            tb = (AVRational){ 1, frame->sample_rate };

            dec_channel_layout = get_valid_channel_layout(frame->channel_layout,
                                                          frame->channels);

            reconfigure =
                cmp_audio_fmts(
                    is->audio_filter_src.fmt, is->audio_filter_src.channels,
                    (AVSampleFormat)frame->format, frame->channels) ||
                is->audio_filter_src.channel_layout != dec_channel_layout ||
                is->audio_filter_src.freq != frame->sample_rate ||
                is->auddec.pkt_serial != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(
                    buf1, sizeof(buf1), -1,
                    is->audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1,
                                             dec_channel_layout);
                av_log(
                    NULL, AV_LOG_DEBUG,
                    "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s "
                    "serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                    is->audio_filter_src.freq, is->audio_filter_src.channels,
                    av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1,
                    last_serial, frame->sample_rate, frame->channels,
                    av_get_sample_fmt_name((AVSampleFormat)frame->format), buf2,
                    is->auddec.pkt_serial);

                is->audio_filter_src.fmt = (AVSampleFormat)frame->format;
                is->audio_filter_src.channels = frame->channels;
                is->audio_filter_src.channel_layout = dec_channel_layout;
                is->audio_filter_src.freq = frame->sample_rate;
                last_serial = is->auddec.pkt_serial;

                if ((ret = is->configure_audio_filters(is->afilters, 1)) < 0)
                    goto the_end;
            }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter,
                                                        frame, 0)) >= 0) {
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = is->sampq.PeekWritable()))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE)
                              ? NAN
                              : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d(
                    (AVRational){ frame->nb_samples, frame->sample_rate });

                av_frame_move_ref(af->frame, frame);
                is->sampq.Push();

                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);
    return ret;
}
int VideoState::video_thread(void *arg)
{
    auto is = (VideoState *)arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = (AVPixelFormat)-2;
    int last_serial = -1;
    int last_vfilter_idx = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = is->get_video_frame(frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (last_w != frame->width || last_h != frame->height ||
            last_format != frame->format ||
            last_serial != is->viddec.pkt_serial ||
            last_vfilter_idx != is->vfilter_idx) {
            av_log(
                NULL, AV_LOG_DEBUG,
                "Video frame changed from size:%dx%d format:%s serial:%d to "
                "size:%dx%d format:%s serial:%d\n",
                last_w, last_h,
                (const char *)av_x_if_null(av_get_pix_fmt_name(last_format),
                                           "none"),
                last_serial, frame->width, frame->height,
                (const char *)av_x_if_null(
                    av_get_pix_fmt_name((AVPixelFormat)frame->format), "none"),
                is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = is->ConfigureVideoFilters(
                     graph,
                     is->vfilters_list ? is->vfilters_list[is->vfilter_idx]
                                       : NULL,
                     frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = (AVPixelFormat)frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 -
                                          is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);

            duration =
                (frame_rate.num && frame_rate.den
                     ? av_q2d((AVRational){ frame_rate.den, frame_rate.num })
                     : 0);
            pts =
                (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = is->queue_picture(frame, pts, duration, frame->pkt_pos,
                                    is->viddec.pkt_serial);
            av_frame_unref(frame);

            if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }

        if (ret < 0)
            goto the_end;
    }
the_end:

    avfilter_graph_free(&graph);

    av_frame_free(&frame);
    return 0;
}
int VideoState::subtitle_thread(void *arg)
{
    auto is = (VideoState *)arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = is->subpq.PeekWritable()))
            return 0;

        if ((got_subtitle = is->subdec.decoder_decode_frame(NULL, &sp->sub)) <
            0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            is->subpq.Push();
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

void VideoState::set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width = screen_width ? screen_width : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height,
                           sar);
    default_width = rect.w;
    default_height = rect.h;
}

int VideoState::read_thread(void *arg)
{
    auto is = (VideoState *)arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;

    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    if (VideoState::find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
        int orig_nb_streams = ic->nb_streams;

        err = avformat_find_stream_info(ic, opts);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use
    // avio_feof() to test for the end

    if (VideoState::seek_by_bytes < 0)
        VideoState::seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                                    strcmp("ogg", ic->iformat->name);

    is->max_frame_duration =
        (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!VideoState::window_title &&
        (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        VideoState::window_title =
            av_asprintf("%s - %s", t->value, VideoState::input_filename);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not seek to position %0.3f\n", is->filename,
                   (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (is->show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && VideoState::wanted_stream_spec[type] &&
            st_index[type] == -1)
            if (avformat_match_stream_specifier(
                    ic, st, VideoState::wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (VideoState::wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR,
                   "Stream specifier %s does not match any %s stream\n",
                   VideoState::wanted_stream_spec[i],
                   av_get_media_type_string((AVMediaType)i));
            st_index[i] = INT_MAX;
        }
    }

    if (!VideoState::video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(
            ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!VideoState::audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(
            ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO],
            st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
    if (!VideoState::video_disable && !VideoState::subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(
            ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
            (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO]
                                               : st_index[AVMEDIA_TYPE_VIDEO]),
            NULL, 0);

    is->show_mode = show_mode_;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            VideoState::set_default_window_size(codecpar->width,
                                                codecpar->height, sar);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        is->StreamComponentOpen(st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = is->StreamComponentOpen(st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == VideoState::SHOW_MODE_NONE)
        is->show_mode =
            ret >= 0 ? VideoState::SHOW_MODE_VIDEO : VideoState::SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        is->StreamComponentOpen(st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (VideoState::infinite_buffer < 0 && is->realtime)
        VideoState::infinite_buffer = 1;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(VideoState::input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min =
                is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max =
                is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct
            // direction in generation
            //      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target,
                                     seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n",
                       is->ic->url);
            } else {
                if (is->audio_stream >= 0) {
                    is->audioq.Flush();
                    is->audioq.Put(&VideoState::flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    is->subtitleq.Flush();
                    is->subtitleq.Put(&VideoState::flush_pkt);
                }
                if (is->video_stream >= 0) {
                    is->videoq.Flush();
                    is->videoq.Put(&VideoState::flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    is->extclk.set_clock(NAN, 0);
                } else {
                    is->extclk.set_clock(seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                is->step_to_next_frame();
        }
        if (is->queue_attachments_req) {
            if (is->video_st &&
                is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) <
                    0)
                    goto fail;
                is->videoq.Put(&copy);
                is->videoq.PutNullPacket(is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (VideoState::infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size >
                 MAX_QUEUE_SIZE ||
             (stream_has_enough_packets(is->audio_st, is->audio_stream,
                                        &is->audioq) &&
              stream_has_enough_packets(is->video_st, is->video_stream,
                                        &is->videoq) &&
              stream_has_enough_packets(is->subtitle_st, is->subtitle_stream,
                                        &is->subtitleq)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial &&
                               is->sampq.NbRemaining() == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial &&
                               is->pictq.NbRemaining() == 0))) {
            if (VideoState::loop != 1 &&
                (!VideoState::loop || --VideoState::loop)) {
                is->StreamSeek(start_time != AV_NOPTS_VALUE ? start_time : 0, 0,
                               0);
            } else if (VideoState::autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    is->videoq.PutNullPacket(is->video_stream);
                if (is->audio_stream >= 0)
                    is->audioq.PutNullPacket(is->audio_stream);
                if (is->subtitle_stream >= 0)
                    is->subtitleq.PutNullPacket(is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                if (VideoState::autoexit)
                    goto fail;
                else
                    break;
            }
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue,
         * otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range =
            duration == AV_NOPTS_VALUE ||
            (pkt_ts -
             (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                        av_q2d(ic->streams[pkt->stream_index]->time_base) -
                    (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) /
                        1000000 <=
                ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            is->audioq.Put(pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
                   !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            is->videoq.Put(pkt);
        } else if (pkt->stream_index == is->subtitle_stream &&
                   pkt_in_play_range) {
            is->subtitleq.Put(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}
