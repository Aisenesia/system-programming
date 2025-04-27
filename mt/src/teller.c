#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>


#include <sys/types.h>
#include <sys/stat.h>

pid_t Teller(void *func, void *arg) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("Error creating process");
        return -1;
    }
    if (pid == 0) {
        // Child process
        ((void (*)(void *))func)(arg); // Execute the function passed as argument
        exit(0); // Exit after execution
    }

    // Parent process
    return pid; // Return the child's PID
}

int waitTeller(pid_t pid, int* status){
    return waitpid(pid, status, WNOHANG); // Wait for the specific child process
}