#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <errno.h>

#define BUFFER_SIZE 2048
#define MAX_MESSAGE_LEN 1024

// ANSI color codes
#define COLOR_RED     "\033[91m"
#define COLOR_GREEN   "\033[92m"
#define COLOR_YELLOW  "\033[93m"
#define COLOR_BLUE    "\033[94m"
#define COLOR_PURPLE  "\033[95m"
#define COLOR_CYAN    "\033[96m"
#define COLOR_WHITE   "\033[97m"
#define COLOR_RESET   "\033[0m"
#define COLOR_DEBUG   "\033[90m"

#define DEBUG 1

#if DEBUG
#define debug_print(fmt, ...) printf(COLOR_DEBUG fmt COLOR_RESET, ##__VA_ARGS__)
#else
#define debug_print(fmt, ...)
#endif

typedef struct {
    int socket;
    int running;
    pthread_t receiver_thread;
} client_t;

client_t client = {0};

// Function prototypes
void signal_handler(int sig);
void print_colored(const char *text, const char *color);
void show_help(void);
char* extract_json_value(const char *json, const char *key);
void process_message(const char *message_str);
void* receive_messages(void *arg);
int connect_to_server(const char *host, int port);
void handle_input(void);
void disconnect_from_server(void);

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT) {
        print_colored("Disconnecting...", COLOR_YELLOW);
        debug_print("[DEBUG] SIGINT received, shutting down client...\n");
        client.running = 0;
        disconnect_from_server();
        exit(0); // Ensure the program terminates
    }
}

// Print text with color
void print_colored(const char *text, const char *color) {
    printf("%s%s%s\n", color, text, COLOR_RESET);
}

// Show help message
void show_help(void) {
    print_colored("\nAvailable Commands:", COLOR_CYAN);
    print_colored("/join <room_name>     - Join or create a room", COLOR_CYAN);
    print_colored("/leave                - Leave current room", COLOR_CYAN);
    print_colored("/broadcast <message>  - Send message to everyone in current room", COLOR_CYAN);
    print_colored("/whisper <user> <msg> - Send private message to user", COLOR_CYAN);
    print_colored("/exit                 - Disconnect from server", COLOR_CYAN);
    print_colored("/help                 - Show this help message", COLOR_CYAN);
}

void process_message(const char *message_str) {
    debug_print("[DEBUG] Raw message received: %s\n", message_str);

    // Buffers to hold values safely
    char type[64] = {0}, content[512] = {0}, status[64] = {0}, timestamp[64] = {0};

    // Look for each key and extract its value manually
    const char *type_key = "\"type\":\"";
    const char *content_key = "\"content\":\"";
    const char *status_key = "\"status\":\"";
    const char *timestamp_key = "\"timestamp\":\"";

    const char *start, *end;

    // Extract "type"
    start = strstr(message_str, type_key);
    if (start) {
        start += strlen(type_key);
        end = strchr(start, '"');
        if (end && (end - start) < sizeof(type))
            strncpy(type, start, end - start);
    }

    // Extract "content"
    start = strstr(message_str, content_key);
    if (start) {
        start += strlen(content_key);
        end = strchr(start, '"');
        if (end && (end - start) < sizeof(content))
            strncpy(content, start, end - start);
    }

    // Extract "status"
    start = strstr(message_str, status_key);
    if (start) {
        start += strlen(status_key);
        end = strchr(start, '"');
        if (end && (end - start) < sizeof(status))
            strncpy(status, start, end - start);
    }

    // Extract "timestamp"
    start = strstr(message_str, timestamp_key);
    if (start) {
        start += strlen(timestamp_key);
        end = strchr(start, '"');
        if (end && (end - start) < sizeof(timestamp))
            strncpy(timestamp, start, end - start);
    }

    // Ensure null-termination
    type[sizeof(type)-1] = '\0';
    content[sizeof(content)-1] = '\0';
    status[sizeof(status)-1] = '\0';
    timestamp[sizeof(timestamp)-1] = '\0';

    if (type[0] == '\0' || content[0] == '\0') {
        print_colored("[ERROR] Malformed message received", COLOR_RED);
        debug_print("[DEBUG] Malformed message: %s\n", message_str);
        return;
    }

    debug_print("[DEBUG] Extracted type: %s, content: %s, status: %s, timestamp: %s\n",
           type, content, status[0] ? status : "N/A", timestamp[0] ? timestamp : "N/A");

    char formatted_message[BUFFER_SIZE];

    if (strcmp(type, "system") == 0) {
        snprintf(formatted_message, sizeof(formatted_message), "[SYSTEM] %s", content);
        print_colored(formatted_message, COLOR_CYAN);
    } else if (strcmp(type, "success") == 0) {
        snprintf(formatted_message, sizeof(formatted_message), "[SUCCESS] %s", content);
        print_colored(formatted_message, COLOR_GREEN);
    } else if (strcmp(type, "error") == 0) {
        snprintf(formatted_message, sizeof(formatted_message), "[ERROR] %s", content);
        print_colored(formatted_message, COLOR_RED);
    } else if (strcmp(type, "broadcast") == 0) {
        snprintf(formatted_message, sizeof(formatted_message), "[%s] %s",
                 timestamp[0] ? timestamp : "", content);
        print_colored(formatted_message, COLOR_YELLOW);
    } else if (strcmp(type, "whisper") == 0) {
        snprintf(formatted_message, sizeof(formatted_message), "[Private] [%s] %s",
                 timestamp[0] ? timestamp : "", content);
        print_colored(formatted_message, COLOR_PURPLE);
    } else if (strcmp(type, "notification") == 0) {
        snprintf(formatted_message, sizeof(formatted_message), "[INFO] %s", content);
        print_colored(formatted_message, COLOR_BLUE);
    } else {
        snprintf(formatted_message, sizeof(formatted_message), "[UNKNOWN] [%s] %s",
                 timestamp[0] ? timestamp : "", content);
        print_colored(formatted_message, COLOR_WHITE);
    }

    debug_print("[DEBUG] Processed message type: %s, content: %s\n", type, content);
}



