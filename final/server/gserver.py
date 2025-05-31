#!/usr/bin/env python3
"""
Exact Python replica of the C chat server - Compatible with C client
"""

import socket
import threading
import time
import signal
import sys
import os
import json
import queue
import re
from datetime import datetime
from pathlib import Path

# Constants matching C server exactly
MAX_CLIENTS = 15
MAX_ROOMS = 50
MAX_USERNAME_LEN = 16
MAX_ROOM_NAME_LEN = 32
MAX_MESSAGE_LEN = 1024
BUFFER_SIZE = 2048
MAX_UPLOAD_QUEUE = 5
MAX_FILE_SIZE = 3 * 1024 * 1024  # 3MB
FILES_DIR = "files"

class Client:
    def __init__(self, socket, address):
        self.socket = socket
        self.username = ""
        self.room_name = ""
        self.address = address
        self.active = True
        self.thread = None

class Room:
    def __init__(self, name):
        self.name = name
        self.users = []
        self.user_count = 0

class FileTransfer:
    def __init__(self, filename, sender, receiver, file_size):
        self.filename = filename
        self.sender = sender
        self.receiver = receiver
        self.file_size = file_size
        self.in_progress = True
        self.timestamp = time.time()
        self.enqueue_time = time.time()

class ChatServer:
    def __init__(self):
        self.server_socket = None
        self.running = False
        self.clients = {}  # socket -> Client
        self.rooms = {}    # room_name -> Room
        self.client_count = 0
        self.room_count = 0
        
        # Thread locks
        self.clients_lock = threading.Lock()
        self.rooms_lock = threading.Lock()
        self.log_lock = threading.Lock()
        
        # File transfer queue
        self.upload_queue = queue.Queue(maxsize=MAX_UPLOAD_QUEUE)
        self.upload_semaphore = threading.Semaphore(MAX_UPLOAD_QUEUE)
        self.file_processor_thread = None
        
        # Log file
        self.log_file = None
        
        # Setup signal handler
        signal.signal(signal.SIGINT, self.signal_handler)

    def signal_handler(self, signum, frame):
        """Handle SIGINT for graceful shutdown - matches C server exactly"""
        self.log_message("[SERVER] Received SIGINT, shutting down gracefully...")
        self.running = False
        
        # Close all client connections
        with self.clients_lock:
            for client in self.clients.values():
                if client.active:
                    self.send_json_message(client.socket, "system", 
                        "Server is shutting down. Goodbye!", "success")
                    client.socket.close()
                    client.active = False
        
        # Close server socket
        if self.server_socket:
            self.server_socket.close()
            
        self.log_message("[SERVER] Server shutdown complete")
        sys.exit(0)

    def log_message(self, message):
        """Thread-safe logging with timestamps - matches C server format exactly"""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        formatted_msg = f"{timestamp} - {message}"
        
        with self.log_lock:
            print(formatted_msg)
            sys.stdout.flush()
            
            if self.log_file:
                self.log_file.write(formatted_msg + "\n")
                self.log_file.flush()

    def validate_username(self, username):
        """Validate username: max 16 chars, alphanumeric only - exact C match"""
        if not username or len(username) == 0 or len(username) > MAX_USERNAME_LEN:
            return False
        return username.isalnum()

    def validate_room_name(self, room_name):
        """Validate room name: max 32 chars, alphanumeric only - exact C match"""
        if not room_name or len(room_name) == 0 or len(room_name) > MAX_ROOM_NAME_LEN:
            return False
        return room_name.isalnum()

    def validate_filename(self, filename):
        """Validate filename: max 255 chars, alphanumeric and special chars - exact C match"""
        if not filename or len(filename) == 0 or len(filename) > 255:
            return False
        return all(c.isalnum() or c in '._-' for c in filename)

    def send_json_message(self, client_socket, msg_type, content, status="success"):
        """Send JSON message with exact C server format"""
        timestamp = datetime.now().strftime("%H:%M:%S")
        
        # Escape content exactly like C server
        escaped_content = content.replace('\\', '\\\\').replace('"', '\\"')
        
        json_message = {
            "type": msg_type,
            "content": escaped_content,
            "status": status,
            "timestamp": timestamp
        }
        
        # Format exactly like C server with \n terminator
        message_str = json.dumps(json_message, separators=(',', ':')) + '\n'
        
        try:
            client_socket.send(message_str.encode('utf-8'))
        except:
            self.log_message("[ERROR] Failed to send message to client")

    def find_client_by_username(self, username):
        """Find client by username"""
        for client in self.clients.values():
            if client.active and client.username == username:
                return client
        return None

    def find_client_by_socket(self, client_socket):
        """Find client by socket"""
        return self.clients.get(client_socket)

    def find_room(self, room_name):
        """Find room by name"""
        return self.rooms.get(room_name)

    def create_room(self, room_name):
        """Create new room"""
        if self.room_count >= MAX_ROOMS:
            return None
        
        room = Room(room_name)
        self.rooms[room_name] = room
        self.room_count += 1
        return room

    def add_user_to_room(self, username, room_name):
        """Add user to room"""
        room = self.find_room(room_name)
        if not room:
            room = self.create_room(room_name)
            if not room:
                return False
        
        # Check if user is already in the room
        if username in room.users:
            return True
        
        # Add user to room
        if room.user_count < MAX_CLIENTS:
            room.users.append(username)
            room.user_count += 1
            return True
        return False

    def remove_user_from_room(self, username):
        """Remove user from their current room"""
        for room in self.rooms.values():
            if username in room.users:
                room.users.remove(username)
                room.user_count -= 1
                return room.name
        return None

    def broadcast_to_room(self, room_name, message, exclude_user=None):
        """Broadcast message to all users in a room"""
        room = self.find_room(room_name)
        if not room:
            return
        
        formatted_message = f"[{room_name}] {message}"
        
        for username in room.users:
            if exclude_user and username == exclude_user:
                continue
            
            client = self.find_client_by_username(username)
            if client:
                self.send_json_message(client.socket, "notification", formatted_message, "success")

    def handle_join_command(self, client, room_name):
        """Handle /join command - exact C server logic"""
        if not self.validate_room_name(room_name):
            self.send_json_message(client.socket, "error", 
                "Invalid room name! Must be max 32 characters, alphanumeric only.", "error")
            return
        
        with self.rooms_lock:
            room = self.find_room(room_name)
            if room and room.user_count >= MAX_CLIENTS:
                self.send_json_message(client.socket, "error", 
                    "Room is full (max 15 users)", "error")
                return
            
            # Leave current room if in one
            if client.room_name:
                old_room = client.room_name
                self.remove_user_from_room(client.username)
                self.broadcast_to_room(old_room, "has left the room", client.username)
                self.log_message(f"[LEAVE] user '{client.username}' left room '{old_room}'")
            
            # Join new room
            self.add_user_to_room(client.username, room_name)
            client.room_name = room_name
        
        self.log_message(f"[JOIN] user '{client.username}' joined room '{room_name}'")
        
        room = self.find_room(room_name)
        success_msg = f"Joined room '{room_name}'. Users in room: {room.user_count if room else 1}"
        self.send_json_message(client.socket, "success", success_msg, "success")
        
        # Notify other users
        notification = f"{client.username} has joined the room"
        self.broadcast_to_room(room_name, notification, client.username)

    def handle_leave_command(self, client):
        """Handle /leave command - exact C server logic"""
        if not client.room_name:
            self.send_json_message(client.socket, "error", "You are not in any room", "error")
            return
        
        with self.rooms_lock:
            old_room = client.room_name
            self.remove_user_from_room(client.username)
            client.room_name = ""
        
        self.log_message(f"[LEAVE] user '{client.username}' left room '{old_room}'")
        
        success_msg = f"Left room '{old_room}'"
        self.send_json_message(client.socket, "success", success_msg, "success")
        
        # Notify other users
        notification = f"{client.username} has left the room"
        self.broadcast_to_room(old_room, notification, client.username)

    def handle_broadcast_command(self, client, message):
        """Handle /broadcast command - exact C server logic"""
        if not client.room_name:
            self.send_json_message(client.socket, "error", 
                "You must join a room first to broadcast messages", "error")
            return
        
        if not message or len(message) == 0:
            self.send_json_message(client.socket, "error", "Message cannot be empty", "error")
            return
        
        self.log_message(f"[BROADCAST] user '{client.username}' in room '{client.room_name}': {message}")
        
        # Broadcast to room
        broadcast_msg = f"[{client.room_name}] {client.username}: {message}"
        
        sent_count = 0
        with self.rooms_lock:
            room = self.find_room(client.room_name)
            if room:
                for username in room.users:
                    if username != client.username:
                        target = self.find_client_by_username(username)
                        if target:
                            self.send_json_message(target.socket, "broadcast", broadcast_msg, "success")
                            sent_count += 1
        
        success_msg = f"Message broadcast to {sent_count} users in room '{client.room_name}'"
        self.send_json_message(client.socket, "success", success_msg, "success")

    def handle_whisper_command(self, client, target_user, message):
        """Handle /whisper command - exact C server logic"""
        with self.clients_lock:
            target = self.find_client_by_username(target_user)
        
        if not target:
            error_msg = f"User '{target_user}' not found or offline"
            self.send_json_message(client.socket, "error", error_msg, "error")
            return
        
        whisper_msg = f"[Private] {client.username}: {message}"
        self.send_json_message(target.socket, "whisper", whisper_msg, "success")
        
        success_msg = f"Private message sent to {target_user}"
        self.send_json_message(client.socket, "success", success_msg, "success")
        
        self.log_message(f"[WHISPER] '{client.username}' to '{target_user}': {message}")

    def handle_sendfile_command(self, client, filename, target_user, file_size_str):
        """Handle /sendfile command - exact C server logic"""
        # Check if user is trying to send file to themselves
        if client.username == target_user:
            self.send_json_message(client.socket, "error", 
                "You cannot send a file to yourself", "error")
            return
        
        if not self.validate_filename(filename):
            self.send_json_message(client.socket, "error", 
                "Invalid filename! Must be max 255 characters, alphanumeric, '.', '_', or '-'.", "error")
            return
        
        with self.clients_lock:
            receiver = self.find_client_by_username(target_user)
        
        if not receiver:
            self.send_json_message(client.socket, "error", 
                "Recipient not found or offline", "error")
            return
        
        # Parse file size from the command
        try:
            file_size = int(file_size_str)
        except ValueError:
            file_size = 0
            
        if file_size <= 0 or file_size > MAX_FILE_SIZE:
            self.send_json_message(client.socket, "error", 
                "Invalid file size (max 3MB)", "error")
            return
        
        # Check file extension
        ext = os.path.splitext(filename)[1].lower()
        if ext not in ['.txt', '.pdf', '.jpg', '.png']:
            self.send_json_message(client.socket, "error", 
                "Invalid file type. Allowed: .txt, .pdf, .jpg, .png", "error")
            return
        
        # Create temp directory if it doesn't exist
        os.makedirs(FILES_DIR, exist_ok=True)
        
        # Create temporary file path
        filename_only = os.path.basename(filename)
        temp_path = os.path.join(FILES_DIR, f"temp_{client.username}_{filename_only}")
        
        # Notify client to start sending file data
        self.send_json_message(client.socket, "success", "Ready to receive file data", "success")
        
        # Receive file data from client
        try:
            with open(temp_path, "wb") as temp_file:
                bytes_received_total = 0
                
                self.log_message(f"[UPLOAD] Receiving file {filename_only} ({file_size} bytes) from {client.username} for {target_user}")
                
                while bytes_received_total < file_size:
                    bytes_to_read = min(8192, file_size - bytes_received_total)
                    data = client.socket.recv(bytes_to_read)
                    
                    if not data:
                        self.log_message(f"[UPLOAD ERROR] Connection lost while receiving file from {client.username}")
                        os.unlink(temp_path)
                        return
                    
                    temp_file.write(data)
                    bytes_received_total += len(data)
                
                if bytes_received_total == file_size:
                    self.log_message(f"[UPLOAD SUCCESS] File {filename_only} ({bytes_received_total} bytes) uploaded by {client.username}")
                    
                    # Notify receiver about incoming file
                    file_message = f"[File] {client.username} is sending you a file: {filename_only} ({file_size / 1024:.1f} KB)"
                    self.send_json_message(receiver.socket, "file_request", file_message, "success")
                    
                    # Queue the file transfer
                    if self.enqueue_file_transfer(temp_path, client.username, target_user, file_size):
                        success_msg = f"File '{filename_only}' uploaded and queued for transfer to {target_user} ({file_size / 1024:.1f} KB)"
                        self.send_json_message(client.socket, "success", success_msg, "success")
                        self.log_message(f"[FILE] File {filename_only} queued for transfer from {client.username} to {target_user}")
                    else:
                        self.send_json_message(client.socket, "error", "Failed to queue file transfer", "error")
                        os.unlink(temp_path)
                else:
                    self.log_message(f"[UPLOAD ERROR] Incomplete upload from {client.username}: expected {file_size}, got {bytes_received_total}")
                    self.send_json_message(client.socket, "error", "File upload incomplete", "error")
                    os.unlink(temp_path)
                    
        except Exception as e:
            self.log_message(f"[UPLOAD ERROR] File upload failed: {e}")
            self.send_json_message(client.socket, "error", "Server error: cannot create temporary file", "error")
            if os.path.exists(temp_path):
                os.unlink(temp_path)

    def enqueue_file_transfer(self, filename, sender, receiver, file_size):
        """Enqueue a file transfer - exact C server logic"""
        # Check current queue size
        current_queue_size = self.upload_queue.qsize()
        
        # If queue is full, notify sender about queuing
        if current_queue_size >= MAX_UPLOAD_QUEUE:
            sender_client = self.find_client_by_username(sender)
            if sender_client:
                queue_msg = f"Upload queue is full ({current_queue_size}/{MAX_UPLOAD_QUEUE}). Your file is waiting to be processed..."
                self.send_json_message(sender_client.socket, "system", queue_msg, "success")
        
        try:
            # This will block if queue is full
            transfer = FileTransfer(filename, sender, receiver, file_size)
            self.upload_queue.put(transfer, timeout=30)
            
            # Enhanced queue logging
            filename_only = os.path.basename(filename)
            self.log_message(f"[FILE-QUEUE] Upload '{filename_only}' from {sender} added to queue. Queue size: {self.upload_queue.qsize()}/{MAX_UPLOAD_QUEUE}")
            
            return True
        except queue.Full:
            return False

    def process_file_transfers(self):
        """Process file transfers in the background - exact C server logic"""
        while self.running:
            try:
                # Wait for a file transfer to be enqueued with timeout
                transfer = self.upload_queue.get(timeout=1.0)
                
                self.log_message(f"[FILE] Processing transfer: {transfer.filename} ({transfer.file_size} bytes) from {transfer.sender} to {transfer.receiver}")
                
                # Find the recipient client
                with self.clients_lock:
                    receiver = self.find_client_by_username(transfer.receiver)
                
                if not receiver:
                    self.log_message(f"[FILE ERROR] Recipient {transfer.receiver} not found or offline")
                    
                    # Clean up temporary file
                    if os.path.exists(transfer.filename):
                        os.unlink(transfer.filename)
                        self.log_message(f"[FILE CLEANUP] Removed temporary file (recipient offline): {transfer.filename}")
                    
                    # Notify sender that recipient is offline
                    sender_client = self.find_client_by_username(transfer.sender)
                    if sender_client:
                        error_msg = f"File transfer failed: {transfer.receiver} is offline"
                        self.send_json_message(sender_client.socket, "error", error_msg, "error")
                    
                    continue
                
                # Extract original filename
                temp_filename = os.path.basename(transfer.filename)
                if temp_filename.startswith("temp_"):
                    # Remove temp_ prefix and username
                    parts = temp_filename.split('_', 2)
                    if len(parts) >= 3:
                        original_filename = parts[2]
                    else:
                        original_filename = temp_filename
                else:
                    original_filename = temp_filename
                
                # Calculate and log queue wait duration
                wait_duration = time.time() - transfer.enqueue_time
                
                if wait_duration > 1.0:
                    self.log_message(f"[FILE] '{original_filename}' from user '{transfer.sender}' started upload after {wait_duration:.0f} seconds in queue")
                    
                    # Notify sender about wait time
                    sender_client = self.find_client_by_username(transfer.sender)
                    if sender_client:
                        wait_msg = f"File '{original_filename}' processed after {wait_duration:.0f} seconds in queue"
                        self.send_json_message(sender_client.socket, "system", wait_msg, "success")
                
                # Implement filename handshake with client
                filename_proposal = f"FILENAME_PROPOSAL:{original_filename}:{transfer.sender}:{transfer.file_size}"
                self.send_json_message(receiver.socket, "filename_proposal", filename_proposal, "success")
                
                self.log_message(f"[FILE] Proposing filename '{original_filename}' to {transfer.receiver}")
                
                # Wait for client response
                try:
                    response = receiver.socket.recv(256).decode('utf-8')
                    
                    if response.startswith("FILENAME_OK"):
                        final_filename = original_filename
                        self.log_message(f"[FILE] Client {transfer.receiver} accepted filename '{final_filename}'")
                    elif response.startswith("FILENAME_NOT"):
                        # Generate alternative filename
                        base_name, ext = os.path.splitext(original_filename)
                        timestamp = int(time.time())
                        final_filename = f"{base_name}_{timestamp}{ext}"
                        self.log_message(f"[FILE] Client {transfer.receiver} requested rename: '{original_filename}' → '{final_filename}'")
                    else:
                        final_filename = original_filename
                        self.log_message(f"[FILE WARNING] Invalid response from {transfer.receiver}, using original filename '{final_filename}'")
                
                except:
                    self.log_message(f"[FILE ERROR] No response from client {transfer.receiver} for filename proposal")
                    self.send_json_message(receiver.socket, "error", "File transfer failed - no response", "error")
                    if os.path.exists(transfer.filename):
                        os.unlink(transfer.filename)
                    continue
                
                # Send file transfer initiation
                file_header = f"FILE_TRANSFER_START:{final_filename}:{transfer.sender}:{transfer.file_size}"
                self.send_json_message(receiver.socket, "file_transfer_start", file_header, "success")
                
                # Give client time to prepare
                time.sleep(0.5)
                
                # Send the file data directly to recipient
                try:
                    with open(transfer.filename, "rb") as src_file:
                        bytes_sent = 0
                        
                        self.log_message(f"[FILE] Sending file data directly to {transfer.receiver}...")
                        
                        while True:
                            chunk = src_file.read(8192)
                            if not chunk:
                                break
                            
                            sent = receiver.socket.send(chunk)
                            if sent <= 0:
                                self.log_message(f"[FILE ERROR] Failed to send file data to {transfer.receiver}")
                                break
                            
                            bytes_sent += sent
                    
                    # Send completion notification
                    if bytes_sent == transfer.file_size:
                        self.log_message(f"[FILE SUCCESS] File {original_filename} ({bytes_sent} bytes) sent directly to {transfer.receiver}")
                        
                        completion_msg = f"FILE_TRANSFER_END:{original_filename}:{bytes_sent}"
                        self.send_json_message(receiver.socket, "file_transfer_end", completion_msg, "success")
                        
                        # Clean up temporary file
                        if os.path.exists(transfer.filename):
                            os.unlink(transfer.filename)
                            self.log_message(f"[FILE CLEANUP] Removed temporary file: {transfer.filename}")
                        
                        # Notify sender of successful transfer
                        sender_client = self.find_client_by_username(transfer.sender)
                        if sender_client:
                            success_msg = f"File '{original_filename}' successfully sent to {transfer.receiver}"
                            self.send_json_message(sender_client.socket, "file_complete", success_msg, "success")
                    else:
                        self.log_message(f"[FILE ERROR] Transfer incomplete to {transfer.receiver}: expected {transfer.file_size}, sent {bytes_sent}")
                        
                        # Clean up temporary file
                        if os.path.exists(transfer.filename):
                            os.unlink(transfer.filename)
                        
                        # Notify both sender and receiver of failure
                        sender_client = self.find_client_by_username(transfer.sender)
                        if sender_client:
                            self.send_json_message(sender_client.socket, "error", "File transfer failed", "error")
                        self.send_json_message(receiver.socket, "error", "File transfer failed", "error")
                
                except Exception as e:
                    self.log_message(f"[FILE ERROR] Cannot open source file: {transfer.filename}")
                    self.send_json_message(receiver.socket, "error", "File transfer failed - source file error", "error")
                
            except queue.Empty:
                continue
            except Exception as e:
                if self.running:
                    self.log_message(f"[FILE ERROR] File processor error: {e}")

    def handle_command(self, client, command):
        """Handle client commands - exact C server logic"""
        parts = command.split(' ')
        cmd = parts[0]
        
        if cmd == "/join":
            if len(parts) < 2:
                self.send_json_message(client.socket, "error", "Usage: /join <room_name>", "error")
                return
            self.handle_join_command(client, parts[1])
            
        elif cmd == "/leave":
            self.handle_leave_command(client)
            
        elif cmd == "/broadcast":
            if len(parts) < 2:
                self.send_json_message(client.socket, "error", "Usage: /broadcast <message>", "error")
                return
            message = ' '.join(parts[1:])
            self.handle_broadcast_command(client, message)
            
        elif cmd == "/whisper":
            if len(parts) < 3:
                self.send_json_message(client.socket, "error", "Usage: /whisper <username> <message>", "error")
                return
            target_user = parts[1]
            message = ' '.join(parts[2:])
            self.handle_whisper_command(client, target_user, message)
            
        elif cmd == "/sendfile":
            if len(parts) < 4:
                self.send_json_message(client.socket, "error", "Usage: /sendfile <filename> <username> <file_size>", "error")
                return
            filename = parts[1]
            target_user = parts[2]
            file_size_str = parts[3]
            self.handle_sendfile_command(client, filename, target_user, file_size_str)
            
        elif cmd == "/exit":
            self.send_json_message(client.socket, "system", "Goodbye!", "success")
            self.cleanup_client(client)
            return False  # Signal to exit client thread
            
        else:
            self.send_json_message(client.socket, "error", 
                "Unknown command. Available: /join, /leave, /broadcast, /whisper, /sendfile, /exit", "error")
        
        return True

    def cleanup_client(self, client):
        """Clean up client resources - exact C server logic"""
        if not client.active:
            return
        
        self.log_message(f"[LOGOUT] user '{client.username}' disconnected")
        
        with self.clients_lock:
            client.active = False
            self.client_count -= 1
            if client.socket in self.clients:
                del self.clients[client.socket]
        
        with self.rooms_lock:
            if client.room_name:
                notification = f"{client.username} has disconnected"
                self.broadcast_to_room(client.room_name, notification, client.username)
                self.remove_user_from_room(client.username)
        
        try:
            client.socket.close()
        except:
            pass

    def handle_client(self, client):
        """Handle individual client connection - exact C server logic"""
        # Send welcome message
        self.send_json_message(client.socket, "system", 
            "Welcome! Please enter your username (max 16 chars, alphanumeric):", "success")
        
        # Get username
        while client.active and self.running:
            try:
                data = client.socket.recv(BUFFER_SIZE)
                if not data:
                    break
                
                message = data.decode('utf-8').strip()
                
                # Remove newlines
                message = message.replace('\n', '').replace('\r', '')
                
                if not client.username:
                    # Username registration
                    if not self.validate_username(message):
                        self.send_json_message(client.socket, "error", 
                            "Invalid username! Must be max 16 characters, alphanumeric only.", "error")
                        continue
                    
                    with self.clients_lock:
                        existing = self.find_client_by_username(message)
                        if existing:
                            self.send_json_message(client.socket, "error", 
                                "Username already taken! Please choose another.", "error")
                            continue
                        
                        client.username = message
                    
                    self.log_message(f"[LOGIN] user '{client.username}' connected from {client.address[0]}")
                    self.send_json_message(client.socket, "success", 
                        "Welcome! You can now use commands: /join, /leave, /broadcast, /whisper, /sendfile, /exit", "success")
                    continue
                
                # Handle commands
                if message.startswith('/'):
                    if not self.handle_command(client, message):
                        break  # Client requested exit
                else:
                    self.send_json_message(client.socket, "error", 
                        "Please use commands starting with '/' or type /exit to quit", "error")
                        
            except Exception as e:
                if client.active:
                    self.log_message(f"[ERROR] Client handler error: {e}")
                break
        
        self.cleanup_client(client)

    def start_server(self, port):
        """Start the server - exact C server logic"""
        # Open log file
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_filename = f"server_log_{timestamp}.txt"
        
        try:
            self.log_file = open(log_filename, "w")
        except:
            print("Failed to open log file")
        
        # Create socket
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        # Bind and listen
        self.server_socket.bind(('', port))
        self.server_socket.listen(MAX_CLIENTS)
        
        self.running = True
        self.log_message(f"[SERVER] Chat server started on port {port}")
        self.log_message(f"[SERVER] Log file: {log_filename}")
        
        # Start file processor thread
        self.file_processor_thread = threading.Thread(target=self.process_file_transfers, daemon=True)
        self.file_processor_thread.start()
        
        # Accept client connections
        while self.running:
            try:
                client_socket, client_address = self.server_socket.accept()
                
                if not self.running:
                    client_socket.close()
                    break
                
                with self.clients_lock:
                    if self.client_count >= MAX_CLIENTS:
                        self.log_message("[ERROR] Maximum clients reached, rejecting connection")
                        self.send_json_message(client_socket, "error", "Server full, try again later", "error")
                        client_socket.close()
                        continue
                    
                    # Create client
                    client = Client(client_socket, client_address)
                    self.clients[client_socket] = client
                    self.client_count += 1
                
                # Create thread to handle client
                client.thread = threading.Thread(target=self.handle_client, args=(client,), daemon=True)
                client.thread.start()
                
            except Exception as e:
                if self.running:
                    self.log_message(f"[ERROR] Accept failed: {e}")
                break
        
        # Cleanup
        self.log_message("[SERVER] Starting graceful shutdown...")
        
        # Clean up remaining clients
        with self.clients_lock:
            for client in list(self.clients.values()):
                if client.active:
                    client.socket.close()
                    client.active = False
        
        # Close server socket
        if self.server_socket:
            self.server_socket.close()
        
        # Close log file
        if self.log_file:
            self.log_file.close()
        
        self.log_message("[SERVER] Server shutdown complete")

def main():
    port = 8888
    
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
            if port <= 0 or port > 65535:
                raise ValueError()
        except ValueError:
            print("Invalid port number")
            sys.exit(1)
    
    print("Starting Multi-threaded Chat Server in Python")
    print(f"Port: {port}")
    print("Press Ctrl+C to shutdown gracefully\n")
    
    server = ChatServer()
    server.start_server(port)

if __name__ == "__main__":
    main()