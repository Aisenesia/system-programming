#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/select.h>

#define MAX_CLIENTS 15
#define MAX_ROOMS 50
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32
#define MAX_MESSAGE_LEN 1024
#define BUFFER_SIZE 2048
#define MAX_UPLOAD_QUEUE 5
#define MAX_FILE_SIZE (3 * 1024 * 1024) // 3MB
#define FILES_DIR "files" // server temporariy holds the files in this directory

// TODO: if no recipient is offline or not found, wrong command messages appear
// TODO: if a recipient is not in a room or in a different one, it still sends the file to the recipient succesfully instead of returning an error

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

// File transfer structure
typedef struct {
    char filename[256];
    char sender[MAX_USERNAME_LEN + 1];
    char receiver[MAX_USERNAME_LEN + 1];
    size_t file_size;
    int in_progress;
    time_t timestamp;
    time_t enqueue_time;  // Track when file was added to queue
} file_transfer_t;

// File upload queue
typedef struct {
    file_transfer_t transfers[MAX_UPLOAD_QUEUE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    sem_t upload_slots;
} upload_queue_t;

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
    upload_queue_t upload_queue;
    pthread_t file_processor_thread;
} server_t;

server_t server = {0};

// Function prototypes
void signal_handler(int sig);
void log_message(const char *format, ...);
int validate_username(const char *username);
int validate_room_name(const char *room_name);
int validate_filename(const char *filename);
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
void handle_sendfile_command(client_t *client, const char *filename, const char *target_user, const char *file_size_str);
void init_upload_queue(void);
void destroy_upload_queue(void);
int enqueue_file_transfer(const char *filename, const char *sender, const char *receiver, size_t file_size);
void* process_file_transfers(void *arg);
void handle_command(client_t *client, const char *command);
void cleanup_client(client_t *client);
void* handle_client(void *arg);
void start_server(int port);
void shutdown_server(void);
char* resolve_filename_collision(const char *original_filename);
void cleanup_pending_files_for_user(const char *username);
void cleanup_all_temporary_files(void);
void cleanup_queue_on_shutdown(void);
void cleanup_orphaned_files(void);
void* periodic_cleanup_thread(void *arg);

