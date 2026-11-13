import sys
sys.path.insert(0, '/home/awa/Projects/zd/awa/tools')
from serial_safe import open_serial
import time, signal
signal.alarm(25)

s = open_serial()
time.sleep(1)
# 清空启动日志
end = time.time() + 1
while time.time() < end:
    s.read(256)

print(">>> 冲洗后写入 HELLO_TEST ...", flush=True)
s.write(b'HELLO_TEST\n')
buf = b''
end = time.time() + 5
while time.time() < end:
    c = s.read(256)
    if c: buf += c
out = buf.decode('utf-8', errors='replace')
lines = [l.strip() for l in out.split('\n') if l.strip()]
print(f"收到 {len(lines)} 行:")
for l in lines[-15:]:
    print(" ", l[:170])
s.close()
