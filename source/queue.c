#include <stdio.h>
#include "queue.h"

void addElement(struct Queue *queue, int value) {
    queue->values[queue->tail] = value;
    queue->tail = (queue->tail + 1) & QUEUE_MASK;
}

int popElement(struct Queue *queue) {
    int value = queue->values[queue->head];
    queue->head = (queue->head + 1) & QUEUE_MASK;
    return value;
}

bool isEmpty(struct Queue *queue) {
    return queue->head == queue->tail;
}

void printQueue(struct Queue *queue) {
    int i = queue->head;
    while (i != queue->tail) {
        printf("%d ", queue->values[i]);
        i = (i + 1) & QUEUE_MASK;
    }
    printf("\n");
}
