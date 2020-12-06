//
// Created by Johnny on 2020/12/6.
//

#pragma once

#include <cassert>
#include <SDL2/SDL_thread.h>

struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;

    static int decoder_reorder_pts;

    void decoder_init(AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
        memset(this, 0, sizeof(Decoder));
        this->avctx = avctx;
        this->queue = queue;
        this->empty_queue_cond = empty_queue_cond;
        start_pts = AV_NOPTS_VALUE;
        pkt_serial = -1;
    }

    int decoder_start(AVPacket& flush_pkt, int (*fn)(void *), const char *thread_name, void* arg)
    {
        assert(queue);
        queue->Start(flush_pkt);
        decoder_tid = SDL_CreateThread(fn, thread_name, arg);
        if (!decoder_tid) {
            av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
            return AVERROR(ENOMEM);
        }
        return 0;
    }

    int decoder_decode_frame(AVFrame *frame, AVSubtitle *sub) {
        int ret = AVERROR(EAGAIN);

        for (;;) {
            AVPacket pkt;

            if (queue->serial == pkt_serial) {
                do {
                    if (queue->abort_request)
                        return -1;

                    switch (avctx->codec_type) {
                        case AVMEDIA_TYPE_VIDEO:
                            ret = avcodec_receive_frame(avctx, frame);
                            if (ret >= 0) {
                                if (decoder_reorder_pts == -1) {
                                    frame->pts = frame->best_effort_timestamp;
                                } else if (!decoder_reorder_pts) {
                                    frame->pts = frame->pkt_dts;
                                }
                            }
                            break;
                        case AVMEDIA_TYPE_AUDIO:
                            ret = avcodec_receive_frame(avctx, frame);
                            if (ret >= 0) {
                                AVRational tb = (AVRational){1, frame->sample_rate};
                                if (frame->pts != AV_NOPTS_VALUE)
                                    frame->pts = av_rescale_q(frame->pts, avctx->pkt_timebase, tb);
                                else if (next_pts != AV_NOPTS_VALUE)
                                    frame->pts = av_rescale_q(next_pts, next_pts_tb, tb);
                                if (frame->pts != AV_NOPTS_VALUE) {
                                    next_pts = frame->pts + frame->nb_samples;
                                    next_pts_tb = tb;
                                }
                            }
                            break;
                        default:
                            assert(false);
                    }
                    if (ret == AVERROR_EOF) {
                        finished = pkt_serial;
                        avcodec_flush_buffers(avctx);
                        return 0;
                    }
                    if (ret >= 0)
                        return 1;
                } while (ret != AVERROR(EAGAIN));
            }

            do {
                if (queue->nb_packets == 0)
                    SDL_CondSignal(empty_queue_cond);
                if (packet_pending) {
                    av_packet_move_ref(&pkt, &pkt);
                    packet_pending = 0;
                } else {
                    if (queue->Get(&pkt, 1, &pkt_serial) < 0)
                        return -1;
                }
                if (queue->serial == pkt_serial)
                    break;
                av_packet_unref(&pkt);
            } while (1);
            assert(queue->flush_pkt_);
            if (pkt.data == queue->flush_pkt_->data) {
                avcodec_flush_buffers(avctx);
                finished = 0;
                next_pts = start_pts;
                next_pts_tb = start_pts_tb;
            } else {
                if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    int got_frame = 0;
                    ret = avcodec_decode_subtitle2(avctx, sub, &got_frame, &pkt);
                    if (ret < 0) {
                        ret = AVERROR(EAGAIN);
                    } else {
                        if (got_frame && !pkt.data) {
                            packet_pending = 1;
                            av_packet_move_ref(&pkt, &pkt);
                        }
                        ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                    }
                } else {
                    if (avcodec_send_packet(avctx, &pkt) == AVERROR(EAGAIN)) {
                        av_log(avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                        packet_pending = 1;
                        av_packet_move_ref(&pkt, &pkt);
                    }
                }
                av_packet_unref(&pkt);
            }
        }
    }

    void decoder_destroy() {
        av_packet_unref(&pkt);
        avcodec_free_context(&avctx);
    }

    void decoder_abort(FrameQueue *fq)
    {
        queue->Abort();
        fq->Signal();
        SDL_WaitThread(decoder_tid, NULL);
        decoder_tid = NULL;
        queue->Flush();
    }

};