// Global shutdown flag
static volatile int shutdown_requested = 0;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT) {
        log_message("[SERVER] Received SIGINT, shutting down gracefully...");
        shutdown_requested = 1;
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

        // Clean up all pending file transfers and temporary files
        cleanup_queue_on_shutdown();

        // Close server socket to break accept() loop
        if (server.server_socket > 0) {
            close(server.server_socket);
            server.server_socket = -1;
        }

        log_message("[SERVER] Server shutdown complete");
        
        // Don't destroy mutexes here - let main cleanup handle it
        // Don't call exit() here - let the program terminate naturally
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

// Validate filename: max 256 chars, alphanumeric and some special chars
int validate_filename(const char *filename) {
    if (!filename || strlen(filename) == 0 || strlen(filename) > 255) {
        return 0;
    }
    
    for (int i = 0; filename[i]; i++) {
        if (!isalnum(filename[i]) && filename[i] != '.' && filename[i] != '_' && filename[i] != '-') {
            return 0;
        }
    }
    return 1;
}

// Ensure proper JSON formatting and escaping
void send_json_message(int socket, const char *type, const char *content, const char *status) {
    char json_message[BUFFER_SIZE + 128]; // Extra space for escaping and formatting
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

    log_message("[BROADCAST] user '%s' in room '%s': %s", 
        client->username, client->room_name, message);

    // Broadcast to room
    char broadcast_msg[BUFFER_SIZE];
    snprintf(broadcast_msg, sizeof(broadcast_msg), "[%s] %s: %s", 
        client->room_name, client->username, message);

    int sent_count = 0;
    pthread_mutex_lock(&server.rooms_mutex);
    room_t *room = find_room(client->room_name);
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

// Handle file sending - Updated to accept file size with initial command
void handle_sendfile_command(client_t *client, const char *filename, const char *target_user, const char *file_size_str) {
    // Check if user is trying to send file to themselves
    if (strcmp(client->username, target_user) == 0) {
        send_json_message(client->socket, "error", 
            "You cannot send a file to yourself", "error");
        return;
    }

    if (!validate_filename(filename)) {
        send_json_message(client->socket, "error", 
            "Invalid filename! Must be max 255 characters, alphanumeric, '.', '_', or '-'.", "error");
        return;
    }

    pthread_mutex_lock(&server.clients_mutex);
    client_t *receiver = find_client_by_username(target_user);
    pthread_mutex_unlock(&server.clients_mutex);

    if (!receiver) {
        send_json_message(client->socket, "error", 
            "Recipient not found or offline", "error");
        return;
    }

    // Parse file size from the command
    size_t file_size = atoi(file_size_str);
    if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
        send_json_message(client->socket, "error", 
            "Invalid file size (max 3MB)", "error");
        return;
    }

    // Check file extension
    const char *ext = strrchr(filename, '.');
    if (!ext || (strcmp(ext, ".txt") != 0 && strcmp(ext, ".pdf") != 0 && 
                 strcmp(ext, ".jpg") != 0 && strcmp(ext, ".png") != 0)) {
        send_json_message(client->socket, "error", 
            "Invalid file type. Allowed: .txt, .pdf, .jpg, .png", "error");
        return;
    }

    // Create temp directory if it doesn't exist
    struct stat st;
    if (stat("files", &st) == -1) {
        if (mkdir("files", 0755) != 0) {
            send_json_message(client->socket, "error", "Server error: cannot create temp directory", "error");
            return;
        }
    }

    // Create temporary file path - keep original name for temp storage
    char temp_path[512];
    const char *filename_only = strrchr(filename, '/');
    if (filename_only) {
        filename_only++; // Skip the '/'
    } else {
        filename_only = filename;
    }
    
    // Use a simple temp naming scheme without timestamp
    snprintf(temp_path, sizeof(temp_path), "files/temp_%s_%s", client->username, filename_only);

    // Notify client to start sending file data
    send_json_message(client->socket, "success", "Ready to receive file data", "success");

    // Receive file data from client
    FILE *temp_file = fopen(temp_path, "wb");
    if (!temp_file) {
        send_json_message(client->socket, "error", "Server error: cannot create temporary file", "error");
        return;
    }

    char file_buffer[8192];
    size_t bytes_received_total = 0;
    size_t bytes_to_read;

    log_message("[UPLOAD] Receiving file %s (%zu bytes) from %s for %s", filename_only, file_size, client->username, target_user);

    while (bytes_received_total < file_size) {
        bytes_to_read = (file_size - bytes_received_total > sizeof(file_buffer)) ? sizeof(file_buffer) : (file_size - bytes_received_total);
        
        int received = recv(client->socket, file_buffer, bytes_to_read, 0);
        if (received <= 0) {
            log_message("[UPLOAD ERROR] Connection lost while receiving file from %s", client->username);
            fclose(temp_file);
            unlink(temp_path); // Delete incomplete file
            return;
        }

        if (fwrite(file_buffer, 1, received, temp_file) != (size_t)received) {
            log_message("[UPLOAD ERROR] Write error while saving file from %s", client->username);
            fclose(temp_file);
            unlink(temp_path); // Delete incomplete file
            send_json_message(client->socket, "error", "Server error: file write failed", "error");
            return;
        }

        bytes_received_total += received;
    }

    fclose(temp_file);

    if (bytes_received_total == file_size) {
        log_message("[UPLOAD SUCCESS] File %s (%zu bytes) uploaded by %s", filename_only, bytes_received_total, client->username);

        // Notify receiver about incoming file
        char file_message[BUFFER_SIZE];
        snprintf(file_message, sizeof(file_message), 
            "[File] %s is sending you a file: %s (%.1f KB)", 
            client->username, filename_only, (float)file_size / 1024);
        send_json_message(receiver->socket, "file_request", file_message, "success");

        // Queue the file transfer using the temp file path
        if (enqueue_file_transfer(temp_path, client->username, target_user, file_size) == 0) {
            char success_msg[256];
            snprintf(success_msg, sizeof(success_msg), 
                "File '%s' uploaded and queued for transfer to %s (%.1f KB)", 
                filename_only, target_user, (float)file_size / 1024);
            send_json_message(client->socket, "success", success_msg, "success");
            
            log_message("[FILE] File %s queued for transfer from %s to %s", filename_only, client->username, target_user);
        } else {
            send_json_message(client->socket, "error", "Failed to queue file transfer", "error");
            unlink(temp_path); // Delete file if queueing failed
        }
    } else {
        log_message("[UPLOAD ERROR] Incomplete upload from %s: expected %zu, got %zu", client->username, file_size, bytes_received_total);
        send_json_message(client->socket, "error", "File upload incomplete", "error");
        unlink(temp_path); // Delete incomplete file
    }
}

// Handle filename collisions by checking for existing files and renaming if necessary
char* resolve_filename_collision(const char *original_filename) {
    static char resolved_filename[512];
    struct stat st;
    
    // Check if original filename exists
    if (stat(original_filename, &st) != 0) {
        // File doesn't exist, no collision
        strcpy(resolved_filename, original_filename);
        return resolved_filename;
    }
    
    // File exists, need to resolve collision
    char base_name[256];
    char extension[64] = "";
    
    // Extract base name and extension
    strcpy(base_name, original_filename);
    char *ext_pos = strrchr(base_name, '.');
    if (ext_pos) {
        strcpy(extension, ext_pos);
        *ext_pos = '\0';
    }
    
    // Try numbered variants until we find one that doesn't exist
    for (int i = 1; i <= 999; i++) {
        snprintf(resolved_filename, sizeof(resolved_filename), "%s_%d%s", base_name, i, extension);
        if (stat(resolved_filename, &st) != 0) {
            // This filename doesn't exist, use it
            log_message("[FILE] Conflict: '%s' already exists → renamed to '%s'", 
                       original_filename, resolved_filename);
            return resolved_filename;
        }
    }
    
    // If we get here, we couldn't find a unique name (very unlikely)
    snprintf(resolved_filename, sizeof(resolved_filename), "%s_conflict_%ld%s", 
             base_name, time(NULL), extension);
    return resolved_filename;
}

// Initialize the file upload queue
void init_upload_queue(void) {
    memset(&server.upload_queue, 0, sizeof(upload_queue_t));
    pthread_mutex_init(&server.upload_queue.mutex, NULL);
    pthread_cond_init(&server.upload_queue.not_full, NULL);
    pthread_cond_init(&server.upload_queue.not_empty, NULL);
    sem_init(&server.upload_queue.upload_slots, 0, MAX_UPLOAD_QUEUE);
}

// Destroy the file upload queue
void destroy_upload_queue(void) {
    pthread_mutex_destroy(&server.upload_queue.mutex);
    pthread_cond_destroy(&server.upload_queue.not_full);
    pthread_cond_destroy(&server.upload_queue.not_empty);
    sem_destroy(&server.upload_queue.upload_slots);
}

// Enqueue a file transfer
int enqueue_file_transfer(const char *filename, const char *sender, const char *receiver, size_t file_size) {
    // Check current queue size before acquiring semaphore for feedback
    pthread_mutex_lock(&server.upload_queue.mutex);
    int current_queue_size = server.upload_queue.count;
    pthread_mutex_unlock(&server.upload_queue.mutex);
    
    // If queue is full, notify sender about queuing
    if (current_queue_size >= MAX_UPLOAD_QUEUE) {
        pthread_mutex_lock(&server.clients_mutex);
        client_t *sender_client = find_client_by_username(sender);
        if (sender_client) {
            char queue_msg[256];
            snprintf(queue_msg, sizeof(queue_msg), 
                    "Upload queue is full (%d/%d). Your file is waiting to be processed...", 
                    current_queue_size, MAX_UPLOAD_QUEUE);
            send_json_message(sender_client->socket, "system", queue_msg, "success");
        }
        pthread_mutex_unlock(&server.clients_mutex);
    }
    
    sem_wait(&server.upload_queue.upload_slots);

    pthread_mutex_lock(&server.upload_queue.mutex);

    // Add transfer to the queue
    file_transfer_t *transfer = &server.upload_queue.transfers[server.upload_queue.tail];
    strncpy(transfer->filename, filename, sizeof(transfer->filename) - 1);
    strncpy(transfer->sender, sender, sizeof(transfer->sender) - 1);
    strncpy(transfer->receiver, receiver, sizeof(transfer->receiver) - 1);
    transfer->file_size = file_size;
    transfer->in_progress = 1;
    transfer->timestamp = time(NULL);
    transfer->enqueue_time = time(NULL);  // Set enqueue time

    server.upload_queue.tail = (server.upload_queue.tail + 1) % MAX_UPLOAD_QUEUE;
    server.upload_queue.count++;
    
    // Enhanced queue logging with current queue size
    const char *filename_only = strrchr(filename, '/');
    if (filename_only) {
        filename_only++; // Skip the '/'
    } else {
        filename_only = filename;
    }
    
    log_message("[FILE-QUEUE] Upload '%s' from %s added to queue. Queue size: %d/%d", 
               filename_only, sender, server.upload_queue.count, MAX_UPLOAD_QUEUE);

    pthread_cond_signal(&server.upload_queue.not_empty);
    pthread_mutex_unlock(&server.upload_queue.mutex);

    return 0;
}

// Process file transfers in the background - Updated for direct client-to-client transfer
void* process_file_transfers(void *arg) {
    (void)arg; // Suppress unused parameter warning
    
    while (server.running) {
        pthread_mutex_lock(&server.upload_queue.mutex);

        // Wait for a file transfer to be enqueued
        while (server.upload_queue.count == 0 && server.running) {
            pthread_cond_wait(&server.upload_queue.not_empty, &server.upload_queue.mutex);
        }

        if (!server.running) {
            pthread_mutex_unlock(&server.upload_queue.mutex);
            break;
        }

        // Get the next file transfer
        file_transfer_t transfer = server.upload_queue.transfers[server.upload_queue.head];
        server.upload_queue.head = (server.upload_queue.head + 1) % MAX_UPLOAD_QUEUE;
        server.upload_queue.count--;

        pthread_cond_signal(&server.upload_queue.not_full);
        pthread_mutex_unlock(&server.upload_queue.mutex);

        // Actually perform the file transfer
        log_message("[FILE] Processing transfer: %s (%zu bytes) from %s to %s", 
                    transfer.filename, transfer.file_size, transfer.sender, transfer.receiver);

        // Find the recipient client with thread safety
        pthread_mutex_lock(&server.clients_mutex);
        client_t *receiver = find_client_by_username(transfer.receiver);
        client_t *sender = find_client_by_username(transfer.sender);
        
        // Check if both users are still connected
        if (!receiver || !receiver->active) {
            pthread_mutex_unlock(&server.clients_mutex);
            log_message("[FILE ERROR] Recipient %s not found or offline", transfer.receiver);
            
            // Clean up temporary file if recipient is offline
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file (recipient offline): %s", transfer.filename);
            }
            
            // Notify sender that recipient is offline (only if sender is still connected)
            if (sender && sender->active) {
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), 
                         "File transfer failed: %s is offline", transfer.receiver);
                send_json_message(sender->socket, "error", error_msg, "error");
            }
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }
        
        if (!sender || !sender->active) {
            pthread_mutex_unlock(&server.clients_mutex);
            log_message("[FILE ERROR] Sender %s disconnected during transfer", transfer.sender);
            
            // Clean up temporary file if sender disconnected
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file (sender offline): %s", transfer.filename);
            }
            
            // Notify receiver that sender is offline
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg), 
                     "File transfer cancelled: %s has disconnected", transfer.sender);
            send_json_message(receiver->socket, "error", error_msg, "error");
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }
        
        // Store socket file descriptors while we have the lock
        int receiver_socket = receiver->socket;
        pthread_mutex_unlock(&server.clients_mutex);

        // Check if the file still exists before processing
        struct stat file_stat;
        if (stat(transfer.filename, &file_stat) != 0) {
            log_message("[FILE ERROR] Source file no longer exists: %s", transfer.filename);
            
            // Notify both users about the error
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg), "File transfer failed: source file missing");
            
            pthread_mutex_lock(&server.clients_mutex);
            client_t *current_receiver = find_client_by_username(transfer.receiver);
            client_t *current_sender = find_client_by_username(transfer.sender);
            
            if (current_receiver && current_receiver->active) {
                send_json_message(current_receiver->socket, "error", error_msg, "error");
            }
            if (current_sender && current_sender->active) {
                send_json_message(current_sender->socket, "error", error_msg, "error");
            }
            pthread_mutex_unlock(&server.clients_mutex);
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }

        // Extract original filename for transfer (remove temp_ prefix and username)
        const char *temp_filename_only = strrchr(transfer.filename, '/');
        if (temp_filename_only) {
            temp_filename_only++; // Skip the '/'
        } else {
            temp_filename_only = transfer.filename;
        }
        
        // Extract original filename by removing temp prefix and username
        char original_filename[256];
        if (strncmp(temp_filename_only, "temp_", 5) == 0) {
            // Find the second underscore to skip "temp_username_"
            const char *after_temp = temp_filename_only + 5; // Skip "temp_"
            const char *second_underscore = strchr(after_temp, '_');
            if (second_underscore) {
                strcpy(original_filename, second_underscore + 1); // Skip the username and underscore
            } else {
                strcpy(original_filename, temp_filename_only); // Fallback
            }
        } else {
            strcpy(original_filename, temp_filename_only);
        }

        // Calculate and log queue wait duration
        time_t current_time = time(NULL);
        double wait_duration = difftime(current_time, transfer.enqueue_time);
        
        if (wait_duration > 1.0) {  // Only log if waited more than 1 second
            log_message("[FILE] '%s' from user '%s' started upload after %.0f seconds in queue", 
                       original_filename, transfer.sender, wait_duration);
        }

        // Use original filename in handshake proposal
        char potential_filename[512];
        snprintf(potential_filename, sizeof(potential_filename), "%s", original_filename);
        
        // Implement filename handshake with client
        char final_filename[512];
        
        // Step 1: Propose original filename to client and wait for response
        char filename_proposal[BUFFER_SIZE];
        snprintf(filename_proposal, sizeof(filename_proposal), 
                 "FILENAME_PROPOSAL:%s:%s:%zu", 
                 potential_filename, transfer.sender, transfer.file_size);
        
        // Double-check receiver is still connected before sending proposal
        pthread_mutex_lock(&server.clients_mutex);
        client_t *current_receiver = find_client_by_username(transfer.receiver);
        if (!current_receiver || !current_receiver->active) {
            pthread_mutex_unlock(&server.clients_mutex);
            log_message("[FILE ERROR] Receiver %s disconnected before filename proposal", transfer.receiver);
            
            // Clean up and notify sender
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file (receiver disconnected): %s", transfer.filename);
            }
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }
        pthread_mutex_unlock(&server.clients_mutex);
        
        send_json_message(receiver_socket, "filename_proposal", filename_proposal, "success");
        log_message("[FILE] Proposing filename '%s' to %s", potential_filename, transfer.receiver);
        
        // Wait for client response with timeout
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);
        FD_SET(receiver_socket, &read_fds);
        timeout.tv_sec = 30; // 30 second timeout
        timeout.tv_usec = 0;
        
        int select_result = select(receiver_socket + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result <= 0) {
            log_message("[FILE ERROR] Timeout or error waiting for filename response from %s", transfer.receiver);
            
            // Clean up temporary file
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file (timeout): %s", transfer.filename);
            }
            
            // Notify both users about timeout
            pthread_mutex_lock(&server.clients_mutex);
            current_receiver = find_client_by_username(transfer.receiver);
            client_t *current_sender = find_client_by_username(transfer.sender);
            
            if (current_receiver && current_receiver->active) {
                send_json_message(current_receiver->socket, "error", "File transfer timeout", "error");
            }
            if (current_sender && current_sender->active) {
                send_json_message(current_sender->socket, "error", "File transfer timeout", "error");
            }
            pthread_mutex_unlock(&server.clients_mutex);
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }
        
        // Wait for client response (OK or NOT)
        char response_buffer[256];
        int response_received = recv(receiver_socket, response_buffer, sizeof(response_buffer) - 1, MSG_DONTWAIT);
        if (response_received <= 0) {
            log_message("[FILE ERROR] No response from client %s for filename proposal", transfer.receiver);
            
            // Clean up temporary file
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file (no response): %s", transfer.filename);
            }
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }
        
        response_buffer[response_received] = '\0';
        
        // Parse client response
        if (strncmp(response_buffer, "FILENAME_OK", 11) == 0) {
            // Client accepts original filename
            strcpy(final_filename, potential_filename);
            log_message("[FILE] Client %s accepted filename '%s'", transfer.receiver, final_filename);
        } else if (strncmp(response_buffer, "FILENAME_NOT", 12) == 0) {
            // Client requests filename change - generate alternative
            char base_name[256];
            char extension[64] = "";
            
            // Extract base name and extension
            strcpy(base_name, potential_filename);
            char *ext_pos = strrchr(base_name, '.');
            if (ext_pos) {
                strcpy(extension, ext_pos);
                *ext_pos = '\0';
            }
            
            // Generate timestamped filename
            time_t now = time(NULL);
            snprintf(final_filename, sizeof(final_filename), "%s_%ld%s", base_name, now, extension);
            log_message("[FILE] Client %s requested rename: '%s' → '%s'", 
                       transfer.receiver, potential_filename, final_filename);
        } else {
            // Invalid response, default to original filename
            strcpy(final_filename, potential_filename);
            log_message("[FILE WARNING] Invalid response from %s, using original filename '%s'", 
                       transfer.receiver, final_filename);
        }

        // Step 1: Send file transfer initiation to recipient with resolved filename
        char file_header[BUFFER_SIZE];
        snprintf(file_header, sizeof(file_header), 
                 "FILE_TRANSFER_START:%s:%s:%zu", 
                 final_filename, transfer.sender, transfer.file_size);
        
        // Check receiver is still connected before starting transfer
        pthread_mutex_lock(&server.clients_mutex);
        current_receiver = find_client_by_username(transfer.receiver);
        if (!current_receiver || !current_receiver->active) {
            pthread_mutex_unlock(&server.clients_mutex);
            log_message("[FILE ERROR] Receiver %s disconnected before file transfer", transfer.receiver);
            
            // Clean up temporary file
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file (receiver disconnected): %s", transfer.filename);
            }
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }
        pthread_mutex_unlock(&server.clients_mutex);
        
        send_json_message(receiver_socket, "file_transfer_start", file_header, "success");

        // Give client time to prepare
        usleep(500000); // 500ms

        // Step 2: Open and send the file data directly to recipient
        FILE *src_file = fopen(transfer.filename, "rb");
        if (!src_file) {
            log_message("[FILE ERROR] Cannot open source file: %s", transfer.filename);
            
            // Notify both users about the error
            pthread_mutex_lock(&server.clients_mutex);
            current_receiver = find_client_by_username(transfer.receiver);
            client_t *current_sender = find_client_by_username(transfer.sender);
            
            if (current_receiver && current_receiver->active) {
                send_json_message(current_receiver->socket, "error", "File transfer failed - source file error", "error");
            }
            if (current_sender && current_sender->active) {
                send_json_message(current_sender->socket, "error", "File transfer failed - source file error", "error");
            }
            pthread_mutex_unlock(&server.clients_mutex);
            
            sem_post(&server.upload_queue.upload_slots);
            continue;
        }

        char buffer[8192];
        size_t bytes_sent = 0;
        size_t bytes_read;
        
        log_message("[FILE] Sending file data directly to %s...", transfer.receiver);
        
        // Send file data with error checking
        int transfer_failed = 0;
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
            int send_result = send(receiver_socket, buffer, bytes_read, MSG_NOSIGNAL);
            if (send_result <= 0) {
                log_message("[FILE ERROR] Failed to send file data to %s (connection lost)", transfer.receiver);
                transfer_failed = 1;
                break;
            }
            bytes_sent += bytes_read;
        }

        fclose(src_file);

        // Step 3: Send completion notification or handle failure
        if (!transfer_failed && bytes_sent == transfer.file_size) {
            log_message("[FILE SUCCESS] File %s (%zu bytes) sent directly to %s", 
                        original_filename, bytes_sent, transfer.receiver);
            
            char completion_msg[BUFFER_SIZE];
            snprintf(completion_msg, sizeof(completion_msg), 
                     "FILE_TRANSFER_END:%s:%zu", original_filename, bytes_sent);
            
            // Check receiver is still connected before sending completion message
            pthread_mutex_lock(&server.clients_mutex);
            current_receiver = find_client_by_username(transfer.receiver);
            if (current_receiver && current_receiver->active) {
                send_json_message(current_receiver->socket, "file_transfer_end", completion_msg, "success");
            }
            pthread_mutex_unlock(&server.clients_mutex);
            
            // Clean up temporary file
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed temporary file: %s", transfer.filename);
            }
            
            // Notify sender of successful transfer
            pthread_mutex_lock(&server.clients_mutex);
            client_t *current_sender = find_client_by_username(transfer.sender);
            if (current_sender && current_sender->active) {
                char success_msg[512];
                snprintf(success_msg, sizeof(success_msg), 
                         "File '%s' successfully sent to %s", original_filename, transfer.receiver);
                send_json_message(current_sender->socket, "file_complete", success_msg, "success");
            }
            pthread_mutex_unlock(&server.clients_mutex);
        } else {
            log_message("[FILE ERROR] Transfer incomplete to %s: expected %zu, sent %zu", 
                        transfer.receiver, transfer.file_size, bytes_sent);
            
            // Clean up temporary file
            if (strstr(transfer.filename, "temp_") == transfer.filename) {
                unlink(transfer.filename);
                log_message("[FILE CLEANUP] Removed failed temporary file: %s", transfer.filename);
            }
            
            // Notify both sender and receiver of failure
            pthread_mutex_lock(&server.clients_mutex);
            current_receiver = find_client_by_username(transfer.receiver);
            client_t *current_sender = find_client_by_username(transfer.sender);
            
            if (current_receiver && current_receiver->active) {
                send_json_message(current_receiver->socket, "error", "File transfer failed", "error");
            }
            if (current_sender && current_sender->active) {
                send_json_message(current_sender->socket, "error", "File transfer failed", "error");
            }
            pthread_mutex_unlock(&server.clients_mutex);
        }

        sem_post(&server.upload_queue.upload_slots);
    }

    return NULL;
}

