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
int maxUserProcesses = 5; // Default max concurrent processes
int maxRuntime = 3; // Default runtime in seconds
char logFileName[256] = "msglog.out"; // Default log file name

// Function prototypes
void cleanup(int code);
void sigHandler(int sig);
void advanceClock(unsigned int ns);
void forkUser(int index);
void unblockProcesses();
void dispatchProcess(int index);
void logQueueState();
void printUsage();

// Clean up resources and exit
void cleanup(int code) {
    fprintf(stderr, "OSS: Cleaning up resources...\n");

    // Close message queue
    if (msgid != -1) {
        msgctl(msgid, IPC_RMID, NULL);
    }

    // Detach and remove shared memory segments
    if (simClock != NULL) {
        shmdt(simClock);
        if (shmidClock != -1) {
            shmctl(shmidClock, IPC_RMID, NULL);
        }
    }

    if (pcbTable != NULL) {
        shmdt(pcbTable);
        if (shmidPCB != -1) {
            shmctl(shmidPCB, IPC_RMID, NULL);
        }
    }

    // Close log file
    if (logFile != NULL) {
        fclose(logFile);
    }

    exit(code);
}

// Signal handler for SIGINT and SIGALRM
void sigHandler(int sig) {
    fprintf(stderr, "OSS: Signal %d received, cleaning up...\n", sig);

    // Terminate all active child processes
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcbTable[i].occupied) {
            kill(pcbTable[i].pid, SIGTERM);
            waitpid(pcbTable[i].pid, NULL, 0); // Wait for child to terminate
        }
    }

    cleanup(1);
}

// Advance the simulated system clock
void advanceClock(unsigned int ns) {
    simClock->ns += ns;
    while (simClock->ns >= 1000000000) {
        simClock->seconds += 1;
        simClock->ns -= 1000000000;
    }
}

// Create a new user process
void forkUser(int index) {
    pid_t child = fork();
    if (child == -1) {
        perror("OSS: fork failed");
        return;
    }

    if (child == 0) {
        // Child process
        char clk[20], pcb[20], idx[20];
        sprintf(clk, "%d", shmidClock);
        sprintf(pcb, "%d", shmidPCB);
        sprintf(idx, "%d", index);

        execl("./user", "user", clk, pcb, idx, NULL);

        // If execl fails
        perror("OSS: execl failed");
        exit(1);
    }

    // Parent process
    // Update process control block
    pcbTable[index].pid = child;
    pcbTable[index].occupied = 1;
    pcbTable[index].startTime = *simClock;
    pcbTable[index].cpuTime.seconds = 0;
    pcbTable[index].cpuTime.ns = 0;
    pcbTable[index].totalTime = *simClock;
    pcbTable[index].queueLevel = 0;
    pcbTable[index].blocked = 0;
    // Add to the highest priority queue
    enqueue(&queues[0], index);

    procCreated++;
    activeProcs++;

    fprintf(logFile, "OSS: Generating process with PID %d and putting in queue 0 at time %u:%u\n",
            child, simClock->seconds, simClock->ns);
}

// Check and unblock processes that are ready
void unblockProcesses() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!pcbTable[i].occupied || !pcbTable[i].blocked) continue;

        // Check if it's time to unblock this process
        if ((simClock->seconds > pcbTable[i].unblockTime.seconds) ||
            (simClock->seconds == pcbTable[i].unblockTime.seconds &&
             simClock->ns >= pcbTable[i].unblockTime.ns)) {

            pcbTable[i].blocked = 0;
            // Unblocked processes go to the highest priority queue
            enqueue(&queues[0], i);
            fprintf(logFile, "OSS: PID %d unblocked and returned to queue 0 at %u:%u\n",
                    pcbTable[i].pid, simClock->seconds, simClock->ns);
            dequeueSpecific(&blockedQueue, i);
        }
    }
}

