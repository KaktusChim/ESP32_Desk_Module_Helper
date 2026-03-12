import socket
import json
import time

HOST = "192.168.137.1"
PORT = 8765

with socket.create_connection((HOST, PORT)) as s:
    s.sendall((json.dumps({"type": "hello", "device": "test"}) + "\n").encode())
    print(s.recv(4096).decode())

    time.sleep(1)

    s.sendall((json.dumps({"type": "button_press", "action": "cut"}) + "\n").encode())
    time.sleep(1)

    s.sendall((json.dumps({"type": "button_press", "action": "play_pause"}) + "\n").encode())
