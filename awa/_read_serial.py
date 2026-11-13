import serial
import time

s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
s.dtr = False
time.sleep(0.1)
s.dtr = True
time.sleep(0.1)
s.dtr = False
time.sleep(4)
data = s.read(16384)
print(data.decode('utf-8', errors='replace'))
s.close()
