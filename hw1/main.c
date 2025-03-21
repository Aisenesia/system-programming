#include <dirent.h>  // required for opendir
#include <fcntl.h>   // for open mode macros and locks
// #include <stdio.h>
#include <stdlib.h>
#include <string.h>    // strlen, sprintf
#include <sys/stat.h>  // required for mkdir
#include <sys/wait.h>  // required for wait
#include <time.h>      // required for logging time
#include <unistd.h>

#define LOG_FILE "log.txt"

// Custom function to convert integer to string
void intToStr(int num, char* str) {
    int i = 0;
    int isNegative = 0;

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }

    if (num < 0) {
        isNegative = 1;
        num = -num;
    }

    while (num != 0) {
        str[i++] = (num % 10) + '0';
        num = num / 10;
    }

    if (isNegative) str[i++] = '-';

    str[i] = '\0';

    // Reverse the string
    for (int j = 0; j < i / 2; j++) {
        char temp = str[j];
        str[j] = str[i - j - 1];
        str[i - j - 1] = temp;
    }
}

// Custom function to concatenate strings
void strConcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

void createLogMessage(char* buffer, const char* message) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timeStr[20];

    intToStr(t->tm_year + 1900, timeStr);
    strConcat(buffer, "[");
    strConcat(buffer, timeStr);
    strConcat(buffer, "-");

    if (t->tm_mon + 1 < 10) strConcat(buffer, "0");
    intToStr(t->tm_mon + 1, timeStr);
    strConcat(buffer, timeStr);
    strConcat(buffer, "-");

    if (t->tm_mday < 10) strConcat(buffer, "0");
    intToStr(t->tm_mday, timeStr);
    strConcat(buffer, timeStr);
    strConcat(buffer, " ");

    if (t->tm_hour < 10) strConcat(buffer, "0");
    intToStr(t->tm_hour, timeStr);
    strConcat(buffer, timeStr);
    strConcat(buffer, ":");

    if (t->tm_min < 10) strConcat(buffer, "0");
    intToStr(t->tm_min, timeStr);
    strConcat(buffer, timeStr);
    strConcat(buffer, ":");

    if (t->tm_sec < 10) strConcat(buffer, "0");
    intToStr(t->tm_sec, timeStr);
    strConcat(buffer, timeStr);
    strConcat(buffer, "] ");
    strConcat(buffer, message);
    strConcat(buffer, "\n");
}

// Helper to log operations
void writeLog(const char* message) {
    int fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;

    char logStr[512] = {0};
    createLogMessage(logStr, message);
    write(fd, logStr, strlen(logStr));
    close(fd);
}

void writeMsg(const char* message) {
    char str[512] = {0};
    strConcat(str, message);
    strConcat(str, "\n");
    write(STDOUT_FILENO, str, strlen(str));
}

// Custom function to create message
void createMessage(char* buffer, const char* prefix, const char* name,
                   const char* suffix) {
    buffer[0] = '\0';  // Initialize the buffer
    strConcat(buffer, prefix);
    strConcat(buffer, name);
    strConcat(buffer, suffix);
}

// Create Directory
void createDir(const char* arg) {
    // mkdir
    char msg[256] = "";
    if (mkdir(arg, 0755) == 0) {  // 0755 is the permission, execution is
                                  // required to list the directory
        createMessage(msg, "Directory \"", arg, "\" created successfully.");
    } else {
        createMessage(msg, "Error: Directory \"", arg, "\" already exists.");
    }
    writeMsg(msg);
    writeLog(msg);
}

// Create File
void createFile(const char* fileName) {
    char msg[256] = "";

    // check if file already exists
    if (access(fileName, F_OK) == 0) {
        createMessage(msg, "Error: File \"", fileName, "\" already exists.");

    } else {
        // use file descriptors to create file
        int fd = open(fileName, O_WRONLY | O_CREAT,
                      0644);  // 0644 is the permission rw-r--r--
        if (fd < 0) {
            createMessage(msg, "Error creating file \"", fileName, "\".");
            writeMsg(msg);
            writeLog(msg);
            return;
        }

        time_t now = time(NULL);
        char timeStr[256] = "Created at: ";
        strConcat(timeStr, ctime(&now));  // create file content

        write(fd, timeStr, strlen(timeStr));  // write content to file
        close(fd);                            // close file descriptor

        createMessage(msg, "File \"", fileName, "\" created successfully.");
    }

    // log operation
    writeMsg(msg);
    writeLog(msg);
}

