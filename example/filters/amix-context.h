//
// Created by Johnny on 2020/11/18.
//

#pragma once

#include <cassert>
#include <mutex>
#include <vector>

#include "io-context.h"
#include "rtp-context.h"

struct AMixContext {
    OutputOption const opus_br24k_out{ "opus-br24k.bin",
                                       "out",
                                       0U,
                                       BitRate::BR24K,
                                       SampleRate::SR48K,
                                       2,
                                       AV_SAMPLE_FMT_FLTP,
                                       AV_CODEC_ID_OPUS };

    OutputOption const opus_br24k{ "output-br24k.opus",
                                   "out",
                                   0U,
                                   BitRate::BR24K,
                                   SampleRate::SR48K,
                                   2,
                                   AV_SAMPLE_FMT_FLTP,
                                   AV_CODEC_ID_OPUS };

    OutputOption const opus_br64k{ "output-br64k.opus",
                                   "out",
                                   0U,
                                   BitRate::BR64K,
                                   SampleRate::SR48K,
                                   2,
                                   AV_SAMPLE_FMT_FLTP,
                                   AV_CODEC_ID_OPUS };

    OutputOption const aac_br64k{ "output-br64k.aac",
                                  "out",
                                  0U,
                                  BitRate::BR64K,
                                  SampleRate::SR48K,
                                  2,
                                  AV_SAMPLE_FMT_FLTP,
                                  AV_CODEC_ID_AAC };

    OutputOption const aac_br24k{ "output-br24k.aac",
                                  "out",
                                  0U,
                                  BitRate::BR24K,
                                  SampleRate::SR48K,
                                  2,
                                  AV_SAMPLE_FMT_FLTP,
                                  AV_CODEC_ID_AAC };

    OutputOption const wav{ "output.wav",
                            "out",
                            0U,
                            BitRate::BRNone,
                            SampleRate::SR48K,
                            2,
                            AV_SAMPLE_FMT_S16,
                            AV_CODEC_ID_PCM_S16LE };

    constexpr static const char *srcs_[] = {
        "src0", "src1", "src2",  "src3",  "src4",  "src5",  "src6",  "src7",
        "src8", "src9", "src10", "src11", "src12", "src13", "src14", "src15"
    };

    InputContextArray inputs_;
    OutputContextPtr output_;
    std::mutex mtx_;

    template <typename Type = MediaInputContextOrigin>
    void AddInputContext(const char *src)
    {
        std::lock_guard<std::mutex> lck(mtx_);
        auto index = inputs_.size();
        assert(index < sizeof(srcs_) / sizeof(*srcs_));
        inputs_.emplace_back(std::make_unique<Type>(src, srcs_[index], index));
    }

    void RTP2Opus()
    {
        auto sdp =
            "/Users/johnny/Workspace/algento/botsession/docs/livestream.sdp";
        auto opus = "/Users/johnny/Workspace/learning/avcpp/build/debug/"
                    "example/api2-samples/output-br64k.opus";
        auto rtp_bin = "/Users/johnny/Documents/rtp/rtp.bin";
        auto wav_0 = "/Users/johnny/Music/就值得了爱.wav";
        auto wav_1 = "/Users/johnny/Music/試著了解.wav";
        auto wav_2 = "/Users/johnny/Music/慢火車.wav";

        auto mp4 = "/Users/johnny/Music/愛上你給的痛.mp4";

        AddInputContext<MediaInputContext>(sdp);
        // AddInputContext<RTPInputContext>(rtp_bin);
        // AddInputContext<RTPInputContext>(opus_br24k_out.filename);
        // AddInputContext<MediaInputContext>(opus);
        // AddInputContext(wav_0);
        AddInputContext<MediaInputContext>(wav_0);
        // AddInputContext(wav_1);
        // AddInputContext(wav_2);

        // AddInputContext(mp4);
        AddInputContext<MediaInputContext>(mp4);

        // Maybe OutputContext unsupport opus
        // Maybe MediaOutputContext unsupport wav
        // output_ = std::make_unique<OutputContext>(wav); // // OutputContext
        // output_ = std::make_unique<MediaOutputContext>(wav); //
        // MediaInputContext also support wav output_ =
        // std::make_unique<MediaOutputContextOrigin>(wav); // MediaInputContext
        // also support wav
        // output_ = std::make_unique<RTPOutputContext>(opus_br24k_out);
        output_ = std::make_unique<MediaOutputContext>(opus_br24k);
        // output_ = std::make_unique<MediaOutputContext>(opus_br64k);
        // output_ = std::make_unique<MediaOutputContext>(aac_br24k);
        // output_ = std::make_unique<MediaOutputContext>(aac_br64k);
    }

