/*
 * copyright (c) 2013 Andrew Kelley
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
 * libavfilter API usage example.
 *
 * @example filter_audio.c
 * This example will generate a sine wave audio,
 * pass it through a simple filter chain, and then compute the MD5 checksum of
 * the output data.
 *
 * The filter chain it uses is:
 * (input) -> abuffer -> volume -> aformat -> abuffersink -> (output)
 *
 * abuffer: This provides the endpoint where you can feed the decoded samples.
 * volume: In this example we hardcode it to 0.90.
 * aformat: This converts the samples to the samplefreq, channel layout,
 *          and sample format required by the audio device.
 * abuffersink: This provides the endpoint where you can read the samples after
 *              they have passed through the filter chain.
 */

// update 2020-11-20 by johzzy

#include <cmath>
#include <cstdio>
#include <cassert>

extern "C" {
#include "libavutil/channel_layout.h"
#include "libavutil/md5.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
}

#define INPUT_SAMPLERATE 48000
#define INPUT_FORMAT AV_SAMPLE_FMT_FLTP
#define INPUT_CHANNEL_LAYOUT AV_CH_LAYOUT_5POINT0

#define VOLUME_VAL 0.90

struct VolumeDetectContext {
    AVFilterGraph *graph{ nullptr };
    AVFilterContext *src{ nullptr };
    AVFilterContext *sink{ nullptr };
    AVFilterContext *volume_detect_ctx{ nullptr };

    bool enable_volumedetect{ true };

    void Free()
    {
        if (graph) {
            avfilter_graph_free(&graph);
            graph = nullptr;
        }
    }
    virtual ~VolumeDetectContext() { Free(); }

