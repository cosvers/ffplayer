#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

typedef enum
{
    FFEVENT_EMPTY = -1,
    FFEVENT_PLAY_PAUSE = 0,
    FFEVENT_SEEK_F,
    FFEVENT_SEEK_B,
    FFEVENT_SEEK_F_F,
    FFEVENT_SEEK_F_B,
    FFEVENT_CLOSE
} FFEVENT;

typedef struct EventHeader EventHeader;

typedef struct
{
    EventHeader *(*const create)();
    void (*const destroy)(EventHeader *handle);
    void (*const push)(EventHeader *handle, int elem);
    int (*const pop)(EventHeader *handle);
    int (*const isEmpty)(EventHeader *handle);
} _EventQueue;

extern _EventQueue const EventQueue;

#endif