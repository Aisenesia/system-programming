#include <dirent.h>    // required for opendir
#include <fcntl.h>     // for open mode macros and locks
#include <string.h>    // required for strlen, strcat
#include <sys/stat.h>  // required for mkdir and stat
#include <sys/wait.h>  // required for wait
#include <time.h>      // required for logging time
#include <unistd.h>    // required for read, write, close, fork

#define LOG_FILE "log.txt"

// function to convert integer to string, required for logging time since stdio
// is not allowed
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

// Helper to create stdout messages
void createMessage(char* buffer, const char* prefix, const char* name,
                   const char* suffix) {
    buffer[0] = '\0';  // Initialize the buffer
    strcat(buffer, prefix);
    strcat(buffer, name);
    strcat(buffer, suffix);
}

// Helper to create log messages
void createLogMessage(char* buffer, const char* message) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timeStr[20];

    intToStr(t->tm_year + 1900, timeStr);
    strcat(buffer, "[");
    strcat(buffer, timeStr);
    strcat(buffer, "-");

    if (t->tm_mon + 1 < 10) strcat(buffer, "0");
    intToStr(t->tm_mon + 1, timeStr);
    strcat(buffer, timeStr);
    strcat(buffer, "-");

    if (t->tm_mday < 10) strcat(buffer, "0");
    intToStr(t->tm_mday, timeStr);
    strcat(buffer, timeStr);
    strcat(buffer, " ");

    if (t->tm_hour < 10) strcat(buffer, "0");
    intToStr(t->tm_hour, timeStr);
    strcat(buffer, timeStr);
    strcat(buffer, ":");

    if (t->tm_min < 10) strcat(buffer, "0");
    intToStr(t->tm_min, timeStr);
    strcat(buffer, timeStr);
    strcat(buffer, ":");

    if (t->tm_sec < 10) strcat(buffer, "0");
    intToStr(t->tm_sec, timeStr);
    strcat(buffer, timeStr);
    strcat(buffer, "] ");
    strcat(buffer, message);
    strcat(buffer, "\n");
}

// Helper to write messages to stdout
void writeMsg(const char* message) {
    char str[512] = {0};
    strcat(str, message);
    strcat(str, "\n");
    write(STDOUT_FILENO, str, strlen(str));
}

// Helper to log operations
void writeLog(const char* message) {
    int fd =
        open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);  // Open log file
    if (fd < 0) return;

    char logStr[512] = {0};  // Create log message with full of null terminators
    createLogMessage(logStr, message);
    write(fd, logStr, strlen(logStr));
    close(fd);
}

// Helper to handle errors
void handleError(const char* prefix, const char* name, const char* suffix) {
    char msg[256] = "";
    createMessage(msg, prefix, name, suffix);
    writeMsg(msg);
    writeLog(msg);
}

// Helper to check if a path exists
int pathExists(const char* path) {
    struct stat path_stat;
    return stat(path, &path_stat) == 0;
}

void createDir(const char* arg) {
    struct stat path_stat;
    stat(arg, &path_stat);

    if (S_ISDIR(path_stat.st_mode)) {
        handleError("Error: Directory \"", arg, "\" already exists.");
    } else if (S_ISREG(path_stat.st_mode)) {
        handleError("Error: A file with the name \"", arg,
                    "\" already exists.");
    } else if (pathExists(arg)) {
        handleError("Error: Path \"", arg, "\" already exists.");
    } else if (mkdir(arg, 0755) == 0) {
        handleError("Directory \"", arg, "\" created successfully.");
    } else {
        handleError("Error: Failed to create directory \"", arg, "\".");
    }
}

// Create File
void createFile(const char* fileName) {
    if (pathExists(fileName)) {
        handleError("Error: File \"", fileName, "\" already exists.");
        return;
    }

    int fd = open(fileName, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        handleError("Error creating file \"", fileName, "\".");
        return;
    }

    time_t now = time(NULL);
    char timeStr[256] = "Created at: ";
    strcat(timeStr, ctime(&now));

    write(fd, timeStr, strlen(timeStr));  // Write creation time to file
    close(fd);

    handleError("File \"", fileName, "\" created successfully.");
}
// List Directory
void listDir(const char* folderName) {
    struct stat path_stat;
    if (stat(folderName, &path_stat) != 0) {
        handleError("Error: Cannot access \"", folderName, "\".");
        return;
    }

    if (S_ISREG(path_stat.st_mode)) {
        handleError("Error: \"", folderName, "\" is a file.");
        return;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        handleError("Error: \"", folderName, "\" is not a directory.");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        DIR* dir = opendir(folderName);
        if (!dir) {
            char errMsg[256] = "";
            createMessage(errMsg, "Error: Directory \"", folderName,
                          "\" not found.");
            writeMsg(errMsg);
            writeLog(errMsg);
            _exit(0);
        }

        struct dirent* entry;  // Read directory entries
        char header[256] = "";
        createMessage(header, "Contents of \"", folderName, "\":\n");
        write(STDOUT_FILENO, header, strlen(header));
        while ((entry = readdir(dir)) !=
               NULL) {  // Print all entries except . and ..
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                write(STDOUT_FILENO, entry->d_name, strlen(entry->d_name));
                write(STDOUT_FILENO, "\n", 1);
            }
        }
        closedir(dir);
        _exit(0);  // Exit child process
    } else {
        wait(NULL);  // Wait for child process to finish
    }
}
const char* getFileExtension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";
    return dot;
}

