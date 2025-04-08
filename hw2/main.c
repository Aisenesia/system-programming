// #define _POSIX_C_SOURCE 200809L // not required to compile/run but if not added vscode will show errors.

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

volatile sig_atomic_t children_reaped = 0;
const int total_children = 2;

pid_t child_pids[2];
time_t child_start_times[2];
int daemon_pid = 0;

// Function prototypes

void setup_signal_handlers();
void create_child_processes();
void create_daemon_process(int pipefd[2]);

void daemonize(int pipefd[2]);
void monitor_children();

void default_handler();
void handle_parent(int sig);
void daemon_signal_handler(int sig);

void child1();
void child2();

void create_fifos();
void write_fifo(int num1, int num2);
void cleanup_fifos();

void log_message(const char *message);
void parse_arguments(int argc, char *argv[], int *num1, int *num2);


/*
    Main function:
    Parses command line arguments for two integers.
    Creates FIFOs for inter-process communication.
    Sets up signal handlers for the parent process.
    Creates child processes and a daemon process.
    Writes the integers to FIFO1.
    Waits for child processes to finish and cleans up resources.
*/
int main(int argc, char *argv[]) {
    int num1, num2;
    parse_arguments(argc, argv, &num1, &num2);

    printf("Main process PID: %d\n", getpid());
    printf("Starting program with integers: %d and %d\n", num1, num2);

    create_fifos();
    printf("FIFOs created successfully\n");

    setup_signal_handlers();

    create_child_processes();

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    }

    create_daemon_process(pipefd);

    printf("Daemon process created with PID: %d\n", daemon_pid);
    printf("Child processes created with PIDs: %d and %d\n", child_pids[0], child_pids[1]);

    write_fifo(num1, num2);
    printf("Integers written to FIFO1: %d and %d\n", num1, num2);

    while (children_reaped < total_children) {
        sleep(2);
        printf("Proceeding...\n");
    }

    cleanup_fifos();
    printf("FIFOs cleaned up successfully\n");

    kill(daemon_pid, SIGTERM);
    printf("Daemon process terminated: %d\n", daemon_pid);

    waitpid(-1, NULL, 0);
    return 0;
}



void setup_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = handle_parent;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/*
    Creates two child processes, sets their PIDs and start times.
*/
void create_child_processes() {
    pid_t child1_pid = fork();
    if (child1_pid < 0) {
        perror("Failed to fork child1");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    } else if (child1_pid == 0) {
        child1();
        exit(0);
    }

    pid_t child2_pid = fork();
    if (child2_pid < 0) {
        perror("Failed to fork child2");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    } else if (child2_pid == 0) {
        child2();
        exit(0);
    }

    child_pids[0] = child1_pid;
    child_pids[1] = child2_pid;
    child_start_times[0] = time(NULL);
    child_start_times[1] = time(NULL);
}

/*
    Creates a daemon process.
    The parent process creates a pipe and forks a child process.
    The child process becomes the daemon and monitors the child processes.
    The parent process closes the write end of the pipe and reads the updated daemon PID.
*/
void create_daemon_process(int pipefd[2]) {
    pid_t daemon_fork_pid = fork();
    if (daemon_fork_pid < 0) {
        perror("Failed to create daemon process");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    } else if (daemon_fork_pid == 0) {
        close(pipefd[0]);
        daemonize(pipefd);
        monitor_children();
        exit(0);
    }

    close(pipefd[1]);

    if (read(pipefd[0], &daemon_pid, sizeof(daemon_pid)) <= 0) {
        perror("Failed to read updated daemon PID");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    }
    close(pipefd[0]);
}

// Signal Handlers

/*
    Signal handler for the parent process.
    Cleans up FIFOs and terminates child processes, then daemon process.
*/
void handle_parent(int sig) {
    pid_t pid;
    int status;

    switch (sig) {
        case SIGINT:
            for (int i = 0; i < total_children; i++) {
                kill(child_pids[i], SIGINT);
            }
            kill(daemon_pid, SIGINT);
            printf("%d : Received SIGINT. Cleaning up...\n", getpid());
            cleanup_fifos();
            exit(0);
        case SIGTERM:
            for (int i = 0; i < total_children; i++) {
                kill(child_pids[i], SIGTERM);
            }
            for (int i = 0; i < total_children; i++) {
                waitpid(child_pids[i], &status, 0);
            }
            kill(daemon_pid, SIGTERM);
            printf("Daemon process terminated: %d\n", daemon_pid);
            printf("%d : Received SIGTERM. Cleaning up...\n", getpid());
            cleanup_fifos();
            exit(0);
        case SIGCHLD:
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                char buffer[256];
                for (int i = 0; i < total_children; i++) {
                    if (pid == child_pids[i]) {
                        children_reaped++;
                        break;
                    }
                }
                if (WIFEXITED(status)) {
                    sprintf(buffer, "Child process %d exited with status %d", pid, WEXITSTATUS(status));
                    printf("%s\n", buffer);
                } else if (WIFSIGNALED(status)) {
                    sprintf(buffer, "Child process %d terminated by signal %d, status: %d", pid, WTERMSIG(status), status);
                    printf("%s\n", buffer);
                } else {
                    sprintf(buffer, "Child process %d terminated unexpectedly", pid);
                    printf("%s\n", buffer);
                }
            }
            break;
        default:
            printf("Unhandled signal %d received\n", sig);
    }
}


