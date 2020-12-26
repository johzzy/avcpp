
#include "amix-context.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/md5.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <cassert>
#include <cstdio>
#include <vector>

int MediaOutputContext::Open() {
    assert(option.filename);
    remove(option.filename);
    AVIOContext* output_io_context = nullptr;
    AVStream* stream = nullptr;
    AVCodec* output_codec = nullptr;

    int error = avio_open(&output_io_context, option.filename, AVIO_FLAG_WRITE);

    /** Open the output file to write to it. */
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open output file '%s' (error '%s')\n",
               option.filename, av_err2str(error));
        return error;
    }

    /** Create a new format context for the output container format. */
    format_context = avformat_alloc_context();
    if (!format_context) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    /** Associate the output file (pointer) with the container format
     * context.
     */
    format_context->pb = output_io_context;

    /** Guess the desired container format based on the file extension. */
    if (!(format_context->oformat =
              av_guess_format(nullptr, option.filename, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "Could not find output file format\n");
        goto cleanup;
    }

    /** Find the encoder to be used by its name. */
    output_codec = avcodec_find_encoder(option.codec_id);
    if (!output_codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find %s encoder.\n",
               avcodec_get_name(option.codec_id));
        goto cleanup;
    }

    /** Create a new audio stream in the output file container. */
    stream = avformat_new_stream(format_context, output_codec);
    if (!stream) {
        av_log(NULL, AV_LOG_ERROR, "Could not create new stream\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    /** Save the encoder context for easiert access later. */
    codec_context = stream->codec;
    /**
     * Set the basic encoder parameters.
     */
    // codec_context->channels = OUTPUT_CHANNELS;
    // codec_context->channel_layout =
    //     av_get_default_channel_layout(OUTPUT_CHANNELS);
    // codec_context->sample_rate = sample_rate;
    // codec_context->sample_fmt = OUTPUT_SAMPLE_FORMAT;
    // // codec_context->sample_fmt = output_codec->sample_fmts[0];
    // codec_context->bit_rate = bit_rate;
    //

    codec_context->sample_fmt = output_codec->sample_fmts
                                    ? output_codec->sample_fmts[0]
                                    : option.sample_format;
    codec_context->bit_rate = option.bit_rate;
    codec_context->sample_rate = option.sample_rate;
    if (output_codec->supported_samplerates) {
        codec_context->sample_rate = output_codec->supported_samplerates[0];
        for (int i = 0; output_codec->supported_samplerates[i]; i++) {
            if (output_codec->supported_samplerates[i] == option.sample_rate)
                codec_context->sample_rate = option.sample_rate;
        }
    }

    codec_context->channel_layout =
        av_get_default_channel_layout(option.channels);
    codec_context->channels = option.channels;
    stream->time_base = (AVRational){ 1, codec_context->sample_rate };

    av_log(NULL, AV_LOG_INFO, "output bitrate %lld\n", codec_context->bit_rate);

    /**
     * Some container formats (like MP4) require global headers to be
     * present Mark the encoder so that it behaves accordingly.
     */
    if (format_context->oformat->flags & AVFMT_GLOBALHEADER)
        format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    error = avcodec_open2(codec_context, output_codec, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open output codec (error '%s')\n",
               av_err2str(error));
        goto cleanup;
    }

    if (codec_context->frame_size == 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Maybe MediaOutputContext unsupport wav(file=%s,codec=%s)\n",
               option.filename, codec_context->codec->name);
        assert(codec_context->sample_fmt != AV_SAMPLE_FMT_NONE);
        // assert(codec_context->frame_size > 0);
    }

    /* Create the FIFO buffer based on the specified output sample format.
     */
    assert(codec_context);
    assert(!fifo);
    fifo = av_audio_fifo_alloc(codec_context->sample_fmt,
                               codec_context->channels, 1);
    /* Initialize the FIFO buffer to store audio samples to be encoded. */
    if (!fifo) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate FIFO\n");
        goto cleanup;
    }
    return 0;

cleanup:
    avio_close(format_context->pb);
    avformat_free_context(format_context);
    format_context = nullptr;
    return error < 0 ? error : AVERROR_EXIT;
}
MediaOutputContext::~MediaOutputContext() {
    if (fifo) {
        av_audio_fifo_free(fifo);
        fifo = nullptr;
    }
}

int MediaOutputContext::WriteFrame(AVPacket& packet) {
    return av_write_frame(format_context, &packet);
}
int MediaOutputContext::WriteHeader() {
    assert(format_context);
    av_dump_format(format_context, 0, option.filename, 1);
    return write_output_file_header(format_context);
}
int MediaOutputContext::WriteTrailer() {
    assert(option.filename);
    int error = av_write_trailer(format_context);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Error while writing trailer (error '%s') %s\n",
               av_err2str(error), option.filename);
    }
    return error;
}

