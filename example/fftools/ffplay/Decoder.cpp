//
// Created by Johnny on 2020/12/6.
//

#include "Decoder.h"
#include "FrameQueue.h"

int Decoder::Init(AVCodecContext *avctx,
                   PacketQueue *queue,
                   SDL_cond *empty_queue_cond)
{
    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);
    this->avctx = avctx;
    this->queue = queue;
    this->empty_queue_cond = empty_queue_cond;
    start_pts = AV_NOPTS_VALUE;
    pkt_serial = -1;
    return 0;
}
int Decoder::Start(int (*fn)(void*), const char* thread_name, void* arg) {
    assert(queue);
    queue->Start();
    decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}
int Decoder::DecodeFrame(AVFrame *frame, AVSubtitle *sub)
{
    int ret = AVERROR(EAGAIN);

    for (;;) {
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
                        AVRational tb = (AVRational){ 1, frame->sample_rate };
                        if (frame->pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(frame->pts,
                                                      avctx->pkt_timebase, tb);
                        else if (next_pts != AV_NOPTS_VALUE)
                            frame->pts =
                                av_rescale_q(next_pts, next_pts_tb, tb);
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
                packet_pending = 0;
            } else {
                int old_serial = pkt_serial;
                if (queue->Get(pkt, 1, &pkt_serial) < 0)
                    return -1;
                if (old_serial != pkt_serial) {
                    avcodec_flush_buffers(avctx);
                    finished = 0;
                    next_pts = start_pts;
                    next_pts_tb = start_pts_tb;
                }
            }
            if (queue->serial == pkt_serial)
                break;
            av_packet_unref(pkt);
        } while (1);

        if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(avctx, sub, &got_frame, pkt);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !pkt->data) {
                    packet_pending = 1;
                } else {
                    av_packet_unref(pkt);
                }
                ret = got_frame
                          ? 0
                          : (pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(pkt);
        } else {
            if (avcodec_send_packet(avctx, pkt) == AVERROR(EAGAIN)) {
                av_log(avctx, AV_LOG_ERROR,
                       "Receive_frame and send_packet both returned "
                       "EAGAIN, which is an API violation.\n");
                packet_pending = 1;
            } else {
                av_packet_unref(pkt);
            }
        }
    }
}
void Decoder::Destroy()
{
    av_packet_free(&pkt);
    avcodec_free_context(&avctx);
}
void Decoder::Abort(FrameQueue *fq)
{
    queue->Abort();
    fq->Signal();
    SDL_WaitThread(decoder_tid, NULL);
    decoder_tid = NULL;
    queue->Flush();
}
