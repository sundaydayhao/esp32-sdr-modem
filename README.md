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
| Scanner | RTL-SDR 实测分段扫描、覆盖率/轮数、峰值保持、暂停/恢复、一键转到最强频点采集 IQ |
| OOK pulses | 原始 OOK/ASK 脉冲宽度与帧计数；尚未识别具体传感器协议 |
| IQ Record | RTL-SDR 原始 CU8，288 kS/s，连续保存到 SD；独立文件、CRC 校验、从 SD 重放频谱/音频；下载 SigMF |

`2x8PSK1200F` 和 AP580 保持移除。语音使用单载波 `8PSK1200F` 或 `8PSK1000F`。当前语音空口为本项目的 Codec2/PSK 协议，完整 FreeDV 700D/700E 互通仍属后续工作。

## 接线与使用

1. 将 RTL-SDR 插入板载高速 USB Host（当前实测 J18），给 P4 和屏幕供电。调试口为独立 Serial/JTAG。
2. 顶部只有“接收机”和“数字通信 / Codec2”两页。接收机的“接收模式”统一选择 USB/LSB、AM/WFM/NFM、频谱、ADS-B、扫描、Sub-GHz、IQ 采集和 SD IQ 重放；选择后自动切换设置及显示内容，并应用接收模式。单独改频后点击“应用频率”。
3. 上电默认 WFM、100 MHz。切换到音频模式会开启监听；“接收设置”可选择前端、上变频器本振及数字解码，监听开关和音量只影响设备 DAC。普通接收棒未接短波前端时先调试 FM、ADS-B、433 MHz 等调谐器范围内的信号。
4. ADS-B 自动调到 1090 MHz、2 MS/s。地图显示有效位置，列表也保留暂未解出坐标的飞机。点选飞机定位，拖动和缩放查看道路、地名和比例尺；没有接收数据时不显示测试飞机。
5. Sub-GHz 提供中国大陆接收参考目录及 OOK 脉冲、频谱、NFM 音频分析。原始脉冲计数不等于具体遥控器协议解码；FSK 信号可用频谱和 IQ 采集检查。目录说明不构成发射授权。
6. “IQ 采集至 SD”每次在卡的 `IQ` 目录新建一对 `IQnnnnnn.IQ`/`.MET`，约 0.55 MiB/s。点击停止，等待保存完成。“SD IQ 重放”选择文件和解调方式后播放，先验证 CRC，再按原采样率读取；重放是设备内频谱/音频分析。录制、保存或重放期间先停止当前操作再切换模式。
7. “频段扫描”填写起止 MHz 并开始，可暂停/继续和转到最强频点采集 IQ。显示的是分段实测峰值保持，不能代表全频段同时采样，也不是校准后的 dBm。
8. USB/LSB 可启用数字解码，把 48 kHz PCM 在设备内送入数字通信页；Codec2 使用 2000 Hz 音频中心。在 LCD 选择 Tx 或按住 PTT 会关闭 SDR 输入并切到线路音频；返回 Rx 后需重新启用 SDR。RTL-SDR 仅接收，手机端当前不提供 PTT。

| RF 输入 | 短波方式 |
| --- | --- |
| Tuner | 普通调谐器，界面范围 24～1766 MHz；实际范围取决于芯片 |
| Direct Q | 具备直接采样接线的 V3/兼容棒；0.5～28.8 MHz 走 Q ADC，更高频率恢复调谐器 |
| Blog V4 | V4 的 R828D 分频段输入、HF 内置 28.8 MHz 上变频与滤波控制 |
| Upconverter | 外接短波上变频器，填写实际本振，例如 125 MHz；界面显示原始 RF 频率 |

普通接收棒需要相应的短波前端，不能只修改调谐数字。直接采样有 Nyquist 镜像，需要匹配天线与滤波。V4L 未作为 V4 宣称支持。

一只接收棒同时只有一个中心频率和采样率。ADS-B、FM、扫描通过切换配置运行；返回数字模式页面不会自动改变 RF 配置。

## Android App 与 SD 文件

2026-09-11 新版固件已烧录，App 已安装到实际手机，Wi-Fi 控制、SD 录制/重放及 CRC 导出已验证，详见 [本次验收记录](docs/NATIVE_UI_VALIDATION_20260911.md)。

