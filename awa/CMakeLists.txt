cmake_minimum_required(VERSION 3.16)

# 固件版本: 会被编进 esp_app_desc, "关于本机"页显示为 "固件 cardputer-adv v2.1.1"
set(PROJECT_VER "2.1.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardputer-adv)