int InputContext::create_filter(AVFilterGraph*& filter_graph) {
    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
    auto abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    /* buffer audio source: the decoded frames from the decoder will be
     * inserted here. */
    if (!codec_context->channel_layout) {
        codec_context->channel_layout =
            av_get_default_channel_layout(codec_context->channels);
    }
    char args[512];
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             codec_context->sample_rate,
             av_get_sample_fmt_name(codec_context->sample_fmt),
             codec_context->channel_layout);

    assert(option.name);
    assert(!src);
    auto err = avfilter_graph_create_filter(&src, abuffer, option.name, args,
                                            nullptr, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source %s\n",
               option.name);
    }
    assert(src && src->graph == filter_graph);
    return err;
}

int InputContext::create_filter2(AVFilterGraph*& filter_graph) {
    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
    auto abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    /* buffer audio source: the decoded frames from the decoder will be
     * inserted here. */
    if (!codec_context->channel_layout) {
        codec_context->channel_layout =
            av_get_default_channel_layout(codec_context->channels);
    }

    char args[512];
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             codec_context->sample_rate,
             av_get_sample_fmt_name(codec_context->sample_fmt),
             codec_context->channel_layout);

    assert(option.name);
    assert(!src);
    // todo: replace avfilter_graph_alloc_filter and av_opt_set
    auto err = avfilter_graph_create_filter(&src, abuffer, option.name, args,
                                            nullptr, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source %s\n",
               option.name);
    }
    assert(src && src->graph == filter_graph);
    return err;
}

int InputContext::DecodingAudio() {
    /** Packet used for temporary storage. */
    AVPacket packet{ 0 };
    av_init_packet(&packet);

    /** Read one audio frame from the input file into a temporary packet. */
    int ret = ReadFrame(packet);
    if (ret < 0) {
        /** If we are the the end of the file, flush the decoder below. */
        if (ret == AVERROR_EOF) {
            // finished = 1;
        } else if (ret == AVERROR(EAGAIN)) {
            return 0;
        } else if (ret == -ETIMEDOUT) {
            to_read = -ETIMEDOUT;
        } else {
            av_log(NULL, AV_LOG_ERROR, "Could not read frame (error '%s')\n",
                   av_err2str(ret));
            return ret;
        }
    }

    /**
     * Decode the audio frame stored in the temporary packet.
     * The input audio stream decoder is used to do this.
     * If we are at the end of the file, pass an empty packet to the decoder
     * to flush it.
     */
    ret = avcodec_send_packet(codec_context, &packet);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Error submitting the packet to the decoder error=%d(%s)\n", ret,
               av_err2str(ret));
        return ret;
    }
    av_packet_unref(&packet);

    auto frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate input frame\n");
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_context, frame);
        if (ret == AVERROR(EAGAIN)) {
            // note: ignore, output frame(zero), audio decoding once complete.
            ret = 0;
            break;
        } else if (ret == AVERROR_EOF) {
            // note: packet(zero) => frame(zero), audio decoding end.
            finished = 1;
            data_present = 0;
        } else if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not decode frame (error '%s')\n",
                   av_err2str(ret));
            break;
        } else {
            /**
             * If the decoder has not been flushed completely, we are not
             * finished, so that this function has to be called again.
             */
            assert(ret >= 0);
            finished = 0;
            data_present = 1;
        }

        ForwardAudio(frame);
        av_frame_unref(frame);
    }
    av_frame_free(&frame);

    return ret;
}
OutputContext::~OutputContext() {
    if (sink) {
        avfilter_free(sink);
        sink = nullptr;
    }
}
int OutputContext::EncodingAudio(const AVFrame* frame) {
    /**
     * Encode the audio frame and store it in the temporary packet.
     * The output audio stream encoder is used to do this.
     */
    auto ret = avcodec_send_frame(codec_context, frame);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not encode frame (error '%s')\n",
               av_err2str(ret));
        return 0;
    }

    /** Packet used for temporary storage. */
    AVPacket packet{ 0 };
    av_init_packet(&packet);

    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_context, &packet);
        if (ret == AVERROR(EAGAIN)) {
            // note: ignore, output packet(zero), audio encoding once complete.
            return 0;
        } else if (ret == AVERROR_EOF) {
            // note: frame(zero) => packet(zero), audio encoding end.
            data_present = 0;
            return 0;
        } else if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not encode frame (error '%s')\n",
                   av_err2str(ret));
            return ret;
        } else {
            data_present = 1;
        }

        /** Write one audio frame from the temporary packet to the output
         * file.
         */
        if (data_present) {
            ret = WriteFrame(packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Could not write frame (error '%s')\n", av_err2str(ret));
                return ret;
            }
        }
    }

    return 0;
}

