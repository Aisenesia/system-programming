#!/usr/bin/env python3
"""
Exact Python replica of the C chat client - Compatible with C server
"""

import socket
import threading
import time
import signal
import sys
import os
import json
import select
from datetime import datetime

# Constants matching C client exactly
BUFFER_SIZE = 2048
MAX_MESSAGE_LEN = 1024
MAX_FILE_SIZE = 3 * 1024 * 1024  # 3MB

# ANSI color codes - exact match to C client
COLOR_RED = "\033[91m"
COLOR_GREEN = "\033[92m"
COLOR_YELLOW = "\033[93m"
COLOR_BLUE = "\033[94m"
COLOR_PURPLE = "\033[95m"
COLOR_CYAN = "\033[96m"
COLOR_WHITE = "\033[97m"
COLOR_RESET = "\033[0m"
COLOR_DEBUG = "\033[90m"

DEBUG = False

def debug_print(message):
    """Debug print function matching C client"""
    if DEBUG:
        print(f"{COLOR_DEBUG}{message}{COLOR_RESET}")

def print_colored(text, color):
    """Print text with color - exact C client match"""
    print(f"{color}{text}{COLOR_RESET}")
    sys.stdout.flush()

def show_help():
    """Show help message - exact C client match"""
    print_colored("\nAvailable Commands:", COLOR_CYAN)
    print_colored("/join <room_name>     - Join or create a room", COLOR_CYAN)
    print_colored("/leave                - Leave current room", COLOR_CYAN)
    print_colored("/broadcast <message>  - Send message to everyone in current room", COLOR_CYAN)
    print_colored("/whisper <user> <msg> - Send private message to user", COLOR_CYAN)
    print_colored("/sendfile <file> <user> - Send file to user (max 3MB, .txt/.pdf/.jpg/.png)", COLOR_CYAN)
    print_colored("/exit                 - Disconnect from server", COLOR_CYAN)
    print_colored("/help                 - Show this help message", COLOR_CYAN)

def validate_file(filename):
    """Validate file for sending - exact C client logic"""
    if not filename or len(filename) == 0:
        return False
    
    # Check if file exists and get size
    try:
        stat_info = os.stat(filename)
    except OSError:
        return False  # File doesn't exist
    
    # Check file size (max 3MB)
    if stat_info.st_size > MAX_FILE_SIZE:
        return False
    
    # Check file extension
    ext = os.path.splitext(filename)[1].lower()
    if ext in ['.txt', '.pdf', '.jpg', '.png']:
        return True
    
    return False

def get_file_size(filename):
    """Get file size in bytes - exact C client logic"""
    try:
        return os.path.getsize(filename)
    except OSError:
        return -1

