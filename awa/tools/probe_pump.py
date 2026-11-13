import serial, time, signal
signal.alarm(15)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3, write_timeout=3)
# 等启动日志
buf = b''
end = time.time() + 6
while time.time() < end:
    c = s.read(256)
    if c: buf += c
out = buf.decode('utf-8', errors='replace')
for l in out.split('\n'):
    if 'stdin nonblock' in l or 'SerialPump' in l:
        print("BOOT:", l.strip()[:150])
# 发测试指令
print(">>> 发送 TEST 指令...", flush=True)
try:
    s.write(b'TESTCMD\n')
    out2 = b''
    end = time.time() + 4
    while time.time() < end:
        c = s.read(256)
        if c: out2 += c
    for l in out2.decode('utf-8', errors='replace').split('\n'):
        ls = l.strip()
        if ls and ('queued' in ls or 'Command' in ls or 'TEST' in ls or 'SerialPump' in ls):
            print("EVT:", ls[:150])
except Exception as e:
    print(f"写入失败: {e}")
s.close()
print("=== 探测完成 ===")
