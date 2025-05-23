#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>

#define MAX_CLIENTS 15
#define MAX_ROOMS 50
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32
#define MAX_MESSAGE_LEN 1024
#define BUFFER_SIZE 2048

// Client structure
typedef struct {
    int socket;
    char username[MAX_USERNAME_LEN + 1];
    char room_name[MAX_ROOM_NAME_LEN + 1];
    struct sockaddr_in address;
    pthread_t thread;
    int active;
} client_t;

// Room structure
typedef struct {
    char name[MAX_ROOM_NAME_LEN + 1];
    char users[MAX_CLIENTS][MAX_USERNAME_LEN + 1];
    int user_count;
} room_t;

// Global server state
typedef struct {
    int server_socket;
    int running;
    client_t clients[MAX_CLIENTS];
    room_t rooms[MAX_ROOMS];
    int client_count;
    int room_count;
    pthread_mutex_t clients_mutex;
    pthread_mutex_t rooms_mutex;
    pthread_mutex_t log_mutex;
    FILE *log_file;
} server_t;

server_t server = {0};

// Function prototypes
void signal_handler(int sig);
void log_message(const char *format, ...);
int validate_username(const char *username);
int validate_room_name(const char *room_name);
void send_json_message(int socket, const char *type, const char *content, const char *status);
client_t* find_client_by_username(const char *username);
client_t* find_client_by_socket(int socket);
room_t* find_room(const char *room_name);
room_t* create_room(const char *room_name);
void add_user_to_room(const char *username, const char *room_name);
void remove_user_from_room(const char *username);
void broadcast_to_room(const char *room_name, const char *message, const char *exclude_user);
void handle_join_command(client_t *client, const char *room_name);
void handle_leave_command(client_t *client);
void handle_broadcast_command(client_t *client, const char *message);
void handle_whisper_command(client_t *client, const char *target_user, const char *message);
void handle_command(client_t *client, const char *command);
void cleanup_client(client_t *client);
void* handle_client(void *arg);
void start_server(int port);
void shutdown_server(void);

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT) {
        log_message("[SERVER] Received SIGINT, shutting down gracefully...");
        server.running = 0; // Stop the server loop

        // Close all client connections
        pthread_mutex_lock(&server.clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (server.clients[i].active) {
                send_json_message(server.clients[i].socket, "system", 
                    "Server is shutting down. Goodbye!", "success");
                close(server.clients[i].socket);
                server.clients[i].active = 0;
            }
        }
        pthread_mutex_unlock(&server.clients_mutex);

        // Close server socket
        if (server.server_socket > 0) {
            close(server.server_socket);
        }

        // Close log file
        if (server.log_file) {
            fclose(server.log_file);
        }

        // Destroy mutexes
        pthread_mutex_destroy(&server.clients_mutex);
        pthread_mutex_destroy(&server.rooms_mutex);
        pthread_mutex_destroy(&server.log_mutex);

        log_message("[SERVER] Server shutdown complete");
        exit(0); // Terminate the process
    }
}

// Thread-safe logging with timestamps
void log_message(const char *format, ...) {
    va_list args;
    time_t now;
    struct tm *timeinfo;
    char timestamp[64];
    char message[1024];
    
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    pthread_mutex_lock(&server.log_mutex);
    
    // Print to console
    printf("%s - %s\n", timestamp, message);
    fflush(stdout);
    
    // Write to log file
    if (server.log_file) {
        fprintf(server.log_file, "%s - %s\n", timestamp, message);
        fflush(server.log_file);
    }
    
    pthread_mutex_unlock(&server.log_mutex);
}

// Validate username: max 16 chars, alphanumeric only
int validate_username(const char *username) {
    if (!username || strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN) {
        return 0;
    }
    
    for (int i = 0; username[i]; i++) {
        if (!isalnum(username[i])) {
            return 0;
        }
    }
    return 1;
}

// Validate room name: max 32 chars, alphanumeric only
int validate_room_name(const char *room_name) {
    if (!room_name || strlen(room_name) == 0 || strlen(room_name) > MAX_ROOM_NAME_LEN) {
        return 0;
    }
    
    for (int i = 0; room_name[i]; i++) {
        if (!isalnum(room_name[i])) {
            return 0;
        }
    }
    return 1;
}