// List Files by Extension
void listFilesByExtension(const char* folderName, const char* extension) {
    struct stat path_stat;
    if (stat(folderName, &path_stat) != 0) {
        handleError("Error: Cannot access \"", folderName, "\".");
        return;
    }

    if (S_ISREG(path_stat.st_mode)) {
        handleError("Error: \"", folderName, "\" is a file.");
        return;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        handleError("Error: \"", folderName, "\" is not a directory.");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        DIR* dir = opendir(folderName);
        if (!dir) {
            char errMsg[256] = "";
            createMessage(errMsg, "Error: Directory \"", folderName,
                          "\" not found.");
            writeMsg(errMsg);
            writeLog(errMsg);
            _exit(0);
        }

        int found = 0;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* fileExt = getFileExtension(entry->d_name);
            if (strcmp(fileExt, extension) == 0) {
                write(STDOUT_FILENO, entry->d_name, strlen(entry->d_name));
                write(STDOUT_FILENO, "\n", 1);
                found = 1;
            }
        }
        if (!found) {
            char noFilesMsg[256] = "";
            createMessage(noFilesMsg, "No files with extension \"", extension,
                          "\" found in \"");
            strcat(noFilesMsg, folderName);
            strcat(noFilesMsg, "\".");
            writeMsg(noFilesMsg);
            writeLog(noFilesMsg);
        }
        closedir(dir);
        _exit(0);
    } else {
        wait(NULL);
    }
}
// Read File
void readFile(const char* fileName) {
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

    buffer[bytesRead] = '\0';
    write(STDOUT_FILENO, buffer, bytesRead);
    write(STDOUT_FILENO, "\n", 1);
    close(fd);
}

// Append to File with Lock
void appendToFile(const char* fileName, const char* content) {
    struct stat path_stat;
    stat(fileName, &path_stat);

    char msg[256] = "";
    if (S_ISDIR(path_stat.st_mode)) {
        createMessage(msg, "Error: \"", fileName, "\" is a directory.");
        writeLog(msg);
        writeMsg(msg);
        return;
    }

    int fd = open(fileName, O_WRONLY | O_APPEND);
    if (access(fileName, F_OK) != 0) {
        createMessage(msg, "Error: File \"", fileName, "\" not found.");
        writeLog(msg);
        writeMsg(msg);
        return;
    } else if (access(fileName, W_OK) != 0) {
        createMessage(msg, "Error: File \"", fileName,
                      "\" is write-protected.");
        writeLog(msg);
        writeMsg(msg);
        return;
    }
    if (fd < 0) {
        createMessage(msg, "Error: Cannot open file \"", fileName,
                      "\" for appending.");
        writeLog(msg);
        writeMsg(msg);
        return;
    }

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

    int len = write(fd, content, strlen(content));
    if (len < 0) {
        createMessage(msg, "Error: Write operation failed, unlocking file \"",
                      fileName, "\".");
        writeLog(msg);
        lock.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &lock);
        close(fd);
        return;
    }

    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    close(fd);

    createMessage(msg, "Content appended to file \"", fileName,
                  "\" successfully.");
    writeMsg(msg);
    writeLog(msg);
}

// Delete File
void deleteFile(const char* fileName) {
    pid_t pid = fork();
    if (pid == 0) {
        struct stat path_stat;
        stat(fileName, &path_stat);

        if (S_ISDIR(path_stat.st_mode)) {
            handleError("Error: \"", fileName, "\" is a directory.");
            _exit(0);
        }

        if (!pathExists(fileName)) {
            handleError("Error: File \"", fileName, "\" not found.");
            _exit(0);
        } else if (access(fileName, W_OK) != 0) {
            handleError("Error: File \"", fileName, "\" is write-protected.");
            _exit(0);
        }

        if (unlink(fileName) == 0) {
            handleError("File \"", fileName, "\" deleted successfully.");
        } else {
            handleError("Error: File deletion failed.", "", "");
        }
        _exit(0);
    } else {
        wait(NULL);
    }
}
// Delete Directory
void deleteDir(const char* folderName) {
    pid_t pid = fork();
    if (pid == 0) {
        struct stat path_stat;
        if (stat(folderName, &path_stat) != 0) {
            handleError("Error: Cannot access \"", folderName, "\".");
            _exit(0);
        }

        if (!S_ISDIR(path_stat.st_mode)) {
            handleError("Error: \"", folderName, "\" is not a directory.");
            _exit(0);
        }

        if (rmdir(folderName) == 0) {
            handleError("Directory \"", folderName, "\" deleted successfully.");
        } else {
            if (!pathExists(folderName)) {
                handleError("Error: Directory \"", folderName, "\" not found.");
            } else if (access(folderName, W_OK) != 0) {
                handleError("Error: Permission denied to delete directory \"",
                            folderName, "\".");
            } else {
                handleError("Error: Directory \"", folderName,
                            "\" is not empty.");
            }
        }
        _exit(0);
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