    void Opus2RTP()
    {
        auto sdp =
            "/Users/johnny/Workspace/algento/botsession/docs/livestream.sdp";
        auto opus = "/Users/johnny/Workspace/learning/avcpp/build/debug/"
                    "example/api2-samples/output-br64k.opus";
        auto rtp_bin = "/Users/johnny/Documents/rtp/rtp.bin";
        auto wav_0 = "/Users/johnny/Music/就值得了爱.wav";
        auto wav_1 = "/Users/johnny/Music/試著了解.wav";
        auto wav_2 = "/Users/johnny/Music/慢火車.wav";

        auto mp4 = "/Users/johnny/Music/愛上你給的痛.mp4";

        // AddInputContext<MediaInputContext>(sdp);
        AddInputContext<RTPInputContext>(rtp_bin);
        // AddInputContext<MediaInputContext>(opus);
        AddInputContext(wav_0);
        AddInputContext<MediaInputContext>(wav_0);
        AddInputContext(wav_1);
        AddInputContext(wav_2);

        // AddInputContext(mp4);
        // AddInputContext<MediaInputContext>(mp4);

        // Maybe OutputContext unsupport opus
        // Maybe MediaOutputContext unsupport wav
        // output_ = std::make_unique<OutputContext>(wav); // // OutputContext
        // output_ = std::make_unique<MediaOutputContext>(wav); //
        // MediaInputContext also support wav output_ =
        // std::make_unique<MediaOutputContextOrigin>(wav); // MediaInputContext
        // also support wav
        output_ = std::make_unique<RTPOutputContext>(opus_br24k_out);
        // output_ = std::make_unique<MediaOutputContext>(opus_br24k);
        // output_ = std::make_unique<MediaOutputContext>(opus_br64k);
        // output_ = std::make_unique<MediaOutputContext>(aac_br24k);
        // output_ = std::make_unique<MediaOutputContext>(aac_br64k);
    }

    void Opus2Opus()
    {
        auto sdp =
            "/Users/johnny/Workspace/algento/botsession/docs/livestream.sdp";
        auto opus = "/Users/johnny/Workspace/learning/avcpp/build/debug/"
                    "example/api2-samples/output-br64k.opus";
        auto rtp_bin = "/Users/johnny/Documents/rtp/rtp.bin";
        auto wav_0 = "/Users/johnny/Music/就值得了爱.wav";
        auto wav_1 = "/Users/johnny/Music/試著了解.wav";
        auto wav_2 = "/Users/johnny/Music/慢火車.wav";

        auto mp4 = "/Users/johnny/Music/愛上你給的痛.mp4";

        auto webm = "/Users/johnny/Music/夜照亮了夜.webm";
        auto opus2 = "/Users/johnny/Music/夜照亮了夜.opus";

        AddInputContext<MediaInputContext>(sdp);
        AddInputContext<RTPInputContext>(rtp_bin);
        // AddInputContext<MediaInputContext>(opus);
        AddInputContext(wav_0);
        AddInputContext<MediaInputContext>(wav_0);
        AddInputContext(wav_1);
        AddInputContext(wav_2);

        // AddInputContext(mp4);
        AddInputContext<MediaInputContext>(mp4);

        AddInputContext<MediaInputContext>(webm);
        AddInputContext<MediaInputContext>(opus2);

        // Maybe OutputContext unsupport opus
        // Maybe MediaOutputContext unsupport wav
        // output_ = std::make_unique<OutputContext>(wav); // // OutputContext
        // output_ = std::make_unique<MediaOutputContext>(wav); //
        // MediaInputContext also support wav output_ =
        // std::make_unique<MediaOutputContextOrigin>(wav); // MediaInputContext
        // also support wav
        // output_ = std::make_unique<RTPOutputContext>(opus_br24k_out);
        output_ = std::make_unique<MediaOutputContext>(opus_br24k);
        // output_ = std::make_unique<MediaOutputContext>(opus_br64k);
        // output_ = std::make_unique<MediaOutputContext>(aac_br24k);
        // output_ = std::make_unique<MediaOutputContext>(aac_br64k);
    }