// Ensure proper JSON formatting and escaping
void send_json_message(int socket, const char *type, const char *content, const char *status) {
    char json_message[BUFFER_SIZE];
    time_t now;
    struct tm *timeinfo;
    char timestamp[32];

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);

    // Escape content to prevent JSON formatting issues
    char escaped_content[BUFFER_SIZE];
    int j = 0;
    for (int i = 0; content[i] != '\0' && j < BUFFER_SIZE - 1; i++) {
        if (content[i] == '"' || content[i] == '\\') {
            escaped_content[j++] = '\\';
        }
        escaped_content[j++] = content[i];
    }
    escaped_content[j] = '\0';

    snprintf(json_message, sizeof(json_message),
        "{\"type\":\"%s\",\"content\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\"}\n",
        type, escaped_content, status ? status : "success", timestamp);

    if (send(socket, json_message, strlen(json_message), 0) < 0) {
        log_message("[ERROR] Failed to send message to client");
    }
}

// Find client by username
client_t* find_client_by_username(const char *username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server.clients[i].active && strcmp(server.clients[i].username, username) == 0) {
            return &server.clients[i];
        }
    }
    return NULL;
}

// Find client by socket
client_t* find_client_by_socket(int socket) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server.clients[i].active && server.clients[i].socket == socket) {
            return &server.clients[i];
        }
    }
    return NULL;
}

// Find room by name
room_t* find_room(const char *room_name) {
    for (int i = 0; i < server.room_count; i++) {
        if (strcmp(server.rooms[i].name, room_name) == 0) {
            return &server.rooms[i];
        }
    }
    return NULL;
}

// Create new room
room_t* create_room(const char *room_name) {
    if (server.room_count >= MAX_ROOMS) {
        return NULL;
    }
    
    room_t *room = &server.rooms[server.room_count];
    strcpy(room->name, room_name);
    room->user_count = 0;
    server.room_count++;
    
    return room;
}

// Add user to room
void add_user_to_room(const char *username, const char *room_name) {
    room_t *room = find_room(room_name);
    if (!room) {
        room = create_room(room_name);
        if (!room) return;
    }
    
    // Check if user is already in the room
    for (int i = 0; i < room->user_count; i++) {
        if (strcmp(room->users[i], username) == 0) {
            return;
        }
    }
    
    // Add user to room
    if (room->user_count < MAX_CLIENTS) {
        strcpy(room->users[room->user_count], username);
        room->user_count++;
    }
}

// Remove user from their current room
void remove_user_from_room(const char *username) {
    for (int i = 0; i < server.room_count; i++) {
        room_t *room = &server.rooms[i];
        for (int j = 0; j < room->user_count; j++) {
            if (strcmp(room->users[j], username) == 0) {
                // Remove user by shifting array
                for (int k = j; k < room->user_count - 1; k++) {
                    strcpy(room->users[k], room->users[k + 1]);
                }
                room->user_count--;
                return;
            }
        }
    }
}

// Broadcast message to all users in a room
void broadcast_to_room(const char *room_name, const char *message, const char *exclude_user) {
    room_t *room = find_room(room_name);
    if (!room) return;
    
    char formatted_message[BUFFER_SIZE];
    snprintf(formatted_message, sizeof(formatted_message), "[%s] %s", room_name, message);
    
    for (int i = 0; i < room->user_count; i++) {
        if (exclude_user && strcmp(room->users[i], exclude_user) == 0) {
            continue;
        }
        
        client_t *client = find_client_by_username(room->users[i]);
        if (client) {
            send_json_message(client->socket, "notification", formatted_message, "success");
        }
    }
}

