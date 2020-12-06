//
// Created by Johnny on 2020/12/6.
//

#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include <SDL2/SDL_mutex.h>


/* Common struct for handling all types of decoded data and allocated render buffers. */
struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;

    void UnrefItem()
    {
        av_frame_unref(frame);
        avsubtitle_free(&sub);
    }
};

constexpr int VIDEO_PICTURE_QUEUE_SIZE = 3;
constexpr int  SUBPICTURE_QUEUE_SIZE = 16;
constexpr int SAMPLE_QUEUE_SIZE = 9;
constexpr int FRAME_QUEUE_SIZE = FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE));

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;

    const AVPacket* flush_pkt_{ nullptr};

    void Flush()
    {
        SDL_LockMutex(mutex);
        MyAVPacketList *pkt1;
        for (auto pkt = first_pkt; pkt; pkt = pkt1) {
            pkt1 = pkt->next;
            av_packet_unref(&pkt->pkt);
            av_freep(&pkt);
        }
        last_pkt = NULL;
        first_pkt = NULL;
        nb_packets = 0;
        size = 0;
        duration = 0;
        SDL_UnlockMutex(mutex);
    }

    void Destroy()
    {
        Flush();
        SDL_DestroyMutex(mutex);
        SDL_DestroyCond(cond);
    }

    void Start(AVPacket& flush_pkt)
    {
        SDL_LockMutex(mutex);
        flush_pkt_ = &flush_pkt;
        abort_request = 0;
        PutPrivate(&flush_pkt);
        SDL_UnlockMutex(mutex);
    }

    int PutPrivate(AVPacket *pkt)
    {
        MyAVPacketList *pkt1;

        if (abort_request)
            return -1;

        pkt1 = (MyAVPacketList *)av_malloc(sizeof(MyAVPacketList));
        if (!pkt1)
            return -1;
        pkt1->pkt = *pkt;
        pkt1->next = NULL;
        if (pkt == flush_pkt_)
            serial++;
        pkt1->serial = serial;

        if (!last_pkt)
            first_pkt = pkt1;
        else
            last_pkt->next = pkt1;
        last_pkt = pkt1;
        nb_packets++;
        size += pkt1->pkt.size + sizeof(*pkt1);
        duration += pkt1->pkt.duration;
        /* XXX: should duplicate packet data in DV case */
        SDL_CondSignal(cond);
        return 0;
    }

    int Put(AVPacket *pkt)
    {
        int ret;

        SDL_LockMutex(mutex);
        ret = PutPrivate(pkt);
        SDL_UnlockMutex(mutex);

        if (pkt != flush_pkt_ && ret < 0)
            av_packet_unref(pkt);

        return ret;
    }

    int PutNullPacket(int stream_index)
    {
        AVPacket pkt1, *pkt = &pkt1;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        pkt->stream_index = stream_index;
        return Put(pkt);
    }

    /* packet queue handling */
    int Init()
    {
        memset(this, 0, sizeof(PacketQueue));
        mutex = SDL_CreateMutex();
        if (!mutex) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            return AVERROR(ENOMEM);
        }
        cond = SDL_CreateCond();
        if (!cond) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
            return AVERROR(ENOMEM);
        }
        abort_request = 1;
        return 0;
    }

    void Abort()
    {
        SDL_LockMutex(mutex);

        abort_request = 1;

        SDL_CondSignal(cond);

        SDL_UnlockMutex(mutex);
    }

    /* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
    int Get(AVPacket *pkt, int block, int *serial)
    {
        MyAVPacketList *pkt1;
        int ret;

        SDL_LockMutex(mutex);

        for (;;) {
            if (abort_request) {
                ret = -1;
                break;
            }

            pkt1 = first_pkt;
            if (pkt1) {
                first_pkt = pkt1->next;
                if (!first_pkt)
                    last_pkt = NULL;
                nb_packets--;
                size -= pkt1->pkt.size + sizeof(*pkt1);
                duration -= pkt1->pkt.duration;
                *pkt = pkt1->pkt;
                if (serial)
                    *serial = pkt1->serial;
                av_free(pkt1);
                ret = 1;
                break;
            } else if (!block) {
                ret = 0;
                break;
            } else {
                SDL_CondWait(cond, mutex);
            }
        }
        SDL_UnlockMutex(mutex);
        return ret;
    }

};

struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;

    int Init(PacketQueue *pktq, int max_size, int keep_last)
    {
        int i;
        memset(this, 0, sizeof(FrameQueue));
        if (!(mutex = SDL_CreateMutex())) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            return AVERROR(ENOMEM);
        }
        if (!(cond = SDL_CreateCond())) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
            return AVERROR(ENOMEM);
        }
        this->pktq = pktq;
        this->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
        this->keep_last = !!keep_last;
        for (i = 0; i < max_size; i++)
            if (!(queue[i].frame = av_frame_alloc()))
                return AVERROR(ENOMEM);
        return 0;
    }

    void Destory()
    {
        for (int i = 0; i < max_size; i++) {
            Frame *vp = &queue[i];
            vp->UnrefItem();
            av_frame_free(&vp->frame);
        }
        SDL_DestroyMutex(mutex);
        SDL_DestroyCond(cond);
    }

    void Signal()
    {
        SDL_LockMutex(mutex);
        SDL_CondSignal(cond);
        SDL_UnlockMutex(mutex);
    }

    Frame *Peek()
    {
        return &queue[(rindex + rindex_shown) % max_size];
    }

    Frame *PeekNext()
    {
        return &queue[(rindex + rindex_shown + 1) % max_size];
    }

    Frame *PeekLast()
    {
        return &queue[rindex];
    }

    Frame *PeekWritable()
    {
        /* wait until we have space to put a new frame */
        SDL_LockMutex(mutex);
        while (size >= max_size &&
               !pktq->abort_request) {
            SDL_CondWait(cond, mutex);
        }
        SDL_UnlockMutex(mutex);

        if (pktq->abort_request)
            return NULL;

        return &queue[windex];
    }

    Frame *PeekReadable()
    {
        /* wait until we have a readable a new frame */
        SDL_LockMutex(mutex);
        while (size - rindex_shown <= 0 &&
               !pktq->abort_request) {
            SDL_CondWait(cond, mutex);
        }
        SDL_UnlockMutex(mutex);

        if (pktq->abort_request)
            return NULL;

        return &queue[(rindex + rindex_shown) % max_size];
    }

    void Push()
    {
        if (++windex == max_size)
            windex = 0;
        SDL_LockMutex(mutex);
        size++;
        SDL_CondSignal(cond);
        SDL_UnlockMutex(mutex);
    }

    void Next()
    {
        if (keep_last && !rindex_shown) {
            rindex_shown = 1;
            return;
        }
        queue[rindex].UnrefItem();
        if (++rindex == max_size)
            rindex = 0;
        SDL_LockMutex(mutex);
        size--;
        SDL_CondSignal(cond);
        SDL_UnlockMutex(mutex);
    }

    /* return the number of undisplayed frames in the queue */
    int NbRemaining() const
    {
        return size - rindex_shown;
    }

    /* return last shown position */
    int64_t LastPosition() const
    {
        auto fp = &queue[rindex];
        if (rindex_shown && fp->serial == pktq->serial)
            return fp->pos;
        else
            return -1;
    }
};
