# thermal_demo

一句话介绍：基于 ESP32-P4 的热成像显示示例，读取大华摄像头的数据并在ST7789屏幕上显示。


## 功能

- 在屏幕上显示图像
- 切换伪彩色
- 开关冷热点显示
- 开关等温线模式
- 屏幕亮度

## 硬件

- 主控：ESP32-P4
- 屏幕：ST7789
- 接线：看代码里写的

## 环境

- ESP-IDF v5.5.5

## 编译与烧录

```bash
git clone https://github.com/mocha-lablalala/thermal_demo.git
cd thermal_demo
idf.py set-target esp32p4
idf.py build
idf.py -p 串口号 flash monitor
```


## 项目结构

- `main/main.cpp`：程序入口
- `main/parser.hpp`：MJPEG解析器
- `main/rotate.hpp`：编码器
- `main/setter.hpp`：向摄像头发送设置API
- `main/jpeg_decoder.hpp`：JPEG 解码

## 许可证

MIT
