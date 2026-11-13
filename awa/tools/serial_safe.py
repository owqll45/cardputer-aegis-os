#!/usr/bin/env python3
"""
安全串口: 预设 DTR/RTS=False + 后台读线程 (ESP32-S3 USB errata: 不读就写不进)
"""
import serial
import time
import threading

class SafeSerial:
    def __init__(self, port=None, baud=115200):
        self.s = serial.Serial()
        self.s.port = port or find_port()
        self.s.baudrate = baud
        self.s.timeout = 0.2
        self.s.write_timeout = 3
        self.s.dtr = False  # open 前预设 (防自动下载电路按住设备)
        self.s.rts = False
        self.s.open()
        self.lines = []
        self._stop = False
        self._buf = b''
        self._thr = threading.Thread(target=self._reader, daemon=True)
        self._thr.start()

    def _reader(self):
        while not self._stop:
            try:
                c = self.s.read(65536)  # 大块读: 镜像帧单行 43KB, 小块读会拖慢
                if c:
                    self._buf += c
                    while b'\n' in self._buf:
                        line, self._buf = self._buf.split(b'\n', 1)
                        self.lines.append(line.decode('utf-8', errors='replace').strip())
                        if len(self.lines) > 500:
                            self.lines = self.lines[-300:]
            except Exception:
                break

    def send(self, data: str):
        try:
            self.s.write((data + "\n").encode())
            return True
        except Exception:
            return False

    def pop_lines(self, contains=None):
        out = self.lines[:]
        self.lines.clear()
        if contains:
            out = [l for l in out if any(k in l for k in contains)]
        return out

    def close(self):
        self._stop = True
        time.sleep(0.3)
        try:
            # 关键: close 前释放 DTR/RTS, 防止串口关闭瞬间自动下载电路复位设备
            self.s.dtr = False
            self.s.rts = False
            time.sleep(0.1)
        except Exception:
            pass
        self.s.close()

def find_port():
    """自动探测设备端口"""
    # Windows: 按 Espressif VID:PID (Cardputer ADV = 303A:1001) 定位
    try:
        from serial.tools import list_ports
        cands = list(list_ports.comports())
        for p in cands:
            if '303A:1001' in (p.hwid or '').upper():
                return p.device
        for p in cands:
            if 'USB' in (p.description or '').upper() and 'BTHENUM' not in (p.hwid or '').upper():
                return p.device
    except Exception:
        pass
    import glob
    for pat in ['/dev/ttyACM*', '/dev/ttyUSB*']:
        ports = sorted(glob.glob(pat))
        if ports:
            return ports[-1]
    return '/dev/ttyACM0'

def open_serial(port=None, baud=115200):
    return SafeSerial(port, baud)

if __name__ == "__main__":
    import signal
    signal.alarm(30)
    ss = open_serial()
    print(f"DTR={ss.s.dtr} RTS={ss.s.rts} (后台读线程已启动)", flush=True)
    time.sleep(2)
    ss.pop_lines()  # 冲洗启动日志
    print(">>> 写入 HELLO_TEST ...", flush=True)
    ss.send("HELLO_TEST")
    time.sleep(3)
    lines = ss.pop_lines()
    print(f"收到 {len(lines)} 行:")
    for l in lines[-15:]:
        print(" ", l[:170])
    ss.close()