// Handle /join command
void handle_join_command(client_t *client, const char *room_name) {
    if (!validate_room_name(room_name)) {
        send_json_message(client->socket, "error", 
            "Invalid room name! Must be max 32 characters, alphanumeric only.", "error");
        return;
    }
    
    pthread_mutex_lock(&server.rooms_mutex);
    
    room_t *room = find_room(room_name);
    if (room && room->user_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&server.rooms_mutex);
        send_json_message(client->socket, "error", 
            "Room is full (max 15 users)", "error");
        return;
    }
    
    // Leave current room if in one
    if (strlen(client->room_name) > 0) {
        remove_user_from_room(client->username);
        broadcast_to_room(client->room_name, 
            "has left the room", client->username);
        log_message("[LEAVE] user '%s' left room '%s'", 
            client->username, client->room_name);
    }
    
    // Join new room
    add_user_to_room(client->username, room_name);
    strcpy(client->room_name, room_name);
    
    pthread_mutex_unlock(&server.rooms_mutex);
    
    log_message("[JOIN] user '%s' joined room '%s'", client->username, room_name);
    
    char success_msg[256];
    room = find_room(room_name);
    snprintf(success_msg, sizeof(success_msg), 
        "Joined room '%s'. Users in room: %d", room_name, room ? room->user_count : 1);
    send_json_message(client->socket, "success", success_msg, "success");
    
    // Notify other users
    char notification[256];
    snprintf(notification, sizeof(notification), "%s has joined the room", client->username);
    broadcast_to_room(room_name, notification, client->username);
}

// Handle /leave command
void handle_leave_command(client_t *client) {
    if (strlen(client->room_name) == 0) {
        send_json_message(client->socket, "error", "You are not in any room", "error");
        return;
    }
    
    pthread_mutex_lock(&server.rooms_mutex);
    
    char old_room[MAX_ROOM_NAME_LEN + 1];
    strcpy(old_room, client->room_name);
    
    remove_user_from_room(client->username);
    client->room_name[0] = '\0';
    
    pthread_mutex_unlock(&server.rooms_mutex);
    
    log_message("[LEAVE] user '%s' left room '%s'", client->username, old_room);
    
    char success_msg[256];
    snprintf(success_msg, sizeof(success_msg), "Left room '%s'", old_room);
    send_json_message(client->socket, "success", success_msg, "success");
    
    // Notify other users
    char notification[256];
    snprintf(notification, sizeof(notification), "%s has left the room", client->username);
    broadcast_to_room(old_room, notification, client->username);
}

// Ensure the message is properly formatted and sent
void handle_broadcast_command(client_t *client, const char *message) {
    if (strlen(client->room_name) == 0) {
        send_json_message(client->socket, "error", 
            "You must join a room first to broadcast messages", "error");
        return;
    }

    if (!message || strlen(message) == 0) {
        send_json_message(client->socket, "error", "Message cannot be empty", "error");
        return;
    }

    pthread_mutex_lock(&server.rooms_mutex);
    room_t *room = find_room(client->room_name);
    int user_count = room ? room->user_count : 0;
    pthread_mutex_unlock(&server.rooms_mutex);

    log_message("[BROADCAST] user '%s' in room '%s': %s", 
        client->username, client->room_name, message);

    // Broadcast to room
    char broadcast_msg[BUFFER_SIZE];
    snprintf(broadcast_msg, sizeof(broadcast_msg), "[%s] %s: %s", 
        client->room_name, client->username, message);

    int sent_count = 0;
    pthread_mutex_lock(&server.rooms_mutex);
    if (room) {
        for (int i = 0; i < room->user_count; i++) {
            if (strcmp(room->users[i], client->username) != 0) {
                client_t *target = find_client_by_username(room->users[i]);
                if (target) {
                    send_json_message(target->socket, "broadcast", broadcast_msg, "success");
                    sent_count++;
                }
            }
        }
    }
    pthread_mutex_unlock(&server.rooms_mutex);

    char success_msg[256];
    snprintf(success_msg, sizeof(success_msg), 
        "Message broadcast to %d users in room '%s'", sent_count, client->room_name);
    send_json_message(client->socket, "success", success_msg, "success");
}

