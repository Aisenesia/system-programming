#!/usr/bin/env python3
import socket
import threading
import json
import time
import signal
import sys
from datetime import datetime
from collections import defaultdict

class ChatServer:
    def __init__(self, host='localhost', port=8888):
        self.host = host
        self.port = port
        self.socket = None
        self.running = False
        
        # Thread-safe data structures
        self.clients = {}  # {username: client_socket}
        self.client_threads = {}  # {username: thread}
        self.rooms = defaultdict(set)  # {room_name: {usernames}}
        self.user_rooms = {}  # {username: room_name}
        
        # Synchronization
        self.clients_lock = threading.Lock()
        self.rooms_lock = threading.Lock()
        self.log_lock = threading.Lock()
        
        # Logging
        self.log_file = f"server_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
        
        # Setup signal handler for graceful shutdown
        signal.signal(signal.SIGINT, self.signal_handler)
    
    def log(self, message):
        """Thread-safe logging with timestamps"""
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        log_entry = f"{timestamp} - {message}"
        
        with self.log_lock:
            print(log_entry)  # Console output
            try:
                with open(self.log_file, 'a', encoding='utf-8') as f:
                    f.write(log_entry + '\n')
                    f.flush()
            except Exception as e:
                print(f"Error writing to log file: {e}")
    
    def signal_handler(self, signum, frame):
        """Handle SIGINT for graceful shutdown"""
        self.log("[SERVER] Received SIGINT, shutting down gracefully...")
        self.shutdown()
    
    def validate_username(self, username):
        """Validate username: max 16 chars, alphanumeric only"""
        if not username or len(username) > 16:
            return False
        return username.isalnum()
    
    def validate_room_name(self, room_name):
        """Validate room name: max 32 chars, alphanumeric only"""
        if not room_name or len(room_name) > 32:
            return False
        return room_name.isalnum()
    
    def send_message(self, client_socket, message_type, content, status="success"):
        """Send JSON message to client"""
        try:
            message = {
                "type": message_type,
                "content": content,
                "status": status,
                "timestamp": datetime.now().strftime('%H:%M:%S')
            }
            data = json.dumps(message) + '\n'
            client_socket.send(data.encode('utf-8'))
            return True
        except Exception as e:
            self.log(f"[ERROR] Failed to send message: {e}")
            return False
    
    def handle_client(self, client_socket, address):
        """Handle individual client connection"""
        username = None
        
        try:
            # Get username
            self.send_message(client_socket, "system", "Welcome! Please enter your username (max 16 chars, alphanumeric):")
            
            while True:
                data = client_socket.recv(1024).decode('utf-8').strip()
                if not data:
                    break
                
                if not username:
                    # Username registration
                    proposed_username = data
                    
                    if not self.validate_username(proposed_username):
                        self.send_message(client_socket, "error", 
                                        "Invalid username! Must be max 16 characters, alphanumeric only.")
                        continue
                    
                    with self.clients_lock:
                        if proposed_username in self.clients:
                            self.send_message(client_socket, "error", 
                                            "Username already taken! Please choose another.")
                            continue
                        
                        # Register user
                        username = proposed_username
                        self.clients[username] = client_socket
                        self.client_threads[username] = threading.current_thread()
                    
                    self.log(f"[LOGIN] user '{username}' connected from {address[0]}")
                    self.send_message(client_socket, "success", 
                                    f"Welcome {username}! You can now use commands: /join, /leave, /broadcast, /whisper, /exit")
                    continue
                
                # Handle commands
                if data.startswith('/'):
                    self.handle_command(username, client_socket, data)
                else:
                    self.send_message(client_socket, "error", 
                                    "Please use commands starting with '/' or type /exit to quit")
        
        except ConnectionResetError:
            self.log(f"[DISCONNECT] user '{username}' disconnected abruptly")
        except Exception as e:
            self.log(f"[ERROR] Error handling client {username}: {e}")
        finally:
            self.cleanup_client(username, client_socket)
    
    def handle_command(self, username, client_socket, command):
        """Process client commands"""
        parts = command.split(' ', 2)
        cmd = parts[0].lower()
        
        if cmd == '/join':
            if len(parts) < 2:
                self.send_message(client_socket, "error", "Usage: /join <room_name>")
                return
            
            room_name = parts[1]
            if not self.validate_room_name(room_name):
                self.send_message(client_socket, "error", 
                                "Invalid room name! Must be max 32 characters, alphanumeric only.")
                return
            
            self.join_room(username, room_name, client_socket)
        
        elif cmd == '/leave':
            self.leave_room(username, client_socket)
        
        elif cmd == '/broadcast':
            if len(parts) < 2:
                self.send_message(client_socket, "error", "Usage: /broadcast <message>")
                return
            
            message = parts[1]
            self.broadcast_message(username, message, client_socket)
        
        elif cmd == '/whisper':
            if len(parts) < 3:
                self.send_message(client_socket, "error", "Usage: /whisper <username> <message>")
                return
            
            target_user = parts[1]
            message = parts[2]
            self.whisper_message(username, target_user, message, client_socket)
        
        elif cmd == '/exit':
            self.send_message(client_socket, "system", "Goodbye!")
            client_socket.close()
        
        else:
            self.send_message(client_socket, "error", 
                            "Unknown command. Available: /join, /leave, /broadcast, /whisper, /exit")
    
    def join_room(self, username, room_name, client_socket):
        """Handle user joining a room"""
        with self.rooms_lock:
            # Check room capacity
            if len(self.rooms[room_name]) >= 15:
                self.send_message(client_socket, "error", f"Room '{room_name}' is full (max 15 users)")
                return
            
            # Leave current room if in one
            if username in self.user_rooms:
                old_room = self.user_rooms[username]
                self.rooms[old_room].discard(username)
                self.log(f"[LEAVE] user '{username}' left room '{old_room}'")
            
            # Join new room
            self.rooms[room_name].add(username)
            self.user_rooms[username] = room_name
        
        self.log(f"[JOIN] user '{username}' joined room '{room_name}'")
        self.send_message(client_socket, "success", f"Joined room '{room_name}'. Users in room: {len(self.rooms[room_name])}")
        
        # Notify other users in the room
        self.notify_room(room_name, f"{username} has joined the room", exclude_user=username)
    
    def leave_room(self, username, client_socket):
        """Handle user leaving current room"""
        with self.rooms_lock:
            if username not in self.user_rooms:
                self.send_message(client_socket, "error", "You are not in any room")
                return
            
            room_name = self.user_rooms[username]
            self.rooms[room_name].discard(username)
            del self.user_rooms[username]
        
        self.log(f"[LEAVE] user '{username}' left room '{room_name}'")
        self.send_message(client_socket, "success", f"Left room '{room_name}'")
        
        # Notify other users in the room
        self.notify_room(room_name, f"{username} has left the room", exclude_user=username)
    
    def broadcast_message(self, username, message, client_socket):
        """Broadcast message to all users in the same room"""
        with self.rooms_lock:
            if username not in self.user_rooms:
                self.send_message(client_socket, "error", "You must join a room first to broadcast messages")
                return
            
            room_name = self.user_rooms[username]
            room_users = self.rooms[room_name].copy()
        
        self.log(f"[BROADCAST] user '{username}' in room '{room_name}': {message}")
        
        # Send to all users in the room except sender
        sent_count = 0
        for user in room_users:
            if user != username and user in self.clients:
                if self.send_message(self.clients[user], "broadcast", 
                                   f"[{room_name}] {username}: {message}"):
                    sent_count += 1
        
        self.send_message(client_socket, "success", f"Message broadcast to {sent_count} users in room '{room_name}'")
    
    def whisper_message(self, sender, target_user, message, client_socket):
        """Send private message to specific user"""
        with self.clients_lock:
            if target_user not in self.clients:
                self.send_message(client_socket, "error", f"User '{target_user}' not found or offline")
                return
            
            target_socket = self.clients[target_user]
        
        # Send message to target user
        if self.send_message(target_socket, "whisper", f"[Private] {sender}: {message}"):
            self.send_message(client_socket, "success", f"Private message sent to {target_user}")
            self.log(f"[WHISPER] '{sender}' to '{target_user}': {message}")
        else:
            self.send_message(client_socket, "error", f"Failed to send message to {target_user}")
    
    def notify_room(self, room_name, message, exclude_user=None):
        """Send notification to all users in a room"""
        with self.rooms_lock:
            room_users = self.rooms[room_name].copy()
        
        for user in room_users:
            if user != exclude_user and user in self.clients:
                self.send_message(self.clients[user], "notification", f"[{room_name}] {message}")
    
    def cleanup_client(self, username, client_socket):
        """Clean up client resources"""
        if username:
            with self.clients_lock:
                self.clients.pop(username, None)
                self.client_threads.pop(username, None)
            
            with self.rooms_lock:
                if username in self.user_rooms:
                    room_name = self.user_rooms[username]
                    self.rooms[room_name].discard(username)
                    del self.user_rooms[username]
                    # Notify room that user left
                    self.notify_room(room_name, f"{username} has disconnected", exclude_user=username)
            
            self.log(f"[LOGOUT] user '{username}' disconnected")
        
        try:
            client_socket.close()
        except:
            pass
    
    def start(self):
        """Start the server"""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.socket.bind((self.host, self.port))
            self.socket.listen(15)  # Support up to 15 concurrent clients
            
            self.running = True
            self.log(f"[SERVER] Chat server started on {self.host}:{self.port}")
            self.log(f"[SERVER] Log file: {self.log_file}")
            
            while self.running:
                try:
                    client_socket, address = self.socket.accept()
                    self.log(f"[CONNECTION] New connection from {address}")
                    
                    # Create thread for client
                    client_thread = threading.Thread(
                        target=self.handle_client,
                        args=(client_socket, address),
                        daemon=True
                    )
                    client_thread.start()
                    
                except socket.error as e:
                    if self.running:
                        self.log(f"[ERROR] Socket error: {e}")
                    break
        
        except Exception as e:
            self.log(f"[ERROR] Failed to start server: {e}")
        finally:
            self.shutdown()
    
    def shutdown(self):
        """Graceful server shutdown"""
        self.running = False
        self.log("[SERVER] Shutting down...")
        
        # Notify all clients
        with self.clients_lock:
            for username, client_socket in list(self.clients.items()):
                try:
                    self.send_message(client_socket, "system", "Server is shutting down. Goodbye!")
                    client_socket.close()
                except:
                    pass
            self.clients.clear()
        
        # Close server socket
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        
        self.log("[SERVER] Server shutdown complete")
        sys.exit(0)

def main():
    if len(sys.argv) > 2:
        host = sys.argv[1]
        port = int(sys.argv[2])
        server = ChatServer(host, port)
    elif len(sys.argv) > 1:
        port = int(sys.argv[1])
        server = ChatServer(port=port)
    else:
        server = ChatServer()
    
    try:
        server.start()
    except KeyboardInterrupt:
        server.shutdown()

if __name__ == "__main__":
    main()
