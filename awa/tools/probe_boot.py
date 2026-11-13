import serial, time, signal
signal.alarm(18)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3, write_timeout=3)
s.dtr = False; s.rts = True
time.sleep(0.2); s.rts = False; s.dtr = True
buf = b''
end = time.time() + 12
while time.time() < end:
    c = s.read(256)
    if c: buf += c
out = buf.decode('utf-8', errors='replace')
found = False
for l in out.split('\n'):
    ls = l.strip()
    if 'nonblock' in ls or 'SerialPump' in ls or 'Launcher installed' in ls or 'MAIN' in ls:
        print(ls[:170])
        found = True
if not found:
    print("(无匹配, 最后 6 行:)")
    for l in out.split('\n')[-6:]:
        print(" ", l.strip()[:170])
s.close()
