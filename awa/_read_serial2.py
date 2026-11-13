import serial
import time

s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
s.dtr = False
time.sleep(0.1)
s.dtr = True
time.sleep(0.1)
s.dtr = False

start = time.time()
while time.time() - start < 15:
    data = s.read(4096)
    if data:
        text = data.decode('utf-8', errors='replace')
        for line in text.split('\n'):
            line = line.strip()
            if line:
                print(f"[{time.time()-start:.1f}s] {line}")
s.close()
