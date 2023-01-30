#include "event_queue.h"

#include <stdio.h>
#include <stdlib.h>

int main()
{
    EventHeader *handle = EventQueue.create();

    int elems[101];
    for (int i = 0; i <= 100; i++)
    {
        elems[i] = i;
        EventQueue.push(handle, i);
    }

    int mem;
    while ((mem = EventQueue.pop(handle)) != -1)
    {
        printf("%i\n", mem);
    }

    EventQueue.destroy(handle);

    return 0;
}