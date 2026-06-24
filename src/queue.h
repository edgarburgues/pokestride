#pragma once
#include <stdbool.h>

/* Static ring buffer — no malloc/free, power-of-2 capacity for fast masking */
#define QUEUE_CAPACITY 64
#define QUEUE_MASK     (QUEUE_CAPACITY - 1)

struct Queue {
    int values[QUEUE_CAPACITY];
    int head;
    int tail;
};

void addElement(struct Queue *queue, int value);
int  popElement(struct Queue *queue);
bool isEmpty(struct Queue *queue);
void printQueue(struct Queue *queue);
