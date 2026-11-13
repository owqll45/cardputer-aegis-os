# M5Stack Cardputer ADV 入门指南

## 一、设备简介

### 1.1 硬件参数
- **主控芯片**: ESP32-S3 QFN56
- **CPU**: 240MHz 双核
- **内存**: 8MB GD Flash
- **显示屏**: 2.13" E-Paper / 240x135 LCD
- **键盘**: 52键 QWERTY 布局
- **扩展接口**: M5Cardputer Slot (G2/G3/G4/G5)
- **电源**: 内置锂电池 + USB-C 充电

### 1.2 物理按键
| 按键 | 位置 | 功能 |
|------|------|------|
| **BtnA** | 键盘区左上角 | 返回键、确认键 |
| **Power** | 机身侧面 | 电源开关 |
| **USB-C** | 机身底部 | 充电/数据传输 |

---

## 二、硬件接口详解

### 2.1 M5Cardputer Slot 引脚定义

| 引脚 | 功能 | 默认用途 | 备注 |
|------|------|----------|------|
| **G2** | GPIO2 | 传感器数据 | DHT11、超声波等 |
| **G3** | GPIO3 | 传感器数据 | 可配置 |
| **G4** | GPIO4 | 传感器数据 | 可配置 |
| **G5** | GPIO5 | 传感器数据 | 可配置 |
| **5V** | 电源 | 5V输出 | 最大500mA |
| **GND** | 接地 | 地线 | 必须连接 |
| **GND** | 接地 | 地线 | - |

### 2.2 常用传感器连接方式

#### DHT11 温湿度传感器
```
DHT11      Cardputer Slot
VCC   →    5V
GND   →    GND
DATA  →    G2
```

#### HC-SR04 超声波传感器
```
HC-SR04    Cardputer Slot
VCC   →    5V
GND   →    GND
TRIG  →    G2
ECHO  →    G3
```

#### 心率传感器 (MAX30102)
```
MAX30102   Cardputer Slot
VCC   →    5V
GND   →    GND
SDA   →    G2 (I2C_SDA)
SCL   →    G3 (I2C_SCL)
```

#### 继电器模块
```
Relay      Cardputer Slot
VCC   →    5V
GND   →    GND
IN    →    G2
```

### 2.3 接口使用注意事项
- 所有 GPIO 引脚支持数字输入/输出
- 模拟输入仅支持特定引脚（需查阅 ESP32-S3 数据手册）
- 输出电流有限，驱动大负载需使用继电器或晶体管
- 建议使用面包板和杜邦线进行原型开发

---

## 三、开发环境搭建

### 3.1 环境要求
- Windows 10/11 (推荐)
- Python 3.10+
- ESP-IDF v5.5.1
- VS Code (推荐编辑器)

### 3.2 安装步骤

#### 步骤1: 安装 ESP-IDF
```powershell
# 从 Espressif 官网下载 ESP-IDF Installer
# https://dl.espressif.com/dl/esp-idf/

# 安装路径建议: C:\Espressif\frameworks\esp-idf-v5.5.1
```

#### 步骤2: 设置环境变量
```powershell
# 每次开发前运行:
cd c:\Users\awa\Documents\trae_projects\zd\awa
& "C:\Espressif\frameworks\esp-idf-v5.5.1\tools\legacy_exports\export_legacy.ps1"
```

#### 步骤3: 安装项目依赖
```powershell
# 进入项目目录
cd c:\Users\awa\Documents\trae_projects\zd\awa

# 设置组件管理器
$env:IDF_COMPONENT_MANAGER="0"
```

---

## 四、项目结构

```
zd/
├── awa/                    # 主项目目录
│   ├── main/              # 源代码目录
│   │   ├── apps/          # 应用程序
│   │   │   ├── app_launcher/   # 启动器
│   │   │   ├── app_legacy_app/   # LegacyApp应用
│   │   │   ├── app_dht11/      # 温湿度传感器
│   │   │   └── ...
│   │   ├── hal/           # 硬件抽象层
│   │   │   ├── hal_facade.h/cpp   # HAL核心
│   │   │   ├── display_manager.h/cpp
│   │   │   └── power_manager.h/cpp
│   │   ├── crash_handler.cpp   # 崩溃处理
│   │   └── main.cpp            # 入口文件
│   ├── components/        # 第三方组件
│   └── CMakeLists.txt     # CMake配置
```