/*
    Signal handler for the daemon process.
    Handles signals like SIGUSR1, SIGHUP, SIGTERM, and SIGINT.
    Only SIGTERM and SIGINT will terminate the daemon process.
    SIGUSR1 and SIGHUP are placeholders.
*/
void daemon_signal_handler(int sig) {
    char buffer[256];
    switch (sig) {
        case SIGUSR1:
            log_message("Received SIGUSR1 signal");
            break;
        case SIGHUP:
            log_message("Received SIGHUP signal - reconfiguring");
            break;
        case SIGTERM:
        case SIGINT:
            log_message(sig == SIGTERM ? "Received SIGTERM signal - shutting down gracefully" : "Received SIGINT signal - cleaning up");
            unlink(FIFO1);
            unlink(FIFO2);
            exit(0);
        default:
            sprintf(buffer, "Received unhandled signal: %d", sig);
            log_message(buffer);
    }
}

/*
    Default handler, parent and daemon has customized handlers and so this is used to revert main's handler when in children.
*/
void default_handler() {
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}

// Daemon Process

/*
    Becomes session leader and detaches from terminal.
    Forks a child process and exits the parent.
    The child process becomes the daemon and redirects standard input to /dev/null, output and error to a log file.
    Sets up signal handlers for the daemon process.
*/
void daemonize(int pipefd[2]) {
    pid_t pid;

    if (setsid() < 0) {
        perror("setsid failed");
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        perror("Second fork failed");
        exit(1);
    }
    if (pid > 0) {
        write(pipefd[1], &pid, sizeof(pid));
        close(pipefd[1]);
        exit(0);
    }

    chdir("/");
    umask(0);

    for (int i = 0; i < 1024; i++) {
        close(i);
    }

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }

    int log_fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    } else {
        int fallback_fd = open("/dev/null", O_RDWR);
        if (fallback_fd >= 0) {
            dup2(fallback_fd, STDOUT_FILENO);
            dup2(fallback_fd, STDERR_FILENO);
            close(fallback_fd);
        }
    }

    struct sigaction sa;
    sa.sa_handler = daemon_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // daemon is created after the children to be able to have the pids without taking them from a pipe, since thats the case
    // only way we can log the start time of the children is to do it here, using the saved start times and pids.
    char buffer[256];
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&child_start_times[0]));
    printf("[%s] Child process %d started\n", time_str, child_pids[0]);

    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&child_start_times[1]));
    printf("[%s] Child process %d started\n", time_str, child_pids[1]);
}

/*
    Monitors child processes and checks if they are alive.
    If a child process has not responded for 20 seconds, it is terminated.
    If a child process exits, it is reaped and logged.
    The function runs in an infinite loop, checking the status of child processes.
    It does not self termiante, waits for parent to signal it to terminate.
*/
void monitor_children() {
    time_t current_time;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "Daemon process started monitoring with PID: %d", getpid());
    log_message(buffer);

    int exited[] = {0, 0};

    while (1) {
        current_time = time(NULL);
        for (int i = 0; i < total_children; i++) {
            if (child_pids[i] > 0) {
                if (kill(child_pids[i], 0) == 0) {
                    if (current_time - child_start_times[i] > 20 && !exited[i]) {
                        sprintf(buffer, "Child process %d has timed out - terminating", child_pids[i]);
                        log_message(buffer);
                        kill(child_pids[i], SIGTERM);
                        exited[i] = 1;
                    }
                } else if (!exited[i]) {
                    exited[i] = 1;
                    sprintf(buffer, "Child process %d has exited - reaping", child_pids[i]);
                    log_message(buffer);
                }
            }
        }
    }
}

// Child Processes

/*
    Child process 1 job:
    Sleeps for 10 seconds.
    Opens FIFO1 in read mode and reads two integers from it.
    Finds the larger of the two integers.
    Opens FIFO2 in write mode and writes the result to it.
    Prints the result to stdout.
*/
void child1() {
    default_handler();
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

/*
    Child process 2 job:
    Sleeps for 10 seconds.
    Opens FIFO2 in read mode and reads the result from it.
    Prints the result to stdout.
*/
void child2() {
    default_handler();
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

// FIFO functions

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

/*
    Opens fifo in non-blocking manner, if it cant it prints proceeding and sleeps
    Retries after sleeping in a loop.
*/
void write_fifo(int num1, int num2) {
    int fd;
    while (1) {
        fd = open(FIFO1, O_WRONLY | O_NONBLOCK);
        if (fd != -1) {
            write(fd, &num1, sizeof(num1));
            write(fd, &num2, sizeof(num2));
            close(fd);
            printf("Integers %d and %d written to FIFO1\n", num1, num2);
            break;
        } else {
            printf("Proceeding...\n");
            sleep(2);
        }
    }
}

void cleanup_fifos() {
    unlink(FIFO1);
    unlink(FIFO2);
}


// Utility

/*
    Logs a message to the logfile with a timestamp
*/
void log_message(const char *message) {
    FILE *log_fp = fopen(LOGFILE, "a");
    if (log_fp) {
        time_t now = time(NULL);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(log_fp, "[%s] %s\n", time_str, message);
        fclose(log_fp);
    }
}

/*
    Used to check validity of arguments passed to the program.
*/
void parse_arguments(int argc, char *argv[], int *num1, int *num2) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <int1> <int2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    *num1 = atoi(argv[1]);
    *num2 = atoi(argv[2]);

    if ((*num1 == 0 && strcmp(argv[1], "0") != 0) || (*num2 == 0 && strcmp(argv[2], "0") != 0)) {
        fprintf(stderr, "Error: Arguments are not valid integers.\n");
        exit(EXIT_FAILURE);
    }
}
