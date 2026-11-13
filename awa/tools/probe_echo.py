import serial, time, signal
signal.alarm(20)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3, write_timeout=3)
# 关键修复: 释放 DTR/RTS (否则自动下载电路拉住复位引脚, 设备被按住)
s.dtr = False
s.rts = False
print(f"DTR={s.dtr} RTS={s.rts}", flush=True)
time.sleep(1)
print(">>> 写入 ABC...", flush=True)
s.write(b'ABC\n')
end = time.time() + 6
buf = b''
while time.time() < end:
    c = s.read(256)
    if c: buf += c
out = buf.decode('utf-8', errors='replace')
lines = [l.strip() for l in out.split('\n') if l.strip()]
print(f"收到 {len(lines)} 行:")
for l in lines[-10:]:
    print(" ", l[:160])
s.close()
