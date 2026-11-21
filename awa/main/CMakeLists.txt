# CONFIGURE_DEPENDS: 源文件增删时自动重新 configure。
# 之前没有这个, GLOB 结果在配置时冻结, 新增/删除文件不会反映到构建里,
# 死代码因此一直被侥幸跳过, 一旦 fullclean 就可能突然链接失败。
# 注意: ESP-IDF 会用 cmake -P (script 模式) include 本文件来扫描依赖,
# 该模式下不允许 CONFIGURE_DEPENDS, 所以只在正常 configure 时启用。
set(_main_glob_dep)
if(NOT CMAKE_SCRIPT_MODE_FILE)
    list(APPEND _main_glob_dep CONFIGURE_DEPENDS)
endif()
file(GLOB_RECURSE MY_SRCS ${_main_glob_dep}
    "*.c"
    "*.cc"
    "*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "apps/*/*.cpp"
    "app_extras/*.c"
    "app_extras/*.cc"
    "app_extras/*.cpp"
)

list(FILTER MY_SRCS EXCLUDE REGEX "ble_hid")  # KEEP EXCLUDED: BT disabled in sdkconfig
list(FILTER MY_SRCS EXCLUDE REGEX "app_legacy_app")  # [2.1.0] LegacyApp 已下架为模块; 代码保留, 恢复见 backup/LegacyApp_v2.0.0/RESTORE.md
list(FILTER MY_SRCS EXCLUDE REGEX "app_brain_server")
# list(FILTER MY_SRCS EXCLUDE REGEX "app_repl")  # RE-ENABLED: needed by main.cpp
list(FILTER MY_SRCS EXCLUDE REGEX "app_combo")  # KEEP EXCLUDED: depends on ble_hid which needs BT
# list(FILTER MY_SRCS EXCLUDE REGEX "app_fps")  # RE-ENABLED: needed by main.cpp
# list(FILTER MY_SRCS EXCLUDE REGEX "app_chat")  # RE-ENABLED: ESP-NOW chat implemented
# list(FILTER MY_SRCS EXCLUDE REGEX "app_launcher")  # RE-ENABLED: this is the main desktop entry point
# list(FILTER MY_SRCS EXCLUDE REGEX "app_life_support")  # RE-ENABLED: health monitoring is core sentinel feature
# list(FILTER MY_SRCS EXCLUDE REGEX "app_crash_test")  # RE-ENABLED: needed by main.cpp
list(FILTER MY_SRCS EXCLUDE REGEX "brain_server")
list(FILTER MY_SRCS EXCLUDE REGEX "brain_coordinator")
list(FILTER MY_SRCS EXCLUDE REGEX "apps_disabled")  # 已停用的应用源码，不参与构建
list(FILTER MY_SRCS EXCLUDE REGEX "app_chat")       # 已删除：无用，且退出异常
list(FILTER MY_SRCS EXCLUDE REGEX "app_remote")     # 已删除：无用，且退出异常

# Debug: Print found sources
message(STATUS "Found sources: ${MY_SRCS}")

set(MY_INCLUDE_DIRS 
    "."
    "./assets"
    "./hal"
    "./hal/framework"
    "./hal/ai"
    "./hal/chat"
    "./hal/ui"
    "./hal/memory"
    "./hal/mcp"
    "./hal/tools"
    "./hal/web"
    "./hal/system"
    "./apps"
)

set(PIKA_PYTHON_INCS
    ./apps/app_stringir_toolkit/rs_toolkit/
)

file(GLOB EXTERN_FILES assets/*)

# Force re-scan - Updated for app_extras and app_heart_rate
# Trigger rebuild for assets
idf_component_register(SRCS "main.cpp" ${MY_SRCS}
                    INCLUDE_DIRS ${MY_INCLUDE_DIRS} ${PIKA_PYTHON_INCS}
                    EMBED_FILES ${EXTERN_FILES}
                    REQUIRES M5Unified M5GFX mooncake smooth_ui_toolkit mooncake_log nvs_flash driver esp_timer esp_wifi esp_netif esp_event lwip json esp_http_client esp_adc esp_http_server fatfs sdmmc mbedtls app_update spiffs spi_flash esp_app_format esp_hw_support heap soc esp_system
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers -Wno-error=infinite-recursion)
