import socket
import time

SERVER = ('127.0.0.1', 5555)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

print("1. Sending RESET (Auto Mode)")
sock.sendto(b"RESET", SERVER)
time.sleep(5)

print("2. Simulating Wakeword! (Sending LOCK)")
sock.sendto(b"LOCK", SERVER)
time.sleep(5)

print("3. Forcing Angle to 45 (Sending SET 45)")
sock.sendto(b"SET 45", SERVER)