---

## 五、编译与上传

### 5.1 编译命令
```powershell
# 进入项目目录
cd c:\Users\awa\Documents\trae_projects\zd\awa

# 设置环境
& "C:\Espressif\frameworks\esp-idf-v5.5.1\tools\legacy_exports\export_legacy.ps1"
$env:IDF_COMPONENT_MANAGER="0"

# 清理构建缓存
idf.py fullclean

# 编译项目
idf.py build
```

### 5.2 上传到设备
```powershell
# 确保设备已连接到 COM9
# 关闭所有占用 COM9 的程序（如串口监视器）

# 刷入固件
idf.py -p COM9 flash

# 打开串口监视器
idf.py -p COM9 monitor
```

### 5.3 常见问题

#### 问题1: COM端口占用
```powershell
# 强制释放端口
taskkill /F /IM python.exe
Start-Sleep -Seconds 2
```

#### 问题2: 编译错误
```powershell
# 清理缓存后重新编译
idf.py fullclean
idf.py build
```

---

## 六、完整应用开发教程

### 6.1 创建新应用步骤

#### 步骤1: 创建目录结构
```powershell
mkdir apps\app_myapp
New-Item apps\app_myapp\app_myapp.h -ItemType File
New-Item apps\app_myapp\app_myapp.cpp -ItemType File
```

#### 步骤2: 编写头文件 (app_myapp.h)
```cpp
#pragma once
#include <mooncake.h>
#include <M5GFX.h>

class AppMyApp : public mooncake::AppAbility {
public:
    AppMyApp();
    ~AppMyApp();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    LGFX_Sprite* _canvas = nullptr;
    int _counter = 0;
};
```

#### 步骤3: 编写实现文件 (app_myapp.cpp)
```cpp
#include "app_myapp.h"
#include <apps/utils/audio/audio.h>
#include <hal.h>

AppMyApp::AppMyApp() {
    setAppInfo().name = "MyApp";
    setAppInfo().icon = nullptr;
}

AppMyApp::~AppMyApp() {
    if (_canvas) {
        _canvas->deleteSprite();
        delete _canvas;
    }
}

void AppMyApp::onOpen() {
    // 创建画布
    _canvas = new LGFX_Sprite(&GetHAL().display());
    _canvas->createSprite(240, 135);
    _canvas->fillScreen(TFT_BLACK);
    _counter = 0;
}

void AppMyApp::onRunning() {
    // 返回键处理
    if (GetHAL().homeButton().wasPressed()) {
        close();
        return;
    }

    // 更新计数器
    _counter++;

    // 绘制界面
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setFont(&fonts::Font0);
    _canvas->setCursor(10, 50);
    _canvas->printf("Hello World!");
    _canvas->setCursor(10, 70);
    _canvas->printf("Counter: %d", _counter);
    
    // 显示到屏幕
    _canvas->pushSprite(0, 0);

    // 延迟
    lgfx::delay(100);
}

void AppMyApp::onClose() {
    // 清理资源
    if (_canvas) {
        _canvas->deleteSprite();
        delete _canvas;
        _canvas = nullptr;
    }
}
```

#### 步骤4: 在 apps.h 中添加引用
```cpp
#include "app_myapp/app_myapp.h"
```

#### 步骤5: 在 main.cpp 中注册应用
```cpp
GetMooncake().installApp(std::make_unique<AppMyApp>());
```

#### 步骤6: 编译并测试
```powershell
idf.py build
idf.py -p COM9 flash
```

---

## 七、常用传感器示例代码

### 7.1 DHT11 温湿度传感器
```cpp
#include <DHT11.h>

DHT11* _dht11 = nullptr;

void initSensor() {
    _dht11 = new DHT11(2);  // GPIO2
}

void readSensor() {
    float temp, humi;
    if (_dht11->readTempAndHumi(&temp, &humi)) {
        mclog::tagInfo("Sensor", "Temp: %.1fC, Humi: %.1f%%", temp, humi);
    }
}
```

### 7.2 HC-SR04 超声波传感器
```cpp
#include <HC_SR04.h>

HC_SR04* _ultrasonic = nullptr;

void initUltrasonic() {
    _ultrasonic = new HC_SR04(2, 3);  // Trig=G2, Echo=G3
}

float readDistance() {
    return _ultrasonic->getDistanceCM();
}
```