AVFrame* FIFOOutputContext::InitOutputFrame(
    AVCodecContext* output_codec_context,
    int frame_size) {
    assert(frame_size > 0);
    auto frame = av_frame_alloc();
    /* Create a new frame to store the audio samples. */
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate output frame\n");
        return nullptr;
    }

    /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
    frame->nb_samples = frame_size;
    frame->channel_layout = output_codec_context->channel_layout;
    frame->format = output_codec_context->sample_fmt;
    frame->sample_rate = output_codec_context->sample_rate;

    /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
    auto error = av_frame_get_buffer(frame, 0);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not allocate output frame samples (error '%s')\n",
               av_err2str(error));
        av_frame_free(&frame);
        assert(false);
        return nullptr;
    }

    return frame;
}
int OutputContext::write_output_file_header(
    AVFormatContext* output_format_context) {
    int error;
    if ((error = avformat_write_header(output_format_context, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not write output file header (error '%s')\n",
               av_err2str(error));
        return error;
    }
    return error;
}
int OutputContext::create_filter(InputContextArray& inputs_) {
    /* Create mix filter. */
    auto mix_filter = avfilter_get_by_name("amix");
    if (!mix_filter) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the amix filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    char args[512];
    snprintf(args, sizeof(args), "inputs=%lu", inputs_.size());
    AVFilterContext* mix_ctx{ nullptr };
    auto err = avfilter_graph_create_filter(&mix_ctx, mix_filter, option.name,
                                            args, nullptr, graph);

    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter %d(%s)\n",
               err, av_err2str(err));
        return err;
    }

    /* Finally create the abuffersink filter;
     * it will be used to get the filtered data out of the graph. */
    auto abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffersink filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    assert(!sink);
    sink = avfilter_graph_alloc_filter(graph, abuffersink, "sink");
    if (!sink) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not allocate the abuffersink instance.\n");
        return AVERROR(ENOMEM);
    }
    assert(sink && sink->graph == graph);

    assert(codec_context);
    /* Same sample fmts as the output file. */
    err = av_opt_set_int_list(
        sink, "sample_fmts",
        ((int[]){ codec_context->sample_fmt, AV_SAMPLE_FMT_NONE }),
        AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could set options to the abuffersink instance. %d(%s)\n", err,
               av_err2str(err));
        return err;
    }

    uint8_t ch_layout[64];
    assert(option.channels == codec_context->channels);
    auto channel_layout = av_get_default_channel_layout(option.channels);
    // channel_layout = AV_CH_LAYOUT_5POINT0;
    av_get_channel_layout_string((char*)ch_layout, sizeof(ch_layout), 0,
                                 channel_layout);
    av_log(NULL, AV_LOG_INFO, "channel_layout: '%s'\n", (const char*)ch_layout);
    auto ret = av_opt_set(sink, "channel_layout", (const char*)ch_layout,
                          AV_OPT_SEARCH_CHILDREN);
    // auto ret = av_opt_set_channel_layout(
    //     sink, "out_channel_layout", channel_layout, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        // todo: fix av_opt_set error
        av_log(NULL, AV_LOG_ERROR,
               "Could set channel_layout='%s' to the abuffersink instance. "
               "%d(%s)\n",
               ch_layout, ret, av_err2str(ret));
    }
    // assert(ret >= 0);

    err = avfilter_init_str(sink, nullptr);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not initialize the abuffersink instance.\n");
        return err;
    }

    /* Connect the filters; */
    for (auto& input_ : inputs_) {
        if (input_->volume >= 0) {
            auto volume = avfilter_get_by_name("volume");
            assert(volume);
            auto volume_ctx =
                avfilter_graph_alloc_filter(graph, volume, "volume");
            assert(volume_ctx);
            AVDictionary* options_dict{ nullptr };
            av_dict_set_int(&options_dict, "volume", input_->volume, 0);
            err = avfilter_init_dict(volume_ctx, &options_dict);
            assert(err >= 0);
            av_dict_free(&options_dict);
            err = avfilter_link(input_->src, 0, volume_ctx, 0);
            assert(err >= 0);
            err = avfilter_link(volume_ctx, 0, mix_ctx, input_->option.dstpad);
            assert(err >= 0);
        } else if (input_->volumedetect) {
        https
            : // stackoverflow.com/questions/60775440/ffmpeg-volumedetect-filter-in-c
            auto volumedetect = avfilter_get_by_name("volumedetect");
            assert(volumedetect);
            auto volumedetect_ctx = avfilter_graph_alloc_filter(
                graph, volumedetect, "volumedetect");
            assert(volumedetect_ctx);
            err = avfilter_init_str(volumedetect_ctx, nullptr);
            assert(err >= 0);
            err = avfilter_link(input_->src, 0, volumedetect_ctx, 0);
            assert(err >= 0);
            err = avfilter_link(volumedetect_ctx, 0, mix_ctx,
                                input_->option.dstpad);
            assert(err >= 0);

            // err = avfilter_link(input_->src, 0, mix_ctx,
            // input_->option.dstpad);
        } else {
            err = avfilter_link(input_->src, 0, mix_ctx, input_->option.dstpad);
        }

        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error connecting filters %s\n",
                   input_->option.name);
            return err;
        }
    }

    err = avfilter_link(mix_ctx, 0, sink, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
        return err;
    }

    /* Configure the graph. */
    err = avfilter_graph_config(graph, nullptr);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while configuring graph : %s\n",
               av_err2str(err));
        return err;
    }

    auto dump = avfilter_graph_dump(graph, nullptr);
    av_log(NULL, AV_LOG_ERROR, "Graph :\n%s\n", dump);

    return 0;
}

