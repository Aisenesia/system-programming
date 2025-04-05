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
#include <bits/sigaction.h>
#include <asm-generic/signal-defs.h>


#define _POSIX_C_SOURCE 200809L


#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOGFILE "/tmp/daemon.log"

volatile sig_atomic_t children_reaped =
    0;  // atomic variable to track reaped children, atomic provides
        // interruption safety
const int total_children = 2;

pid_t child_pids[2];          // Array to store child PIDs
time_t child_start_times[2];  // Array to store child start times
int child_count = 0;          // Number of children spawned
int exit_counter = 0;  // Counter for exited children
int daemon_pid = 0;  // PID of the daemon process


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


/* SIGCHLD handler to reap terminated child processes
   (implements zombie protection and prints exit statuses) */
void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        char buffer[256];
        sprintf(buffer, "Child process %d has terminated", pid);
        printf("%s\n", buffer);
        
        if (WIFEXITED(status)) {
            sprintf(buffer, "Child process %d exited with status %d", pid, WEXITSTATUS(status));
            printf("%s\n", buffer);
        } else if (WIFSIGNALED(status)) {
            sprintf(buffer, "Child process %d terminated by signal %d", pid, WTERMSIG(status));
            printf("%s\n", buffer);
        }
        
        exit_counter += 2;
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
            // Code to reconfigure the daemon would go here
            break;
        case SIGTERM:
            log_message("Received SIGTERM signal - shutting down gracefully");
            // Cleanup before exiting
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

    // Fork and exit parent - this detaches from controlling terminal
    pid = fork();
    if (pid < 0) {
        perror("First fork failed");
        exit(1);
    }
    if (pid > 0) {
        // Parent exits
        exit(0);
    }

    // Child becomes session leader
    if (setsid() < 0) {
        perror("setsid failed");
        exit(1);
    }

    // Fork again to ensure process cannot acquire a controlling terminal
    pid = fork();
    if (pid < 0) {
        perror("Second fork failed");
        exit(1);
    }
    if (pid > 0) {
        // Intermediate child writes the updated PID to the pipe and exits
        printf("Daemon PID: %d\n", pid);
        write(pipefd[1], &pid, sizeof(pid));
        close(pipefd[1]);  // Close the write end of the pipe
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

        // Redirect stdout and stderr to log file
        int log_fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        } else {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        close(null_fd);
    }

    // Set up signal handlers
    signal(SIGUSR1, daemon_signal_handler);
    signal(SIGHUP, daemon_signal_handler);
    signal(SIGTERM, daemon_signal_handler);

    char buffer[256];
    snprintf(buffer, sizeof(buffer), "Daemon process started with PID: %d", (int)getpid());
    log_message(buffer);
}

/* Function to monitor child processes */
void monitor_children() {
    time_t current_time;
    char buffer[256];

    int child_count = 2;
    
    while (1) {
        current_time = time(NULL);
        
        // Check each child process
        for (int i = 0; i < child_count; i++) {
            if (child_pids[i] > 0) {
                // Check if process is still running
                if (kill(child_pids[i], 0) == 0) {
                    // Process exists, check for timeout
                    if (current_time - child_start_times[i] > 20) {
                        sprintf(buffer, "Child process %d has timed out - terminating", child_pids[i]);
                        log_message(buffer);
                        kill(child_pids[i], SIGTERM);
                    }
                }
            }
            
        }
        
        sleep(5);  // Check every 5 seconds
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
    // Open FIFO1 for writing
    int fd = open(FIFO1, O_WRONLY);
    if (fd == -1) {
        perror("Error opening FIFO1");
        exit(EXIT_FAILURE);
    }

    // Write integers to FIFO1
    write(fd, &num1, sizeof(num1));
    write(fd, &num2, sizeof(num2));

    // Close FIFO1
    close(fd);
    printf("Integers %d and %d written to FIFO1\n", num1, num2);
}

/* Function to fork the first child */
void child1() {
    // Sleep for 10 seconds
    printf("Child 1: Sleeping for 10 seconds...\n");
    sleep(10);
    // Open FIFO1 for reading
    int fd = open(FIFO1, O_RDONLY);
    if (fd == -1) {
        perror("Error opening FIFO1");
        exit(EXIT_FAILURE);
    }

    // Read integers from FIFO1
    int num1, num2;
    read(fd, &num1, sizeof(num1));
    read(fd, &num2, sizeof(num2));

    // Close FIFO1
    close(fd);

    // Perform operation (e.g., find the larger number)
    int result = (num1 > num2) ? num1 : num2;

    // Open FIFO2 for writing
    fd = open(FIFO2, O_WRONLY);
    if (fd == -1) {
        perror("Error opening FIFO2");
        exit(EXIT_FAILURE);
    }

    // Write result to FIFO2
    write(fd, &result, sizeof(result));

    // Close FIFO2
    close(fd);
    printf("Child 1: The larger number is %d\n", result);
}

/* Function to fork the second child */
void child2() {
    printf("Child 2: Sleeping for 10 seconds...\n");

    // Sleep for 10 seconds
    sleep(10);
    // Open FIFO2 for reading
    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) {
        perror("Error opening FIFO2");
        exit(EXIT_FAILURE);
    }

    // Read result from FIFO2
    int result;
    read(fd, &result, sizeof(result));

    // Close FIFO2
    close(fd);

    printf("Child 2: Received result %d\n", result);
}

