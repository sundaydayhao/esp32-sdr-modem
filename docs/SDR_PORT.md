# ESP32 SDR Modem 移植记录

## 来源和硬件目标

- 基础工程：`C:\projects\esp32p4-psk-modem-codec2`，提交 `97743de1967a4e2bc3eb18cd0f515f3f51f56788`。
- SDR 来源：`C:\projects\esp32-rtlsdr-display`，提交 `f854514898caf7ed703140e130fe21780350c96c`。
- 目标目录：`C:\projects\esp32-sdr-modem`；导出基础工程全部受版本控制的文件，复制依赖缓存辅助本地构建，未复制旧 `.git`。两个源工程未改动。
- 目标硬件沿用 ESP32-P4 Function EV Board 的 LCD、触摸、ES8311、C6 Wi-Fi 及配置；SDK 为 ESP-IDF 5.5.5。没有将 S31/Korvo 的 RGB LCD、ES8389 或 IDF 6.1 配置带入。
- 高速 USB 使用 P4 的 controller 0、UTMI PHY；Serial/JTAG 保留独立接口。驱动确认设备协商为 USB High Speed，ADS-B 需要 2 MS/s 的 IQ 数据。

板载 Type-A 与高速控制器的连接见 [Espressif P4 开发板指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)。旧板版本的 USB 连接器应按实际原理图接线。

## 实际数据流

```text
RTL2832U USB bulk CU8 IQ
  -> 带配置 generation/sequence 的有界 IQ 队列
  -> 512 点 FFT -> RF 频谱 / 瀑布图 / 扫描峰值
  -> USB / LSB / AM / WFM / NFM -> 48 kHz PCM
       -> 数字解码队列 -> 原有 PSK / RsID / 图像 / Codec2 收包
       -> 监听队列 -> 既有 tx_task -> ES8311 扬声器
  -> 2 MS/s ADS-B -> CRC / airborne CPR / 飞机列表
  -> OOK 原始脉冲分析
  -> 512 KiB IQ 捕获 -> Flash 保存 -> SigMF 下载
```

USB 回调只复制到预分配的 IQ 池，DSP 在独立任务中执行。16 个 8192 字节 IQ 块在 PSRAM；解码和监听各使用 32 个 128 点 PCM 块。队列满时统计丢弃。硬件变化在驱动任务中暂停、取消并排空 bulk transfer 后统一应用，旧 generation 的 IQ/PCM 不进入新会话。

控制传输使用持续存活的 transfer 和完成上下文。发生超时或拔插时，只有确认回调/在途请求完成后才能释放；无法排空时保留上下文并显示错误。驱动错误后可拔出、重新插入接收棒恢复枚举。

`audio_task` 继续独占数字模式状态；`tx_task` 继续独占 DAC。启用 SDR 时不会把 ADC 麦克风误作射频音频，且拒绝进入调制解调器发射。关闭 SDR 后恢复原有线路音频及 PTT。音量和监听开关不触发重新调谐。

## 解调和短波输入

`sdr_demod.c` 的 FM 相位差检波、FIR 抽取和去加重由原 `fm_receiver.c` 适配。去掉 Korvo 的独立音频后端，复用 P4 的 DAC 所有者。AM 使用包络检波；USB/LSB 在抽取后使用 193 tap 复数带通选择对应边带，音频通带约 300～3300 Hz。输出固定 48 kHz 单声道。WFM 为单声道，不含立体声/RDS。

普通接收使用 288 kS/s，ADS-B 固定 2 MS/s。频率单位与边带需要用户按真实信号设置；界面上的 RF 频率与数字模式的音频中心是两个参数。

- **Tuner**：FC0012、FC0013、R820T/R820T2、R828D 的既有驱动路径。界面的宽范围不等于每种调谐器都覆盖整个范围。
- **Direct Q**：具备相应硬件接线的直接采样棒，低于等于 28.8 MHz 使用 Q ADC，并设置 RTL2832U DDC；更高频率恢复调谐器 IF。存在 Nyquist 镜像，需要外部滤波。
- **Blog V4**：显式选择 V4，要求 R828D；实现 HF 28.8 MHz 上变频、HF/VHF/UHF 输入切换、tracking/notch 控制以及 GPIO5 上变频开关。未把普通 R828D、V4L 或 V3 自动视为 V4。
- **Upconverter**：校验并加上用户设置的 24～250 MHz 外部本振，显示/导出的频率仍是原 RF。

