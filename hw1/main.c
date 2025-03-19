#include <dirent.h>  // required for opendir
#include <fcntl.h>   // for open mode macros
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>  // required for mkdir
#include <sys/wait.h>  // required for wait
#include <time.h>
#include <unistd.h>

#define LOG_FILE "log.txt"

// Helper to log operations
void writeLog(const char* message) {
    // log format: [2025-03-19 15:28:16] Directory "test" created successfully.

    // open log file using file descriptor
    int fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;

    // get current time
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char logStr[512];
    // create log string
    int len =
        snprintf(logStr, sizeof(logStr), "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour,
                 t->tm_min, t->tm_sec, message);
    write(fd, logStr, len);
    close(fd);
}

// Create Directory
void createDir(const char* arg) {
    // mkdir
    char msg[256];
    if (mkdir(arg, 0755) == 0) {  // 0755 is the permission, execution is
                                  // required to list the directory
        int len = snprintf(msg, sizeof(msg),
                           "Directory \"%s\" created successfully.", arg);
        write(STDOUT_FILENO, msg, len);
        write(STDOUT_FILENO, "\n", 1);
    } else {
        int len = snprintf(msg, sizeof(msg),
                           "Error: Directory \"%s\" already exists.\n", arg);
        write(STDOUT_FILENO, msg, len);
    }
    writeLog(msg);
}

// Create File
void createFile(const char* fileName) {
    char msg[256];

    // check if file already exists
    if (access(fileName, F_OK) == 0) {
        int len = snprintf(msg, sizeof(msg),
                           "Error: File \"%s\" already exists.\n", fileName);
        write(STDOUT_FILENO, msg, len);
    } else {
        // use file descriptors to create file
        int fd = open(fileName, O_WRONLY | O_CREAT,
                      0644);  // 0644 is the permission rw-r--r--
        if (fd < 0) {
            perror("Error creating file, file descriptor is invalid");
            return;
        }

        time_t now = time(NULL);
        int len = snprintf(msg, sizeof(msg), "Created at: %s",
                           ctime(&now));  // create file content

        write(fd, msg, len);  // write content to file
        close(fd);            // close file descriptor

        len = snprintf(msg, sizeof(msg), "File \"%s\" created successfully.",
                       fileName);
        write(STDOUT_FILENO, msg, len);
        write(STDOUT_FILENO, "\n", 1);
    }

    // log operation
    writeLog(msg);
}

// List Directory
void listDir(const char* folderName) {
    pid_t pid = fork();  // fork the process
    // if pid is 0, then it is the child process
    if (pid == 0) {
        DIR* dir = opendir(folderName);  // open directory
        if (!dir) {
            char errMsg[256];
            int len =
                snprintf(errMsg, sizeof(errMsg),
                         "Error: Directory \"%s\" not found.\n", folderName);
            write(STDOUT_FILENO, errMsg, len);
            exit(0);
        }

        struct dirent* entry;
        char header[256];
        int len = snprintf(header, sizeof(header), "Contents of \"%s\":\n",
                           folderName);
        write(STDOUT_FILENO, header, len);
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                write(STDOUT_FILENO, entry->d_name, strlen(entry->d_name));
                write(STDOUT_FILENO, "\n", 1);
            }
        }
        closedir(dir);
        exit(0);
    } else {
        wait(NULL);
    }
}

// List Files by Extension
void listFilesByExtension(const char* folderName, const char* extension) {
    pid_t pid = fork();
    if (pid == 0) {  // Child
        DIR* dir = opendir(folderName);
        if (!dir) {
            char errMsg[256];
            int len =
                snprintf(errMsg, sizeof(errMsg),
                         "Error: Directory \"%s\" not found.\n", folderName);
            write(STDOUT_FILENO, errMsg, len);
            exit(0);
        }

        int found = 0;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strstr(entry->d_name, extension)) {
                write(STDOUT_FILENO, entry->d_name, strlen(entry->d_name));
                write(STDOUT_FILENO, "\n", 1);
                found = 1;
            }
        }
        if (!found) {
            char noFilesMsg[256];
            int len =
                snprintf(noFilesMsg, sizeof(noFilesMsg),
                         "No files with extension \"%s\" found in \"%s\".\n",
                         extension, folderName);
            write(STDOUT_FILENO, noFilesMsg, len);
        }
        closedir(dir);
        exit(0);
    } else {
        wait(NULL);
    }
}

