//
// Created by Johnny on 2020/11/18.
//

#pragma once

extern "C" {

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>

// #include <libavutil/channel_layout.h>
// #include <libavutil/md5.h>

// #include <libavutil/opt.h>
// #include <libavutil/samplefmt.h>
// #include <libavfilter/avfilter.h>

// #include <libavformat/avformat.h>
// #include <libavformat/avio.h>
// #include <libavutil/audio_fifo.h>
// #include <libavutil/avassert.h>
// #include <libavutil/avstring.h>
// #include <libavutil/frame.h>
}

namespace {

auto constexpr INPUT_SAMPLERATE = 44100;
auto constexpr INPUT_FORMAT = AV_SAMPLE_FMT_S16;
auto constexpr INPUT_CHANNEL_LAYOUT = AV_CH_LAYOUT_STEREO;

/** The output bit rate in kbit/s */
auto constexpr OUTPUT_BIT_RATE = 64000LL; // 24000LL
auto constexpr OUTPUT_SAMPLERATE = 48000;
/** The number of output channels */
auto constexpr OUTPUT_CHANNELS = 2;

/** The audio sample output format */
// a=rtpmap:111 opus/48000/2
// a=fmtp:111
// minptime=60;useinbandfec=1;maxaveragebitrate=24000;usedtx=1;stereo=0;sprop-stereo=0

auto constexpr VOLUME_VAL = 0.90f;

} // namespace

struct InputOption {
    const char *filename;
    const char *name;
    unsigned int dstpad;
};

enum BitRate : int64_t {
    BRNone = 0LL,
    BR24K = 24000LL,
    BR48K = 48000LL,
    BR64K = 64000LL,
    BR96K = 96000LL
};
enum SampleRate : int { SR48K = 48000 };

struct AudioOption {
    int64_t bit_rate;
    int sample_rate;
    int channels;
    AVSampleFormat sample_format;
    AVCodecID codec_id;
};

struct OutputOption : InputOption, AudioOption {
};

struct InputContext;
using InputContextArray = std::vector<std::unique_ptr<InputContext>>;

struct OutputContext;
using OutputContextPtr = std::unique_ptr<OutputContext>;

class InputContext {
public:
    AVCodecContext *codec_context = nullptr;
    AVFilterContext *src = nullptr;

    InputOption option;
    int volume = -1;
    bool volumedetect{ true };

    int finished = 0;
    int data_present = 0;
    int to_read = 1;
    int total_samples = 0;

    InputContext() = default;
    InputContext(const char *f, const char *n, unsigned d) : option{ f, n, d }
    {
    }
    virtual ~InputContext() {}

    void ResetFlags()
    {
        finished = 0;
        to_read = 1;
        total_samples = 0;
        data_present = 0;
    }

    virtual int ReadFrame(AVPacket &packet) = 0;

    virtual int Open() = 0;

    virtual int DecodingAudio();

    int ForwardAudio(const AVFrame *frame)
    {
        /**
         * If we are at the end of the file and there are no more
         * samples in the decoder which are delayed, we are actually
         * finished. This must not be treated as an error.
         */
        if (finished && !data_present) {
            av_log(NULL, AV_LOG_INFO,
                   "Input %s finished. Write NULL frame %s\n", option.name,
                   option.filename);

            auto err = av_buffersrc_write_frame(src, nullptr);
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Error writing EOF null frame for input %d(%s) %s\n",
                       err, av_err2str(err), option.name);
                assert(err >= 0);
            }
        } else if (data_present) {
            /** If there is decoded data, convert and store it */
            /* push the audio data from decoded frame into the
             * filtergraph
             */
            auto ret = av_buffersrc_write_frame(src, frame);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Error=%d(%s) while feeding the audio filtergraph %s\n",
                       ret, av_err2str(ret), option.name);
            } else {
                av_log(NULL, AV_LOG_INFO,
                       "add %d samples on input %s (%d Hz, time=%f, "
                       "ttime=%f)\n",
                       frame->nb_samples, option.name,
                       codec_context->sample_rate,
                       (double)frame->nb_samples / codec_context->sample_rate,
                       (double)(total_samples += frame->nb_samples) /
                           codec_context->sample_rate);
            }
        } else {
            assert(data_present == 0);
            assert(finished == 0);
        }
    }

    int create_filter(AVFilterGraph *&filter_graph);
    int create_filter2(AVFilterGraph *&filter_graph);
};