// Receive messages from server thread
void* receive_messages(void *arg) {
    char buffer[BUFFER_SIZE];
    char message_buffer[BUFFER_SIZE * 2] = {0};

    fd_set read_fds;
    struct timeval timeout;

    while (client.running) {
        FD_ZERO(&read_fds);
        FD_SET(client.socket, &read_fds);

        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;

        int activity = select(client.socket + 1, &read_fds, NULL, NULL, &timeout);

        if (activity < 0 && errno != EINTR) {
            perror("select error");
            break;
        }

        if (activity == 0) {
            // Timeout, no data, check if we should still run
            continue;
        }

        if (FD_ISSET(client.socket, &read_fds)) {
            int bytes_received = recv(client.socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_received <= 0) {
                if (client.running) {
                    print_colored("Connection lost or closed by server", COLOR_RED);
                }
                break;
            }

            buffer[bytes_received] = '\0';
            strcat(message_buffer, buffer);

            // Process complete messages (ending with newline)
            char *newline_pos;
            while ((newline_pos = strchr(message_buffer, '\n')) != NULL) {
                *newline_pos = '\0';
                if (strlen(message_buffer) > 0) {
                    process_message(message_buffer);
                }
                // Move remaining data to beginning of buffer
                memmove(message_buffer, newline_pos + 1, strlen(newline_pos + 1) + 1);
            }
        }
    }

    return NULL;
}

// Connect to server
int connect_to_server(const char *host, int port) {
    struct sockaddr_in server_addr;
    
    // Create socket
    client.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client.socket < 0) {
        perror("Socket creation failed");
        return 0;
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(client.socket);
        return 0;
    }
    
    // Connect to server
    if (connect(client.socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(client.socket);
        return 0;
    }
    
    client.running = 1;
    
    char success_msg[256];
    snprintf(success_msg, sizeof(success_msg), "Connected to server at %s:%d", host, port);
    print_colored(success_msg, COLOR_GREEN);
    
    // Start receiver thread
    if (pthread_create(&client.receiver_thread, NULL, receive_messages, NULL) != 0) {
        perror("Thread creation failed");
        close(client.socket);
        client.running = 0;
        return 0;
    }
    
    return 1;
}

// Handle user input
void handle_input(void) {
    char input[MAX_MESSAGE_LEN];
    
    while (client.running) {
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove newline
        char *newline = strchr(input, '\n');
        if (newline) *newline = '\0';
        
        // Skip empty input
        if (strlen(input) == 0) {
            continue;
        }
        
        // Handle local commands
        if (strcmp(input, "/help") == 0) {
            show_help();
            continue;
        }
        
        if (strcmp(input, "/exit") == 0) {
            client.running = 0;
            break;
        }
        
        // Send message to server
        if (send(client.socket, input, strlen(input), 0) < 0) {
            print_colored("Failed to send message", COLOR_RED);
            break;
        }
    }
}

// Disconnect from server
void disconnect_from_server(void) {
    client.running = 0;
    
    if (client.socket > 0) {
        close(client.socket);
        client.socket = 0;
    }
    
    // Wait for receiver thread to finish
    if (client.receiver_thread) {
        pthread_join(client.receiver_thread, NULL);
    }
    
    print_colored("Disconnected from server", COLOR_YELLOW);
}

int main(int argc, char *argv[]) {
    char *host = "127.0.0.1";
    int port = 8888;
    
    // Parse command line arguments
    if (argc >= 3) {
        host = argv[1];
        port = atoi(argv[2]);
    } else if (argc >= 2) {
        port = atoi(argv[1]);
    }
    
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        exit(EXIT_FAILURE);
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    
    printf("=== Multi-threaded Chat Client ===\n");
    printf("Connecting to %s:%d...\n", host, port);
    printf("Type '/help' for available commands\n");
    printf("Press Ctrl+C to quit\n\n");
    
    if (!connect_to_server(host, port)) {
        fprintf(stderr, "Failed to connect to server\n");
        exit(EXIT_FAILURE);
    }
    
    // Handle user input
    handle_input();
    
    // Clean up
    disconnect_from_server();
    
    printf("Connection closed\n");
    return 0;
}