static int64_t pts = 0;

int MediaOutputContextOrigin::encode_audio_frame(
    AVFrame* frame,
    AVFormatContext* output_format_context,
    AVCodecContext* output_codec_context,
    int* data_present) {
    /* Packet used for temporary storage. */
    AVPacket output_packet{ 0 };
    av_init_packet(&output_packet);

    /* Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }

    /* Send the audio frame stored in the temporary packet to the encoder.
     * The output audio stream encoder is used to do this. */
    int error = avcodec_send_frame(output_codec_context, frame);
    /* The encoder signals that it has nothing more to encode. */
    if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not send packet for encoding (error '%s')\n",
               av_err2str(error));
        return error;
    }

    /* Receive one encoded frame from the encoder. */
    error = avcodec_receive_packet(output_codec_context, &output_packet);
    /* If the encoder asks for more data to be able to provide an
     * encoded frame, return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
        /* If the last frame has been encoded, stop encoding. */
    } else if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not encode frame (error '%s')\n",
               av_err2str(error));
        goto cleanup;
        /* Default case: Return encoded data. */
    } else {
        *data_present = 1;
    }

    /* Write one audio frame from the temporary packet to the output file.
     */
    if (*data_present &&
        (error = av_write_frame(output_format_context, &output_packet)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not write frame (error '%s')\n",
               av_err2str(error));
        goto cleanup;
    }

