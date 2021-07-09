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

    void Flush();

    void Destroy();

    void Start();

    int PutPrivate(AVPacket *pkt);

    int Put(AVPacket *pkt);

    int PutNullPacket(int stream_index);

    /* packet queue handling */
    int Init();

    void Abort();

    /* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
    int Get(AVPacket *pkt, int block, int *serial);

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

    int Init(PacketQueue *pktq, int max_size, int keep_last);

    void Destory();

    void Signal();

    Frame *Peek();

    Frame *PeekNext();

    Frame *PeekLast()
    {
        return &queue[rindex];
    }

    Frame *PeekWritable();

    Frame *PeekReadable();

    void Push();

    void Next();

    /* return the number of undisplayed frames in the queue */
    int NbRemaining() const;

    /* return last shown position */
    int64_t LastPosition() const;
};
