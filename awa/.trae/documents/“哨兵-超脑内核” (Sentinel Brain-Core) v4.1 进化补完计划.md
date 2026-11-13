# “哨兵-超脑内核” (Sentinel Brain-Core) v4.1 补完计划

## 1. 紧急除颤：内存与 BLE 稳定性修复
- **内存重构**: 
  - 将 `std::deque` 成员从全局静态移动到 `Hal` 类成员，并在 `init()` 中显式初始化，避免 PSRAM 故障导致的静态初始化崩溃。
  - 增加 `MALLOC_CAP_8BIT` 降级逻辑，若 PSRAM 申请失败则使用内部 RAM。
- **BLE 栈优化**: 
  - `nimble_hr` 任务栈提升至 8192 字节。
  - 增加 `nimble_port_init()` 返回值校验。

## 2. 核心算法：8 维度检测矩阵 (Sentinel-8D)
- **维度补全**: 
  1. HR (实时心率)
  2. HRV (RMSSD 时域分析)
  3. ΔHR (1s 脉冲检测)
  4. RR (基于 RSA 的呼吸频率)
  5. Motion (IMU 加速度物理体动)
  6. Persistence (持续时间窗口)
  7. Slope (心率变化趋势)
  8. LF/HF (近似频域自主神经平衡)

## 3. 全新测试应用：Sentinel-Test (功能实验室)
- **实时看板**: 显示 8 维度原始数据流。
- **调试模式**: 支持手动模拟 5 大异常状态，验证三端联动提醒。
- **实时调优**: 支持在 UI 上修改冷却期 (Cooling Period) 和报警阈值。

## 4. 存储与日志
- **生命快照**: 记录 8 维度原始数据至 `/sdcard/sys/log/sentinel_raw.csv`。

## 5. 待执行操作
- 修改 `main/hal/hal.h` (延迟初始化结构)。
- 修改 `main/hal/hal.cpp` (修复崩溃、实装 8D 算法)。
- 创建 `main/apps/app_extras/app_sentinel_test.cpp` (全新测试应用)。
- 修改 `main/apps/apps.h` (注册新应用)。
