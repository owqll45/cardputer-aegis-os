# “哨兵-超脑内核” (Sentinel Brain-Core) v4.0：生命自救系统落地计划

## 1. 内核生理数据中枢 (Data Core)
- **RR 间期提取**: 升级 `hal.cpp` 的 BLE 接收器，完整解析 H613 的 RR-Interval 数据包。
- **算法实装**:
  - 实现 HRV (RMSSD) 实时计算逻辑。
  - 实现基于 RSA (呼吸性窦性心律不齐) 的呼吸频率推算。
  - 建立 60s 滑动窗口，存储 HR/HRV 趋势。

## 2. 8 维度评分与 5 状态机 (State Machine)
- **评分引擎**: 实现 `calculateRiskScore()`，综合 HR, HRV, ΔHR, RR, Motion, Duration 等 8 个维度。
- **状态判定**:
  - **静默区**: 正常/深睡 (分数 ≤3)，全系统静默。
  - **鬼压床**: 高分 + HR 骤升 + 零体动 (2s)。
  - **沉浸梦**: 高分 + 持续高心率 + 零体动 (120s)。
  - **日间应激**: 中分 + 持续高心率 + 非运动态 (60s)。
- **冷却期控制**: 触发后进入 10 分钟静默期，防止重复打扰。

## 3. 三端分级联动提醒 (Alert System)
- **ADV 本地 (一级)**: 
  - **视觉**: 弹出定制颜色/文字的全屏提醒（如红色沉浸梦、蓝色应激）。
  - **听觉**: 编写不同频率的渐进式提示音。
- **BLE 外部 (二/三级)**: 
  - 向手机推送 `ALERT|TYPE|HR` 指令，触发手机和手表的后续动作。

## 4. 存储与安全隔离
- **生命日志**: 在 32GB SD 卡自动记录异常触发详情 (`/sdcard/sys/log/guard.csv`)。
- **分级保护**: 监控逻辑强制驻留 Flash，不进行 SD 载录，确保 100% 运行。

## 5. 待执行操作
- 修改 `main/hal/hal.h` (状态定义、数据结构)。
- 修改 `main/hal/hal.cpp` (RR 解析、评分算法、冷却期、BLE 推送)。
- 修改 `main/apps/app_heart_rate/app_heart_rate.cpp` (增加 HRV/呼吸率显示)。
- 修改 `main/crash_handler.cpp` (增加多级 Life Alert UI)。
