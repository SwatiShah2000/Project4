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

#define MSG_KEY 1234

// Shared clock structure
struct timer {
    unsigned int seconds;
    unsigned int ns;
};

// Process Control Block structure
struct PCB {
    int occupied;
    pid_t pid;
    struct timer startTime;
    struct timer cpuTime;
    struct timer totalTime;
    int queueLevel;
    int blocked;
    struct timer unblockTime;
};

// Message structure
struct msgbuf {
    long mtype;
    pid_t pid;
    int quantum;
    int used_time;
};

// Globals
int shmidClock, shmidPCB;
struct timer *simClock;
struct PCB *pcbTable;
int pcbIndex; // Changed from "index" to avoid conflict with built-in function
pid_t myPid;

// Cleanup handler
void cleanup(int signum) {
    shmdt(simClock);
    shmdt(pcbTable);
    exit(signum);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <clockShmID> <pcbShmID> <index>\n", argv[0]);
        exit(1);
    }
    
    // Set up signal handling
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    
    // Get process ID
    myPid = getpid();
    
    // Parse arguments
    shmidClock = atoi(argv[1]);
    shmidPCB = atoi(argv[2]);
    pcbIndex = atoi(argv[3]);
    
    // Attach to shared memory
    simClock = (struct timer *)shmat(shmidClock, NULL, 0);
    if (simClock == (void *)-1) {
        perror("USER: Failed to attach to shared clock memory");
        exit(1);
    }
    
    pcbTable = (struct PCB *)shmat(shmidPCB, NULL, 0);
    if (pcbTable == (void *)-1) {
        perror("USER: Failed to attach to PCB memory");
        shmdt(simClock);
        exit(1);
    }
    
    // Access message queue
    int msqid = msgget(MSG_KEY, 0666);
    if (msqid == -1) {
        perror("USER: Failed to get message queue");
        cleanup(1);
    }
    
    // Initialize random seed
    srand(getpid() ^ time(NULL));
    
    // Wait for a message from OSS with our time quantum
    struct msgbuf msg;
    if (msgrcv(msqid, &msg, sizeof(struct msgbuf) - sizeof(long), myPid, 0) == -1) {
        perror("USER: Failed to receive message from OSS");
        cleanup(1);
    }
    
    // Get assigned quantum
    int quantum = msg.quantum;
    
    // Decide behavior: terminate, block, or use full quantum
    int decision = rand() % 100;
    int usedTime = 0;
    
    if (decision < 25) {
        // Terminate early
        usedTime = -(rand() % (quantum - 1000) + 1000);
    } else if (decision < 60) {
        // Block before using full quantum
        usedTime = rand() % (quantum - 1000) + 1000;
    } else {
        // Use full quantum
        usedTime = quantum;
    }
    
    // Send response back to OSS
    msg.mtype = 1; // OSS is always mtype 1
    msg.used_time = usedTime;
    
    if (msgsnd(msqid, &msg, sizeof(struct msgbuf) - sizeof(long), 0) == -1) {
        perror("USER: Failed to send message to OSS");
        cleanup(1);
    }
    
    // Log to stderr for debugging
    fprintf(stderr, "USER %d: Decision = %s, Time used = %d\n", 
            myPid,
            (usedTime < 0) ? "terminate" : 
            (usedTime < quantum) ? "block" : "full quantum",
            abs(usedTime));
    
    // Detach from shared memory
    shmdt(simClock);
    shmdt(pcbTable);
    
    return 0;
}
