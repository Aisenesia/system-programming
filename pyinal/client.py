#!/usr/bin/env python3
import socket
import json
import threading
import sys

class ChatClient:
    def __init__(self, host='localhost', port=8888):
        self.host = host
        self.port = port
        self.socket = None
        self.running = False
        self.username = None
        
        # ANSI color codes
        self.COLORS = {
            'red': '\033[91m',
            'green': '\033[92m',
            'yellow': '\033[93m',
            'blue': '\033[94m',
            'purple': '\033[95m',
            'cyan': '\033[96m',
            'white': '\033[97m',
            'reset': '\033[0m'
        }
    
    def colorize(self, text, color):
        """Add color to text"""
        return f"{self.COLORS.get(color, '')}{text}{self.COLORS['reset']}"
    
    def connect(self):
        """Connect to the server"""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect((self.host, self.port))
            self.running = True
            
            print(self.colorize(f"Connected to server at {self.host}:{self.port}", 'green'))
            
            # Start receiver thread
            receiver_thread = threading.Thread(target=self.receive_messages, daemon=True)
            receiver_thread.start()
            
            # Handle username input
            self.handle_input()
            
        except Exception as e:
            print(self.colorize(f"Failed to connect to server: {e}", 'red'))
            return False
        
        return True
    
    def receive_messages(self):
        """Receive messages from server"""
        buffer = ""
        while self.running:
            try:
                data = self.socket.recv(1024).decode('utf-8')
                if not data:
                    break
                
                buffer += data
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    if line.strip():
                        self.process_message(line.strip())
                        
            except Exception as e:
                if self.running:
                    print(self.colorize(f"Error receiving message: {e}", 'red'))
                break
    
    def process_message(self, message_str):
        """Process incoming message from server"""
        try:
            message = json.loads(message_str)
            msg_type = message.get('type', '')
            content = message.get('content', '')
            status = message.get('status', '')
            timestamp = message.get('timestamp', '')
            
            if msg_type == 'system':
                print(self.colorize(f"[SYSTEM] {content}", 'cyan'))
            elif msg_type == 'success':
                print(self.colorize(f"[SUCCESS] {content}", 'green'))
            elif msg_type == 'error':
                print(self.colorize(f"[ERROR] {content}", 'red'))
            elif msg_type == 'broadcast':
                print(self.colorize(f"[{timestamp}] {content}", 'yellow'))
            elif msg_type == 'whisper':
                print(self.colorize(f"[{timestamp}] {content}", 'purple'))
            elif msg_type == 'notification':
                print(self.colorize(f"[INFO] {content}", 'blue'))
            else:
                print(f"[{timestamp}] {content}")
                
        except json.JSONDecodeError:
            print(f"Raw message: {message_str}")
    
    def handle_input(self):
        """Handle user input"""
        try:
            while self.running:
                try:
                    user_input = input().strip()
                    if user_input:
                        if user_input.lower() == '/exit':
                            self.running = False
                            break
                        
                        self.socket.send(user_input.encode('utf-8'))
                        
                        if user_input.lower() == '/help':
                            self.show_help()
                            
                except EOFError:
                    break
                except KeyboardInterrupt:
                    print(self.colorize("\nDisconnecting...", 'yellow'))
                    self.running = False
                    break
                    
        except Exception as e:
            print(self.colorize(f"Input error: {e}", 'red'))
        finally:
            self.disconnect()
    
    def show_help(self):
        """Show available commands"""
        help_text = """
Available Commands:
/join <room_name>     - Join or create a room
/leave                - Leave current room
/broadcast <message>  - Send message to everyone in current room
/whisper <user> <msg> - Send private message to user
/exit                 - Disconnect from server
/help                 - Show this help message
        """
        print(self.colorize(help_text, 'cyan'))
    
    def disconnect(self):
        """Disconnect from server"""
        self.running = False
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        print(self.colorize("Disconnected from server", 'yellow'))

def main():
    print("=== Multi-threaded Chat Client ===")
    
    # Parse command line arguments
    host = 'localhost'
    port = 8888
    
    if len(sys.argv) >= 3:
        host = sys.argv[1]
        port = int(sys.argv[2])
    elif len(sys.argv) >= 2:
        port = int(sys.argv[1])
    
    client = ChatClient(host, port)
    
    print(f"Connecting to {host}:{port}...")
    print("Type '/help' for available commands")
    print("Press Ctrl+C to quit\n")
    
    if client.connect():
        print("Connection closed")
    else:
        print("Failed to connect")

if __name__ == "__main__":
    main()