    int InitFilterGraph()
    {
        /* Create a new filtergraph, which will contain all the filters. */
        auto filter_graph = avfilter_graph_alloc();
        if (!filter_graph) {
            fprintf(stderr, "Unable to create filter graph.\n");
            return AVERROR(ENOMEM);
        }

        /* Create the abuffer filter;
         * it will be used for feeding the data into the graph. */
        auto abuffer = avfilter_get_by_name("abuffer");
        if (!abuffer) {
            fprintf(stderr, "Could not find the abuffer filter.\n");
            return AVERROR_FILTER_NOT_FOUND;
        }

        auto abuffer_ctx =
            avfilter_graph_alloc_filter(filter_graph, abuffer, "src");
        if (!abuffer_ctx) {
            fprintf(stderr, "Could not allocate the abuffer instance.\n");
            return AVERROR(ENOMEM);
        }

        /* Set the filter options through the AVOptions API. */
        uint8_t ch_layout[64];
        av_get_channel_layout_string((char *)ch_layout, sizeof(ch_layout), 0,
                                     INPUT_CHANNEL_LAYOUT);
        auto ret = av_opt_set(abuffer_ctx, "channel_layout",
                              (const char *)ch_layout, AV_OPT_SEARCH_CHILDREN);
        assert(ret >= 0);
        // ret = av_opt_set_(abuffer_ctx, "channel_layout",
        // INPUT_CHANNEL_LAYOUT,
        //                      AV_OPT_SEARCH_CHILDREN);
        // assert(ret >= 0);
        ret = av_opt_set(abuffer_ctx, "sample_fmt",
                         av_get_sample_fmt_name(INPUT_FORMAT),
                         AV_OPT_SEARCH_CHILDREN);
        assert(ret >= 0);
        ret = av_opt_set_q(abuffer_ctx, "time_base",
                           (AVRational){ 1, INPUT_SAMPLERATE },
                           AV_OPT_SEARCH_CHILDREN);
        assert(ret >= 0);
        ret = av_opt_set_int(abuffer_ctx, "sample_rate", INPUT_SAMPLERATE,
                             AV_OPT_SEARCH_CHILDREN);
        assert(ret >= 0);

        /* Now initialize the filter; we pass NULL options, since we have
         * already set all the options above. */
        auto err = avfilter_init_str(abuffer_ctx, nullptr);
        if (err < 0) {
            fprintf(stderr, "Could not initialize the abuffer filter.\n");
            return err;
        }

        /* Create volume filter. */
        auto volume = avfilter_get_by_name("volume");
        if (!volume) {
            fprintf(stderr, "Could not find the volume filter.\n");
            return AVERROR_FILTER_NOT_FOUND;
        }

        auto volume_ctx =
            avfilter_graph_alloc_filter(filter_graph, volume, "volume");
        if (!volume_ctx) {
            fprintf(stderr, "Could not allocate the volume instance.\n");
            return AVERROR(ENOMEM);
        }

        /* A different way of passing the options is as key/value pairs in a
         * dictionary. */
        AVDictionary *options_dict{ nullptr };
        av_dict_set(&options_dict, "volume", AV_STRINGIFY(VOLUME_VAL), 0);
        err = avfilter_init_dict(volume_ctx, &options_dict);
        av_dict_free(&options_dict);
        if (err < 0) {
            fprintf(stderr, "Could not initialize the volume filter.\n");
            return err;
        }

        /* Create the aformat filter;
         * it ensures that the output is of the format we want. */
        auto aformat = avfilter_get_by_name("aformat");
        if (!aformat) {
            fprintf(stderr, "Could not find the aformat filter.\n");
            return AVERROR_FILTER_NOT_FOUND;
        }

        auto aformat_ctx =
            avfilter_graph_alloc_filter(filter_graph, aformat, "aformat");
        if (!aformat_ctx) {
            fprintf(stderr, "Could not allocate the aformat instance.\n");
            return AVERROR(ENOMEM);
        }

        /* A third way of passing the options is in a string of the form
         * key1=value1:key2=value2.... */
        uint8_t options_str[1024];
        snprintf((char *)options_str, sizeof(options_str),
                 "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%" PRIx64,
                 av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), 44100,
                 (uint64_t)AV_CH_LAYOUT_STEREO);
        err = avfilter_init_str(aformat_ctx, (const char *)options_str);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Could not initialize the aformat filter.\n");
            return err;
        }

        /* Finally create the abuffersink filter;
         * it will be used to get the filtered data out of the graph. */
        auto abuffersink = avfilter_get_by_name("abuffersink");
        if (!abuffersink) {
            fprintf(stderr, "Could not find the abuffersink filter.\n");
            return AVERROR_FILTER_NOT_FOUND;
        }

        auto abuffersink_ctx =
            avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
        if (!abuffersink_ctx) {
            fprintf(stderr, "Could not allocate the abuffersink instance.\n");
            return AVERROR(ENOMEM);
        }

        /* This filter takes no options. */
        err = avfilter_init_str(abuffersink_ctx, NULL);
        if (err < 0) {
            fprintf(stderr, "Could not initialize the abuffersink instance.\n");
            return err;
        }

        /* Connect the filters;
         * in this simple case the filters just form a linear chain. */
        err = avfilter_link(abuffer_ctx, 0, volume_ctx, 0);
        assert(err >= 0);
        err = avfilter_link(volume_ctx, 0, aformat_ctx, 0);
        assert(err >= 0);

        if (enable_volumedetect) {
            /* enable_volumedetect: create the volumedetect filter;
             * it will be used to get the filtered data out of the graph. */
            auto volumedetect = avfilter_get_by_name("volumedetect");
            if (!volumedetect) {
                fprintf(stderr, "Could not find the volumedetect filter.\n");
                return AVERROR_FILTER_NOT_FOUND;
            }
            auto volumedetect_ctx = avfilter_graph_alloc_filter(
                filter_graph, volumedetect, "volumedetect");
            if (!volumedetect_ctx) {
                fprintf(stderr,
                        "Could not allocate the volumedetect instance.\n");
                return AVERROR(ENOMEM);
            }

            /* This filter takes no options. */
            err = avfilter_init_str(volumedetect_ctx, NULL);
            if (err < 0) {
                fprintf(stderr,
                        "Could not initialize the volumedetect instance.\n");
                return err;
            }

            err = avfilter_link(aformat_ctx, 0, volumedetect_ctx, 0);
            assert(err >= 0);
            err = avfilter_link(volumedetect_ctx, 0, abuffersink_ctx, 0);
            assert(err >= 0);
            volume_detect_ctx = volumedetect_ctx;
        } else {
            err = avfilter_link(aformat_ctx, 0, abuffersink_ctx, 0);
            assert(err >= 0);
        }

        if (err < 0) {
            fprintf(stderr, "Error connecting filters\n");
            return err;
        }

        /* Configure the graph. */
        err = avfilter_graph_config(filter_graph, NULL);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
            return err;
        }

        graph = filter_graph;
        src = abuffer_ctx;
        sink = abuffersink_ctx;

        return 0;
    }

    static constexpr int MAX_DB = 91;
    static inline double logdb(uint64_t v)
    {
        double d = v / (double)(0x8000 * 0x8000);
        if (!v)
            return MAX_DB;
        return -log10(d) * 10;
    }

    static void PrintStats(AVFilterContext *ctx)
    {
        av_log(ctx, AV_LOG_INFO, "========== volume detect stats begin\n");
        /**
         * Number of samples at each PCM value.
         * histogram[0x8000 + i] is the number of samples at value i.
         * The extra element is there for symmetry.
         */
        // VolDetectContext *vd = (VolDetectContext *)ctx->priv;
        constexpr auto kHistogramLength{ 0x10001 };
        auto histogram = reinterpret_cast<uint64_t *>(ctx->priv);

        int i, max_volume, shift;
        uint64_t nb_samples = 0, power = 0, nb_samples_shift = 0, sum = 0;
        uint64_t histdb[MAX_DB + 1] = { 0 };

        for (i = 0; i < 0x10000; i++)
            nb_samples += histogram[i];
        av_log(ctx, AV_LOG_INFO, "n_samples: %" PRId64 "\n", nb_samples);
        if (!nb_samples)
            return;

        /* If nb_samples > 1<<34, there is a risk of overflow in the
           multiplication or the sum: shift all histogram values to avoid that.
           The total number of samples must be recomputed to avoid rounding
           errors. */
        shift = av_log2(nb_samples >> 33);
        for (i = 0; i < 0x10000; i++) {
            nb_samples_shift += histogram[i] >> shift;
            power += (i - 0x8000) * (i - 0x8000) * (histogram[i] >> shift);
        }
        if (!nb_samples_shift)
            return;
        power = (power + nb_samples_shift / 2) / nb_samples_shift;
        av_assert0(power <= 0x8000 * 0x8000);
        av_log(ctx, AV_LOG_INFO, "mean_volume: %.1f dB\n", -logdb(power));

        max_volume = 0x8000;
        while (max_volume > 0 && !histogram[0x8000 + max_volume] &&
               !histogram[0x8000 - max_volume])
            max_volume--;
        av_log(ctx, AV_LOG_INFO, "max_volume: %.1f dB\n",
               -logdb(max_volume * max_volume));

        for (i = 0; i < 0x10000; i++)
            histdb[(int)logdb((i - 0x8000) * (i - 0x8000))] += histogram[i];
        for (i = 0; i <= MAX_DB && !histdb[i]; i++)
            ;
        for (; i <= MAX_DB && sum < nb_samples / 1000; i++) {
            av_log(ctx, AV_LOG_INFO, "histogram_%ddb: %" PRId64 "\n", i,
                   histdb[i]);
            sum += histdb[i];
        }
        av_log(ctx, AV_LOG_INFO, "========== volume detect stats end\n");
    }
};

