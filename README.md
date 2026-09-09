# ESP32 SDR Modem

在 **ESP32-P4 Function EV Board** 上，通过板载 **USB 2.0 High-Speed Host** 连接 RTL2832U 接收棒，完成射频瀑布图、USB/LSB 短波接收、AM/FM、ADS-B 和数字通信解码。

本工程从 `esp32p4-psk-modem-codec2` 独立迁出，保留 PSK、图像、Codec2 700C、LCD 和 Wi-Fi 功能；移植 `esp32-rtlsdr-display` 的 USB/RTL-SDR、FFT、FM 和 ADS-B 接收能力，并将音频与界面适配到 P4。迁移时未复制旧 `.git`，新仓库使用独立历史。

## 当前功能

| 应用 | 实现 |
| --- | --- |
| USB / LSB | 288 kS/s IQ → 48 kHz 音频；复数边带滤波；监听或直接送数字解码器 |
| 数字通信 | 原有 29 个 PSK 模式菜单，含 PSK125R/250R；部分文本 RsID；图像与 Codec2 700C |
| AM / WFM / NFM | 包络/相位差检波、滤波、抽取；WFM 50 µs 去加重；48 kHz 单声道与音频波形 |
| Waterfall | 512 点 IQ FFT、频谱与瀑布图；触摸选择 RF 标记，再 Apply 调谐 |
| ADS-B | 1090 MHz、2 MS/s；DF17/18 CRC、呼号/高度/速度、成对 airborne CPR 位置及飞机列表 |
| Scanner | 自定义起止频率、重叠分段调谐与峰值频谱 |
| OOK pulses | 原始 OOK/ASK 脉冲宽度与帧计数；尚未识别具体传感器协议 |
| IQ Record | 单次 512 KiB CU8；PSRAM 缓冲后保存 SPIFFS；重启校验恢复；下载 SigMF 数据及元数据 |

`2x8PSK1200F` 和 AP580 保持移除。语音使用单载波 `8PSK1200F` 或 `8PSK1000F`。当前语音空口为本项目的 Codec2/PSK 协议，完整 FreeDV 700D/700E 互通仍属后续工作。

## 接线与使用

1. 给 P4 和屏幕供电，将 RTL-SDR 插入板载高速 **USB Type-A Host**。调试/Serial-JTAG 使用对应调试口；旧版开发板的接口形态以其原理图为准。
2. 上电默认 WFM、100 MHz、Tuner。界面显示真实 USB 状态，没有接收棒时显示无数据。
3. 接收短波时选择 USB 或 LSB，输入 RF 频率，并按下表选择短波输入方式，点击 **Apply**。
4. 勾选 **Modem decode**，在 **Modem / Codec2** 页选择数字模式、音频中心和 Text/Image/Audio。SSB PCM 在设备内进入解码器，无需音频线回接。PSK 音频中心可先用 1500～2000 Hz；Codec2 固定 2000 Hz。
5. 普通 SSB/FM 勾选 **Speaker** 监听。收到 Codec2 时播放解码后的语音。监听音量不影响数字解码输入。
6. Modem / Codec2 页选择 **Tx** 或按住 **PTT** 时，自动关闭 SDR 输入并切到板载麦克风/线路音频。正在录制 IQ 时需先在 Receiver 页停止录制。页面底部显示当前输入及操作提示；回到 **Rx** 后保持线路音频，要恢复射频接收，在 Receiver 页勾选 **SDR input** 并 Apply。RTL-SDR 本身只能接收。
7. **ADS-B** 页点击 Apply 后接收 1090 MHz 报文，右侧为飞机列表，中央离线地图可点选定位、拖动、缩放。无新坐标的飞机只显示在列表；没有接收棒时不显示测试飞机。
8. **Sub-GHz** 页默认中国大陆目录，提供 433.92、315、431、490、868.30 MHz 接收参考及用途说明；选择预设后 Apply。当前分析能力为 OOK 脉冲、频谱、IQ 和扫描，尚无具体传感器协议解码。

| RF 输入 | 短波方式 |
| --- | --- |
| Tuner | 普通调谐器，界面范围 24～1766 MHz；实际范围取决于芯片 |
| Direct Q | 具备直接采样接线的 V3/兼容棒；0.5～28.8 MHz 走 Q ADC，更高频率恢复调谐器 |
| Blog V4 | V4 的 R828D 分频段输入、HF 内置 28.8 MHz 上变频与滤波控制 |
| Upconverter | 外接短波上变频器，填写实际本振，例如 125 MHz；界面显示原始 RF 频率 |

普通接收棒需要相应的短波前端，不能只修改调谐数字。直接采样有 Nyquist 镜像，需要匹配天线与滤波。V4L 未作为 V4 宣称支持。

一只接收棒同时只有一个中心频率和采样率。ADS-B、FM、扫描通过切换配置运行；返回数字模式页面不会自动改变 RF 配置。

## 网页与录制

连接热点 **ESP32-SDR**（默认密码 `pskmodem32`），打开 `http://192.168.4.1`。

- `/` 或 `/sdr`：射频设置、瀑布图、FM 波形、ADS-B、扫描、脉冲和 IQ 录制。
- `/modem`：数字模式、文本、图像和 Codec2 诊断。
- 录制结束后等待显示 Flash 保存完成，再断电。保存失败时仍能下载 RAM 中的数据。新录制替换上一份录制。
- 将下载的 `rtl-iq.sigmf-data` 与 `rtl-iq.sigmf-meta` 放在同一目录。CU8 是交错无符号 8-bit I/Q，不是 WAV。

## 构建与验证

使用 **ESP-IDF 5.5.5 / ESP32-P4**。源 SDR 工程的 ESP32-S31、IDF 6.1 和 Korvo 配置留在其原工程。

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1
python tools/run_sdr_checks.py
python tools/run_voice_checks.py --sdr
powershell -ExecutionPolicy Bypass -File tools/run_rate_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_content_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_rsid_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/preview_lcd.ps1
node tools/check_sdr_web.cjs
```

固件在 `build-sdr/esp32_sdr_modem.bin`。Windows 脚本默认使用本机 Espressif 5.5.5 和 Cygwin GCC；其他环境激活 ESP-IDF 后运行 `idf.py -B build-sdr build`。

分区表保留 4 MiB 应用分区，新增 `0x420000` 起的 1.5 MiB `sdr_store`。刷入新固件时需要同步更新分区表。2026-09-09 已完成首次上板和启动故障修复，记录见下方移植说明。

数据流、来源、测试结果和硬件验收事项见 [SDR 移植说明](docs/SDR_PORT.md)。较早的 AP580/PSK 文档与录音保留为历史记录。

界面预览来自运行生产 LVGL 代码的主机测试，信号及飞机均为 HOST TEST 数据：[SSB](docs/screenshots/lcd-sdr-ssb.png)、[ADS-B 地图](docs/screenshots/lcd-sdr-adsb.png)、[FM](docs/screenshots/lcd-sdr-fm.png)、[Sub-GHz](docs/screenshots/lcd-sdr-subghz.png)、[Modem / Codec2](docs/screenshots/lcd-audio.png)。公共主题、页面管理、频段目录、地图来源与 UI skills 参考见 [界面维护说明](docs/SDR_PORT.md#统一界面ads-b-地图与中国大陆频段2026-09-09)。

USB 枚举、真实调谐、持续吞吐、接收音质和屏幕/网络并发仍需实机验证。主机合成 IQ 结果不代表真实短波性能或 FreeDV 互通验收。