cleanup:
    av_packet_unref(&output_packet);
    return error;
}
MediaOutputContextOrigin::~MediaOutputContextOrigin() {
    if (fifo) {
        av_audio_fifo_free(fifo);
        fifo = nullptr;
    }
}
int MediaOutputContextOrigin::load_encode_and_write(
    AVAudioFifo* fifo,
    AVFormatContext* output_format_context,
    AVCodecContext* output_codec_context) {
    /* Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame
     * size.
     */
    const int frame_size =
        FFMIN(av_audio_fifo_size(fifo), output_codec_context->frame_size);
    int data_written;

    /* Temporary storage of the output samples of the frame written to the
     * file.
     */
    auto output_frame = InitOutputFrame(output_codec_context, frame_size);

    /* Initialize temporary storage for one output frame. */
    if (!output_frame) {
        assert(false);
        return AVERROR_EXIT;
    }

    /* Read as many samples from the FIFO buffer as required to fill the
     * frame. The samples are stored in the frame temporarily. */
    if (av_audio_fifo_read(fifo, (void**)output_frame->data, frame_size) <
        frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /* Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

int MediaOutputContextOrigin::Open() {
    assert(option.filename);
    remove(option.filename);
    AVIOContext* output_io_context = nullptr;
    AVStream* stream = nullptr;
    AVCodec* output_codec = nullptr;

    int error = avio_open(&output_io_context, option.filename, AVIO_FLAG_WRITE);

    /** Open the output file to write to it. */
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open output file '%s' (error '%s')\n",
               option.filename, av_err2str(error));
        return error;
    }

    /** Create a new format context for the output container format. */
    format_context = avformat_alloc_context();
    if (!format_context) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    /** Associate the output file (pointer) with the container format
     * context.
     */
    format_context->pb = output_io_context;

    /** Guess the desired container format based on the file extension. */
    if (!(format_context->oformat =
              av_guess_format(nullptr, option.filename, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "Could not find output file format\n");
        goto cleanup;
    }

    /** Find the encoder to be used by its name. */
    output_codec = avcodec_find_encoder(option.codec_id);
    if (!output_codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find %s encoder.\n",
               avcodec_get_name(option.codec_id));
        goto cleanup;
    }

    /** Create a new audio stream in the output file container. */
    stream = avformat_new_stream(format_context, output_codec);
    if (!stream) {
        av_log(NULL, AV_LOG_ERROR, "Could not create new stream\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    /** Save the encoder context for easiert access later. */
    codec_context = stream->codec;
    /**
     * Set the basic encoder parameters.
     */
    // codec_context->channels = OUTPUT_CHANNELS;
    // codec_context->channel_layout =
    //     av_get_default_channel_layout(OUTPUT_CHANNELS);
    // codec_context->sample_rate = sample_rate;
    // codec_context->sample_fmt = OUTPUT_SAMPLE_FORMAT;
    // // codec_context->sample_fmt = output_codec->sample_fmts[0];
    // codec_context->bit_rate = bit_rate;
    //

    codec_context->sample_fmt = output_codec->sample_fmts
                                    ? output_codec->sample_fmts[0]
                                    : option.sample_format;
    codec_context->bit_rate = option.bit_rate;
    codec_context->sample_rate = option.sample_rate;
    if (output_codec->supported_samplerates) {
        codec_context->sample_rate = output_codec->supported_samplerates[0];
        for (int i = 0; output_codec->supported_samplerates[i]; i++) {
            if (output_codec->supported_samplerates[i] == option.sample_rate)
                codec_context->sample_rate = option.sample_rate;
        }
    }

    codec_context->channel_layout =
        av_get_default_channel_layout(option.channels);
    codec_context->channels = option.channels;
    stream->time_base = (AVRational){ 1, codec_context->sample_rate };

    av_log(NULL, AV_LOG_INFO, "output bitrate %lld\n", codec_context->bit_rate);

    /**
     * Some container formats (like MP4) require global headers to be
     * present Mark the encoder so that it behaves accordingly.
     */
    if (format_context->oformat->flags & AVFMT_GLOBALHEADER)
        format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    error = avcodec_open2(codec_context, output_codec, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open output codec (error '%s')\n",
               av_err2str(error));
        goto cleanup;
    }

    if (codec_context->frame_size == 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Maybe MediaOutputContext unsupport wav(file=%s,codec=%s)\n",
               option.filename, codec_context->codec->name);
        assert(codec_context->sample_fmt != AV_SAMPLE_FMT_NONE);
        // assert(codec_context->frame_size > 0);
    }

    /* Create the FIFO buffer based on the specified output sample format.
     */
    assert(codec_context);
    assert(!fifo);
    fifo = av_audio_fifo_alloc(codec_context->sample_fmt,
                               codec_context->channels, 1);
    /* Initialize the FIFO buffer to store audio samples to be encoded. */
    if (!fifo) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate FIFO\n");
        goto cleanup;
    }
    return 0;

cleanup:
    avio_close(format_context->pb);
    avformat_free_context(format_context);
    format_context = nullptr;
    return error < 0 ? error : AVERROR_EXIT;
}

int MediaOutputContextOrigin::WriteFrame(AVPacket& packet) {
    return av_write_frame(format_context, &packet);
}
int MediaOutputContextOrigin::WriteHeader() {
    assert(format_context);
    av_dump_format(format_context, 0, option.filename, 1);
    return write_output_file_header(format_context);
}
int MediaOutputContextOrigin::WriteTrailer() {
    assert(option.filename);
    int error = av_write_trailer(format_context);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Error while writing trailer (error '%s') %s\n",
               av_err2str(error), option.filename);
    }
    return error;
}