    void SetupTodo()
    {
        auto rtp_bin = "/Users/johnny/Documents/rtp/rtp.bin";
        auto webm_0 = "/Users/johnny/Music/就值得了爱.webm";
        auto webm_1 = "/Users/johnny/Music/試著了解.webm";
        auto webm_2 = "/Users/johnny/Music/慢火車.webm";
        auto webm_3 = "/Users/johnny/Music/夜照亮了夜.webm";

        auto mp4 = "/Users/johnny/Music/愛上你給的痛.mp4";
        auto mp4_1 = "/Users/johnny/Music/不換.mp4";
        auto mkv = "/Users/johnny/Music/每當變幻時.mkv";
        auto opus = "/Users/johnny/Music/夜照亮了夜.opus";

        // AddInputContext<RTPInputContext>(rtp_bin);
        // AddInputContext<MediaInputContext>(webm_0);
        // AddInputContext<MediaInputContext>(webm_1);
        // AddInputContext<MediaInputContext>(webm_2);
        // AddInputContext<MediaInputContext>(webm_3);
        // AddInputContext<MediaInputContext>(opus);
        // AddInputContext<MediaInputContext>(mp4);
        // AddInputContext<MediaInputContext>(mp4_1);
        AddInputContext<MediaInputContext>(mkv);

        output_ = std::make_unique<MediaOutputContext>(opus_br64k);
    }

    void Setup()
    {
        // Opus2RTP(); // 1
        // RTP2Opus(); // 2
        Opus2Opus(); // 3
        // SetupTodo();
    }

    int ProcessGraph()
    {
        auto frame = av_frame_alloc();
        int ret = 0;
        /* pull filtered audio from the filtergraph */
        while (1) {
            ret = av_buffersink_get_frame(output_->sink, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                for (auto &input_ : inputs_) {
                    if (av_buffersrc_get_nb_failed_requests(input_->src) > 0) {
                        input_->to_read = 1;
                        av_log(NULL, AV_LOG_INFO, "Need to read input %s\n",
                               input_->option.name);
                    }
                }
                break;
            } else if (ret < 0) {
                break;
            } else {
                output_->total_out_samples += frame->nb_samples;
                av_log(NULL, AV_LOG_INFO,
                       "remove %d samples from sink (%d Hz, time=%f, "
                       "ttime=%f)\n",
                       frame->nb_samples, output_->codec_context->sample_rate,
                       (double)frame->nb_samples /
                           output_->codec_context->sample_rate,
                       (double)output_->total_out_samples /
                           output_->codec_context->sample_rate);

                av_log(NULL, AV_LOG_INFO, "Data read from graph %s\n",
                       output_->option.name);
                ret = output_->EncodingAudio(frame);
                if (ret < 0)
                    break;

                av_frame_unref(frame);
            }
        }

        av_frame_free(&frame);
        return ret;
    }
    int ProcessAll()
    {
        for (auto &input_ : inputs_) {
            input_->ResetFlags();
        }
        output_->ResetFlags();

        int ret = 0;
        while (output_->nb_finished < inputs_.size()) {
            int data_present_in_graph = 0;

            for (auto &input_ : inputs_) {
                if (input_->finished || input_->to_read <= 0) {
                    continue;
                }

                input_->to_read = 0;

                /** Decode one frame worth of audio samples. */
                ret = input_->DecodingAudio();
                if (input_->finished && !input_->data_present) {
                    output_->nb_finished++;
                }

                data_present_in_graph |= input_->data_present;
            }

            if (data_present_in_graph) {
                ret = ProcessGraph();
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    goto end;
                }
            } else {
                av_log(NULL, AV_LOG_INFO, "No data in graph %s\n",
                       output_->option.filename);
                for (auto &input_ : inputs_) {
                    if (input_->to_read < 0) {
                        av_log(NULL, AV_LOG_ERROR, "%s read error=%d(%s), %s\n",
                               input_->option.name, input_->to_read,
                               av_err2str(input_->to_read),
                               input_->option.filename);
                    }
                    input_->to_read = 1;
                }
            }
        }
        for (auto &input_ : inputs_) {
            if (input_->to_read < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s read error=%d(%s), %s\n",
                       input_->option.name, input_->to_read,
                       av_err2str(input_->to_read), input_->option.filename);
            }
        }
        return 0;

    end:

        if (ret < 0 && ret != AVERROR_EOF) {
            av_log(NULL, AV_LOG_ERROR, "Error occurred: %d(%s), filename=%s\n",
                   ret, av_err2str(ret), output_->option.filename);
            if (ret == -ETIMEDOUT) {
                av_log(NULL, AV_LOG_WARNING, "Maybe rtp timeout.\n");
                return 0;
            }
            exit(1);
        }
        return ret;
    }

    int InitFilterGraph()
    {
        /* Create a new filtergraph, which will contain all the filters. */
        assert(!output_->graph);
        output_->graph = avfilter_graph_alloc();
        if (!output_->graph) {
            av_log(NULL, AV_LOG_ERROR, "Unable to create filter graph.\n");
            return AVERROR(ENOMEM);
        }

        /****** abuffer ********/
        for (auto &input_ : inputs_) {
            auto err = input_->create_filter(output_->graph);
            assert(err >= 0);
            if (err < 0) {
                return err;
            }
        }

        /* Create mix filter. */
        return output_->create_filter(inputs_);
    }
};