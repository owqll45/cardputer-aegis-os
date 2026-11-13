import serial, time
s = serial.Serial()
s.port = '/dev/ttyACM0'
s.baudrate = 115200
s.timeout = 0.3
s.write_timeout = 3
s.dtr = False  # GPIO0 保持高 (正常启动模式)
s.rts = False
s.open()
s.rts = True   # EN 拉低 (复位)
time.sleep(0.15)
s.rts = False  # EN 释放 -> 启动应用
time.sleep(3)
buf = b''
end = time.time() + 5
while time.time() < end:
    c = s.read(256)
    if c: buf += c
out = buf.decode('utf-8', errors='replace')
lines = [l.strip() for l in out.split('\n') if l.strip()]
print(f"收到 {len(lines)} 行:")
for l in lines[-12:]:
    print(" ", l[:170])
s.close()
