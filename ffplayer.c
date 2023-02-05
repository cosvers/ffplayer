/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>

#include "libavutil/fifo.h"
#include "libavutil/time.h"
#include "libavutil/common.h"
#include "libavdevice/avdevice.h"

#include "ffplayer.h"
#include "event_queue.h"

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define AUDIO_ENABLED 0 // Work In Progress
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
typedef struct MyAVPacketList
{
    AVPacket *pkt;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue
{
    AVFifoBuffer *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int initialized;
    int abort_request;
    int serial;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct Clock
{
    double pts;       /* clock base */
    double pts_drift; /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial; /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial; /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

// Using Packet in the name of Frame since not to change the original structure.
typedef struct Frame
{
    AVPacket *pkt;
    int serial;
    double pts;      /* presentation timestamp for the frame */
    double duration; /* estimated duration of the frame */
    int64_t pos;     /* byte position of the frame in the input file */
} Frame;

typedef struct FrameQueue
{
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    PacketQueue *pktq;
} FrameQueue;

enum
{
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct VideoState
{
    pthread_t read_tid;
    pthread_t loop_tid;
    pthread_t video_tid;
#if AUDIO_ENABLED
    pthread_t audio_tid;
#endif
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
#if AUDIO_ENABLED
    FrameQueue sampq;
#endif

    int av_sync_type;
#if AUDIO_ENABLED
    int audio_stream;
    int audio_clock_serial;
    AVStream *audio_st;
    PacketQueue audioq;
#endif

    int frame_drops_early;
    int frame_drops_late;
    double last_vis_time;

    double frame_timer;
    int video_stream;
    int pkt_serial;
    AVStream *video_st;
    PacketQueue videoq;
    PacketQueue outputvq;
#if AUDIO_ENABLED
    PacketQueue outputaq;
#endif
    double max_frame_duration; // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    int eof;

    char *filename;
    int step;

    pthread_cond_t continue_read_thread;
    EventHeader *handle;
} VideoState;

static float seek_interval = 10; // in seconds
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int infinite_buffer = -1;
double rdftspeed = 0.02;
static int framedrop = 0;

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList pkt1;

    if (q->abort_request || !q->initialized)
        return -1;

    if (av_fifo_space(q->pkt_list) < sizeof(pkt1))
    {
        if (av_fifo_grow(q->pkt_list, sizeof(pkt1)) < 0)
            return -1;
    }

    pkt1.pkt = pkt;
    pkt1.serial = q->serial;

    av_fifo_generic_write(q->pkt_list, &pkt1, sizeof(pkt1), NULL);
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    pthread_cond_signal(&q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1)
    {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    pthread_mutex_lock(&q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    pthread_mutex_unlock(&q->mutex);

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc(sizeof(MyAVPacketList));

    if (!q->pkt_list)
        return AVERROR(ENOMEM);

    if (pthread_mutex_init(&q->mutex, NULL) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Mutex init failed\n");
        return AVERROR(ENOMEM);
    }

    if (pthread_cond_init(&q->cond, NULL) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Condition variable init failed\n");
        return AVERROR(ENOMEM);
    }

    q->initialized = 0;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList pkt1;

    pthread_mutex_lock(&q->mutex);
    while (av_fifo_size(q->pkt_list) >= sizeof(pkt1))
    {
        av_fifo_generic_read(q->pkt_list, &pkt1, sizeof(pkt1), NULL);
        av_packet_free(&pkt1.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep(&q->pkt_list);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);

    q->abort_request = 1;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->initialized = 1;
    q->abort_request = 0;
    q->serial++;
    pthread_mutex_unlock(&q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;;)
    {
        if (q->abort_request)
        {
            ret = -1;
            break;
        }

        if (!q->initialized)
        {
            ret = 0;
            break;
        }

        if (av_fifo_size(q->pkt_list) >= sizeof(pkt1))
        {
            av_fifo_generic_read(q->pkt_list, &pkt1, sizeof(pkt1), NULL);
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        }
        else if (!block)
        {
            ret = 0;
            break;
        }
        else
        {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }

    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static void frame_queue_unref_item(Frame *vp)
{
    av_packet_unref(vp->pkt);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));

    if (pthread_mutex_init(&f->mutex, NULL) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Mutex init failed\n");
        return AVERROR(ENOMEM);
    }

    if (pthread_cond_init(&f->cond, NULL) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Condition variable init failed\n");
        return AVERROR(ENOMEM);
    }

    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].pkt = av_packet_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destory(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++)
    {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_packet_free(&vp->pkt);
    }
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    pthread_mutex_lock(&f->mutex);
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    pthread_mutex_lock(&f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request)
    {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    pthread_mutex_lock(&f->mutex);
    f->size++;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown)
    {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    pthread_mutex_lock(&f->mutex);
    f->size--;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;

    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type)
    {
#if AUDIO_ENABLED
    case AVMEDIA_TYPE_AUDIO:
        packet_queue_abort(&is->outputaq);
        packet_queue_abort(&is->audioq);
        frame_queue_signal(&is->sampq);
        pthread_join(is->audio_tid, NULL);
        break;
#endif
    case AVMEDIA_TYPE_VIDEO:
        packet_queue_abort(&is->outputvq);
        packet_queue_abort(&is->videoq);
        frame_queue_signal(&is->pictq);
        pthread_join(is->video_tid, NULL);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type)
    {
#if AUDIO_ENABLED
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
#endif
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    pthread_join(is->read_tid, NULL);
    pthread_join(is->loop_tid, NULL);

/* close each stream */
#if AUDIO_ENABLED
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
#endif
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->outputvq);
    packet_queue_destroy(&is->videoq);
#if AUDIO_ENABLED
    packet_queue_destroy(&is->outputaq);
    packet_queue_destroy(&is->audioq);
#endif

    frame_queue_destory(&is->pictq);
#if AUDIO_ENABLED
    frame_queue_destory(&is->sampq);
#endif

    pthread_cond_destroy(&is->continue_read_thread);
    av_free(is->filename);
    av_free(is);
}

static void do_exit(VideoState *is)
{
    if (is && is->handle)
    {
        EventQueue.destroy(is->handle);
    }

    if (is)
    {
        stream_close(is);
    }

    avformat_network_deinit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused)
    {
        return c->pts;
    }
    else
    {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is)
{
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
    {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    }
    else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
    {
#if AUDIO_ENABLED
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
#endif
            return AV_SYNC_EXTERNAL_CLOCK;
    }
    else
    {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is))
    {
    case AV_SYNC_VIDEO_MASTER:
        val = get_clock(&is->vidclk);
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = get_clock(&is->audclk);
        break;
    default:
        val = get_clock(&is->extclk);
        break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is)
{
    if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
#if AUDIO_ENABLED
        || (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
#endif
    )
    {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)
#if AUDIO_ENABLED
             && (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)
#endif
    )
    {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else
    {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    if (!is->seek_req)
    {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        pthread_cond_signal(&is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused)
    {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS))
        {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
    {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
        {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
           delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial)
    {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    }
    else
    {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = opaque;
    double time;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

#if AUDIO_ENABLED
    if (is->audio_st)
    {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time)
        {
            // TODO: video audio sync need to be implemented
            // video_audio_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);

        Frame *vp;
        AVPacket *pkt;

        vp = frame_queue_peek_last(&is->sampq);
        pkt = av_packet_clone(vp->pkt);
        packet_queue_put(&is->outputaq, pkt);
    }
#endif

    if (is->video_st)
    {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0)
        {
            // nothing to do, no picture to display in the queue
        }
        else
        {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial)
            {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay)
            {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            pthread_mutex_lock(&is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            pthread_mutex_unlock(&is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1)
            {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if (!is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration)
                {
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
    display:
        if (is->force_refresh && is->pictq.rindex_shown && is->video_st)
        {
            Frame *vp;
            AVPacket *pkt;

            vp = frame_queue_peek_last(&is->pictq);
            pkt = av_packet_clone(vp->pkt);
            packet_queue_put(&is->outputvq, pkt);
        }
    }

    is->force_refresh = 0;
}

static int queue_picture(VideoState *is, AVPacket *src_pkt, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    av_packet_move_ref(vp->pkt, src_pkt);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVPacket *packet)
{
    int got_picture;

    if ((got_picture = packet_queue_get(&is->videoq, packet, 1, &is->pkt_serial)) < 0)
        return -1;

    if (got_picture)
    {
        double dpts = NAN;

        if (packet->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * packet->pts;

        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
        {
            if (packet->pts != AV_NOPTS_VALUE)
            {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff < 0 &&
                    is->pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets)
                {
                    is->frame_drops_early++;
                    av_packet_unref(packet);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

#if AUDIO_ENABLED
static void *audio_thread(void *arg)
{
    VideoState *is = arg;
    AVPacket *pkt = av_packet_alloc();
    Frame *af;
    int got_frame = 0;
    int ret = 0;

    if (!pkt)
    {
        printf("Out of memory: %d\n", AVERROR(ENOMEM));
        return NULL;
    }

    do
    {
        if ((got_frame = packet_queue_get(&is->audioq, pkt, 1, &is->pkt_serial)) < 0)
            goto the_end;

        if (got_frame)
        {
            if (!(af = frame_queue_peek_writable(&is->sampq)))
                goto the_end;

            af->pts = (pkt->pts == AV_NOPTS_VALUE) ? NAN : pkt->pts * av_q2d(is->audio_st->time_base);
            af->pos = pkt->pos;
            af->serial = is->pkt_serial;
            af->duration = pkt->duration;

            av_packet_move_ref(af->pkt, pkt);
            frame_queue_push(&is->sampq);
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
    av_packet_free(&pkt);
    return NULL;
}
#endif

static void *video_thread(void *arg)
{
    VideoState *is = arg;
    AVPacket *pkt = av_packet_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    if (!pkt)
    {
        printf("Out of memory: %d\n", AVERROR(ENOMEM));
        return NULL;
    }

    for (;;)
    {
        ret = get_video_frame(is, pkt);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
        pts = (pkt->pts == AV_NOPTS_VALUE) ? NAN : pkt->pts * av_q2d(tb);
        ret = queue_picture(is, pkt, pts, duration, pkt->pos, is->pkt_serial);
        av_packet_unref(pkt);

        if (ret < 0)
            goto the_end;
    }
the_end:
    av_packet_free(&pkt);
    return NULL;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    return stream_id < 0 || !queue->initialized ||
           queue->abort_request ||
           (queue->nb_packets > MIN_FRAMES && (!queue->duration || (av_q2d(st->time_base) * queue->duration > 1.0)));
}

static int is_realtime(VideoState *s)
{
    if (!strcmp(s->filename, "rtp") || !strcmp(s->filename, "rtsp") || !strcmp(s->filename, "sdp"))
        return 1;

    if (s->ic->pb && (!strncmp(s->ic->url, "rtp:", 4) || !strncmp(s->ic->url, "udp:", 4)))
        return 1;
    return 0;
}

// https://stackoverflow.com/a/18290183/7059370 - Previous comment does not seem to work
static int pthread_cond_wait_timeout(pthread_cond_t *cond, pthread_mutex_t *mutex, int ms)
{
    int rt;
    struct timeval tv;
    struct timespec ts;

    gettimeofday(&tv, NULL);
    ts.tv_sec = time(NULL) + ms / 1000;
    ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (ms % 1000);
    ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
    ts.tv_nsec %= (1000 * 1000 * 1000);

    rt = pthread_cond_timedwait(cond, mutex, &ts);
    return rt;
}

/* this thread gets the stream from the disk or the network */
static void *read_thread(void *arg)
{
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    pthread_mutex_t wait_mutex;

    if (pthread_mutex_init(&wait_mutex, NULL) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Mutex init failed \n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt)
    {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic)
    {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    err = avformat_open_input(&ic, is->filename, NULL, NULL);
    if (err < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s': error %d\n", is->filename, err);
        ret = -1;
        goto fail;
    }

    is->ic = ic;

    av_format_inject_global_side_data(ic);

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0)
    {
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    is->realtime = is_realtime(is);

    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);

    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);

    packet_queue_start(&is->outputvq);
    packet_queue_start(&is->videoq);
#if AUDIO_ENABLED
    packet_queue_start(&is->outputaq);
    packet_queue_start(&is->audioq);

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
    {
        is->audio_stream = st_index[AVMEDIA_TYPE_AUDIO];
        is->audio_st = ic->streams[st_index[AVMEDIA_TYPE_AUDIO]];
    }
#endif

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
    {
        is->video_stream = st_index[AVMEDIA_TYPE_VIDEO];
        is->video_st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    }

    if (is->video_stream < 0
#if AUDIO_ENABLED
        && is->audio_stream < 0
#endif
    )
    {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s'\n", is->filename);
        ret = -1;
        goto fail;
    }

#if AUDIO_ENABLED
    pthread_create(&is->audio_tid, NULL, audio_thread, is);
#endif
    pthread_create(&is->video_tid, NULL, video_thread, is);
    is->queue_attachments_req = 1;

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

    for (;;)
    {
        if (is->abort_request)
            break;

        if (is->paused != is->last_paused)
        {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
            (!strcmp(is->filename, "rtsp") ||
             (ic->pb && !strncmp(is->filename, "mmsh:", 5))))
        {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            av_usleep(10 * 1000);
            continue;
        }
#endif

        if (is->seek_req)
        {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            }
            else
            {
#if AUDIO_ENABLED
                if (is->audio_stream >= 0)
                    packet_queue_flush(&is->audioq);
#endif
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);
                if (is->seek_flags & AVSEEK_FLAG_BYTE)
                {
                    set_clock(&is->extclk, NAN, 0);
                }
                else
                {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }

        if (is->queue_attachments_req)
        {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, pkt);
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer < 1 &&
            (
#if AUDIO_ENABLED
                is->audioq.size +
#endif
                        is->videoq.size >
                    MAX_QUEUE_SIZE ||
                (stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq)
#if AUDIO_ENABLED
                 && stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq)
#endif
                     )))
        {
            /* wait 10 ms */
            pthread_mutex_lock(&wait_mutex);
            pthread_cond_wait_timeout(&is->continue_read_thread, &wait_mutex, 10);
            pthread_mutex_unlock(&wait_mutex);
            continue;
        }

        if (!is->paused && is->eof && is->videoq.size == 0
#if AUDIO_ENABLED
            && is->audioq.size == 0
#endif
        )
        {
            // stream ended - seek to beginning -
            // stream_seek(is, 0, 0, 0);
        }
        ret = av_read_frame(ic, pkt);

        if (ret < 0)
        {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof)
            {
                printf("EOF error %d\n", ret);
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
#if AUDIO_ENABLED
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
#endif
                is->eof = 1;
            }

            if (ic->pb && ic->pb->error)
            {
                break;
            }

            pthread_mutex_lock(&wait_mutex);
            pthread_cond_wait_timeout(&is->continue_read_thread, &wait_mutex, 10);
            pthread_mutex_unlock(&wait_mutex);
            continue;
        }
        else
        {
            is->eof = 0;
        }

        if (pkt->stream_index == is->video_stream)
        {
            packet_queue_put(&is->videoq, pkt);
        }
#if AUDIO_ENABLED
        else if (pkt->stream_index == is->audio_stream)
        {
            packet_queue_put(&is->audioq, pkt);
        }
#endif
        else
        {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);
    if (ret != 0)
    {
        EventQueue.push(is->handle, FFEVENT_CLOSE);
    }

    pthread_mutex_destroy(&wait_mutex);
    return NULL;
}

static VideoState *stream_open(const char *filename)
{
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;

    is->video_stream = -1;
#if AUDIO_ENABLED
    is->audio_stream = -1;
#endif
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
#if AUDIO_ENABLED
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;
#endif

    if (packet_queue_init(&is->outputvq) < 0 || packet_queue_init(&is->videoq) < 0
#if AUDIO_ENABLED
        || packet_queue_init(&is->outputaq) < 0 || packet_queue_init(&is->audioq) < 0
#endif
    )
        goto fail;

    if (pthread_cond_init(&is->continue_read_thread, NULL) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "Condition variable init failed \n");
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
#if AUDIO_ENABLED
    init_clock(&is->audclk, &is->audioq.serial);
#endif
    init_clock(&is->extclk, &is->extclk.serial);

#if AUDIO_ENABLED
    is->audio_clock_serial = -1;
#endif
    is->av_sync_type = av_sync_type;

    pthread_create(&is->read_tid, NULL, read_thread, is);

    if (!is->read_tid)
    {
        av_log(NULL, AV_LOG_FATAL, "Create read thread failed\n");
    fail:
        stream_close(is);
        return NULL;
    }
    return is;
}

static FFEvent get_event(EventHeader *handle)
{
    int event = -1;

    if (handle != NULL)
    {
        return EventQueue.pop(handle);
    }

    return event;
}

static void refresh_loop_wait_event(VideoState *is)
{
    double remaining_time = 0.0;

    while (EventQueue.isEmpty(is->handle))
    {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));

        remaining_time = REFRESH_RATE;

        if (!is->paused || is->force_refresh)
            video_refresh(is, &remaining_time);
    }
}

static void *event_loop(void *arg)
{
    VideoState *cur_stream = arg;
    FFEvent ffevent;
    int quit = 0;
    double incr, pos;

    while (!quit)
    {
        refresh_loop_wait_event(cur_stream);

        // If we don't yet have a video stream, skip all key events, because read_thread might still be initializing...
        if (cur_stream->video_stream < 0)
            continue;

        ffevent = get_event(cur_stream->handle);

        switch (ffevent)
        {
        case FFEVENT_PLAY_PAUSE:
            toggle_pause(cur_stream);
            break;

        case FFEVENT_CLOSE:
            do_exit(cur_stream);
            quit = 1;
            break;

        case FFEVENT_SEEK:
            stream_seek(cur_stream, cur_stream->seek_pos, 0, 0);
            break;

        case FFEVENT_SEEK_F:
            incr = seek_interval;
            goto do_seek;

        case FFEVENT_SEEK_B:
            incr = -seek_interval;
        do_seek:
            pos = get_master_clock(cur_stream);
            if (isnan(pos))
                pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
            pos += incr;
            if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
            stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
            break;

        default:
            break;
        }
    }

    return NULL;
}

int ffplayer_open(VideoState **state, char *input_filename)
{
    *state = NULL;

    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    if (!input_filename)
    {
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        return -1;
    }

    *state = stream_open(input_filename);
    if (!(*state))
    {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
        return -1;
    }

    (*state)->handle = EventQueue.create();

    pthread_create(&(*state)->loop_tid, NULL, event_loop, *state);

    if (!(*state)->loop_tid)
    {
        av_log(NULL, AV_LOG_FATAL, "Create read thread failed\n");
        do_exit(*state);
        return -1;
    }

    return 0;
}

int ffplayer_play_or_pause(VideoState *state)
{
    if (!state || !state->handle)
        return -1;

    EventQueue.push(state->handle, FFEVENT_PLAY_PAUSE);

    return 0;
}

int ffplayer_seek(VideoState *state, double pos)
{
    if (!state || !state->handle)
        return -1;

    if (state->ic->start_time != AV_NOPTS_VALUE && pos < state->ic->start_time / (double)AV_TIME_BASE)
        pos = state->ic->start_time / (double)AV_TIME_BASE;

    state->seek_pos = (int64_t)(pos * AV_TIME_BASE);

    EventQueue.push(state->handle, FFEVENT_SEEK);

    return 0;
}

int ffplayer_fast_seek(VideoState *state, int forward)
{
    if (!state || !state->handle)
        return -1;

    if (forward)
    {
        EventQueue.push(state->handle, FFEVENT_SEEK_F);
    }
    else
    {
        EventQueue.push(state->handle, FFEVENT_SEEK_B);
    }

    return 0;
}

int ffplayer_get_fps(VideoState *state)
{
    if (!state)
        return -1;

    while (!state->abort_request)
    {
        if (state->video_st)
            return (int)av_q2d(state->video_st->r_frame_rate);

        av_usleep(10);
    }

    return -1;
}

int ffplayer_get_duration(VideoState *state)
{
    if (!state)
        return -1;

    while (!state->abort_request)
    {
        if (state->ic)
        {
            if (state->ic->duration != AV_NOPTS_VALUE)
                return state->ic->duration / (double)AV_TIME_BASE;
            else
                return -1;
        }

        av_usleep(10);
    }

    return -1;
}

int ffplayer_get_video_pkt(VideoState *state, AVPacket *pkt, double *curr_pos)
{
    if (!state)
        return -1;

    *curr_pos = get_master_clock(state);
    return packet_queue_get(&state->outputvq, pkt, 1, NULL);
}

#if AUDIO_ENABLED
int ffplayer_get_audio_pkt(VideoState *state, AVPacket *pkt)
{
    if (!state || !state->handle)
        return -1;

    return packet_queue_get(&state->audioq, pkt, 1, NULL);
}
#endif

int ffplayer_close(VideoState *state)
{
    if (!state || !state->handle)
        return -1;

    EventQueue.push(state->handle, FFEVENT_CLOSE);
    return 0;
}