### 7.3 心率传感器
```cpp
#include <MAX30102.h>

MAX30102* _heartRate = nullptr;

void initHeartRate() {
    _heartRate = new MAX30102();
    _heartRate->begin();
}

int readHeartRate() {
    return _heartRate->getHeartRate();
}
```

---

## 八、系统架构详解

### 8.1 整体架构

```
┌─────────────────────────────────────────────┐
│           Application Layer                 │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐      │
│  │ App1    │ │ App2    │ │ App3    │ ...  │
│  └────┬────┘ └────┬────┘ └────┬────┘      │
└───────┼───────────┼───────────┼───────────┘
        │           │           │
┌───────▼───────────▼───────────▼───────────┐
│           HAL Facade Layer                  │
│  ┌─────────────────────────────────────┐   │
│  │  HalFacade - 统一硬件抽象接口      │   │
│  └─────────────────────────────────────┘   │
└───────┬───────────┬───────────┬───────────┘
        │           │           │
┌───────▼───────────▼───────────▼───────────┐
│           Manager Layer                    │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
│  │ Display  │ │ Input    │ │ Power    │   │
│  │ Manager  │ │ Manager  │ │ Manager  │   │
│  └──────────┘ └──────────┘ └──────────┘   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
│  │ Audio    │ │ Network  │ │ Storage  │   │
│  │ Manager  │ │ Manager  │ │ Manager  │   │
│  └──────────┘ └──────────┘ └──────────┘   │
└───────┬───────────┬───────────┬───────────┘
        │           │           │
┌───────▼───────────▼───────────▼───────────┐
│           M5Unified / ESP-IDF              │
└─────────────────────────────────────────────┘
```

### 8.2 HAL层设计

#### 核心特点
- **统一接口**: 所有硬件操作通过 `GetHAL()` 访问
- **延迟初始化**: 指针模式避免初始化时序问题
- **模块解耦**: 各Manager独立管理各自功能

#### 关键设计模式

```cpp
// 指针延迟初始化模式（解决ESP32时序问题）
class HalFacade {
private:
    M5GFX* _display_ptr = nullptr;  // 指针声明
    
public:
    void init() {
        M5.begin();  // 先初始化M5
        _display_ptr = &M5.Display;  // 再绑定指针
    }
    
    M5GFX& display() { return *_display_ptr; }  // 函数访问器
};
```

### 8.3 应用生命周期

| 阶段 | 方法 | 说明 |
|------|------|------|
| 创建 | 构造函数 | 设置应用名称和图标 |
| 打开 | `onOpen()` | 初始化资源、创建画布 |
| 运行 | `onRunning()` | 主循环、处理输入、更新显示 |
| 关闭 | `onClose()` | 释放资源、清理内存 |

---

## 九、调试技巧

### 9.1 串口日志

#### 基本日志输出
```cpp
#include <mooncake_log.h>

// 不同级别日志
mclog::tagInfo("MyApp", "Info message");
mclog::tagWarn("MyApp", "Warning message");
mclog::tagError("MyApp", "Error message");

// 格式化输出
mclog::tagInfo("Sensor", "Temp: %.1f, Humi: %.1f", temp, humi);
```

#### 查看日志
```powershell
# 打开串口监视器（默认波特率115200）
idf.py -p COM9 monitor

# 快捷键
Ctrl+] : 退出监视器
```

### 9.2 断点调试

#### 使用 VS Code 调试
1. **安装 ESP-IDF 扩展**
2. **配置 launch.json**
```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "ESP-IDF Debug",
            "type": "espidf",
            "request": "launch",
            "target": "c:\\Users\\awa\\Documents\\trae_projects\\zd\\awa",
            "port": 9216,
            "debugPort": 9216
        }
    ]
}
```

3. **设置断点**: 点击代码行号左侧
4. **开始调试**: F5

### 9.3 性能分析

#### 测量代码执行时间
```cpp
uint32_t start = GetHAL().millis();

// 执行代码
processData();

uint32_t elapsed = GetHAL().millis() - start;
mclog::tagInfo("Perf", "Process took %dms", elapsed);
```