// Handle /whisper command
void handle_whisper_command(client_t *client, const char *target_user, const char *message) {
    pthread_mutex_lock(&server.clients_mutex);
    client_t *target = find_client_by_username(target_user);
    pthread_mutex_unlock(&server.clients_mutex);
    
    if (!target) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "User '%s' not found or offline", target_user);
        send_json_message(client->socket, "error", error_msg, "error");
        return;
    }
    
    char whisper_msg[BUFFER_SIZE];
    snprintf(whisper_msg, sizeof(whisper_msg), "[Private] %s: %s", client->username, message);
    
    send_json_message(target->socket, "whisper", whisper_msg, "success");
    
    char success_msg[256];
    snprintf(success_msg, sizeof(success_msg), "Private message sent to %s", target_user);
    send_json_message(client->socket, "success", success_msg, "success");
    
    log_message("[WHISPER] '%s' to '%s': %s", client->username, target_user, message);
}

// Handle client commands
void handle_command(client_t *client, const char *command) {
    char cmd_copy[BUFFER_SIZE];
    strcpy(cmd_copy, command);
    
    char *token = strtok(cmd_copy, " ");
    if (!token) return;
    
    if (strcmp(token, "/join") == 0) {
        char *room_name = strtok(NULL, " ");
        if (!room_name) {
            send_json_message(client->socket, "error", "Usage: /join <room_name>", "error");
            return;
        }
        handle_join_command(client, room_name);
    }
    else if (strcmp(token, "/leave") == 0) {
        handle_leave_command(client);
    }
    else if (strcmp(token, "/broadcast") == 0) {
        char *message = strtok(NULL, "");
        if (!message) {
            send_json_message(client->socket, "error", "Usage: /broadcast <message>", "error");
            return;
        }
        // Skip the space after /broadcast
        if (message[0] == ' ') message++;
        handle_broadcast_command(client, message);
    }
    else if (strcmp(token, "/whisper") == 0) {
        char *target_user = strtok(NULL, " ");
        char *message = strtok(NULL, "");
        if (!target_user || !message) {
            send_json_message(client->socket, "error", "Usage: /whisper <username> <message>", "error");
            return;
        }
        // Skip the space after username
        if (message[0] == ' ') message++;
        handle_whisper_command(client, target_user, message);
    }
    // Update /exit command handling to clean up client resources immediately
    else if (strcmp(token, "/exit") == 0) {
        send_json_message(client->socket, "system", "Goodbye!", "success");
        cleanup_client(client); // Clean up client resources immediately
        pthread_exit(NULL); // Exit the client thread
    }
    else {
        send_json_message(client->socket, "error", 
            "Unknown command. Available: /join, /leave, /broadcast, /whisper, /exit", "error");
    }
}

// Clean up client resources
void cleanup_client(client_t *client) {
    if (!client->active) return;
    
    log_message("[LOGOUT] user '%s' disconnected", client->username);
    
    pthread_mutex_lock(&server.clients_mutex);
    client->active = 0;
    server.client_count--;
    pthread_mutex_unlock(&server.clients_mutex);
    
    pthread_mutex_lock(&server.rooms_mutex);
    if (strlen(client->room_name) > 0) {
        char notification[256];
        snprintf(notification, sizeof(notification), "%s has disconnected", client->username);
        broadcast_to_room(client->room_name, notification, client->username);
        remove_user_from_room(client->username);
    }
    pthread_mutex_unlock(&server.rooms_mutex);
    
    close(client->socket);
    memset(client, 0, sizeof(client_t));
}

