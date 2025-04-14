#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>
#include "queue.h"

#define MAX_PROCESSES 18
#define MAX_TOTAL_PROCESSES 100
#define MAX_BLOCKED 18
#define BASE_QUANTUM 10000000  // 10 ms in ns
#define MSG_KEY 1234
#define QUEUE_LOG_INTERVAL_NS 500000000 // 0.5 sec

struct timer {
    unsigned int seconds;
    unsigned int ns;
};

struct PCB {
    int occupied;
    pid_t pid;
    struct timer startTime;
    struct timer cpuTime;
    struct timer totalTime;
    int queueLevel; // 0 = highest, 2 = lowest
    int blocked;
    struct timer unblockTime;
};

struct msgbuf {
    long mtype;
    pid_t pid;
    int quantum;
    int used_time;
};

char errmsg[200];
int shmidClock, shmidPCB, msgid;
struct timer *simClock;
struct PCB *pcbTable;
FILE *logFile;

Queue queues[3]; // multi-level feedback queues
Queue blockedQueue;

int procCreated = 0, activeProcs = 0, idleTime = 0;
int queueStats[3] = {0};
int blockedCount = 0;
time_t startRealTime;

void cleanup(int code) {
    msgctl(msgid, IPC_RMID, NULL);
    shmdt(simClock);
    shmctl(shmidClock, IPC_RMID, NULL);
    shmdt(pcbTable);
    shmctl(shmidPCB, IPC_RMID, NULL);
    if (logFile) fclose(logFile);
    exit(code);
}

void sigHandler(int sig) {
    fprintf(stderr, "OSS: SIGINT received, cleaning up...\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcbTable[i].occupied) {
            kill(pcbTable[i].pid, SIGTERM);
        }
    }
    cleanup(1);
}

void advanceClock(unsigned int ns) {
    simClock->ns += ns;
    while (simClock->ns >= 1000000000) {
        simClock->seconds += 1;
        simClock->ns -= 1000000000;
    }
}

void forkUser(int index) {
    pid_t child = fork();
    if (child == 0) {
        char clk[10], pcb[10], idx[10];
        sprintf(clk, "%d", shmidClock);
        sprintf(pcb, "%d", shmidPCB);
        sprintf(idx, "%d", index);
        execl("./user", "user", clk, pcb, idx, NULL);
        perror("OSS: execl failed");
        exit(1);
    }

    pcbTable[index].pid = child;
    pcbTable[index].occupied = 1;
    pcbTable[index].startTime = *simClock;
    pcbTable[index].cpuTime.seconds = pcbTable[index].cpuTime.ns = 0;
    pcbTable[index].totalTime = *simClock;
    pcbTable[index].queueLevel = 0;
    enqueue(&queues[0], index);
    procCreated++;
    activeProcs++;

    fprintf(logFile, "OSS: Generating process with PID %d and putting in queue 0 at time %u:%u\n",
            child, simClock->seconds, simClock->ns);
}

void unblockProcesses() {
    for (int i = 0; i < MAX_BLOCKED; i++) {
        int pid = blockedQueue.items[i];
        if (pid == -1 || !pcbTable[pid].blocked) continue;

        if ((simClock->seconds > pcbTable[pid].unblockTime.seconds) ||
            (simClock->seconds == pcbTable[pid].unblockTime.seconds &&
             simClock->ns >= pcbTable[pid].unblockTime.ns)) {

            pcbTable[pid].blocked = 0;
            enqueue(&queues[0], pid);
            fprintf(logFile, "OSS: Unblocking process PID %d at time %u:%u, returning to queue 0\n",
                    pcbTable[pid].pid, simClock->seconds, simClock->ns);
            dequeueSpecific(&blockedQueue, pid);
        }
    }
}

