#include "event_queue.h"
#include <stdlib.h>
#include <pthread.h>

typedef struct EventElement
{
    void *next;
    int event;
} EventElement;

struct EventHeader
{
    EventElement *head;
    EventElement *tail;
    pthread_mutex_t *mutex;
};

static EventHeader *create();
static EventHeader *create()
{
    EventHeader *handle = malloc(sizeof(*handle));
    handle->head = NULL;
    handle->tail = NULL;

    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    handle->mutex = mutex;
    pthread_mutex_init(handle->mutex, NULL);

    return handle;
}

static void destroy(EventHeader *header);
static void destroy(EventHeader *header)
{
    EventElement *head = header->head;

    while (head != NULL)
    {
        header->head = head->next;
        free(head);
        head = header->head;
    }

    pthread_mutex_destroy(header->mutex);
    header->mutex = NULL;
    header->head = NULL;
    header->tail = NULL;
    free(header);
    header = NULL;
}

static void push(EventHeader *header, int elem);
static void push(EventHeader *header, int elem)
{
    EventElement *element = malloc(sizeof(*element));
    element->event = elem;
    element->next = NULL;

    pthread_mutex_lock(header->mutex);

    if (header->head == NULL)
    {
        header->head = element;
        header->tail = element;
    }
    else
    {
        EventElement *oldTail = header->tail;
        oldTail->next = element;
        header->tail = element;
    }
    pthread_mutex_unlock(header->mutex);
}

static int pop(EventHeader *header);
static int pop(EventHeader *header)
{
    pthread_mutex_lock(header->mutex);
    EventElement *head = header->head;

    if (head == NULL)
    {
        pthread_mutex_unlock(header->mutex);
        return -1;
    }
    else
    {
        header->head = head->next;
        int event = head->event;
        free(head);

        pthread_mutex_unlock(header->mutex);
        return event;
    }
}

static int isEmpty(EventHeader *header);
static int isEmpty(EventHeader *header)
{
    pthread_mutex_lock(header->mutex);
    EventElement *head = header->head;

    if (head == NULL)
    {
        pthread_mutex_unlock(header->mutex);
        return 1;
    }
    else
    {
        pthread_mutex_unlock(header->mutex);
        return 0;
    }
}

_EventQueue const EventQueue = {
    create,
    destroy,
    push,
    pop,
    isEmpty};