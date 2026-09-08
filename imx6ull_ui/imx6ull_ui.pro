# imx6ull_ui.pro - EdgeMonitor 车载中控风格终端（7 寸触摸屏，i.MX6ULL 本机运行）
#
# 模块说明：
#   network    - WeatherClient 用 QNetworkAccessManager 查天气
#   websockets - 讯飞语音听写/合成/星火大模型都是 WebSocket 协议
#   multimedia - 麦克风采集(QAudioInput) + TTS 播放(QAudioOutput)
# 板子上的 Qt SDK 如果没带 websockets/multimedia 模块，需要额外安装，
# 具体看你 SDK 的 sysroot 里 Qt5WebSockets/Qt5Multimedia 的 .so 在不在。
#
# MQTT 复用 libmosquitto（跟 gateway_mqtt.c 等网关工具同一份依赖），
# 没有再引入 QtMqtt。
#
# 交叉编译（在开发板配套的 Qt5 SDK 环境里，具体 qmake 路径以你的 SDK 为准）：
#   /opt/fsl-imx-x11/<version>/sysroots/x86_64-pokysdk-linux/usr/bin/qmake imx6ull_ui.pro
#   make

QT       += core gui widgets network websockets multimedia
TARGET    = edgemonitor_ui
TEMPLATE  = app
CONFIG   += c++11

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    mqttclient.cpp \
    weatherclient.cpp \
    xfyunauth.cpp \
    voiceassistant.cpp \
    appconfig.cpp \
    settingspage.cpp \
    dashboardpage.cpp \
    controlpage.cpp \
    chatpage.cpp \
    attitudewidget.cpp \
    notificationbanner.cpp \
    naviconbutton.cpp \
    micbutton.cpp \
    virtualkeyboard.cpp

HEADERS += \
    mainwindow.h \
    mqttclient.h \
    weatherclient.h \
    xfyunauth.h \
    voiceassistant.h \
    appconfig.h \
    settingspage.h \
    dashboardpage.h \
    controlpage.h \
    chatpage.h \
    attitudewidget.h \
    notificationbanner.h \
    naviconbutton.h \
    micbutton.h \
    virtualkeyboard.h

# NXP 官方 Qt SDK（meta-toolchain-qt5）不带 mosquitto 头文件/库，用之前给
# gateway_mqtt 编译过的 mosquitto-1.6.15 源码目录（同一份源码，这次要用
# arm-poky-linux-gnueabi-gcc 重新编一遍静态库，两套交叉编译工具链的 ABI/
# sysroot 不通用，不能直接复用之前 arm-linux-gnueabihf-gcc 编出的 .a）。
# 约定这个项目跟 mosquitto-1.6.15 源码目录同级（都在 ~/edgemonitor/ 下），
# 目录结构不同的话改这两行路径即可。
INCLUDEPATH += $$PWD/../mosquitto-1.6.15/lib
LIBS += -L$$PWD/../mosquitto-1.6.15/lib -lmosquitto -lpthread
