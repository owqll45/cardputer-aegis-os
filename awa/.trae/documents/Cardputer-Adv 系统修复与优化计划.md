## **任务总览**
1.  **修复键盘回车键映射**：解决部分应用对回车无响应的问题。
2.  **修复系统弹窗“返回桌面”无响应**：完善崩溃处理的回调与应用切换逻辑。
3.  **SD 卡存储规范化与 Settings 改造**：建立 `/sd/system` 和 `/sd/apps` 结构，支持配置持久化到 SD 卡。
4.  **心率原始数据 HEX 日志记录**：将蓝牙心率数据包原始字节流输出到串口和 SD 卡。

---

## **技术方案与步骤**

### **1. 键盘回车键修复 (Keyboard Fix)**
*   **分析**：在 [keybaord.cpp](file:///c:\Users\awa\Documents\trae_projects\awa\main\hal\keyboard\keybaord.cpp) 中，`KEY_ENTER` 被映射为 HID 标准码 `0x28`。部分应用（如文本输入或特定移植软件）可能期望 ASCII 码 `13` (\r) 或 `10` (\n)。
*   **动作**：
    *   在 `Keyboard::convertToKeyEvent` 中，确保 `KEY_ENTER` 和 `KEY_KPENTER` (小键盘回车) 被统一处理。
    *   检查全局导航映射，确保回车键没有被其他逻辑截获。
    *   在 `Hal::update` 中增加对强制退出键（如 `ESC` 长按）的保护，避免与正常输入冲突。

### **2. 弹窗“返回桌面”修复 (Popup Fix)**
*   **分析**：[crash_handler.cpp](file:///c:\Users\awa\Documents\trae_projects\awa\main\crash_handler.cpp) 中的 `return_to_desktop()` 仅调用了 `openApp(0)`，但未关闭已崩溃的应用实例，这可能导致 Mooncake 状态机死锁。
*   **动作**：
    *   修改 `return_to_desktop()`：在打开 Launcher 之前，遍历并强制关闭所有处于 `Running` 状态的非系统应用。
    *   优化 `ESC` 键检测：在弹窗循环中增加更可靠的按键消抖和状态检查，确保“返回桌面”指令被正确捕获。
    *   增加 UI 反馈：按下按钮时改变按钮颜色，提供视觉确认。

### **3. SD 存储规范化 (SD & Settings)**
*   **分析**：目前的 [settings.cc](file:///c:\Users\awa\Documents\trae_projects\awa\main\hal\utils\settings\settings.cc) 完全依赖 NVS。
*   **动作**：
    *   **目录创建**：在 `Hal::sd_card_init()` 中增加逻辑，自动创建 `/sdcard/system` 和 `/sdcard/apps` 文件夹。
    *   **Settings 改造**：修改 `Settings` 类，使其优先尝试读写 `/sdcard/system/settings.json`。
    *   **双重备份**：关键系统设置（如 WiFi、密码）保留在 NVS 中以防 SD 卡损坏，用户偏好设置迁移至 SD 卡。
    *   **日志路径**：统一系统日志存放于 `/sdcard/system/logs/`。

### **4. 心率 HEX 日志记录 (Heart Rate Logging)**
*   **分析**：数据在 [app_heart_rate.cpp](file:///c:\Users\awa\Documents\trae_projects\awa\main\apps\app_heart_rate\app_heart_rate.cpp) 的蓝牙回调中接收。
*   **动作**：
    *   在 `BLE_GAP_EVENT_NOTIFY_RX` 和 `_on_read` 回调中增加 HEX 格式化输出。
    *   实现 `_log_raw_data(uint8_t* data, size_t len)` 函数。
    *   输出格式示例：`[HR_RAW] <2026-01-29 12:00:00> HEX: 16 4B 00 ...`。
    *   保存路径：`/sdcard/apps/HeartRate/logs/raw_data.log`。

---

## **验证与刷入计划**
1.  **编译检查**：运行 `idf.py build` 确保所有修改无编译错误。
2.  **刷入设备**：使用 `idf.py flash monitor` 进行自动化部署。
3.  **功能测试**：
    *   在 Launcher 和应用中使用 Enter 键确认。
    *   触发一个模拟崩溃，测试 ESC 键能否顺畅返回桌面。
    *   检查 SD 卡中是否生成了 `system` 文件夹和心率日志。
    *   观察串口输出，验证心率原始数据的 HEX 流。

请确认上述计划，我将立即开始执行。