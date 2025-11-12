#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include <stdarg.h>

#define TYPE_CPU_INTENSIVE 0
#define TYPE_IO_INTENSIVE 1
#define TYPE_MIXED 2

#define MLFQ_DEBUG 1//be used for print info. DONE.

void busy_wait(int ticks) {
    volatile int i =0;
    while (i<ticks) {
        i++;
    }
}

void cpu_worker(int id, int duration) {
    int pid = getpid();
    int work_units = duration / 100;
    
    printf("[START] PID %d (CPU Worker %d) started - will do %d work units\n", 
                  pid, id, work_units);
    
    for (int i = 0; i < work_units; i++) {
        busy_wait(1000*work_units); 
#if MLFQ_DEBUG
        if (i % 100 == 0) {
            printf("[PRIORITY] PID %d (CPU Worker %d) at priority level %d, completed %d/%d work units\n", 
                      pid, id, getpriority(), i, work_units);
        }
#endif 
    }//DONE.
    
    printf("[Finish] PID %d (CPU Worker %d) completed all %d work units\n", 
                  pid, id, work_units);
    exit(0);
}

void io_worker(int id, int duration) {
    int pid = getpid();
    int io_operations = duration / 200;
    
    printf("[TEST] PID %d (IO Worker %d) started - will do %d IO operations\n", 
                  pid, id, io_operations);
    for (int i = 0; i < io_operations; i++) {
        busy_wait(50);   
#if MLFQ_DEBUG
        printf("[PRIORITY] PID %d (IO Worker %d) at priority level %d, completed operation %d/%d\n", 
                      pid, id, getpriority(), i + 1, io_operations);
#endif 
        pause(1);        
    }//DONE. 

    
    printf("[TEST] PID %d (IO Worker %d) completed all %d IO operations\n", 
                  pid, id, io_operations);
    exit(0);
}


void mixed_worker(int id, int duration) {
    int pid = getpid();
    int cycles = duration / 300;
    
    printf("[TEST] PID %d (Mixed Worker %d) started - will do %d cycles\n", 
                  pid, id, cycles);
    for (int i = 0; i < cycles; i++) {
        busy_wait(150);
 #if MLFQ_DEBUG
        printf("[PRIORITY] PID %d (Mixed Worker %d) at priority level %d, completed cycle %d/%d\n", 
                      pid, id, getpriority(), i + 1, cycles);
#endif 
        pause(8);
        busy_wait(50);
    }//DONE. 
    printf("[TEST] PID %d (Mixed Worker %d) completed all %d cycles\n", 
                  pid, id, cycles);
    exit(0);
}

int main(int argc, char *argv[]) {

#if MLFQ_DEBUG
    printf("[PARENT] PID %d at priority level %d\n", getpid(), getpriority());//DONE. 
#endif
    
    printf("\n");
    printf("===============================================\n");
    printf("=        MLFQ Scheduler Test Program         =\n");
    printf("=        Testing SECH under MLFQ            =\n");
    printf("===============================================\n");
    printf("===============================================\n\n");
    
        printf("[TEST] Starting basic MLFQ test with mixed workloads\n");
        int start_ticks = uptime();
        if (fork() == 0) cpu_worker(1, 200000);    
        if (fork() == 0) io_worker(1, 2000);      
        if (fork() == 0) cpu_worker(1, 250000); 
        if (fork() == 0) cpu_worker(2, 180000);    
        if (fork() == 0) io_worker(2, 2200);      
        //if (fork() == 0) mixed_worker(1, 10000);//DONE. 
    int children = 0;
    children = 5;

    
    printf("[TEST] Waiting for %d child processes to complete...\n", children);
    
    int completed = 0;
    while (completed < children) {
        int status;
        int pid = wait(&status);
        if (pid > 0) {
            
            printf("[TEST] Child PID %d exited with status %d\n", pid, status);
            completed++;
        }
    }
    int end_ticks = uptime();
    
    printf("\n");
    printf("===============================================\n");
    printf("=           Processes completed: %d          =\n", completed);
    printf("===============================================\n\n");
    
 #if MLFQ_DEBUG
    printf("[PARENT] PID %d at priority level %d\n", getpid(), getpriority());
#endif
    printf("Throughput: start:%d, end:%d\n, number of processes:%d\n",start_ticks,end_ticks,completed);//DONE. 
    exit(0);
}