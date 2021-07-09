//
// Created by Johnny on 2020/12/6.
//

#include "FrameQueue.h"

void PacketQueue::Flush()
{
    MyAVPacketList pkt1;

    SDL_LockMutex(mutex);
    while (av_fifo_size(pkt_list) >= sizeof(pkt1)) {
        av_fifo_generic_read(pkt_list, &pkt1, sizeof(pkt1), NULL);
        av_packet_free(&pkt1.pkt);
    }
    nb_packets = 0;
    size = 0;
    duration = 0;
    ++serial;
    SDL_UnlockMutex(mutex);
}
void PacketQueue::Destroy()
{
    Flush();
    av_fifo_freep(&pkt_list);
    SDL_DestroyMutex(mutex);
    SDL_DestroyCond(cond);
}
void PacketQueue::Start()
{
    SDL_LockMutex(mutex);
    abort_request = 0;
    ++serial;
    SDL_UnlockMutex(mutex);
}
int PacketQueue::PutPrivate(AVPacket *pkt)
{
    MyAVPacketList pkt1;

    if (abort_request)
        return -1;
    if (av_fifo_space(pkt_list) < sizeof(pkt1)) {
        if (av_fifo_grow(pkt_list, sizeof(pkt1)) < 0)
            return -1;
    }

    pkt1.pkt = pkt;
    pkt1.serial = serial;

    av_fifo_generic_write(pkt_list, &pkt1, sizeof(pkt1), nullptr);
    nb_packets++;
    size += pkt1.pkt->size + sizeof(pkt1);
    duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(cond);
    return 0;
}
int PacketQueue::Put(AVPacket *pkt)
{
    int ret;

    AVPacket *pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(mutex);
    ret = PutPrivate(pkt1);
    SDL_UnlockMutex(mutex);

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}
int PacketQueue::PutNullPacket(AVPacket *pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return Put(pkt);
}
int PacketQueue::Init()
{
    memset(this, 0, sizeof(PacketQueue));
    pkt_list = av_fifo_alloc(sizeof(MyAVPacketList));
    if (!pkt_list)
        return AVERROR(ENOMEM);
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
void PacketQueue::Abort()
{
    SDL_LockMutex(mutex);

    abort_request = 1;

    SDL_CondSignal(cond);

    SDL_UnlockMutex(mutex);
}
int PacketQueue::Get(AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(mutex);

    for (;;) {
        if (abort_request) {
            ret = -1;
            break;
        }

        if (av_fifo_size(pkt_list) >= sizeof(pkt1)) {
            av_fifo_generic_read(pkt_list, &pkt1, sizeof(pkt1), NULL);
            nb_packets--;
            size -= pkt1.pkt->size + sizeof(pkt1);
            duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
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
int FrameQueue::Init(PacketQueue *pktq, int max_size, int keep_last)
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
void FrameQueue::Destory()
{
    for (int i = 0; i < max_size; i++) {
        Frame *vp = &queue[i];
        vp->UnrefItem();
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(mutex);
    SDL_DestroyCond(cond);
}
void FrameQueue::Signal()
{
    SDL_LockMutex(mutex);
    SDL_CondSignal(cond);
    SDL_UnlockMutex(mutex);
}
Frame *FrameQueue::Peek()
{
    return &queue[(rindex + rindex_shown) % max_size];
}
Frame *FrameQueue::PeekNext()
{
    return &queue[(rindex + rindex_shown + 1) % max_size];
}
Frame *FrameQueue::PeekWritable()
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
Frame *FrameQueue::PeekReadable()
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
void FrameQueue::Push()
{
    if (++windex == max_size)
        windex = 0;
    SDL_LockMutex(mutex);
    size++;
    SDL_CondSignal(cond);
    SDL_UnlockMutex(mutex);
}
void FrameQueue::Next()
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
int FrameQueue::NbRemaining() const
{
    return size - rindex_shown;
}
int64_t FrameQueue::LastPosition() const
{
    auto fp = &queue[rindex];
    auto valid = rindex_shown && fp->serial == pktq->serial;
    return valid ? fp->pos : -1;
}
