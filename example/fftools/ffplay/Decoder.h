//
// Created by Johnny on 2020/12/6.
//

#pragma once

#include <cassert>
#include <SDL2/SDL_thread.h>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/bprint.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
}

struct PacketQueue;
struct FrameQueue;

struct Decoder {
    AVPacket *pkt;
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

    int Init(AVCodecContext *avctx,
              PacketQueue *queue,
              SDL_cond *empty_queue_cond);

    int Start(int (*fn)(void*), const char* thread_name, void* arg);

    int DecodeFrame(AVFrame *frame, AVSubtitle *sub);

    void Destroy();

    void Abort(FrameQueue *fq);
};