当前固件只提供设备 API，已移除 `/`、`/sdr`、`/modem` 网页。手机使用 `mobile/android` 的原生 Java App，无 WebView。

1. 安装 `build/android/sdr-studio-0.1.0-debug.apk`，连接热点 **ESP32-SDR**（密码 `pskmodem32`）；手机提示 WLAN 无互联网时选择继续使用。
2. 打开“SDR 工作台”，默认设备地址 `192.168.4.1`，连接后显示真实状态并直接控制板子。
3. 手机支持接收模式、频率、频谱/瀑布图、ADS-B 地图及列表、Sub-GHz、扫描、SD 录制/重放，以及数字通信接收设置、文本/图像/Codec2 诊断。
4. “导出 IQ”通过系统文件选择器保存 ZIP，包含同名 `.sigmf-data` 和 `.sigmf-meta`，校验字节数与 CRC。SD 原文件仍保留。CU8 是交错无符号 8-bit I/Q，不是 WAV。

LCD 离线地图内置深圳核心及周边道路、地名（22.40–22.85°N、113.75–114.45°E），最小经度跨度 0.01°；区域外保留世界海岸线并提示缺少详细地图。Android 使用相同离线区域，联网时按当前视野加载 OpenStreetMap 街道瓦片，可使用手机移动网络；网络不可达时回退到缓存和离线底图。地图来源、ODbL 许可和构建说明见 [Android 说明](mobile/android/README.md)。

FreeDV 桌面程序与本项目的 Codec2/PSK 空口不是同一协议；安装 FreeDV 不代表已经实现 FreeDV 700D/700E 互通。

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
powershell -ExecutionPolicy Bypass -File tools/build_android.ps1 -Test
```

固件在 `build-sdr/esp32_sdr_modem.bin`。Windows 脚本默认使用本机 Espressif 5.5.5 和 Cygwin GCC；其他环境激活 ESP-IDF 后运行 `idf.py -B build-sdr build`。

分区表保留 4 MiB 应用分区，新增 `0x420000` 起的 1.5 MiB `sdr_store`。刷入新固件时需要同步更新分区表。2026-09-09 已完成首次上板和启动故障修复，记录见下方移植说明。

数据流、来源、测试结果和硬件验收事项见 [SDR 移植说明](docs/SDR_PORT.md)。较早的 AP580/PSK 文档与录音保留为历史记录。

界面预览来自运行生产 LVGL 代码的主机测试，信号及飞机均为 HOST TEST 数据：[SSB](docs/screenshots/lcd-sdr-ssb.png)、[ADS-B 地图](docs/screenshots/lcd-sdr-adsb.png)、[FM](docs/screenshots/lcd-sdr-fm.png)、[Sub-GHz](docs/screenshots/lcd-sdr-subghz.png)、[Modem / Codec2](docs/screenshots/lcd-audio.png)。公共主题、页面管理、频段目录、地图来源与 UI skills 参考见 [界面维护说明](docs/SDR_PORT.md#统一界面ads-b-地图与中国大陆频段2026-09-09)。

IQ/Scan 的主机布局预览：[IQ 采集](docs/screenshots/lcd-sdr-iq.png)、[频段扫描](docs/screenshots/lcd-sdr-scan.png)。实物 433.92 MHz IQ 已导出并通过 CRC 校验，433.05～434.79 MHz 及 88～108 MHz 扫描均已覆盖全频段，详见移植说明中的 IQ/Scan 验收记录。

2026-09-10 已连接真实 R820T/R820T2 接收棒，验证 106.2 MHz FM 的 USB/IQ/48 kHz PCM 链路，并解出真实 1090 MHz ADS-B 报文和飞机坐标；修复插棒时的 DMA 内存不足、关闭设备断言，以及复位后的枚举恢复。实测范围、日志和音频采集方法见 [上板记录](docs/SDR_PORT.md#rtl-sdr-实物接收与重启修复2026-09-10)。扬声器听感、短波前端和屏幕/网络/录制并发仍需验收；主机合成 IQ 不代表真实短波性能或 FreeDV 互通。