struct SinVolumeDetectContext : VolumeDetectContext {
    struct AVMD5 *md5{ nullptr };
    AVFrame *frame{ nullptr };

    void Free()
    {
        if (md5) {
            av_freep(&md5);
            md5 = nullptr;
        }
        if (frame) {
            av_frame_free(&frame);
            frame = nullptr;
        }
    }

    ~SinVolumeDetectContext() override { Free(); }

    /* Do something useful with the filtered data: this simple
     * example just prints the MD5 checksum of each plane to stdout. */
    int ProcessOutput()
    {
        int planar = av_sample_fmt_is_planar((AVSampleFormat)frame->format);
        int channels = av_get_channel_layout_nb_channels(frame->channel_layout);
        int planes = planar ? channels : 1;
        int bps = av_get_bytes_per_sample((AVSampleFormat)frame->format);
        int plane_size = bps * frame->nb_samples * (planar ? 1 : channels);
        int i, j;

        for (i = 0; i < planes; i++) {
            uint8_t checksum[16];

            av_md5_init(md5);
            av_md5_sum(checksum, frame->extended_data[i], plane_size);

            fprintf(stdout, "plane %d: 0x", i);
            for (j = 0; j < sizeof(checksum); j++)
                fprintf(stdout, "%02X", checksum[j]);
            fprintf(stdout, "\n");
        }

        return 0;
    }
};