#### 内存使用检查
```cpp
#include <esp_system.h>

void checkMemory() {
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    mclog::tagInfo("Memory", "Free heap: %d bytes", free_heap);
}
```

### 9.4 常见调试场景

#### 场景1: 应用崩溃
```cpp
// 启用崩溃处理
#include "crash_handler.h"

// 在 main.cpp 中初始化
crash_handler::init();
```

#### 场景2: 按键无响应
```cpp
// 检查按键初始化
void onRunning() {
    M5.update();  // 必须调用！
    GetHAL().keyboard.update();
    
    if (GetHAL().homeButton().wasPressed()) {
        close();
    }
}
```

---

## 十、基本操作指南

### 10.1 开机与关机
```
开机: 长按 Power 键 3 秒
关机: 在桌面状态下长按 Power 键 5 秒
重启: 短按 Power 键
```

### 10.2 主界面操作
```
方向键/触摸: 选择应用
Enter/BtnA: 打开应用
Esc/BtnA: 返回上一级
```

### 10.3 应用操作
| 按键 | 功能 |
|------|------|
| **BtnA** | 返回桌面 |
| **Enter** | 确认/执行 |
| **方向键** | 导航选择 |

### 10.4 键盘快捷键
```
Ctrl + C: 强制退出当前应用
Ctrl + R: 重启设备
Fn + 方向键: 功能键模式
```

---

## 十一、常用命令速查

| 命令 | 功能 |
|------|------|
| `idf.py build` | 编译项目 |
| `idf.py -p COM9 flash` | 刷入固件 |
| `idf.py -p COM9 monitor` | 串口监视器 |
| `idf.py fullclean` | 清理缓存 |
| `idf.py menuconfig` | 配置菜单 |
| `idf.py size` | 查看固件大小 |
| `idf.py flash monitor` | 刷入并打开监视器 |

---

## 十二、故障排除

### 12.1 设备无法开机
- 检查电池电量（充电至少30分钟）
- 尝试 USB 直连电脑充电
- 检查电源开关位置

### 12.2 无法上传固件
- 确认 COM 端口正确（设备管理器查看）
- 关闭其他串口工具（如 Arduino IDE）
- 检查 USB 线缆是否支持数据传输
- 尝试不同的 USB 端口

### 12.3 应用崩溃
- 查看串口日志定位问题
- 使用 crash_handler 分析错误
- 检查内存使用情况
- 验证指针是否正确初始化

### 12.4 充电异常提示
- 检查底部电源开关位置（OFF 为快充模式）
- 尝试更换充电线和充电器
- 清洁 USB-C 接口

---

## 十三、高级配置

### 13.1 分区表配置
编辑 `partitions.csv` 调整分区大小

### 13.2 电源管理
```cpp
// 设置充电阈值
M5.Power.setChargeLevel(100);

// 获取电池状态
uint8_t battery = GetHAL().getBatLevel();
bool charging = M5.Power.isCharging();

// 低电量自动关机
if (battery < 10) {
    // 执行关机逻辑
}
```

### 13.3 屏幕亮度
```cpp
M5.Display.setBrightness(255);  // 0-255
```

### 13.4 音频控制
```cpp
// 播放音调
audio::play_tone(440, 100);  // 频率(Hz), 时长(ms)

// 播放随机音调
audio::play_random_tone();

// 设置全局静音
GetHAL().setGlobalMute(true);
```

---

## 附录：项目特殊说明

### HAL指针模式修复
由于 ESP32-S3 的初始化时序问题，HALFacade 使用指针延迟初始化模式：

```cpp
// 在 hal_facade.h 中声明指针
M5GFX* _display_ptr = nullptr;

// 在 init() 中初始化（必须在 M5.begin() 之后）
_display_ptr = &M5.Display;
```

### 充电检测优化
为避免误报，充电检测使用 3 秒防抖和 4.9V 阈值：

```cpp
// power_manager.cpp
if (!charging && avg_voltage > 4.9f && v > 4.8f) {
    // 显示充电异常警告
}
```

### 画布架构
系统使用三画布布局：
- **主画布** (204x109): 应用内容
- **系统栏** (204x26): 时间、电量、WiFi状态
- **键盘栏** (36x135): 左侧功能键显示

---

**文档版本**: v2.0  
**更新日期**: 2026-04-27  
**适用设备**: M5Stack Cardputer ADV