// Read File
void readFile(const char* fileName) {
    // use file descriptors to read file
    int fd = open(fileName, O_RDONLY);
    if (fd < 0) {
        perror("Error reading file, file descriptor is invalid");
        return;
    }

    char buffer[1024];
    int bytesRead = read(fd, buffer, sizeof(buffer));
    if (bytesRead < 0) {
        perror("Error reading file, read operation failed");
        return;
    }

    buffer[bytesRead] = '\0';                 // null terminate the buffer
    write(STDOUT_FILENO, buffer, bytesRead);  // print file content
    write(STDOUT_FILENO, "\n", 1);            // print newline
    close(fd);                                // close file descriptor
}

// Append to File with Lock
void appendToFile(const char* fileName, const char* content) {
    // use file descriptors to append to file
    int fd = open(fileName, O_WRONLY | O_APPEND);
    char msg[256];
    if (fd < 0) {
        snprintf(msg, "Error: Cannot open file \"%s\" for appending.", fileName);
        perror(msg);
        writeLog(msg);
        return;
    }

    // lock the file
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    if (fcntl(fd, F_SETLK, &lock) == -1) {
        snprintf(msg, "Error: Cannot lock file \"%s\" for appending.", fileName);
        perror(msg);
        writeLog(msg);

        close(fd);
        return;
    }

    // write content to file
    int len = write(fd, content, strlen(content));
    if (len < 0) {
        snprintf(msg, "Error: Write operation failed, unlocking file \"%s\".", fileName);
        perror(msg);
        writeLog(msg);
        // unlock the file
        lock.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &lock);
        close(fd);
        return;
    }

    // unlock the file
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    close(fd);  // close file descriptor

    snprintf(msg, "Content appended to file \"%s\" successfully.", fileName);
    writeLog(msg);
}

void deleteFile(const char* fileName) {
    if (unlink(fileName) == 0) {
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "File \"%s\" deleted successfully.", fileName);
        write(STDOUT_FILENO, msg, len);
        write(STDOUT_FILENO, "\n", 1);
        writeLog(msg);
    } else {
        char* msg = "Error: File deletion failed.";
        perror(msg);
        writeLog(msg);
    }
}

// Delete Directory
void deleteDir(const char* folderName) {
    // use fork and do rmdir in child process
    pid_t pid = fork();
    if (pid == 0) {
        if (rmdir(folderName) == 0) {
            char msg[256];
            int len = snprintf(msg, sizeof(msg),
                               "Directory \"%s\" deleted successfully.",
                               folderName);
            write(STDOUT_FILENO, msg, len);
            write(STDOUT_FILENO, "\n", 1);
            writeLog(msg);
        } else {
            char errMsg[256];
            int len = snprintf(errMsg, sizeof(errMsg),
                               "Directory \"%s\" is not empty.\n", // Directory "folderName" is not empty
                               folderName);
            write(STDOUT_FILENO, errMsg, len);
            writeLog(errMsg);
        }
        exit(0);
    } else {
        wait(NULL);
    }
}

// Show Logs
void showLogs() { readFile(LOG_FILE); }

// Usage Guide
void printUsage() {
    const char* usage =
        "Usage: fileManager <command> [arguments]\n"
        "Commands:\n"
        "createDir \"folderName\"                   - Create a new directory\n"
        "createFile \"fileName\"                    - Create a new file\n"
        "listDir \"folderName\"                     - List all files in a "
        "directory\n"
        "listFilesByExtension \"folderName\" \".ext\" - List files with "
        "specific extension\n"
        "readFile \"fileName\"                      - Read a file's content\n"
        "appendToFile \"fileName\" \"new content\"    - Append content to a "
        "file\n"
        "deleteFile \"fileName\"                    - Delete a file\n"
        "deleteDir \"folderName\"                   - Delete an empty "
        "directory\n"
        "showLogs                                  - Display operation logs\n";

    write(STDOUT_FILENO, usage, strlen(usage));
}
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 0;
    }

    if (strcmp(argv[1], "createDir") == 0)
        createDir(argv[2]);
    else if (strcmp(argv[1], "createFile") == 0)
        createFile(argv[2]);
    else if (strcmp(argv[1], "listDir") == 0)
        listDir(argv[2]);
    else if (strcmp(argv[1], "listFilesByExtension") == 0)
        listFilesByExtension(argv[2], argv[3]);
    else if (strcmp(argv[1], "readFile") == 0)
        readFile(argv[2]);
    else if (strcmp(argv[1], "appendToFile") == 0)
        appendToFile(argv[2], argv[3]);
    else if (strcmp(argv[1], "deleteFile") == 0)
        deleteFile(argv[2]);
    else if (strcmp(argv[1], "deleteDir") == 0)
        deleteDir(argv[2]);
    else if (strcmp(argv[1], "showLogs") == 0)
        showLogs();
    else
        printUsage();

    return 0;
}