V4 方式依据 [RTL-SDR Blog V4 指南](https://www.rtl-sdr.com/v4/) 和固定版本驱动代码；来源及 GPL 许可在 `third_party/rtl-sdr-blog/README.md`。Codec2 的原始 LGPL 源码及码本继续完整保留。

## SDR 应用与保存

应用通过一个接收器状态统一切换，不会同时调谐 FM、短波和 ADS-B。Scanner 使用 144 kHz 步进、288 kHz 采样窗和约 500 ms 的调谐周期，显示每段峰值；实际扫描速度受 USB 控制完成时间影响。

ADS-B 接收器修正了短输入片段未累计、跨 USB 块重复计数的问题。只处理 112-bit DF17/18，支持 airborne CPR 偶/奇帧组合；不是完整 Mode-S、地面 CPR、MLAT 或空中交通管理系统。列表保留最多 24 个目标，过期目标不继续展示。

OOK 页面输出原始脉冲宽度及帧计数，未加入具体厂商设备协议。RF 瀑布图支持触摸 RF 标记，设置仍由 Apply 确认。FM 页面显示实际音频波形。

录制先进入最多 512 KiB 的 PSRAM，再由低优先级任务保存到专用 SPIFFS。保存期间同一份数据保持不可变；下载和保存持有读引用，新录制等待读引用释放。USB 错误、超时或丢块会标记断样，部分捕获仍可保存。

Flash 内部格式为 `SDR2`、32 字节大端头和 CU8 数据，CRC-32 覆盖头的前 28 字节及全部 IQ。头内包括长度、RF、本振、前端、采样率和断样计数。重启只恢复长度/范围/CRC 都有效的记录；对非空损坏文件系统不自动格式化。空白的新数据分区可初始化。

下载时拆成标准 SigMF `cu8` 数据和 JSON 元数据，内部头不混入 IQ。新版分区表添加 `sdr_store`，偏移 `0x420000`、大小 `0x180000`，没有在本次工作中烧录或修改设备 Flash。

## 界面与 API

LCD 默认打开 SDR 接收台，保留 Modem/Codec2 页面和返回 RF 按钮。提供应用、前端、MHz 频率、外部本振、输入/直接解码/监听、音量、扫描区间及录制控制。无数据时显示 USB 状态，生产界面不启用合成瀑布图。

网页热点为 `ESP32-SDR`，默认密码 `pskmodem32`。

| 路径 | 用途 |
| --- | --- |
| `GET /`、`GET /sdr` | SDR 接收界面 |
| `GET /modem` | 原有数字通信界面 |
| `GET /api/sdr` | 配置、实际频率、USB、IQ/PCM 丢弃、FFT、波形、ADS-B、录制状态 |
| `POST /api/sdr` | 完整接收配置，校验类型、频率、输入方式、ADS-B 和数字解码组合 |
| `POST /api/sdr/record` | `{"action":"start"}` 或 `{"action":"stop"}` |
| `GET /api/sdr/record` | 完成捕获的 `rtl-iq.sigmf-data` |
| `GET /api/sdr/record-meta` | 同一捕获的 `rtl-iq.sigmf-meta` |

原 `/api/rx?cursor=...` 返回二进制及 `X-Next-Cursor`、`X-Content-Epoch`；网页按原协议读取，并在设备 boot id 改变后清空游标。Codec2/图像状态单独显示，不把编码数据作为 UTF-8 文本显示。

## 已执行的主机验证

- `tools/run_sdr_checks.py`：两种 SSB 的 1 kHz 单音测试均约 62.5 dB 镜像抑制；AM/WFM/NFM 各输出 48000 个 PCM 样本，并检查主频；这是指定理想测试向量的结果。
- 相同测试经生产 SDR 解调器恢复 USB/LSB 的完整 65 字节 PSK125R 文本。
- ADS-B 已知 CRC/CPR 报文在 2～512 字节片段下各只计数一次，验证 38000 ft 与参考位置，拒绝坏 CRC。
- IQ 存储格式验证全部头部单 bit 错误、数据损坏和截断。
- `tools/run_voice_checks.py --sdr`：在原语音测试中实际插入 CU8 IQ 和生产 SSB 前端，两种单载波模式各恢复 90/90 包，CRC 错包为 0，END/最后一帧完整。
- 原模式回归：PSK125R/250R 完整 4096 字节、单载波 8PSK1200F 的 ±20 Hz、96 行/9216 像素图像、Codec2 包/播放逻辑及 RsID 固定向量通过。
- LVGL 运行实际生产界面：原有 29 模式/PTT、SDR 导航、HF 校验、ADS-B 预置、FM 波形页面；预览画面使用明确标注的 HOST TEST 数据。
- `node tools/check_sdr_web.cjs`：网页原始字节游标、重启重置、ADS-B 预置、配置 POST、错误响应和 Codec2 状态通过。

构建与各项运行日志位于本地 `build/sdr-*.log`；生成缓存不进入 Git 仓库。

2026-09-09 最终 ESP-IDF 5.5.5 / ESP32-P4 构建通过，应用镜像为 1,797,568 字节，4 MiB 应用分区剩余约 57%。LCD 预览的 SSB、ADS-B 和 FM 页面已进行图像检查，截图位于 `docs/screenshots/`，均为标注的主机测试画面。

## 尚需硬件验收

本次没有烧录新固件，也没有把主机合成 IQ 当作真实接收棒测试。需要在实际 P4 和指定接收棒上检查 USB 枚举、V4/Direct-Q/变频器调谐、USB 拔插、2 MS/s 连续流、SSB 听感与频偏、真实 ADS-B、Flash 保存恢复，以及 LCD/Wi-Fi/录制并行时的内存和持续丢样。

短波链路仍需真实电台和 fldigi 双向音频验证。MFSK16 完整收发、Olivia、FreeDV 700D/700E、宽带立体声/RDS、已命名传感器协议、SD 卡长时间 IQ 录制等不在当前已实现列表中。
