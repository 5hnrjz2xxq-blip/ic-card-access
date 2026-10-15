# IC 卡门禁系统（ESP32 双读卡器）

电子系统设计实训项目：基于 ESP32 的 NFC 门禁控制器。双读卡器同时轮询，Ntag215 卡做 UID 白名单 + 卡内密钥双重认证，Classic 卡兼容只验 UID。

## 功能

- **双读卡器同时工作**：PN532（I2C）为主力，MFRC522（SPI）同时轮询
  - MFRC522 用 WUPA 轮询（能发现已 Halt 的卡），读完必 Halt，状态机干净
  - MFRC522 读到 Ntag215（7 字节 UID）时自动转交 PN532 做密钥验证
- **双重认证（防复制卡）**
  - Ntag215：UID 在白名单 **且** 卡内 Page 4–7 的 16 字节密钥匹配 → 放行；密钥经 PN532 读写（带 3 次重试，贴纸卡天线小、位置敏感）
  - Mifare Classic（4 字节 UID）：只验 UID 白名单（兼容旧卡）
- **管理卡**：轻碰进入添加模式，按住 3 秒进入删除模式；5 秒无操作自动退出；删除模式下管理卡离场 2 秒内再刷忽略（防误判）
- **Web 管理页**：ESP32 开热点，手机连上后访问 `http://192.168.4.1` 增删白名单、刷卡录入（Ntag215 自动写密钥）
- **外设**：0.96" OLED（SSD1306，与 PN532 共用 I2C 总线，显示"就绪，请刷卡"/"欢迎"/"错误"）、有源蜂鸣器（不同事件不同蜂鸣节奏）、SG90 舵机开门演示、继电器（低电平触发）
- **白名单**存 ESP32 NVS（Preferences），最多 20 张，默认两张测试卡；串口支持 `LIST` / `CLEAR` 指令

## 硬件接线

| 模块 | 接线 |
|---|---|
| PN532 | SDA=GPIO21, SCL=GPIO22（I2C；拨码 1→ON, 2→OFF）|
| MFRC522 | SS=GPIO5, RST=GPIO27, SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23（SPI）|
| OLED SSD1306 | SDA=GPIO21, SCL=GPIO22（与 PN532 共用 I2C）|
| 继电器 | IN=GPIO26（低电平触发）|
| SG90 舵机 | 信号=GPIO14（5V 供电）|
| 有源蜂鸣器 | 信号=GPIO13（高电平触发）|

## 编译与烧录

```bash
pio run                 # 只编译
pio run -t upload       # 烧录
pio device monitor      # 串口监视（115200）

# 或用快捷脚本
./build.sh              # 编译
./build.sh up           # 编译+烧录（upload port 按自己机器改）
```

依赖库（`platformio.ini` 已声明，首次编译自动下载）：`MFRC522`、`ESP32Servo`、`U8g2`、`Adafruit PN532`。

## ⚠️ 安全提示

`src/main.cpp` 中的卡内密钥（`NFC_SECRET`）与 WiFi 热点密码（`AP_PASS`）在本仓库中为**占位符**。部署前请替换为你自己的值，并用管理卡/网页录入把密钥写入你的 Ntag215 卡（Page 4–7）。切勿将真实密钥提交到公开仓库。

## 项目结构

```text
├── platformio.ini   # ESP32 环境配置
├── build.sh         # 快捷编译/烧录脚本
└── src/
    └── main.cpp      # 全部固件逻辑（约 560 行）：读卡调度、双重认证、
                      # 管理卡状态机、Web 管理页、OLED/蜂鸣器/舵机/继电器驱动
```