class ChatClient:
    def __init__(self):
        self.socket = None
        self.running = False
        self.receiver_thread = None
        
        # Setup signal handler
        signal.signal(signal.SIGINT, self.signal_handler)

    def signal_handler(self, signum, frame):
        """Handle SIGINT for graceful shutdown - exact C client match"""
        print_colored("Disconnecting...", COLOR_YELLOW)
        debug_print("[DEBUG] SIGINT received, shutting down client...")
        self.running = False
        self.disconnect_from_server()
        sys.exit(0)

    def extract_json_fields(self, message_str):
        """Extract JSON fields manually for better compatibility with C version"""
        type_val = content_val = status_val = timestamp_val = ""
        
        # Extract type
        type_start = message_str.find('"type":"')
        if type_start != -1:
            type_start += 8
            type_end = message_str.find('"', type_start)
            if type_end != -1:
                type_val = message_str[type_start:type_end]
        
        # Extract content
        content_start = message_str.find('"content":"')
        if content_start != -1:
            content_start += 11
            content_end = message_str.find('"', content_start)
            if content_end != -1:
                content_val = message_str[content_start:content_end]
        
        # Extract status
        status_start = message_str.find('"status":"')
        if status_start != -1:
            status_start += 10
            status_end = message_str.find('"', status_start)
            if status_end != -1:
                status_val = message_str[status_start:status_end]
        
        # Extract timestamp
        timestamp_start = message_str.find('"timestamp":"')
        if timestamp_start != -1:
            timestamp_start += 13
            timestamp_end = message_str.find('"', timestamp_start)
            if timestamp_end != -1:
                timestamp_val = message_str[timestamp_start:timestamp_end]
        
        return type_val, content_val, status_val, timestamp_val

    def process_message(self, message_str):
        """Process incoming message - exact C client logic"""
        debug_print(f"[DEBUG] Raw message received: {message_str}")
        
        # Extract JSON fields manually like C client
        msg_type, content, status, timestamp = self.extract_json_fields(message_str)
        
        if not msg_type or not content:
            print_colored("[ERROR] Malformed message received", COLOR_RED)
            debug_print(f"[DEBUG] Malformed message: {message_str}")
            return
        
        debug_print(f"[DEBUG] Extracted type: {msg_type}, content: {content}, status: {status or 'N/A'}, timestamp: {timestamp or 'N/A'}")
        
        if msg_type == "system":
            formatted_message = f"[SYSTEM] {content}"
            print_colored(formatted_message, COLOR_CYAN)
            
        elif msg_type == "success":
            formatted_message = f"[SUCCESS] {content}"
            print_colored(formatted_message, COLOR_GREEN)
            
        elif msg_type == "error":
            formatted_message = f"[ERROR] {content}"
            print_colored(formatted_message, COLOR_RED)
            
        elif msg_type == "broadcast":
            formatted_message = f"[{timestamp}] {content}" if timestamp else content
            print_colored(formatted_message, COLOR_YELLOW)
            
        elif msg_type == "whisper":
            formatted_message = f"[Private] [{timestamp}] {content}" if timestamp else f"[Private] {content}"
            print_colored(formatted_message, COLOR_PURPLE)
            
        elif msg_type == "notification":
            formatted_message = f"[INFO] {content}"
            print_colored(formatted_message, COLOR_BLUE)
            
        elif msg_type == "file_request":
            formatted_message = f"[FILE REQUEST] {content}"
            print_colored(formatted_message, COLOR_CYAN)
            
        elif msg_type == "file_incoming":
            formatted_message = f"[FILE INCOMING] {content}"
            print_colored(formatted_message, COLOR_CYAN)
            print_colored("[INFO] File is being prepared by server...", COLOR_CYAN)
            
        elif msg_type == "file_transfer_start":
            # Handle file transfer start - exact C client logic
            self.handle_file_transfer_start(content)
            
        elif msg_type == "file_transfer_end":
            # Handle file transfer completion notification
            self.handle_file_transfer_end(content)
            
        elif msg_type == "file_complete":
            formatted_message = f"[SUCCESS] [{timestamp}] {content}" if timestamp else f"[SUCCESS] {content}"
            print_colored(formatted_message, COLOR_GREEN)
            
        elif msg_type == "filename_proposal":
            # Handle filename proposal from server - exact C client logic
            self.handle_filename_proposal(content)
            
        else:
            formatted_message = f"[UNKNOWN] [{timestamp}] {content}" if timestamp else f"[UNKNOWN] {content}"
            print_colored(formatted_message, COLOR_WHITE)
        
        debug_print(f"[DEBUG] Processed message type: {msg_type}, content: {content}")

    def handle_file_transfer_start(self, content):
        """Handle file transfer start - exact C client logic"""
        # Parse content: "FILE_TRANSFER_START:filename:sender:size"
        parts = content.split(':')
        if len(parts) >= 4 and parts[0] == "FILE_TRANSFER_START":
            filename = parts[1]
            sender = parts[2]
            try:
                file_size = int(parts[3])
            except ValueError:
                print_colored("[ERROR] Invalid file transfer start message", COLOR_RED)
                return
            
            formatted_message = f"[FILE INCOMING] Receiving '{filename}' from {sender} ({file_size / 1024:.1f} KB)..."
            print_colored(formatted_message, COLOR_CYAN)
            
            # Prepare to receive file data
            try:
                with open(filename, "wb") as file:
                    bytes_received = 0
                    
                    print_colored("[INFO] Receiving file data...", COLOR_CYAN)
                    
                    # Receive file data in chunks
                    while bytes_received < file_size:
                        bytes_to_read = min(8192, file_size - bytes_received)
                        
                        try:
                            data = self.socket.recv(bytes_to_read)
                            if not data:
                                print_colored("[ERROR] Connection lost during file transfer", COLOR_RED)
                                os.unlink(filename)  # Remove incomplete file
                                return
                            
                            file.write(data)
                            bytes_received += len(data)
                            
                        except socket.error:
                            print_colored("[ERROR] Failed to receive file data", COLOR_RED)
                            os.unlink(filename)  # Remove incomplete file
                            return
                    
                    if bytes_received == file_size:
                        formatted_message = f"[FILE SUCCESS] File '{filename}' received from {sender} ({bytes_received / 1024:.1f} KB) - saved locally"
                        print_colored(formatted_message, COLOR_GREEN)
                    else:
                        print_colored("[ERROR] File transfer incomplete", COLOR_RED)
                        os.unlink(filename)  # Remove incomplete file
                        
            except IOError:
                print_colored("[ERROR] Cannot create file for writing", COLOR_RED)
        else:
            print_colored("[ERROR] Invalid file transfer start message", COLOR_RED)

    def handle_file_transfer_end(self, content):
        """Handle file transfer completion notification - exact C client logic"""
        # Parse content: "FILE_TRANSFER_END:filename:size"
        parts = content.split(':')
        if len(parts) >= 3 and parts[0] == "FILE_TRANSFER_END":
            filename = parts[1]
            try:
                file_size = int(parts[2])
                formatted_message = f"[FILE COMPLETE] Transfer of '{filename}' completed ({file_size / 1024:.1f} KB)"
                print_colored(formatted_message, COLOR_GREEN)
            except ValueError:
                print_colored("[ERROR] Invalid file transfer end message", COLOR_RED)

    def handle_filename_proposal(self, content):
        """Handle filename proposal from server - exact C client logic"""
        # Parse content: "FILENAME_PROPOSAL:filename:sender:size"
        parts = content.split(':')
        if len(parts) >= 4 and parts[0] == "FILENAME_PROPOSAL":
            proposed_filename = parts[1]
            sender = parts[2]
            try:
                file_size = int(parts[3])
            except ValueError:
                print_colored("[ERROR] Invalid filename proposal message", COLOR_RED)
                self.socket.send(b"FILENAME_OK")  # Send OK as fallback
                return
            
            formatted_message = f"[FILE PROPOSAL] {sender} wants to send '{proposed_filename}' ({file_size / 1024:.1f} KB)"
            print_colored(formatted_message, COLOR_CYAN)
            sys.stdout.flush()
            
            # Automatically check if file already exists locally
            file_exists = os.path.exists(proposed_filename)
            
            if file_exists:
                try:
                    existing_size = os.path.getsize(proposed_filename)
                    conflict_msg = f"[WARNING] File '{proposed_filename}' already exists locally ({existing_size / 1024:.1f} KB) - requesting rename"
                    print_colored(conflict_msg, COLOR_YELLOW)
                    sys.stdout.flush()
                    
                    # Automatically request filename change to avoid overwriting
                    self.socket.send(b"FILENAME_NOT")
                    print_colored("[INFO] Filename will be automatically renamed by server", COLOR_CYAN)
                    sys.stdout.flush()
                except OSError:
                    # File exists but can't get size, still request rename
                    self.socket.send(b"FILENAME_NOT")
                    print_colored("[INFO] Filename will be automatically renamed by server", COLOR_CYAN)
                    sys.stdout.flush()
            else:
                # File doesn't exist, accept original filename
                self.socket.send(b"FILENAME_OK")
                print_colored("[INFO] Filename accepted. Preparing to receive file...", COLOR_GREEN)
                sys.stdout.flush()
        else:
            print_colored("[ERROR] Invalid filename proposal message", COLOR_RED)
            sys.stdout.flush()
            # Send OK as fallback
            self.socket.send(b"FILENAME_OK")

    def receive_messages(self):
        """Receive messages from server thread - exact C client logic"""
        message_buffer = ""
        
        while self.running:
            # Use select with timeout like C client
            ready, _, _ = select.select([self.socket], [], [], 1.0)
            
            if not ready:
                continue  # Timeout, check if we should still run
            
            try:
                data = self.socket.recv(BUFFER_SIZE)
                if not data:
                    if self.running:
                        print_colored("Connection lost or closed by server", COLOR_RED)
                    break
                
                message_buffer += data.decode('utf-8')
                
                # Process complete messages (ending with newline)
                while '\n' in message_buffer:
                    message, message_buffer = message_buffer.split('\n', 1)
                    if message.strip():
                        self.process_message(message.strip())
                        
            except socket.error as e:
                if self.running:
                    print_colored(f"Socket error: {e}", COLOR_RED)
                break
            except UnicodeDecodeError:
                # Skip invalid UTF-8 data
                continue

    def connect_to_server(self, host, port):
        """Connect to server - exact C client logic"""
        try:
            # Create socket
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            
            # Connect to server
            self.socket.connect((host, port))
            
            self.running = True
            
            success_msg = f"Connected to server at {host}:{port}"
            print_colored(success_msg, COLOR_GREEN)
            
            # Start receiver thread
            self.receiver_thread = threading.Thread(target=self.receive_messages, daemon=True)
            self.receiver_thread.start()
            
            return True
            
        except socket.error as e:
            print(f"Connection failed: {e}")
            if self.socket:
                self.socket.close()
            return False

    def handle_sendfile_command(self, input_line):
        """Handle /sendfile command locally - exact C client logic"""
        parts = input_line.split(' ')
        if len(parts) < 3:
            print_colored("[ERROR] Usage: /sendfile <filename> <username>", COLOR_RED)
            return
        
        filename = parts[1]
        username = parts[2]
        
        # Validate file exists and get size
        file_size = get_file_size(filename)
        if file_size < 0:
            print_colored("[ERROR] File not found or cannot be accessed", COLOR_RED)
            return
        
        # Validate file type and size
        if not validate_file(filename):
            print_colored("[ERROR] Invalid file type or size. Allowed: .txt, .pdf, .jpg, .png (max 3MB)", COLOR_RED)
            return
        
        confirm_msg = f"[INFO] Sending file '{filename}' ({file_size / 1024:.1f} KB) to {username}..."
        print_colored(confirm_msg, COLOR_CYAN)
        
        # Send command with file size to server
        sendfile_cmd = f"/sendfile {filename} {username} {file_size}"
        
        try:
            self.socket.send(sendfile_cmd.encode('utf-8'))
            
            # Wait for server response - either error or ready for file data
            response = self.socket.recv(BUFFER_SIZE).decode('utf-8')
            
            # Check if server is ready for file data or gave an error
            if "Ready to receive file data" in response:
                print_colored("[INFO] Uploading file data...", COLOR_CYAN)
                
                # Send file data
                try:
                    with open(filename, "rb") as file:
                        bytes_sent = 0
                        
                        # Send file data in chunks
                        while True:
                            chunk = file.read(8192)
                            if not chunk:
                                break
                            
                            self.socket.send(chunk)
                            bytes_sent += len(chunk)
                        
                        if bytes_sent == file_size:
                            print_colored("[INFO] File upload completed. Processing...", COLOR_GREEN)
                        else:
                            print_colored("[ERROR] File upload incomplete", COLOR_RED)
                            
                except IOError:
                    print_colored("[ERROR] Failed to open file for reading", COLOR_RED)
            else:
                # Server gave an error, process the response
                self.process_message(response)
                
        except socket.error:
            print_colored("[ERROR] Failed to send command to server", COLOR_RED)

    def handle_input(self):
        """Handle user input - exact C client logic"""
        while self.running:
            try:
                user_input = input().strip()
                
                # Skip empty input
                if not user_input:
                    continue
                
                # Handle local commands
                if user_input == "/help":
                    show_help()
                    continue
                
                if user_input == "/exit":
                    self.running = False
                    break
                
                # Handle /sendfile command locally
                if user_input.startswith("/sendfile "):
                    self.handle_sendfile_command(user_input)
                    continue
                
                # Send message to server
                self.socket.send(user_input.encode('utf-8'))
                
            except EOFError:
                break
            except socket.error:
                print_colored("Failed to send message", COLOR_RED)
                break

    def disconnect_from_server(self):
        """Disconnect from server - exact C client logic"""
        self.running = False
        
        if self.socket:
            self.socket.close()
            self.socket = None
        
        # Wait for receiver thread to finish
        if self.receiver_thread and self.receiver_thread.is_alive():
            self.receiver_thread.join(timeout=2.0)
        
        print_colored("Disconnected from server", COLOR_YELLOW)

def main():
    host = "127.0.0.1"
    port = 8888
    
    # Parse command line arguments - exact C client logic
    if len(sys.argv) >= 3:
        host = sys.argv[1]
        try:
            port = int(sys.argv[2])
        except ValueError:
            print("Invalid port number", file=sys.stderr)
            sys.exit(1)
    elif len(sys.argv) >= 2:
        try:
            port = int(sys.argv[1])
        except ValueError:
            print("Invalid port number", file=sys.stderr)
            sys.exit(1)
    
    if port <= 0 or port > 65535:
        print("Invalid port number", file=sys.stderr)
        sys.exit(1)
    
    print("=== Multi-threaded Chat Client ===")
    print(f"Connecting to {host}:{port}...")
    print("Type '/help' for available commands")
    print("Press Ctrl+C to quit\n")
    
    client = ChatClient()
    
    if not client.connect_to_server(host, port):
        print("Failed to connect to server", file=sys.stderr)
        sys.exit(1)
    
    # Handle user input
    client.handle_input()
    
    # Clean up
    client.disconnect_from_server()
    
    print("Connection closed")

if __name__ == "__main__":
    main()