int FIFOOutputContext::TransferOut(int frame_size) {
    /* Temporary storage of the output samples of the frame written to the
     * file.
     */
    auto frame = InitOutputFrame(codec_context, frame_size);

    /* Initialize temporary storage for one output frame. */
    if (!frame) {
        assert(false);
        return AVERROR_EXIT;
    }

    /* Read as many samples from the FIFO buffer as required to fill the
     * frame. The samples are stored in the frame temporarily. */
    if (av_audio_fifo_read(fifo, (void**)frame->data, frame_size) <
        frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not read data from FIFO\n");
        av_frame_free(&frame);
        return AVERROR_EXIT;
    }

    /* Encode one frame worth of audio samples. */
    if (OutputContext::EncodingAudio(frame)) {
        av_frame_free(&frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&frame);
    return 0;
}

int FIFOOutputContext::TransferIn(AVAudioFifo* fifo,
                                  uint8_t** samples,
                                  const int size) {
    /* Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples. */
    auto error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + size);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not reallocate FIFO\n");
        return error;
    }

    /* Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void**)samples, size) < size) {
        av_log(NULL, AV_LOG_ERROR, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

int FIFOOutputContext::EncodingAudio(const AVFrame* frame) {
    assert(fifo);
    TransferIn(fifo, (uint8_t**)frame->data, frame->nb_samples);
    int size = codec_context->frame_size;
    if (size == 0) {
        size = frame->linesize[0];
    }
    assert(size > 0);

    while (av_audio_fifo_size(fifo) >= size) {
        /* Use the maximum number of possible samples per frame.
         * If there is less than the maximum possible frame size in the FIFO
         * buffer use this number. Otherwise, use the maximum possible frame
         * size.
         */
        auto frame_size = FFMIN(av_audio_fifo_size(fifo), size);
        /* Take one frame worth of audio samples from the FIFO buffer,
         * encode it and write it to the output file. */
        if (TransferOut(frame_size)) {
            assert(false);
            return -1;
        }
    }

    return 0;
}