// Clean up pending file transfers for a specific user when they disconnect
void cleanup_pending_files_for_user(const char *username) {
    if (!username) return;
    
    pthread_mutex_lock(&server.upload_queue.mutex);
    
    int cleaned_count = 0;
    // Check all pending transfers in the queue
    for (int i = 0; i < server.upload_queue.count; i++) {
        int index = (server.upload_queue.head + i) % MAX_UPLOAD_QUEUE;
        file_transfer_t *transfer = &server.upload_queue.transfers[index];
        
        // Clean up files where the user is sender or receiver
        if (strcmp(transfer->sender, username) == 0 || strcmp(transfer->receiver, username) == 0) {
            // Delete the temporary file if it exists
            if (strstr(transfer->filename, "temp_") == transfer->filename) {
                if (unlink(transfer->filename) == 0) {
                    log_message("[FILE CLEANUP] Removed pending file for disconnected user %s: %s", 
                               username, transfer->filename);
                    cleaned_count++;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&server.upload_queue.mutex);
    
    // Also clean up any orphaned temp files for this user
    char user_pattern[128];
    snprintf(user_pattern, sizeof(user_pattern), "files/temp_%s_", username);
    
    // Use system command to find and remove user-specific temp files
    char cleanup_cmd[256];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "find files/ -name 'temp_%s_*' -type f -delete 2>/dev/null", username);
    int result = system(cleanup_cmd);
    (void)result; // Suppress unused variable warning
    
    if (cleaned_count > 0) {
        log_message("[FILE CLEANUP] Cleaned up %d pending files for user %s", cleaned_count, username);
    }
}

// Clean up all temporary files in the files directory
void cleanup_all_temporary_files(void) {
    log_message("[FILE CLEANUP] Starting cleanup of all temporary files...");
    
    // Use system command to remove all temp files
    int result = system("find files/ -name 'temp_*' -type f -delete 2>/dev/null");
    (void)result; // Suppress unused variable warning
    
    // Try to remove empty directories
    result = system("find files/ -type d -empty -delete 2>/dev/null");
    (void)result; // Suppress unused variable warning
    
    log_message("[FILE CLEANUP] Temporary file cleanup completed");
}

// Clean up all pending files in the queue when server shuts down
void cleanup_queue_on_shutdown(void) {
    log_message("[FILE CLEANUP] Cleaning up pending file transfers on shutdown...");
    
    pthread_mutex_lock(&server.upload_queue.mutex);
    
    int cleaned_count = 0;
    // Clean up all pending transfers
    for (int i = 0; i < server.upload_queue.count; i++) {
        int index = (server.upload_queue.head + i) % MAX_UPLOAD_QUEUE;
        file_transfer_t *transfer = &server.upload_queue.transfers[index];
        
        // Delete the temporary file if it exists
        if (strstr(transfer->filename, "temp_") == transfer->filename) {
            if (unlink(transfer->filename) == 0) {
                log_message("[FILE CLEANUP] Removed pending file: %s", transfer->filename);
                cleaned_count++;
            }
        }
    }
    
    // Clear the queue
    server.upload_queue.head = 0;
    server.upload_queue.tail = 0;
    server.upload_queue.count = 0;
    
    pthread_mutex_unlock(&server.upload_queue.mutex);
    
    // Clean up any remaining temporary files
    cleanup_all_temporary_files();
    
    log_message("[FILE CLEANUP] Cleaned up %d pending files on shutdown", cleaned_count);
}

// Clean up orphaned files that are older than a certain threshold
void cleanup_orphaned_files(void) {
    log_message("[FILE CLEANUP] Checking for orphaned temporary files...");
    
    // Use a more sophisticated approach to clean old files
    char find_cmd[512];
    snprintf(find_cmd, sizeof(find_cmd), 
             "find files/ -name 'temp_*' -type f -mmin +60 -delete 2>/dev/null"); // Files older than 60 minutes
    
    int result = system(find_cmd);
    (void)result; // Suppress unused variable warning
    
    log_message("[FILE CLEANUP] Orphaned file cleanup completed");
}

// Periodic cleanup thread function
void* periodic_cleanup_thread(void *arg) {
    (void)arg; // Suppress unused parameter warning
    
    while (server.running) {
        // Sleep for 30 minutes
        sleep(1800);
        
        if (!server.running) break;
        
        // Perform periodic cleanup
        cleanup_orphaned_files();
    }
    
    return NULL;
}

// Clean up client resources
void cleanup_client(client_t *client) {
    if (!client->active) return;
    
    log_message("[LOGOUT] user '%s' disconnected", client->username);
    
    // Clean up pending file transfers for this user
    cleanup_pending_files_for_user(client->username);
    
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
    else if (strcmp(token, "/sendfile") == 0) {
        char *filename = strtok(NULL, " ");
        char *target_user = strtok(NULL, " ");
        char *file_size_str = strtok(NULL, " ");
        if (!filename || !target_user || !file_size_str) {
            send_json_message(client->socket, "error", "Usage: /sendfile <filename> <username> <file_size>", "error");
            return;
        }
        
        handle_sendfile_command(client, filename, target_user, file_size_str);
    }
    // Update /exit command handling to clean up client resources immediately
    else if (strcmp(token, "/exit") == 0) {
        send_json_message(client->socket, "system", "Goodbye!", "success");
        
        // Clean up pending file transfers for this user before disconnecting
        cleanup_pending_files_for_user(client->username);
        
        cleanup_client(client); // Clean up client resources immediately
        pthread_exit(NULL); // Exit the client thread
    }
    else {
        send_json_message(client->socket, "error", 
            "Unknown command. Available: /join, /leave, /broadcast, /whisper, /sendfile, /exit", "error");
    }
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
                "Welcome! You can now use commands: /join, /leave, /broadcast, /whisper, /sendfile, /exit", "success");
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
    
    // Initialize the file upload queue
    init_upload_queue();
    
    // Create the file processor thread
    if (pthread_create(&server.file_processor_thread, NULL, process_file_transfers, NULL) != 0) {
        perror("Failed to create file processor thread");
        exit(EXIT_FAILURE);
    }
    
    // Create the periodic cleanup thread
    pthread_t cleanup_thread;
    if (pthread_create(&cleanup_thread, NULL, periodic_cleanup_thread, NULL) != 0) {
        perror("Failed to create periodic cleanup thread");
        exit(EXIT_FAILURE);
    }
    
    // Accept client connections
    while (server.running && !shutdown_requested) {
        int client_socket = accept(server.server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (server.running && !shutdown_requested) {
                perror("Accept failed");
            }
            continue;
        }
        
        // Check if shutdown was requested
        if (shutdown_requested) {
            close(client_socket);
            break;
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
    
    // Graceful shutdown sequence
    log_message("[SERVER] Starting graceful shutdown...");
    
    // Wait for file processor thread to finish
    if (server.file_processor_thread) {
        pthread_cond_broadcast(&server.upload_queue.not_empty); // Wake up the thread
        pthread_join(server.file_processor_thread, NULL);
    }
    
    // Clean up remaining clients
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
        server.log_file = NULL;
    }
    
    // Destroy the file upload queue
    destroy_upload_queue();
    
    // Destroy mutexes last
    pthread_mutex_destroy(&server.clients_mutex);
    pthread_mutex_destroy(&server.rooms_mutex);
    pthread_mutex_destroy(&server.log_mutex);
    
    log_message("[SERVER] Server shutdown complete");
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