// Dispatch a process for execution
void dispatchProcess(int index) {
    int level = pcbTable[index].queueLevel;
    int quantum = BASE_QUANTUM * (1 << level); // Double quantum for each lower level

    // Prepare message to send to user process
    struct msgbuf msg;
    msg.mtype = pcbTable[index].pid; // Set message type to PID so child can receive it
    msg.pid = pcbTable[index].pid;
    msg.quantum = quantum;
    msg.used_time = 0;

    // Add dispatch overhead to simulate context switch
    advanceClock((rand() % 10000) + 100);

    // Send message to user process with quantum information
    if (msgsnd(msgid, &msg, sizeof(struct msgbuf) - sizeof(long), 0) == -1) {
        perror("OSS: msgsnd failed");
        return;
    }

    fprintf(logFile, "OSS: Dispatching PID %d from queue %d at time %u:%u\n",
            msg.pid, level, simClock->seconds, simClock->ns);

    // Wait for response from user process
    struct msgbuf rcv;
    if (msgrcv(msgid, &rcv, sizeof(struct msgbuf) - sizeof(long), 1, 0) == -1) {
        perror("OSS: msgrcv failed");
        return;
    }

    // Process the response
    int used = abs(rcv.used_time);
    advanceClock(used);

    if (rcv.used_time < 0) {
        // Process terminated
        fprintf(logFile, "OSS: PID %d terminated after %d ns\n", rcv.pid, used);
        pcbTable[index].occupied = 0;
        activeProcs--;
    } else if (used < quantum) {
        // Process blocked
        fprintf(logFile, "OSS: PID %d blocked after %d ns, moving to blocked queue\n", rcv.pid, used);
        pcbTable[index].blocked = 1;

        // Set time when process will be unblocked
        pcbTable[index].unblockTime.seconds = simClock->seconds;
        pcbTable[index].unblockTime.ns = simClock->ns + (rand() % 500000000);

        // Adjust if ns overflows
        if (pcbTable[index].unblockTime.ns >= 1000000000) {
            pcbTable[index].unblockTime.seconds++;
            pcbTable[index].unblockTime.ns -= 1000000000;
        }

        enqueue(&blockedQueue, index);
    } else {
        // Process used full quantum, demote it to next queue level
        pcbTable[index].queueLevel = (level < 2) ? level + 1 : 2;
        enqueue(&queues[pcbTable[index].queueLevel], index);
        fprintf(logFile, "OSS: PID %d used full quantum, moving to queue %d\n",
                rcv.pid, pcbTable[index].queueLevel);
    }

    // Update CPU usage statistics
    pcbTable[index].cpuTime.ns += used;
    if (pcbTable[index].cpuTime.ns >= 1000000000) {
        pcbTable[index].cpuTime.seconds += pcbTable[index].cpuTime.ns / 1000000000;
        pcbTable[index].cpuTime.ns %= 1000000000;
    }

    queueStats[level]++;
}

// Log the current state of all queues
void logQueueState() {
    fprintf(logFile, "OSS: Queue states at time %u:%u\n", simClock->seconds, simClock->ns);

    for (int i = 0; i < 3; i++) {
        fprintf(logFile, "Queue %d: ", i);
        printQueue(&queues[i], logFile);
    }

    fprintf(logFile, "Blocked Queue: ");
    printQueue(&blockedQueue, logFile);
}

// Print usage information
void printUsage() {
    printf("Usage: ./oss [-s max_user_processes] [-l log_file] [-t max_runtime]\n");
    printf("  -s: Maximum number of concurrent user processes (default: 5)\n");
    printf("  -l: Log file name (default: msglog.out)\n");
    printf("  -t: Maximum runtime in seconds (default: 3)\n");
    printf("  -h: Show this help message\n");
}

