#include <stdio.h>
#include "queue.h"

void initQueue(Queue* q) {
    q->front = -1;
    q->rear = -1;
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        q->items[i] = -1;
    }
}

int isEmpty(Queue* q) {
    return q->front == -1;
}

int isFull(Queue* q) {
    return (q->rear + 1) % MAX_QUEUE_SIZE == q->front;
}

void enqueue(Queue* q, int value) {
    if (isFull(q)) return;
    if (isEmpty(q)) {
        q->front = 0;
        q->rear = 0;
    } else {
        q->rear = (q->rear + 1) % MAX_QUEUE_SIZE;
    }
    q->items[q->rear] = value;
}

int dequeue(Queue* q) {
    if (isEmpty(q)) return -1;
    int value = q->items[q->front];
    if (q->front == q->rear) {
        q->front = q->rear = -1;
    } else {
        q->front = (q->front + 1) % MAX_QUEUE_SIZE;
    }
    return value;
}

void dequeueSpecific(Queue* q, int value) {
    if (isEmpty(q)) return;
    int newItems[MAX_QUEUE_SIZE];
    int newFront = -1, newRear = -1;
    for (int i = q->front; i != q->rear; i = (i + 1) % MAX_QUEUE_SIZE) {
        if (q->items[i] == value) continue;
        if (newFront == -1) newFront = 0;
        newRear = (newRear + 1) % MAX_QUEUE_SIZE;
        newItems[newRear] = q->items[i];
    }
    if (q->items[q->rear] != value) {
        if (newFront == -1) newFront = 0;
        newRear = (newRear + 1) % MAX_QUEUE_SIZE;
        newItems[newRear] = q->items[q->rear];
    }
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        q->items[i] = -1;
    }
    q->front = newFront;
    q->rear = newRear;
    for (int i = 0; i <= newRear && newFront != -1; i++) {
        q->items[i] = newItems[i];
    }
}

void printQueue(Queue* q, FILE *fp) {
    if (isEmpty(q)) {
        fprintf(fp, "EMPTY\n");
        return;
    }
    for (int i = q->front; ; i = (i + 1) % MAX_QUEUE_SIZE) {
        fprintf(fp, "%d ", q->items[i]);
        if (i == q->rear) break;
    }
    fprintf(fp, "\n");
}