FIFOOutputContext::~FIFOOutputContext() {
    if (fifo) {
        av_audio_fifo_free(fifo);
        fifo = nullptr;
    }
}
int MediaInputContext::ReadFrame(AVPacket& packet) {
    auto ret = av_read_frame(format_context, &packet);
    if (packet.stream_index != audio_index_ && ret == 0) {
        av_packet_unref(&packet);
        return AVERROR(EAGAIN);
    }
    return ret;
}
int MediaInputContext::Open() {
    AVCodec* input_codec;
    int error;

    AVDictionary* options = nullptr;
    error = av_dict_set(&options, "protocol_whitelist", "file,udp,rtp", 0);
    assert(error >= 0);
    // av_dict_set(&options, "flags", "+global_header", 0);
    // av_dict_set(&options, "strict", "experimental", 0);

    /** Open the input file to read from it. */
    error = avformat_open_input(&format_context, option.filename, nullptr,
                                &options);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open input file '%s' (error '%s')\n", option.filename,
               av_err2str(error));
        format_context = nullptr;
        return error;
    }

    /** Get information on the input file (number of streams etc.). */
    error = avformat_find_stream_info(format_context, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open find stream info (error '%s')\n",
               av_err2str(error));
        avformat_close_input(&format_context);
        return error;
    }

    auto audio_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO,
                                           -1, -1, NULL, 0);

    // audio_index = 0;
    assert(audio_index >= 0);
    /** Make sure that there is only one stream in the input file. */
    if (format_context->nb_streams != 1 && audio_index < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Expected one audio input stream, but found %d\n",
               format_context->nb_streams);
        avformat_close_input(&format_context);
        return AVERROR_EXIT;
    }

    /** Find a decoder for the audio stream. */
    input_codec = avcodec_find_decoder(
        format_context->streams[audio_index]->codec->codec_id);
    if (!input_codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find input codec\n");
        avformat_close_input(&format_context);
        return AVERROR_EXIT;
    }

    /** Open the decoder for the audio stream to use it later. */
    error = avcodec_open2(format_context->streams[audio_index]->codec,
                          input_codec, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input codec (error '%s')\n",
               av_err2str(error));
        avformat_close_input(&format_context);
        return error;
    }

    /** Save the decoder context for easier access later. */
    codec_context = format_context->streams[audio_index]->codec;
    audio_index_ = audio_index;
    av_dump_format(format_context, 0, option.filename, 0);
    assert(codec_context->sample_rate > 0);
    return error;
}
int MediaInputContextOrigin::ReadFrame(AVPacket& packet) {
    auto ret = av_read_frame(format_context, &packet);
    if (packet.stream_index != audio_index_ && ret == 0) {
        av_packet_unref(&packet);
        return AVERROR(EAGAIN);
    }
    return ret;
}
int MediaInputContextOrigin::Open() {
    AVCodec* input_codec;
    int error;

    /** Open the input file to read from it. */
    error =
        avformat_open_input(&format_context, option.filename, nullptr, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open input file '%s' (error '%s')\n", option.filename,
               av_err2str(error));
        format_context = nullptr;
        return error;
    }

    /** Get information on the input file (number of streams etc.). */
    error = avformat_find_stream_info(format_context, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open find stream info (error '%s')\n",
               av_err2str(error));
        avformat_close_input(&format_context);
        return error;
    }

    auto audio_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO,
                                           -1, -1, NULL, 0);

    /** Make sure that there is only one stream in the input file. */
    if (format_context->nb_streams != 1) {
        av_log(NULL, AV_LOG_ERROR,
               "Expected one audio input stream, but found %d\n",
               format_context->nb_streams);
        avformat_close_input(&format_context);
        return AVERROR_EXIT;
    }

    /** Find a decoder for the audio stream. */
    input_codec = avcodec_find_decoder(
        format_context->streams[audio_index]->codec->codec_id);
    if (!input_codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find input codec\n");
        avformat_close_input(&format_context);
        return AVERROR_EXIT;
    }

    /** Open the decoder for the audio stream to use it later. */
    error = avcodec_open2(format_context->streams[audio_index]->codec,
                          input_codec, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input codec (error '%s')\n",
               av_err2str(error));
        avformat_close_input(&format_context);
        return error;
    }

    /** Save the decoder context for easier access later. */
    codec_context = format_context->streams[audio_index]->codec;
    audio_index_ = audio_index;
    av_dump_format(format_context, 0, option.filename, 0);
    assert(codec_context->sample_rate > 0);
    return error;
}
int RTPInputContext::Open() {
    assert(!codec_context);
    auto decoder = avcodec_find_decoder(codec_option_.codec_id);
    if (!decoder) {
        av_log(NULL, AV_LOG_ERROR, "Could not find %s decoder.\n",
               avcodec_get_name(codec_option_.codec_id));
        return -1;
    }
    codec_context = avcodec_alloc_context3(decoder);
    assert(codec_context);

    codec_context->sample_fmt = decoder->sample_fmts
                                    ? decoder->sample_fmts[0]
                                    : codec_option_.sample_format;
    codec_context->bit_rate = codec_option_.bit_rate;
    codec_context->sample_rate = codec_option_.sample_rate;
    if (decoder->supported_samplerates) {
        codec_context->sample_rate = decoder->supported_samplerates[0];
        for (int i = 0; decoder->supported_samplerates[i]; i++) {
            if (decoder->supported_samplerates[i] == codec_option_.sample_rate)
                codec_context->sample_rate = codec_option_.sample_rate;
        }
    }

    codec_context->channel_layout =
        av_get_default_channel_layout(codec_option_.channels);
    codec_context->channels = codec_option_.channels;
    codec_context->time_base = (AVRational){ 1, codec_context->sample_rate };

    av_log(NULL, AV_LOG_INFO, "decoder bitrate %lld\n",
           codec_context->bit_rate);

    auto error = avcodec_open2(codec_context, decoder, nullptr);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open input decoder codec (error '%s')\n",
               av_err2str(error));
        return error;
    }

    reader.Open(option.filename);

    return 0;
}
int RTPInputContext::ReadFrame(AVPacket& packet) {
    auto buffer = reader.ReadPacket();
    if (buffer.empty()) {
        if (reader.f.eof()) {
            return AVERROR_EOF;
        }
        return AVERROR(EAGAIN);
    }

    // auto type = erizo::packetType::AUDIO_PACKET;
    // auto data = std::make_shared<erizo::DataPacket>(
    //     0, reinterpret_cast<char *>(&buffer[0]), buffer.size(), type);

    // memcpy(inbuf, data->data, data->length);
    // packet.data = inbuf;
    // packet.size = data->length;
    // SPDLOG_INFO("===== {} raw {}, audio {}", reader.index_,
    // buffer.size(),
    //             data->length);
    packet.data = (uint8_t*)buffer.data();
    packet.size = buffer.size();

    return 0;
}
int RTPOutputContext::Open() {
    assert(!codec_context);
    /** Find the encoder to be used by its name. */
    auto encoder = avcodec_find_encoder(option.codec_id);
    if (!encoder) {
        av_log(NULL, AV_LOG_ERROR, "Could not find %s encoder.\n",
               avcodec_get_name(option.codec_id));
        return -1;
    }

    /** Save the encoder context for easiert access later. */
    codec_context = avcodec_alloc_context3(encoder);
    assert(codec_context);

    codec_context->sample_fmt =
        encoder->sample_fmts ? encoder->sample_fmts[0] : option.sample_format;
    codec_context->bit_rate = option.bit_rate;
    codec_context->sample_rate = option.sample_rate;
    if (encoder->supported_samplerates) {
        codec_context->sample_rate = encoder->supported_samplerates[0];
        for (int i = 0; encoder->supported_samplerates[i]; i++) {
            if (encoder->supported_samplerates[i] == option.sample_rate)
                codec_context->sample_rate = option.sample_rate;
        }
    }

    codec_context->channel_layout =
        av_get_default_channel_layout(option.channels);
    codec_context->channels = option.channels;
    codec_context->time_base = (AVRational){ 1, codec_context->sample_rate };

    av_log(NULL, AV_LOG_INFO, "encoder bitrate %lld\n",
           codec_context->bit_rate);

    auto error = avcodec_open2(codec_context, encoder, nullptr);
    /** Open the encoder for the audio stream to use it later. */
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not open output encoder codec (error '%s')\n",
               av_err2str(error));
        return error;
    }

    if (codec_context->frame_size == 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Maybe MediaOutputContext unsupport wav(file=%s,codec=%s)\n",
               option.filename, codec_context->codec->name);
        assert(codec_context->sample_fmt != AV_SAMPLE_FMT_NONE);
        // assert(codec_context->frame_size > 0);
    }

    /* Create the FIFO buffer based on the specified output sample format.
     */
    assert(codec_context);
    assert(!fifo);
    fifo = av_audio_fifo_alloc(codec_context->sample_fmt,
                               codec_context->channels, 1);
    /* Initialize the FIFO buffer to store audio samples to be encoded. */
    if (!fifo) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate FIFO\n");
    }
    return 0;
}
int RTPOutputContext::WriteFrame(AVPacket& packet) {
    writer.WritePacket(packet.data, packet.size);
    // todo: output mixed frame
    // SPDLOG_INFO("{} {}", ++packet_index, packet.size);
    return 0;
}
int RTPOutputContext::WriteTrailer() {
    writer.Close();
    return 0;
}
int RTPOutputContext::WriteHeader() {
    writer.Open(option.filename);
    return 0;
}