// Handle individual client connection
void* handle_client(void *arg) {
    client_t *client = (client_t*)arg;
    char buffer[BUFFER_SIZE];
    int bytes_received;
    
    // Send welcome message
    send_json_message(client->socket, "system", 
        "Welcome! Please enter your username (max 16 chars, alphanumeric):", "success");
    
    // Get username
    while (client->active && server.running) {
        bytes_received = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) break;
        
        buffer[bytes_received] = '\0';
        
        // Remove newline if present
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
        newline = strchr(buffer, '\r');
        if (newline) *newline = '\0';
        
        if (strlen(client->username) == 0) {
            // Username registration
            if (!validate_username(buffer)) {
                send_json_message(client->socket, "error", 
                    "Invalid username! Must be max 16 characters, alphanumeric only.", "error");
                continue;
            }
            
            pthread_mutex_lock(&server.clients_mutex);
            client_t *existing = find_client_by_username(buffer);
            if (existing) {
                pthread_mutex_unlock(&server.clients_mutex);
                send_json_message(client->socket, "error", 
                    "Username already taken! Please choose another.", "error");
                continue;
            }
            
            strcpy(client->username, buffer);
            pthread_mutex_unlock(&server.clients_mutex);
            
            log_message("[LOGIN] user '%s' connected from %s", 
                client->username, inet_ntoa(client->address.sin_addr));
            send_json_message(client->socket, "success", 
                "Welcome! You can now use commands: /join, /leave, /broadcast, /whisper, /exit", "success");
            continue;
        }
        
        // Handle commands
        if (buffer[0] == '/') {
            handle_command(client, buffer);
        } else {
            send_json_message(client->socket, "error", 
                "Please use commands starting with '/' or type /exit to quit", "error");
        }
    }
    
    cleanup_client(client);
    return NULL;
}

// Start the server
void start_server(int port) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;
    
    // Initialize mutexes
    pthread_mutex_init(&server.clients_mutex, NULL);
    pthread_mutex_init(&server.rooms_mutex, NULL);
    pthread_mutex_init(&server.log_mutex, NULL);
    
    // Open log file
    time_t now;
    struct tm *timeinfo;
    char log_filename[256];
    
    time(&now);
    timeinfo = localtime(&now);
    strftime(log_filename, sizeof(log_filename), "server_log_%Y%m%d_%H%M%S.txt", timeinfo);
    
    server.log_file = fopen(log_filename, "w");
    if (!server.log_file) {
        perror("Failed to open log file");
    }
    
    // Create socket
    server.server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server.server_socket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    if (setsockopt(server.server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(server.server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server.server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    server.running = 1;
    log_message("[SERVER] Chat server started on port %d", port);
    log_message("[SERVER] Log file: %s", log_filename);
    
    // Accept client connections
    while (server.running) {
        int client_socket = accept(server.server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (server.running) {
                perror("Accept failed");
            }
            continue;
        }
        
        pthread_mutex_lock(&server.clients_mutex);
        
        // Find empty client slot
        client_t *client = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!server.clients[i].active) {
                client = &server.clients[i];
                break;
            }
        }
        
        if (!client) {
            pthread_mutex_unlock(&server.clients_mutex);
            log_message("[ERROR] Maximum clients reached, rejecting connection");
            send_json_message(client_socket, "error", "Server full, try again later", "error");
            close(client_socket);
            continue;
        }
        
        // Initialize client
        client->socket = client_socket;
        client->address = client_addr;
        client->active = 1;
        server.client_count++;
        
        pthread_mutex_unlock(&server.clients_mutex);
        
        // Create thread to handle client
        if (pthread_create(&client->thread, NULL, handle_client, (void*)client) != 0) {
            perror("Failed to create thread");
            close(client_socket);
            continue;
        }
        
        pthread_detach(client->thread);
    }
    
    // Cleanup code (should never reach here in normal operation)
    pthread_mutex_lock(&server.clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server.clients[i].active) {
            close(server.clients[i].socket);
            server.clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&server.clients_mutex);
    
    // Close server socket
    if (server.server_socket > 0) {
        close(server.server_socket);
    }
    
    // Close log file
    if (server.log_file) {
        fclose(server.log_file);
    }
    
    // Destroy mutexes
    pthread_mutex_destroy(&server.clients_mutex);
    pthread_mutex_destroy(&server.rooms_mutex);
    pthread_mutex_destroy(&server.log_mutex);
    
    log_message("[SERVER] Server shutdown complete");
    exit(0);
}

int main(int argc, char *argv[]) {
    int port = 8888;
    
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number\n");
            exit(EXIT_FAILURE);
        }
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    
    printf("Starting Multi-threaded Chat Server in C\n");
    printf("Port: %d\n", port);
    printf("Press Ctrl+C to shutdown gracefully\n\n");
    
    start_server(port);
    
    return 0;
}