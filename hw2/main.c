#define _POSIX_C_SOURCE 200809L

#include <asm-generic/signal-defs.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOGFILE "/tmp/daemon.log"

volatile sig_atomic_t children_reaped =
    0;  // Atomic variable to track reaped children
const int total_children = 2;

pid_t main_pid;

pid_t child_pids[2];          // Array to store child PIDs
time_t child_start_times[2];  // Array to store child start times
int child_count = 0;          // Number of children spawned
int daemon_pid = 0;           // PID of the daemon process

void log_message(const char *message) {
    FILE *log_fp = fopen(LOGFILE, "a");
    if (log_fp) {
        time_t now = time(NULL);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
                 localtime(&now));
        fprintf(log_fp, "[%s] %s\n", time_str, message);
        fclose(log_fp);
    }
}

/* SIGCHLD handler to reap terminated child processes */
void sigchld_handler(int sig) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        char buffer[256];

        // Check if the child process is one of the monitored children
        for (int i = 0; i < child_count; i++) {
            if (pid == child_pids[i]) {
                children_reaped++;  // Increment reaped children counter
                break;
            }
        }

        if (WIFEXITED(status)) {
            sprintf(buffer, "Child process %d exited with status %d", pid,
                    WEXITSTATUS(status));
            printf("%s\n", buffer);
        } else if (WIFSIGNALED(status)) {
            sprintf(buffer, "Child process %d terminated by signal %d", pid,
                    WTERMSIG(status));
            printf("%s\n", buffer);
        } else {
            sprintf(buffer, "Child process %d terminated unexpectedly", pid);
            printf("%s\n", buffer);
        }
    }
}

/* Signal handler for daemon signals */
void daemon_signal_handler(int sig) {
    char buffer[256];

    switch (sig) {
        case SIGUSR1:
            log_message("Received SIGUSR1 signal");
            break;
        case SIGHUP:
            log_message("Received SIGHUP signal - reconfiguring");
            // idk what to do here
            break;
        case SIGTERM:
            log_message("Received SIGTERM signal - shutting down gracefully");
            unlink(FIFO1);
            unlink(FIFO2);
            exit(0);
            break;
        default:
            sprintf(buffer, "Received unhandled signal: %d", sig);
            log_message(buffer);
    }
}

/* Function to convert the current process into a daemon */
void daemonize(int pipefd[2]) {
    pid_t pid;

    // Child becomes session leader
    if (setsid() < 0) {
        perror("setsid failed");
        exit(1);
    }

    // Second fork
    pid = fork();
    if (pid < 0) {
        perror("Second fork failed");
        exit(1);
    }
    if (pid > 0) {
        // Intermediate child writes the updated PID to the pipe and exits
        write(pipefd[1], &pid, sizeof(pid));
        close(pipefd[1]);
        exit(0);
    }

    // Change working directory to root
    chdir("/");

    // Reset file creation mask
    umask(0);

    // Close all open file descriptors
    for (int i = 0; i < 1024; i++) {
        close(i);
    }

    // Redirect standard file descriptors to /dev/null
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }

    // Redirect stdout and stderr to the log file
    int log_fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    } else {
        // If log file cannot be opened, redirect to /dev/null
        int fallback_fd = open("/dev/null", O_RDWR);
        if (fallback_fd >= 0) {
            dup2(fallback_fd, STDOUT_FILENO);
            dup2(fallback_fd, STDERR_FILENO);
            close(fallback_fd);
        }
    }

    // Set up signal handlers
    signal(SIGUSR1, daemon_signal_handler);
    signal(SIGHUP, daemon_signal_handler);
    signal(SIGTERM, daemon_signal_handler);

    char buffer[256];

    int first_child_index = (child_pids[0] < child_pids[1]) ? 0 : 1;
    int second_child_index =
        1 - first_child_index;  // Automatically determine the other index

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
             localtime(&child_start_times[first_child_index]));
    printf("[%s] Child process %d started\n", time_str,
           child_pids[first_child_index]);

    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
             localtime(&child_start_times[second_child_index]));
    printf("[%s] Child process %d started\n", time_str,
           child_pids[second_child_index]);
}
/* Function to monitor child processes */
void monitor_children() {
    time_t current_time;
    char buffer[256];
    sprintf(buffer, "Daemon process started monitoring with PID: %d",
            (int)getpid());
    log_message(buffer);

    int exited[] = {0, 0};  // Array to track exited children

    // from the global variable children_start_times, log the children start
    // times, correctly by their order of start (sorted by time)

    while (1) {
        current_time = time(NULL);

        for (int i = 0; i < child_count; i++) {
            if (child_pids[i] > 0) {
                if (kill(child_pids[i], 0) == 0) {  // if child is alive
                    if (current_time - child_start_times[i] >
                        20) {  // if child has been alive for more than 20
                               // seconds
                        sprintf(buffer,
                                "Child process %d has timed out - terminating",
                                child_pids[i]);
                        log_message(buffer);
                        kill(child_pids[i], SIGTERM);
                    }
                } else {
                    if (!exited[i]) {   // if child has exited
                        exited[i] = 1;  // mark as exited
                        sprintf(buffer, "Child process %d has exited - reaping",
                                child_pids[i]);
                        log_message(buffer);
                    }
                }
                if (kill(main_pid, 0) !=
                    0) {  // Check if the main process is dead
                    if (errno ==
                        ESRCH) {  // ESRCH indicates the process does not exist
                        sprintf(
                            buffer,
                            "Main process %d has exited - terminating daemon", // daemon is now a zombie
                            main_pid);
                        log_message(buffer);
                        exit(0);  // Terminate the daemon process
                    } else {
                        sprintf(buffer, "Error checking main process %d: %s",
                                main_pid, strerror(errno));
                        log_message(buffer);
                    }
                }
            }
        }

        // sleep(2);  // Check every 5 seconds
    }
}