// List Directory
void listDir(const char* folderName) {
    pid_t pid = fork();  // fork the process
    // if pid is 0, then it is the child process
    if (pid == 0) {
        DIR* dir = opendir(folderName);  // open directory
        if (!dir) {
            char errMsg[256] = "";
            createMessage(errMsg, "Error: Directory \"", folderName,
                          "\" not found.");
            writeMsg(errMsg);
            writeLog(errMsg);
            _exit(0);
        }

        struct dirent* entry;
        char header[256] = "";
        createMessage(header, "Contents of \"", folderName, "\":\n");
        write(STDOUT_FILENO, header, strlen(header));
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                write(STDOUT_FILENO, entry->d_name, strlen(entry->d_name));
                write(STDOUT_FILENO, "\n", 1);
            }
        }
        closedir(dir);
        _exit(0);
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
            char errMsg[256] = "";
            createMessage(errMsg, "Error: Directory \"", folderName,
                          "\" not found.");
            write(STDOUT_FILENO, errMsg, strlen(errMsg));
            _exit(0);
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
            char noFilesMsg[256] = "";
            createMessage(noFilesMsg, "No files with extension \"", extension,
                          "\" found in \"");
            strConcat(noFilesMsg, folderName);
            strConcat(noFilesMsg, "\".\n");
            write(STDOUT_FILENO, noFilesMsg, strlen(noFilesMsg));
        }
        closedir(dir);
        _exit(0);
    } else {
        wait(NULL);
    }
}

// Read File
void readFile(const char* fileName) {
    // use file descriptors to read file
    int fd = open(fileName, O_RDONLY);
    if (fd < 0) {
        char msg[256] = "";
        createMessage(msg, "Error reading file \"", fileName, "\".");
        writeLog(msg);
        return;
    }

    char buffer[1024];
    int bytesRead = read(fd, buffer, sizeof(buffer));
    if (bytesRead < 0) {
        char msg[256] = "";
        createMessage(msg, "Error reading file \"", fileName, "\".");
        writeLog(msg);
        close(fd);
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
    char msg[256] = "";
    if (fd < 0) {
        createMessage(msg, "Error: Cannot open file \"", fileName,
                      "\" for appending.");
        writeLog(msg);
        return;
    }

    // lock the file
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    if (fcntl(fd, F_SETLK, &lock) == -1) {
        createMessage(msg, "Error: Cannot lock file \"", fileName,
                      "\" for appending.");
        writeLog(msg);

        close(fd);
        return;
    }

    // write content to file
    int len = write(fd, content, strlen(content));
    if (len < 0) {
        createMessage(msg, "Error: Write operation failed, unlocking file \"",
                      fileName, "\".");
        writeLog(msg);
        return;
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

    createMessage(msg, "Content appended to file \"", fileName,
                  "\" successfully.");
    writeLog(msg);
}

void deleteFile(const char* fileName) {
    pid_t pid = fork();
    if (pid == 0) {  // Child process
        if (access(fileName, F_OK) != 0) {
            char msg[256] = "";
            createMessage(msg, "Error: File \"", fileName, "\" not found.");
            writeMsg(msg);
            writeLog(msg);
            _exit(0);
        } else if (access(fileName, W_OK) != 0) {
            char msg[256] = "";
            createMessage(msg, "Error: File \"", fileName,
                          "\" is write-protected.");
            writeMsg(msg);
            writeLog(msg);
            _exit(0);
        }

        if (unlink(fileName) == 0) {
            char msg[256] = "";
            createMessage(msg, "File \"", fileName, "\" deleted successfully.");
            writeMsg(msg);
            writeLog(msg);
        } else {
            char msg[256] = "Error: File deletion failed.";
            writeMsg(msg);
            writeLog(msg);
        }
        _exit(0);
    } else {
        wait(NULL);
    }
}

// Delete Directory
void deleteDir(const char* folderName) {
    // use fork and do rmdir in child process
    pid_t pid = fork();
    if (pid == 0) {
        if (rmdir(folderName) == 0) {
            char msg[256] = "";
            createMessage(msg, "Directory \"", folderName,
                          "\" deleted successfully.");
            writeMsg(msg);
            writeLog(msg);
        } else {
            char errMsg[256] = "";
            createMessage(errMsg, "Directory \"", folderName,
                          "\" is not empty.\n");
            writeMsg(errMsg);
            writeLog(errMsg);
        }
        _exit(0);  // use _exit to terminate the child process
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

    if (strcmp(argv[1], "createDir") == 0 && argc == 3)
        createDir(argv[2]);
    else if (strcmp(argv[1], "createFile") == 0 && argc == 3)
        createFile(argv[2]);
    else if (strcmp(argv[1], "listDir") == 0 && argc == 3)
        listDir(argv[2]);
    else if (strcmp(argv[1], "listFilesByExtension") == 0 && argc == 4)
        listFilesByExtension(argv[2], argv[3]);
    else if (strcmp(argv[1], "readFile") == 0 && argc == 3)
        readFile(argv[2]);
    else if (strcmp(argv[1], "appendToFile") == 0 && argc == 4)
        appendToFile(argv[2], argv[3]);
    else if (strcmp(argv[1], "deleteFile") == 0 && argc == 3)
        deleteFile(argv[2]);
    else if (strcmp(argv[1], "deleteDir") == 0 && argc == 3)
        deleteDir(argv[2]);
    else if (strcmp(argv[1], "showLogs") == 0 && argc == 2)
        showLogs();
    else
        printUsage();

    return 0;
}