class OutputContext {
public:
    AVCodecContext *codec_context = nullptr;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    OutputOption option;

    int total_out_samples = 0;
    int data_present = 0;
    size_t nb_finished{ 0 };

    void ResetFlags()
    {
        nb_finished = 0;
        // to_read = 1;
        total_out_samples = 0;
        data_present = 0;
    }

    OutputContext(OutputOption const &o) : option(o) {}
    virtual ~OutputContext();

    virtual int WriteFrame(AVPacket &packet) = 0;

    virtual int Open() = 0;

    virtual int WriteHeader() { return 0; }

    /** Write the trailer of the output file container. */
    virtual int WriteTrailer() { return 0; }

    virtual int EncodingAudio(const AVFrame *frame);

    /** Write the header of the output file container. */
    static int write_output_file_header(AVFormatContext *output_format_context);

    int create_filter(InputContextArray &inputs_);
};

struct FIFOOutputContext : OutputContext {
    using OutputContext::OutputContext;
    ~FIFOOutputContext() override;
    AVAudioFifo *fifo = nullptr;

    /**
     * Initialize one input frame for writing to the output file.
     * The frame will be exactly frame_size samples large.
     * @param[out] frame                Frame to be initialized
     * @param      output_codec_context Codec context of the output file
     * @param      frame_size           Size of the frame
     * @return Error code (0 if successful)
     */
    static AVFrame *InitOutputFrame(AVCodecContext *output_codec_context,
                                    int frame_size);

    /**
     * Add converted input audio samples to the FIFO buffer for later
     * processing.
     * @param fifo                    Buffer to add the samples to
     * @param samples Samples to be added. The dimensions are
     * channel (for multi-channel audio), sample.
     * @param size              Number of samples to be converted
     * @return Error code (0 if successful)
     */
    static int TransferIn(AVAudioFifo *fifo,
                                uint8_t **samples,
                                const int size);

    int TransferOut(int frame_size);
    int EncodingAudio(const AVFrame *frame) override;
};

struct MediaOutputContext : FIFOOutputContext {
    using FIFOOutputContext::FIFOOutputContext;
    ~MediaOutputContext() override;
    AVFormatContext *format_context = nullptr;

    int Open() override;
    int WriteFrame(AVPacket &packet) override;
    int WriteHeader() override;
    /** Write the trailer of the output file container. */
    int WriteTrailer() override;
};

struct MediaInputContext : InputContext {
    using InputContext::InputContext;
    AVFormatContext *format_context = nullptr;
    int audio_index_ = -1;
    int ReadFrame(AVPacket &packet) override;

    int Open() override;
};

// todo: implement AudioDecoder/AudioEncoder
struct AudioDecoder {
    AVCodecContext *codec_context = nullptr;
    int DecodingAudio(void *data, int size)
    {
        /** Packet used for temporary storage. */
        AVPacket input_packet{ 0 };
        av_init_packet(&input_packet);
        input_packet.data = (uint8_t *)data;
        input_packet.size = size;

        /**
         * Decode the audio frame stored in the temporary packet.
         * The input audio stream decoder is used to do this.
         * If we are at the end of the file, pass an empty packet to the decoder
         * to flush it.
         */
        auto ret = avcodec_send_packet(codec_context, &input_packet);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Error submitting the packet to the decoder error=%d(%s)\n",
                   ret, av_err2str(ret));
            return ret;
        }

        av_packet_unref(&input_packet);

        auto frame = av_frame_alloc();
        if (!frame) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate input frame\n");
            return -1;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_context, frame);
            if (ret == AVERROR(EAGAIN)) {
                // note: ignore
                ret = 0;
                break;
            } else if (ret == AVERROR_EOF) {
                // finished = 1;
                // data_present = 0;
                // return 0;
            } else if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Could not decode frame (error '%s')\n",
                       av_err2str(ret));
                // return ret;
                break;
            }
        }
    }
};

struct AudioEncoder {
};