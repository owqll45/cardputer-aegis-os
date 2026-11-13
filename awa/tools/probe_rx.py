import serial, time, signal
signal.alarm(25)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3, write_timeout=3)
s.dtr = False; s.rts = True
time.sleep(0.2); s.rts = False; s.dtr = True
# 等启动完
buf = b''
end = time.time() + 8
while time.time() < end:
    c = s.read(256)
    if c: buf += c
out = buf.decode('utf-8', errors='replace')
print("--- 启动日志 (找 nonblock) ---")
for l in out.split('\n'):
    if 'nonblock' in l:
        print(" ", l.strip()[:150])
# 发指令
print(">>> 写入 TESTCMD_123 ...", flush=True)
try:
    n = s.write(b'TESTCMD_123\n')
    print(f"  写入 {n} 字节成功")
    out2 = b''
    end = time.time() + 5
    while time.time() < end:
        c = s.read(256)
        if c: out2 += c
    print("--- 写入后输出 (找 RX/queued/Command) ---")
    for l in out2.decode('utf-8', errors='replace').split('\n'):
        ls = l.strip()
        if ls:
            print(" ", ls[:160])
except Exception as e:
    print(f"  写入异常: {e}")
s.close()