// Main function
int main(int argc, char *argv[]) {
    int opt;

    // Initialize resources to invalid values for error checking
    shmidClock = shmidPCB = msgid = -1;
    simClock = NULL;
    pcbTable = NULL;
    logFile = NULL;

    // Parse command line arguments
    while ((opt = getopt(argc, argv, "hs:l:t:")) != -1) {
        switch (opt) {
            case 'h':
                printUsage();
                exit(0);
            case 's':
                maxUserProcesses = atoi(optarg);
                if (maxUserProcesses <= 0 || maxUserProcesses > MAX_PROCESSES) {
                    fprintf(stderr, "Invalid number of user processes. Using default (5).\n");
                    maxUserProcesses = 5;
                }
                break;
            case 'l':
                strncpy(logFileName, optarg, sizeof(logFileName) - 1);
                break;
            case 't':
                maxRuntime = atoi(optarg);
                if (maxRuntime <= 0) {
                    fprintf(stderr, "Invalid runtime. Using default (3).\n");
                    maxRuntime = 3;
                }
                break;
            default:
                printUsage();
                exit(1);
        }
    }

    // Set up signal handling
    signal(SIGINT, sigHandler);
    signal(SIGALRM, sigHandler);

    // Set alarm for cleanup
    alarm(maxRuntime);

    // Initialize the shared memory for clock
    shmidClock = shmget(IPC_PRIVATE, sizeof(struct timer), IPC_CREAT | 0666);
    if (shmidClock == -1) {
        perror("OSS: shmget failed for clock");
        cleanup(1);
    }

    // Initialize the shared memory for PCB table
    shmidPCB = shmget(IPC_PRIVATE, sizeof(struct PCB) * MAX_PROCESSES, IPC_CREAT | 0666);
    if (shmidPCB == -1) {
        perror("OSS: shmget failed for PCB");
        cleanup(1);
    }

    // Create message queue
    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("OSS: msgget failed");
        cleanup(1);
    }

    // Attach to shared memory segments
    simClock = (struct timer*)shmat(shmidClock, NULL, 0);
    if (simClock == (void*)-1) {
        perror("OSS: shmat failed for clock");
        simClock = NULL;
        cleanup(1);
    }

    pcbTable = (struct PCB*)shmat(shmidPCB, NULL, 0);
    if (pcbTable == (void*)-1) {
        perror("OSS: shmat failed for PCB");
        pcbTable = NULL;
        cleanup(1);
    }

    // Initialize the clock and PCB table
    memset(simClock, 0, sizeof(struct timer));
    memset(pcbTable, 0, sizeof(struct PCB) * MAX_PROCESSES);

    // Initialize queues
    for (int i = 0; i < 3; i++) {
        initQueue(&queues[i]);
    }
    initQueue(&blockedQueue);

    // Open log file
    logFile = fopen(logFileName, "w");
    if (!logFile) {
        perror("OSS: fopen log file failed");
        cleanup(1);
    }

    // Initialize random seed
    srand(time(NULL));

    // Record start time
    startRealTime = time(NULL);

    // Initialize last log time
    struct timer lastLogTime = {0, 0};

    // Main simulation loop
    while ((procCreated < MAX_TOTAL_PROCESSES || activeProcs > 0) &&
           (time(NULL) - startRealTime) < maxRuntime) {

        // Check if we should create a new process
        if (procCreated < MAX_TOTAL_PROCESSES && activeProcs < maxUserProcesses) {
            // Find an empty slot in the PCB table
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (!pcbTable[i].occupied) {
                    forkUser(i);
                    break;
                }
            }
        }

        // Check for unblocked processes
        unblockProcesses();

        // Select a process to run from the highest priority non-empty queue
        int selected = -1;
        for (int q = 0; q < 3; q++) {
            if (!isEmpty(&queues[q])) {
                selected = dequeue(&queues[q]);
                break;
            }
        }

        // Dispatch the selected process or advance the clock if idle
        if (selected != -1) {
            dispatchProcess(selected);
        } else {
            advanceClock(100000); // CPU idle time - 100 microseconds
            idleTime++;
        }

        // Log queue states periodically
        if ((simClock->seconds > lastLogTime.seconds) ||
            (simClock->seconds == lastLogTime.seconds &&
             simClock->ns >= lastLogTime.ns + QUEUE_LOG_INTERVAL_NS)) {
            logQueueState();
            lastLogTime = *simClock;
        }
    }

    // Log final statistics
    fprintf(logFile, "OSS: Simulation complete.\n");
    fprintf(logFile, "OSS: Total processes created: %d\n", procCreated);
    fprintf(logFile, "OSS: Total idle time: %d\n", idleTime);
    fprintf(logFile, "OSS: Queue usage statistics: [Q0: %d, Q1: %d, Q2: %d]\n",
            queueStats[0], queueStats[1], queueStats[2]);
    // Calculate and log average wait time and CPU utilization if needed

    // Clean up resources
    cleanup(0);

    return 0; // This line is never reached due to cleanup()
}