void dispatchProcess(int index) {
    int level = pcbTable[index].queueLevel;
    int quantum = BASE_QUANTUM * (1 << level);

    struct msgbuf msg;
    msg.mtype = pcbTable[index].pid;
    msg.pid = pcbTable[index].pid;
    msg.quantum = quantum;
    msg.used_time = 0;

    advanceClock((rand() % 10000) + 100); // dispatch overhead

    msgsnd(msgid, &msg, sizeof(struct msgbuf) - sizeof(long), 0);
    fprintf(logFile, "OSS: Dispatching PID %d from queue %d at time %u:%u\n",
            msg.pid, level, simClock->seconds, simClock->ns);

    struct msgbuf rcv;
    msgrcv(msgid, &rcv, sizeof(struct msgbuf) - sizeof(long), 1, 0);

    int used = abs(rcv.used_time);
    advanceClock(used);

    if (rcv.used_time < 0) {
        fprintf(logFile, "OSS: PID %d terminated after %d ns\n", msg.pid, used);
        pcbTable[index].occupied = 0;
        activeProcs--;
    } else if (used < quantum) {
        fprintf(logFile, "OSS: PID %d blocked after %d ns, moving to blocked queue\n", msg.pid, used);
        pcbTable[index].blocked = 1;
        pcbTable[index].unblockTime.seconds = simClock->seconds + (rand() % 5);
        pcbTable[index].unblockTime.ns = simClock->ns + (rand() % 1000);
        enqueue(&blockedQueue, index);
    } else {
        pcbTable[index].queueLevel = (level < 2) ? level + 1 : 2;
        enqueue(&queues[pcbTable[index].queueLevel], index);
        fprintf(logFile, "OSS: PID %d used full quantum, moving to queue %d\n",
                msg.pid, pcbTable[index].queueLevel);
    }

    pcbTable[index].cpuTime.ns += used;
    queueStats[level]++;
}

void logQueueState() {
    fprintf(logFile, "OSS: Queue states at time %u:%u\n", simClock->seconds, simClock->ns);
    for (int i = 0; i < 3; i++) {
        fprintf(logFile, "Queue %d: ", i);
        printQueue(&queues[i], logFile);
    }
    fprintf(logFile, "Blocked Queue: ");
    printQueue(&blockedQueue, logFile);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigHandler);

    shmidClock = shmget(IPC_PRIVATE, sizeof(struct timer), IPC_CREAT | 0666);
    shmidPCB = shmget(IPC_PRIVATE, sizeof(struct PCB) * MAX_PROCESSES, IPC_CREAT | 0666);
    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);

    simClock = (struct timer*)shmat(shmidClock, NULL, 0);
    pcbTable = (struct PCB*)shmat(shmidPCB, NULL, 0);

    memset(simClock, 0, sizeof(struct timer));
    memset(pcbTable, 0, sizeof(struct PCB) * MAX_PROCESSES);

    for (int i = 0; i < 3; i++) initQueue(&queues[i]);
    initQueue(&blockedQueue);

    logFile = fopen("msglog.out", "w");
    if (!logFile) {
        perror("OSS: fopen log file failed");
        cleanup(1);
    }

    startRealTime = time(NULL);
    struct timer lastLogTime = {0, 0};

    while (procCreated < MAX_TOTAL_PROCESSES || activeProcs > 0) {
        if (procCreated < MAX_TOTAL_PROCESSES) {
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (!pcbTable[i].occupied) {
                    forkUser(i);
                    break;
                }
            }
        }

        unblockProcesses();

        int selected = -1;
        for (int q = 0; q < 3; q++) {
            if (!isEmpty(&queues[q])) {
                selected = dequeue(&queues[q]);
                break;
            }
        }

        if (selected != -1) {
            dispatchProcess(selected);
        } else {
            advanceClock(100000); // CPU idle
            idleTime++;
        }

        if ((simClock->seconds > lastLogTime.seconds) ||
            (simClock->seconds == lastLogTime.seconds &&
             simClock->ns >= lastLogTime.ns + QUEUE_LOG_INTERVAL_NS)) {
            logQueueState();
            lastLogTime = *simClock;
        }

        if ((time(NULL) - startRealTime) >= 3) break;
    }

    fprintf(logFile, "OSS: Termination. Total idle time: %d, Queue stats: [Q0: %d, Q1: %d, Q2: %d]\n",
            idleTime, queueStats[0], queueStats[1], queueStats[2]);

    cleanup(0);
}

