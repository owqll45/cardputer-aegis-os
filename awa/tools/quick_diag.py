import serial, time, signal
signal.alarm(8)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3, write_timeout=3)
buf = b''
end = time.time() + 5
while time.time() < end:
    c = s.read(256)
    if c: buf += c
s.close()
out = buf.decode('utf-8', errors='replace')
lines = [l.strip() for l in out.split('\n') if l.strip()]
print(f"最近 {len(lines)} 行:")
for l in lines[-12:]:
    print(" ", l[:170])
