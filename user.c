#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <errno.h>

#define BILLION 1000000000
#define MSG_KEY 1234  // Must match oss.c

// Shared clock structure
struct SimClock {
    unsigned int seconds;
    unsigned int nanoseconds;
};

// Process Control Block structure
struct PCB {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
    int serviceTimeSeconds;
    int serviceTimeNano;
    int eventWaitSec;
    int eventWaitNano;
    int blocked;
};

// Message from OSS to user
struct msgbuf {
    long mtype;
    pid_t pid;
    int quantum;
    int used_time;
};

// Message from user to OSS
struct msgback {
    long mtype;
    int timeUsed;
};

// Globals
int shm_clock_id, shm_pcb_id;
struct SimClock *shmTime;
struct PCB *shmPCB;
int index;

// Cleanup handler
void cleanup(int signum) {
    shmdt(shmTime);
    shmdt(shmPCB);
    exit(signum);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <clockShmID> <pcbShmID> <index>\n", argv[0]);
        exit(1);
    }

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // Parse arguments
    shm_clock_id = atoi(argv[1]);
    shm_pcb_id = atoi(argv[2]);
    index = atoi(argv[3]);

    // Attach to shared memory
    shmTime = (struct SimClock *)shmat(shm_clock_id, NULL, 0);
    if (shmTime == (void *)-1) {
        perror("USER: Failed to attach to shared clock memory");
        exit(1);
    }

    shmPCB = (struct PCB *)shmat(shm_pcb_id, NULL, 0);
    if (shmPCB == (void *)-1) {
        perror("USER: Failed to attach to PCB memory");
        shmdt(shmTime);
        exit(1);
    }

    // Access message queue
    int msqid = msgget(MSG_KEY, 0666);
    if (msqid == -1) {
        perror("USER: Failed to get message queue");
        cleanup(1);
    }

    // Wait for a message from OSS with our time quantum
    struct msgbuf msg;
    if (msgrcv(msqid, &msg, sizeof(struct msgbuf) - sizeof(long), getpid(), 0) == -1) {
        perror("USER: Failed to receive message from OSS");
        cleanup(1);
    }
    srand(getpid() ^ time(NULL));

    int quantum = msg.quantum;
    int decision = rand() % 100;
    int usedTime = 0;
    int maxSlice = msg.quantum;


    usleep((rand() % 100 + 1));

    // Decide behavior based on random value
    if (decision < 25) {
        // Terminate early
        usedTime = (rand() % (maxSlice - 1000)) + 1000;
        struct msgback ret = { .mtype = 1, .timeUsed = -usedTime };
        msgsnd(msqid, &ret, sizeof(ret.timeUsed), 0);
    } else if (decision < 60) {
        // Block before using full quantum
        usedTime = (rand() % (maxSlice - 1000)) + 1000;
        struct msgback ret = { .mtype = 1, .timeUsed = usedTime };
        msgsnd(msqid, &ret, sizeof(ret.timeUsed), 0);
    } else {
        // Use full quantum
        usedTime = maxSlice;
        struct msgback ret = { .mtype = 1, .timeUsed = usedTime };
        msgsnd(msqid, &ret, sizeof(ret.timeUsed), 0);
    }

    // Optional: Log to stderr (for debugging)
    fprintf(stderr, "USER %d: Decision = %s, Time used = %d\n", getpid(),
        (decision < 10) ? "terminate" :
        (decision < 50) ? "block" : "full quantum",
        usedTime);

    // Detach shared memory
    shmdt(shmTime);
    shmdt(shmPCB);

    return 0;
}