int Process(AMixContext& context) {
    for (auto& input_ : context.inputs_) {
        if (input_->Open() < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while opening file 1\n");
            exit(1);
        }
    }

    av_log(NULL, AV_LOG_INFO, "Output file : %s\n",
           context.output_->option.filename);
    auto err = context.output_->Open();
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "open output file %s err : %d\n",
               context.output_->option.filename, err);
    }

    /* Set up the filtergraph. */
    err = context.InitFilterGraph();
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "InitFilterGraph err=%d(%s)\n", err,
               av_err2str(err));
    }
    assert(err >= 0);

    if (context.output_->WriteHeader() < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while writing header %s\n",
               context.output_->option.filename);
        exit(1);
    }

    context.ProcessAll();

    if (context.output_->WriteTrailer() < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while writing trailer\n");
        exit(1);
    }

    av_log(NULL, AV_LOG_INFO, "FINISHED\n");
    return 0;
}

namespace go {
using std::string;
namespace time {
using Duration = int64_t;
constexpr auto Second = 1LL;
} // namespace time
namespace tls {
using Config = string;
}

struct Server {
    string addr;
    int port;
    string protocol;
    time::Duration timeout;
    int max_conns;
    tls::Config* tls_;
};

using Option = std::function<void(Server& s)>;

auto Protocol(string p) -> Option {
    return [p](Server& s) { s.protocol = p; };
}
auto Timeout(time::Duration timeout) -> Option {
    return [timeout](Server& s) { s.timeout = timeout; };
}
auto MaxConns(int max_conns) -> Option {
    return [max_conns](Server& s) { s.max_conns = max_conns; };
}
auto TLS(tls::Config* tls_) -> Option {
    return [tls_](Server& s) { s.tls_ = tls_; };
}

Server NewServer(string addr,
                 int port,
                 std::initializer_list<Option> options = {}) {
    Server s{ addr, port, "tcp", 30 * time::Second, 1000, nullptr };
    for (auto& option : options) {
        option(s);
    }
    return s;
}

void UsageServer() {
    auto s1 = NewServer("localhost", 1024);
    auto s2 = NewServer("localhost", 2048, { Protocol("udp") });
    auto s3 = NewServer("0.0.0.0", 8080,
                        { Timeout(300 * time::Second), MaxConns(1000) });
}
} // namespace go

#if defined(FFMPEG_AMIX_MAIN)
int main()
#else
int ffmpeg_amix_main()
#endif
{
    av_log_set_level(AV_LOG_VERBOSE);

#if !defined(FF_API_NEXT)
    av_register_all();
    avfilter_register_all();
#endif

    AMixContext context;
    context.Setup();
    Process(context);

    for (int i = 0; i < 0; ++i) {
        context.inputs_.clear();
        context.Opus2Opus();
        Process(context);
    }
    return 0;
}