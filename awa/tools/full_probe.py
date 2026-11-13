import sys, time, signal
sys.path.insert(0, '/home/awa/Projects/zd/awa/tools')
import serial
from serial_safe import SafeSerial
signal.alarm(30)

# 1. RTS 复位设备 (run 状态: RTS=EN)
s = serial.Serial()
s.port = '/dev/ttyACM0'; s.baudrate = 115200; s.timeout = 0.3; s.write_timeout = 3
s.dtr = False; s.rts = False
s.open()
s.rts = True
time.sleep(0.3)
s.rts = False
s.close()
print(">>> 已复位设备", flush=True)

# 2. 等启动 + 开 SafeSerial (读线程)
time.sleep(2)
ss = SafeSerial()
time.sleep(3)  # 读线程消化启动日志 (IN 端点持续消费)
n = len(ss.pop_lines())
print(f">>> 读线程已消费 {n} 行启动日志", flush=True)

# 3. 清内核 TX 残留后写入
ss.s.reset_output_buffer()
print(">>> 写入 PING ...", flush=True)
ss.send("PING")
time.sleep(3)
lines = ss.pop_lines()
print(f"响应 {len(lines)} 行:")
for l in lines[-12:]:
    print(" ", l[:170])
ss.close()
