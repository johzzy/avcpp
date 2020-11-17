//
// Created by Johnny on 2020/11/23.
//

#pragma once

#include "io-context.h"

#include <fstream>
#include <memory>

namespace av {
static const char *rtp_bin = "/Users/johnny/Documents/rtp/rtp.bin";
struct RTPStreamReader {
    // index, length, bytes
    // index, length, bytes
    std::ifstream f;
    using Type = uint64_t;
    Type index_{ 0 };
    void Open(const char *rtp_bin)
    {
        f.open(rtp_bin, std::ios::binary | std::ios::in);
    }
    void Close() { f.close(); }

    std::string ReadPacket()
    {
        assert(f.is_open());
        if (f.eof()) {
            return std::string{};
        }
        Type index = 0;
        Type length1 = 0;
        Type length2 = 0;
        f.read((char *)&index, sizeof(index));
        f.read((char *)&length1, sizeof(length1));
        std::string packet(length1, 0);
        f.read((char *)&packet[0], packet.size());
        f.read((char *)&length2, sizeof(length2));
        assert(index == index_ || index == 0);
        assert(length1 == length2);
        ++index_;
        return packet;
    }
};

struct RTPStreamWriter {
    // index, length, bytes
    // index, length, bytes
    std::ofstream f;
    using Type = uint64_t;
    Type index_{ 0 };
    void Open(const char *rtp_bin)
    {
        f.open(rtp_bin, std::ios::binary | std::ios::out);
    }
    void Close() { f.close(); }

    void WritePacket(uint8_t *data, int size)
    {
        assert(f.is_open());
        if (f.eof()) {
            return;
        }
        Type index = index_;
        Type length1 = size;
        Type length2 = size;
        f.write((char *)&index, sizeof(index));
        f.write((char *)&length1, sizeof(length1));
        f.write((char *)data, length1);
        f.write((char *)&length2, sizeof(length2));
        ++index_;
    }
};

} // namespace av

#define AUDIO_INBUF_SIZE 20480

struct MediaInputContextOrigin : InputContext {
    using InputContext::InputContext;
    AVFormatContext *format_context = nullptr;
    int audio_index_ = -1;
    int ReadFrame(AVPacket &packet) override;

    int Open() override;
};

struct RTPInputContext : InputContext {
    using InputContext::InputContext;

    AudioOption codec_option_{ BitRate::BR24K, SampleRate::SR48K, 2,
                               AV_SAMPLE_FMT_FLTP, AV_CODEC_ID_OPUS };

    av::RTPStreamReader reader;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];

    int Open() override;

    int ReadFrame(AVPacket &packet) override;
};

struct MediaOutputContextOrigin final : FIFOOutputContext {
    using FIFOOutputContext::FIFOOutputContext;
    ~MediaOutputContextOrigin() override;
    AVFormatContext *format_context = nullptr;

    /**
     * Encode one frame worth of audio to the output file.
     * @param      frame                 Samples to be encoded
     * @param      output_format_context Format context of the output file
     * @param      output_codec_context  Codec context of the output file
     * @param[out] data_present          Indicates whether data has been
     *                                   encoded
     * @return Error code (0 if successful)
     */
    static int encode_audio_frame(AVFrame *frame,
                                  AVFormatContext *output_format_context,
                                  AVCodecContext *output_codec_context,
                                  int *data_present);

    /**
     * Load one audio frame from the FIFO buffer, encode and write it to the
     * output file.
     * @param fifo                  Buffer used for temporary storage
     * @param output_format_context Format context of the output file
     * @param output_codec_context  Codec context of the output file
     * @return Error code (0 if successful)
     */
    static int load_encode_and_write(AVAudioFifo *fifo,
                                     AVFormatContext *output_format_context,
                                     AVCodecContext *output_codec_context);

    // int LoadEncodeAndWrite(int frame_size);

    int Open() override;

    // int EncodingAudio(const AVFrame *frame) override;

    virtual int WriteFrame(AVPacket &packet) override;
    int WriteHeader() override;
    /** Write the trailer of the output file container. */
    int WriteTrailer() override;
};

struct RTPOutputContext final : FIFOOutputContext {
    using FIFOOutputContext::FIFOOutputContext;

    av::RTPStreamWriter writer;
    int Open() override;

    size_t packet_index{ 0 };
    int WriteFrame(AVPacket &packet) override;
    int WriteTrailer() override;
    int WriteHeader() override;
};