/* Construct a frame of audio data to be filtered;
 * this simple example just synthesizes a sine wave. */
static int get_input(AVFrame *frame, int frame_num)
{
    int err, i, j;

#define FRAME_SIZE 1024

    /* Set up the frame properties and allocate the buffer for the data. */
    frame->sample_rate = INPUT_SAMPLERATE;
    frame->format = INPUT_FORMAT;
    frame->channel_layout = INPUT_CHANNEL_LAYOUT;
    frame->nb_samples = FRAME_SIZE;
    frame->pts = frame_num * FRAME_SIZE;

    err = av_frame_get_buffer(frame, 0);
    if (err < 0)
        return err;

    /* Fill the data for each channel. */
    for (i = 0; i < 5; i++) {
        float *data = (float *)frame->extended_data[i];

        for (j = 0; j < frame->nb_samples; j++)
            data[j] = sin(2 * M_PI * (frame_num + j) * (i + 1) / FRAME_SIZE);
    }

    return 0;
}

int main()
{
    auto duration = 1000;
    auto nb_frames = duration * INPUT_SAMPLERATE / FRAME_SIZE;
    assert(nb_frames > 0);

    SinVolumeDetectContext ctx_;

    /* Allocate the frame we will be using to store the data. */
    ctx_.frame = av_frame_alloc();
    if (!ctx_.frame) {
        fprintf(stderr, "Error allocating the frame\n");
        return 1;
    }

    ctx_.md5 = av_md5_alloc();
    if (!ctx_.md5) {
        fprintf(stderr, "Error allocating the MD5 context\n");
        return 1;
    }

    /* Set up the filtergraph. */
    auto err = ctx_.InitFilterGraph();
    if (err < 0) {
        fprintf(stderr, "Unable to init filter graph:%s\n", av_err2str(err));
        return 1;
    }

    /* the main filtering loop */
    for (int i = 0; i < nb_frames; i++) {
        if (i % 10000 == 0) {
            ctx_.PrintStats(ctx_.volume_detect_ctx);
        }
        /* get an input frame to be filtered */
        err = get_input(ctx_.frame, i);
        if (err < 0) {
            fprintf(stderr, "Error generating input frame:%s\n",
                    av_err2str(err));
            return 1;
        }

        /* Send the frame to the input of the filtergraph. */
        assert(ctx_.src);
        err = av_buffersrc_add_frame(ctx_.src, ctx_.frame);
        if (err < 0) {
            av_frame_unref(ctx_.frame);
            fprintf(stderr,
                    "Error submitting the frame to the filtergraph:%s\n",
                    av_err2str(err));
            return 1;
        }

        /* Get all the filtered output that is available. */
        assert(ctx_.sink);
        while ((err = av_buffersink_get_frame(ctx_.sink, ctx_.frame)) >= 0) {
            /* now do something with our filtered frame */
            // err = ctx_.ProcessOutput();
            if (err < 0) {
                fprintf(stderr, "Error processing the filtered frame:%s\n",
                        av_err2str(err));
                return 1;
            }
            av_frame_unref(ctx_.frame);
        }

        if (err == AVERROR(EAGAIN)) {
            /* Need to feed more frames in. */
            continue;
        } else if (err == AVERROR_EOF) {
            /* Nothing more to do, finish. */
            break;
        } else if (err < 0) {
            /* An error occurred. */
            fprintf(stderr, "Error filtering the data: %s\n", av_err2str(err));
            return 1;
        }
    }

    ctx_.PrintStats(ctx_.volume_detect_ctx);
    return 0;
}
