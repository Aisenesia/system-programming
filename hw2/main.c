 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <sys/types.h>
 #include <sys/stat.h>
 #include <fcntl.h>
 #include <string.h>
 #include <signal.h>
 #include <sys/wait.h>
 #include <time.h>
 #include <errno.h>
 
 #define FIFO1 "fifo1"
 #define FIFO2 "fifo2"
 #define LOGFILE "/tmp/daemon.log"
 
 volatile sig_atomic_t children_reaped = 0;
 const int total_children = 2;
 
 /* SIGCHLD handler to reap terminated child processes
    (implements zombie protection and prints exit statuses) */
 void sigchld_handler(int sig) {
     int status;
     pid_t pid;
     // Use a loop to reap all terminated children.
     while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
         int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
         // Log to stdout (which may be redirected in daemon mode)
         printf("Child with PID %d terminated with exit status %d\n", pid, exit_status);
         fflush(stdout);
         children_reaped++;
     }
 }
 
 /* Signal handler for daemon signals */
 void daemon_signal_handler(int sig) {
     FILE *log = fopen(LOGFILE, "a");
     if (!log) return;
     time_t now = time(NULL);
     if(sig == SIGUSR1) {
         fprintf(log, "[%ld] Received SIGUSR1\n", now);
     } else if(sig == SIGHUP) {
         fprintf(log, "[%ld] Received SIGHUP (reconfiguring)\n", now);
     } else if(sig == SIGTERM) {
         fprintf(log, "[%ld] Received SIGTERM, shutting down daemon\n", now);
         fclose(log);
         exit(0);
     }
     fclose(log);
 }
 
 /* Function to convert the current process into a daemon */
 void become_daemon() {
     pid_t pid;
 
     // Fork the first time
     pid = fork();
     if (pid < 0) {
         perror("First fork failed");
         exit(EXIT_FAILURE);
     }
     if (pid > 0)
         exit(EXIT_SUCCESS); // Parent exits
 
     // Become session leader to lose controlling terminal
     if (setsid() < 0) {
         perror("setsid failed");
         exit(EXIT_FAILURE);
     }
 
     // Fork again so the daemon cannot reacquire a terminal.
     pid = fork();
     if (pid < 0) {
         perror("Second fork failed");
         exit(EXIT_FAILURE);
     }
     if (pid > 0)
         exit(EXIT_SUCCESS);
 
     // Change working directory to root and reset file mode mask.
     chdir("/");
     umask(0);
 
     // Redirect standard file descriptors to the log file.
     FILE *log = fopen(LOGFILE, "a");
     if (log) {
         dup2(fileno(log), STDOUT_FILENO);
         dup2(fileno(log), STDERR_FILENO);
         fclose(log);
     } else {
         perror("Failed to open log file");
         exit(EXIT_FAILURE);
     }
 
     // Set up signal handlers for daemon signals.
     signal(SIGUSR1, daemon_signal_handler);
     signal(SIGHUP, daemon_signal_handler);
     signal(SIGTERM, daemon_signal_handler);
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
     int fd_fifo1 = open(FIFO1, O_WRONLY);
     if (fd_fifo1 < 0) {
         perror("Parent: Cannot open FIFO1 for writing");
         exit(EXIT_FAILURE);
     }
     char num_buffer[100];
     snprintf(num_buffer, sizeof(num_buffer), "%d %d", num1, num2);
     if (write(fd_fifo1, num_buffer, strlen(num_buffer) + 1) == -1) {
         perror("Parent: Error writing numbers to FIFO1");
         exit(EXIT_FAILURE);
     }
     close(fd_fifo1);
 }
 
 /* Function to write command to FIFO2 */
 void write_command_to_fifo2() {
     int fd_fifo2 = open(FIFO2, O_WRONLY);
     if (fd_fifo2 < 0) {
         perror("Parent: Cannot open FIFO2 for writing");
         exit(EXIT_FAILURE);
     }
     char command[] = "COMPARE";
     if (write(fd_fifo2, command, strlen(command) + 1) == -1) {
         perror("Parent: Error writing command to FIFO2");
         exit(EXIT_FAILURE);
     }
     close(fd_fifo2);
 }
 
 /* Function to fork the first child */
 void fork_child1() {
     pid_t pid1 = fork();
     if (pid1 < 0) {
         perror("fork for first child failed");
         exit(EXIT_FAILURE);
     }
     if (pid1 == 0) {
         sleep(10);  // Simulate delay before processing
 
         int fd_read = open(FIFO1, O_RDONLY);
         if(fd_read < 0) {
             perror("Child 1: Error opening FIFO1 for reading");
             exit(EXIT_FAILURE);
         }
         char read_buffer[100] = {0};
         if(read(fd_read, read_buffer, sizeof(read_buffer)) <= 0) {
             perror("Child 1: Error reading from FIFO1");
             exit(EXIT_FAILURE);
         }
         close(fd_read);
 
         int a, b;
         if(sscanf(read_buffer, "%d %d", &a, &b) != 2) {
             fprintf(stderr, "Child 1: Failed to parse numbers\n");
             exit(EXIT_FAILURE);
         }
         int result = (a > b) ? a : b;
 
         // Open FIFO2 for writing the result (append mode)
         int fd_append = open(FIFO2, O_WRONLY | O_APPEND);
         if(fd_append < 0) {
             perror("Child 1: Error opening FIFO2 for writing");
             exit(EXIT_FAILURE);
         }
         char res_str[50];
         snprintf(res_str, sizeof(res_str), "%d", result);
         if(write(fd_append, res_str, strlen(res_str) + 1) == -1) {
             perror("Child 1: Error writing result to FIFO2");
             exit(EXIT_FAILURE);
         }
         close(fd_append);
         exit(EXIT_SUCCESS);
     }
 }
 
 /* Function to fork the second child */
 void fork_child2() {
     pid_t pid2 = fork();
     if (pid2 < 0) {
         perror("fork for second child failed");
         exit(EXIT_FAILURE);
     }
     if (pid2 == 0) {
         sleep(10);  // Simulate delay
 
         int fd_read2 = open(FIFO2, O_RDONLY);
         if(fd_read2 < 0) {
             perror("Child 2: Error opening FIFO2 for reading");
             exit(EXIT_FAILURE);
         }
         char fifo_data[100] = {0};
 
         /* Read the first message: the command */
         if(read(fd_read2, fifo_data, sizeof(fifo_data)) <= 0) {
             perror("Child 2: Error reading command from FIFO2");
             exit(EXIT_FAILURE);
         }
         // Optionally verify that fifo_data equals "COMPARE"
         if(strcmp(fifo_data, "COMPARE") != 0) {
             fprintf(stderr, "Child 2: Unexpected command received: %s\n", fifo_data);
         }
         /* Now read the second message: the result */
         char result_str[50] = {0};
         if(read(fd_read2, result_str, sizeof(result_str)) <= 0) {
             perror("Child 2: Error reading result from FIFO2");
             exit(EXIT_FAILURE);
         }
         close(fd_read2);
         printf("The larger number is: %s\n", result_str);
         exit(EXIT_SUCCESS);
     }
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

     create_fifos();

     signal(SIGCHLD, sigchld_handler);

     // Fork child processes first
     fork_child1();
     fork_child2();

     // Write to FIFOs after forking
     write_integers_to_fifo1(num1, num2);
     write_command_to_fifo2();

     //become_daemon();

     while (children_reaped < total_children) {
         printf("proceeding\n");
         fflush(stdout);
         sleep(2);
     }

     cleanup_fifos();
     return 0;
 }
