# Sentinel Brain-Core: Android Migration Specification (v4.9d)
## 哨兵-超脑内核：Android 移植技术规范

### 1. 系统概述
本系统是一套基于生物特征反馈的闭环安全干预架构。在 Android 平台上，应作为 `Foreground Service` (前台服务) 运行，以确保在灭屏状态下仍能保持高频监控。

### 2. 核心服务架构 (Core Services)

#### A. BiometricMonitorService (生物监测服务)
- **接口**: BluetoothLeScanner
- **逻辑**: 
    - 订阅 Heart Rate Measurement (UUID: 0x2A37) 特征值。
    - **HRV 计算**: 实现 RMSSD 算法，窗口大小设为 60 样本。
    - **数据持久化**: 采用 SQLite 或 Room 存储 24 小时内的生理指标趋势。

#### B. LifeGuardEngine (生命卫士引擎)
- **风险评分算法**:
    - `Score = (abs(HR - 65) / 65 * 3) + (HRV_Stress * 3) + (Delta_HR * 2) + (Stationary_Bonus * 2)`
- **干预分发器 (Intervention Dispatcher)**:
    - **惊恐模式**: 开启全屏浮窗，绘制缩放光圈，引导呼吸。
    - **唤醒模式**: 循环调用 `Vibrator` 的最高强度，配合 `MediaPlayer` 播放特定频率音调。
- **生物特征验证 (Bio-Verification)**:
    - 监听药效：服用德巴金/舍曲林后，监测 60 分钟内的 HRV 稳定性走势。

### 3. 关键 UI/UX 规范 (Android Implementation)
- **警报窗口**: 必须使用 `LayoutParams.TYPE_APPLICATION_OVERLAY`，并设置 `FLAG_SHOW_WHEN_LOCKED` 以穿透锁屏。
- **字体规范**: 强行指定 `fonts/efontCN_12.ttf` 或同级高清中文字体，标题 24sp，正文 14sp，行高 1.2x。
- **颜色代码**:
    - 惊恐: `#FF00FF` (Magenta)
    - 睡眠瘫痪: `#FFFF00` (Yellow)
    - 深层梦境: `#FF0000` (Red)
    - 情绪压力: `#00FFFF` (Cyan)

### 4. 传感器需求
- **BLE**: 必须支持多设备并发扫描（心率带 + 其他外设）。
- **IMU (Accelerometer)**: 采样率不低于 25Hz，用于检测佩戴者的运动状态以过滤生理数据噪声。

### 5. 安全协议
- **紧急 SOS**: 当 `RiskScore` 持续 30 秒处于 9.0 以上且无体动时，触发 Android 系统的紧急拨号或发送带位置信息的短信。
