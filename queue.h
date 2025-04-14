#ifndef QUEUE_H
#define QUEUE_H

#define MAX_QUEUE_SIZE 20

typedef struct {
    int items[MAX_QUEUE_SIZE];
    int front;
    int rear;
} Queue;

void initQueue(Queue* q);
int isEmpty(Queue* q);
int isFull(Queue* q);
void enqueue(Queue* q, int value);
int dequeue(Queue* q);
void dequeueSpecific(Queue* q, int value);
void printQueue(Queue* q, FILE *fp);

#endif