/* Function to create FIFOs */
void create_fifos() {
    if (mkfifo(FIFO1, 0666) == -1 && errno != EEXIST) {
        perror("Error creating FIFO1");
        exit(EXIT_FAILURE);
    }
    if (mkfifo(FIFO2, 0666) == -1 && errno != EEXIST) {
        perror("Error creating FIFO2");
        exit(EXIT_FAILURE);
    }
}

/* Function to write integers to FIFO1 */
void write_integers_to_fifo1(int num1, int num2) {
    int fd = open(FIFO1, O_WRONLY);  // make this non-blocking
    if (fd == -1) {
        perror("Error opening FIFO1");
        exit(EXIT_FAILURE);
    }

    write(fd, &num1, sizeof(num1));
    write(fd, &num2, sizeof(num2));
    close(fd);

    printf("Integers %d and %d written to FIFO1\n", num1, num2);
}

void write_fifo_child(int num1, int num2) {
    pid_t writer_pid = fork();

    if (writer_pid < 0) {
        perror("Failed to fork writer process");
        return;
    } else if (writer_pid == 0) {
        // Child process for writing to FIFO
        int fd =
            open(FIFO1, O_WRONLY);  // This will block until reader is available
        if (fd == -1) {
            perror("Error opening FIFO1");
            exit(EXIT_FAILURE);
        }

        write(fd, &num1, sizeof(num1));
        write(fd, &num2, sizeof(num2));
        close(fd);

        printf("Writer child: Integers %d and %d written to FIFO1\n", num1,
               num2);
        exit(0);  // Exit the writer child process
    }

    // Parent continues without waiting
    printf("Parent: Launched writer process with PID: %d\n", writer_pid);
}

/* Function to fork the first child */
void child1() {
    printf("Child 1: Sleeping for 10 seconds...\n");
    sleep(10);

    int fd = open(FIFO1, O_RDONLY);
    if (fd == -1) {
        perror("Error opening FIFO1");
        exit(EXIT_FAILURE);
    }

    int num1, num2;
    read(fd, &num1, sizeof(num1));
    read(fd, &num2, sizeof(num2));
    close(fd);

    int result = (num1 > num2) ? num1 : num2;

    fd = open(FIFO2, O_WRONLY);
    if (fd == -1) {
        perror("Error opening FIFO2");
        exit(EXIT_FAILURE);
    }

    write(fd, &result, sizeof(result));
    close(fd);

    printf("Child 1: The larger number is %d\n", result);
}

/* Function to fork the second child */
void child2() {
    printf("Child 2: Sleeping for 10 seconds...\n");
    sleep(10);

    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) {
        perror("Error opening FIFO2");
        exit(EXIT_FAILURE);
    }

    int result;
    read(fd, &result, sizeof(result));
    close(fd);

    printf("Child 2: Received result %d\n", result);
}

/* Function to clean up FIFOs */
void cleanup_fifos() {
    unlink(FIFO1);
    unlink(FIFO2);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <int1> <int2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    if ((num1 == 0 && strcmp(argv[1], "0") != 0) ||
        (num2 == 0 && strcmp(argv[2], "0") != 0)) {
        fprintf(stderr, "Error: Arguments are not valid integers.\n");
        exit(EXIT_FAILURE);
    }

    printf("Starting program with integers: %d and %d\n", num1, num2);

    create_fifos();
    printf("FIFOs created successfully\n");

    signal(SIGCHLD, sigchld_handler);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("Error setting SIGCHLD handler");
        exit(EXIT_FAILURE);
    }

    main_pid = getpid();

    pid_t child1_pid = fork();
    if (child1_pid < 0) {
        perror("Failed to fork child1");
        cleanup_fifos();
        return 1;
    } else if (child1_pid == 0) {
        child1();
        exit(0);
    }

    pid_t child2_pid = fork();
    if (child2_pid < 0) {
        perror("Failed to fork child2");
        cleanup_fifos();
        return 1;
    } else if (child2_pid == 0) {
        child2();
        exit(0);
    }

    child_pids[0] = child1_pid;
    child_pids[1] = child2_pid;
    child_start_times[0] = time(NULL);
    child_start_times[1] = time(NULL);
    child_count = total_children;  // Use total_children here

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    }

    pid_t daemon_fork_pid = fork();
    if (daemon_fork_pid < 0) {
        perror("Failed to create daemon process");
        cleanup_fifos();
        return 1;
    } else if (daemon_fork_pid == 0) {
        close(pipefd[0]);
        daemonize(pipefd);
        monitor_children();
        exit(0);
    }

    close(pipefd[1]);

    if (read(pipefd[0], &daemon_pid, sizeof(daemon_pid)) <=
        0) {  // Use global daemon_pid
        perror("Failed to read updated daemon PID");
        cleanup_fifos();
        return 1;
    }
    close(pipefd[0]);

    printf("Daemon process created with PID: %d\n", daemon_pid);

    printf("Child processes created with PIDs: %d and %d\n", child1_pid,
           child2_pid);

    // write_integers_to_fifo1(num1, num2);  //
    write_fifo_child(num1, num2);  // Use the new function to write to FIFO
    printf("Integers written to FIFO1: %d and %d\n", num1, num2);

    while (children_reaped < total_children) {  // Use children_reaped here
        sleep(2);
        printf("Proceeding...\n");
    }

    cleanup_fifos();
    printf("FIFOs cleaned up successfully\n");

    while (1);

    kill(daemon_pid, SIGTERM);  // Use global daemon_pid here
    printf("Daemon process terminated: %d\n", daemon_pid);

    waitpid(daemon_fork_pid, NULL, 0);
    exit(0);

    return 0;
}
