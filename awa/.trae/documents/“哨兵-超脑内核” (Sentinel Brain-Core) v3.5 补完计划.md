# “哨兵-超脑内核” (Sentinel Brain-Core) v3.9 补完计划

## 1. 核心安全：噩梦监测 (Nightmare Guard)
- **异常识别算法**: 在内核后台守护进程中实现心率跳变监测。
  - 自动建立睡眠心率基准。
  - 检测到突然的剧烈跳变（噩梦风险）时触发提醒。
- **温和唤醒系统**: 结合声音（递进音效）与视觉（呼吸律动 UI）提醒用户。

## 2. 心率链路全修复 (Heart Rate Fix)
- **MTU 自动协商**: 修复 H613 链路，确保数据流稳定。
- **内核级后台连接**: 开机自启连接任务，全程静默守护，数据全局共享。
- **ECG 仿真**: 实现动态 P-QRS-T 仿真波形渲染。

## 3. 32GB 极限压榨：Infinity Link 2.0
- **Super-RAM (Swap)**: 建立 16MB+ SD 交换空间。
- **分级自动载录**: 
  - 非核心应用自动缓存至 SD。
  - **安全红线**: 噩梦监测、心率、SOS、电源等应用强制驻留 Flash。

## 4. 硬件唤醒与 UI 优化
- **PSRAM 激活**: 解决 `Free PSRAM: 0` 问题，释放全部硬件性能。
- **弹窗字体升级**: 退出确认字体调大（12px -> 14px）。
- **MathPro 修复**: 修正布局遮挡，增加常用函数预设库。

## 5. 待执行操作
- 修改 `main/hal/hal.cpp` (噩梦监测算法, BLE 后台服务, PSRAM)。
- 修改 `main/apps/app_launcher/app_launcher_fixed.cpp` (分级自动载录逻辑)。
- 修改 `main/apps/app_heart_rate/app_heart_rate.cpp` (仿真波形, 噩梦 UI)。
- 修改 `main/crash_handler.cpp` (字体升级)。
- 修改 `main/apps/app_extras/app_math_pro.cpp` (布局与预设)。