/* Function to clean up FIFOs */
void cleanup_fifos() {
    unlink(FIFO1);
    unlink(FIFO2);
}

/*

• Create a program that takes two integer arguments.
• Create two FIFOs (named pipes).
• Send the two integer values to the first FIFO.
• Send a command (e.g., determining the larger number) to the second FIFO.
• Use the fork() system call to create two child processes and assign each to a
FIFO.
• Convert the process into a daemon to run in the background and handle
logging operations.
• All child processes sleep for 10 seconds, execute their tasks, and then exit.
• Set a signal handler for SIGCHLD in the parent process to handle child process
termination.
• Enter a loop, printing a message containing "proceeding" every two seconds.
• The signal handler should call waitpid() to reap the terminated child process,
print out the process ID of the exited child, and increment a counter by two.
• When the counter reaches the number of children originally spawned, the
program exits.
• The daemon should handle SIGUSR1, SIGHUP, and SIGTERM signals to manage
process execution.

*/
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <int1> <int2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    // Convert command line arguments to integers, if they are not valid
    // integers, exit
    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    if (num1 == 0 && strcmp(argv[1], "0") != 0 ||
        num2 == 0 && strcmp(argv[2], "0") != 0) {
        // Check if the conversion was successful
        // If not, print an error message and exit
        // Note: atoi does not set errno, so we check if the string is "0" to
        // handle that case
        fprintf(stderr, "Error: Arguments are not valid integers.\n");
        exit(EXIT_FAILURE);
    }

    printf("Starting program with integers: %d and %d\n", num1, num2);

    int result = 0;  // Initialize result to zero as required

    create_fifos();
    printf("FIFOs created successfully\n");

    // Create a pipe to communicate with the daemon
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        cleanup_fifos();
        exit(EXIT_FAILURE);
    }

    // Daemonize the process
    pid_t daemon_pid = fork();
    if (daemon_pid < 0) {
        perror("Failed to create daemon process");
        cleanup_fifos();
        return 1;
    } else if (daemon_pid == 0) {
        // Close the read end of the pipe in the daemon
        close(pipefd[0]);

        // Convert process to daemon
        daemonize(pipefd);

        // Start monitoring child processes
        monitor_children();

        exit(0);
    }

    // Close the write end of the pipe in the parent
    close(pipefd[1]);

    // Read the updated PID of the daemon process from the pipe
    int updated_daemon_pid;
    if (read(pipefd[0], &updated_daemon_pid, sizeof(updated_daemon_pid)) <= 0) {
        perror("Failed to read updated daemon PID");
        cleanup_fifos();
        return 1;
    }
    close(pipefd[0]);

    printf("Daemon process created with PID: %d\n", updated_daemon_pid);

    // SIGCHLD signal handler
    // TODO: Implement signal handler for SIGCHLD to reap child processes
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("Error setting SIGCHLD handler");
        exit(EXIT_FAILURE);
    }

    // Fork the first child process
    pid_t child1_pid = fork();
    if (child1_pid < 0) {
        perror("Failed to fork child1");
        cleanup_fifos();
        return 1;
    } else if (child1_pid == 0) {
        // Child process 1
        child1();
        exit(0);
    }
    // Fork the second child process
    pid_t child2_pid = fork();
    if (child2_pid < 0) {
        perror("Failed to fork child2");
        cleanup_fifos();
        return 1;
    } else if (child2_pid == 0) {
        // Child process 2
        child2();
        exit(0);
    }
    // Parent process
    child_pids[0] = child1_pid;
    child_pids[1] = child2_pid;
    child_start_times[0] = time(NULL);
    child_start_times[1] = time(NULL);
    child_count = 2;
    printf("Child processes created with PIDs: %d and %d\n", child1_pid,
           child2_pid);

    // Write integers to FIFO1
    write_integers_to_fifo1(num1, num2);
    printf("Integers written to FIFO1: %d and %d\n", num1, num2);

    // parent process loop
    while (exit_counter < total_children) {
        // Sleep for 2 seconds
        sleep(2);
        // Print "proceeding" message
        printf("Proceeding...\n");
    }

    for (int i = 0; i < child_count; i++) {
        waitpid(child_pids[i], NULL, 0);
        printf("Child process %d reaped\n", child_pids[i]);
    }


    // Cleanup FIFOs
    cleanup_fifos();
    printf("FIFOs cleaned up successfully\n");

    // Wait for all child processes to exit
    
    // Kill the daemon process
    kill(updated_daemon_pid, SIGTERM);
    printf("Daemon process terminated : %d\n", updated_daemon_pid); // since daemon forks twice, it does not terminate
    // therefore we need to get the pid from a pipe
    

    // Wait for the daemon process to exit
    waitpid(daemon_pid, NULL, 0);
    printf("Daemon process reaped\n");
    // Exit the program
    exit(0);

    return 0;
}
