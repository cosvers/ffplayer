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
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>

#include "libavutil/fifo.h"
#include "libavutil/time.h"
#include "libavdevice/avdevice.h"

#include "ffplayer.h"
#include "event_queue.h"

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define AUDIO_ENABLED 0
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
    int abort_request;
    int paused;
    int last_paused;
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

    int av_sync_type;
#if AUDIO_ENABLED
    int audio_stream;
    int audio_clock_serial;
    AVStream *audio_st;
    PacketQueue audioq;
#endif

    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    int eof;

    char *filename;

    pthread_cond_t continue_read_thread;
    EventHeader *handle;
} VideoState;

static float seek_interval = 10; // in seconds
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int infinite_buffer = -1;

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

        // printf("queu size %d %ld\n", av_fifo_size(q->pkt_list), sizeof(pkt1));
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

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;

    codecpar = ic->streams[stream_index]->codecpar;
    ic->streams[stream_index]->discard = AVDISCARD_ALL;

    switch (codecpar->codec_type)
    {
#if AUDIO_ENABLED
    case AVMEDIA_TYPE_AUDIO:
        packet_queue_abort(&is->audioq);
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
#endif

    case AVMEDIA_TYPE_VIDEO:
        packet_queue_abort(&is->videoq);
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

    packet_queue_destroy(&is->videoq);
#if AUDIO_ENABLED
    packet_queue_destroy(&is->audioq);
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

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
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
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it*/
    if (is->paused)
        stream_toggle_pause(is);
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    // printf("came inside: %d %d %d %d %ld %d\n", stream_id, queue->abort_request, queue->nb_packets, MIN_FRAMES, queue->duration, (av_q2d(st->time_base) * queue->duration > 1.0));
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

    // av_format_inject_global_side_data(ic);

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

    is->realtime = is_realtime(is);

    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);

    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);

    packet_queue_start(&is->videoq);
#if AUDIO_ENABLED
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
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }

        // printf("came inside 3.5 %d %d %d %d %d\n", infinite_buffer, (is->audioq.size + is->videoq.size), MAX_QUEUE_SIZE, stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq), stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq));

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

    if (packet_queue_init(&is->videoq) < 0
#if AUDIO_ENABLED
        || packet_queue_init(&is->audioq) < 0
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

static void *event_loop(void *arg)
{
    VideoState *cur_stream = arg;
    FFEvent ffevent;
    int quit = 0;
    double incr, pos, frac;

    while (!quit)
    {
        double x;

        while (EventQueue.isEmpty(cur_stream->handle))
        {
            av_usleep((int64_t)(REFRESH_RATE * 1000000.0));
        }

        ffevent = get_event(cur_stream->handle);

        // If we don't yet have a video stream, skip all key events, because read_thread might still be initializing...
        if (cur_stream->video_stream < 0)
            continue;

        switch (ffevent)
        {
        case FFEVENT_PLAY_PAUSE:
            toggle_pause(cur_stream);
            break;

        case FFEVENT_CLOSE:
            do_exit(cur_stream);
            quit = 1;
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

        // TODO: seek based on UI seek bar
        case FFEVENT_SEEK:
            x = 0;           // seek value from user
            int width = 100; // total length of seek bar;
            if (cur_stream->ic->duration <= 0)
            {
                uint64_t size = avio_size(cur_stream->ic->pb);
                stream_seek(cur_stream, size * x / width, 0, 1);
            }
            else
            {
                int64_t ts;
                int ns, hh, mm, ss;
                int tns, thh, tmm, tss;
                tns = cur_stream->ic->duration / 1000000LL;
                thh = tns / 3600;
                tmm = (tns % 3600) / 60;
                tss = (tns % 60);
                frac = x / width;
                ns = frac * tns;
                hh = ns / 3600;
                mm = (ns % 3600) / 60;
                ss = (ns % 60);
                av_log(NULL, AV_LOG_INFO,
                       "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                       hh, mm, ss, thh, tmm, tss);
                ts = frac * cur_stream->ic->duration;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                    ts += cur_stream->ic->start_time;
                stream_seek(cur_stream, ts, 0, 0);
            }
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

int ffplayer_seek(VideoState *state, int value)
{
    if (!state || !state->handle)
        return -1;

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

int ffplayer_get_video_pkt(VideoState *state, AVPacket *pkt)
{
    if (!state || !state->handle)
        return -1;

    return packet_queue_get(&state->videoq, pkt, 1, NULL);
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