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
       -> WFM 独立有界队列 -> Wi-Fi UDP -> Android AudioTrack
  -> 2 MS/s ADS-B -> CRC / airborne CPR / 飞机列表
  -> OOK 原始脉冲分析
  -> 有界 PSRAM 队列 -> SD 卡 /IQ/*.IQ + *.MET -> SigMF 下载
SD IQ 文件 -> 长度/CRC 校验 -> 带数据源/generation 的 IQ 队列 -> FFT / 解调 / DAC
```

USB 回调只复制到预分配的 IQ 池，DSP 在独立任务中执行。16 个 8192 字节 IQ 块在 PSRAM；解码和监听各使用 32 个 128 点 PCM 块。队列满时统计丢弃。硬件变化在驱动任务中暂停、取消并排空 bulk transfer 后统一应用，旧 generation 的 IQ/PCM 不进入新会话。

控制传输使用持续存活的 transfer 和完成上下文。发生超时或拔插时，只有确认回调/在途请求完成后才能释放；无法排空时保留上下文并显示错误。USB bulk 使用 4 个 4096 字节内部 DMA 缓冲，和上述 PSRAM IQ 队列分别管理。USB daemon/client 固定在 core 0，优先级分别为 7/6，保证 IDF 完成在途控制传输记账后才由客户端关闭设备。首次启动未枚举设备时自动重试根端口一次；串口 `SDR USBRESET` 可请求驱动排空传输后重新枚举。

`audio_task` 继续独占数字模式状态；`tx_task` 继续独占 DAC。启用 SDR 时不会把 ADC 麦克风误作射频音频。PSK Modem 页的“接收来源”可立即选择“音频线”或“RTL-SDR”：前者关闭 SDR 接收处理和解码输入，后者在同一服务锁内开启接收与解码、保留当前 RF 频率及前端，保留已有 LSB，否则使用 USB 上边带。对应串口命令为 `SDR INPUT LINE` / `SDR INPUT RTL`。仅开启 FM、ADS-B 等接收而未接入数字解码时，显示“RTL-SDR 待解码”，再次选择 RTL-SDR 即可接入。来源改变会递增 generation、重置旧 PCM 队列和解码会话，阻塞读取期间跨代的音频块也会丢弃。

显式选择 Tx、按下 PTT 或串口请求 TX 时，在服务锁内关闭 SDR 输入、更新 generation 并保留 TX 互锁，随后启动线路音频会话；录制 IQ 期间拒绝此切换。TX、语音排空、SD 录制/保存和重放期间不能切换接收来源。返回 Rx 后保持线路音频，可直接在 PSK Modem 页选择 RTL-SDR 恢复射频解码；仅切换页面不会改变输入。底部显示实际来源、RF 频率/边带、等待状态或互锁原因。RTL-SDR 页的“开启接收”是接收处理开关，需点击应用才生效，不是 USB 供电开关；音量和监听开关不触发重新调谐。

## 解调和短波输入

`sdr_demod.c` 的 FM 相位差检波、FIR 抽取和去加重由原 `fm_receiver.c` 适配。去掉 Korvo 的独立音频后端，复用 P4 的 DAC 所有者。AM 使用包络检波；USB/LSB 在抽取后使用 193 tap 复数带通选择对应边带，音频通带约 300～3300 Hz。输出固定 48 kHz 单声道。WFM 为单声道，不含立体声/RDS。

普通接收使用 288 kS/s，ADS-B 固定 2 MS/s。频率单位与边带需要用户按真实信号设置；界面上的 RF 频率与数字模式的音频中心是两个参数。

- **Tuner**：FC0012、FC0013、R820T/R820T2、R828D 的既有驱动路径。界面的宽范围不等于每种调谐器都覆盖整个范围。
- **Direct Q**：具备相应硬件接线的直接采样棒，低于等于 28.8 MHz 使用 Q ADC，并设置 RTL2832U DDC；更高频率恢复调谐器 IF。存在 Nyquist 镜像，需要外部滤波。
- **Blog V4**：显式选择 V4，要求 R828D；实现 HF 28.8 MHz 上变频、HF/VHF/UHF 输入切换、tracking/notch 控制以及 GPIO5 上变频开关。未把普通 R828D、V4L 或 V3 自动视为 V4。
- **Upconverter**：校验并加上用户设置的 24～250 MHz 外部本振，显示/导出的频率仍是原 RF。

V4 方式依据 [RTL-SDR Blog V4 指南](https://www.rtl-sdr.com/v4/) 和固定版本驱动代码；来源及 GPL 许可在 `third_party/rtl-sdr-blog/README.md`。Codec2 的原始 LGPL 源码及码本继续完整保留。

## SDR 应用与保存

应用通过一个接收器状态统一切换，不会同时调谐 FM、短波和 ADS-B。Scanner 使用 144 kHz 步进、288 kS/s 采样，仅将每窗中央 144 kHz 映射到全频段的 512 个显示点；末窗贴合终点，避免遗漏边界。每次跳频排空旧 generation，从新 IQ 到达起等待 100 ms，再取得 4 帧真实 FFT 才推进；没有收流时不会空走扫描进度。实际速度包含 USB 控制耗时，88～108 MHz 实测约 58 秒完成首轮。

`main/sdr_scan.c` 集中维护扫描中心、覆盖范围和峰值保持。硬件每跳一次仍递增 IQ `generation`，另用 `view_generation` 标识用户改变频段/模式；LCD/Web 不因内部跳频清空全景图。同一范围暂停/恢复保留峰值、覆盖率和轮数，换范围或离开扫描后重新开始。最强频点来自保留的 FFT 峰值，未经功率校准，不能当作电台识别或精确载频测量。

ADS-B 接收器修正了短输入片段未累计、跨 USB 块重复计数的问题。只处理 112-bit DF17/18，支持 airborne CPR 偶/奇帧组合；不是完整 Mode-S、地面 CPR、MLAT 或空中交通管理系统。列表保留最多 24 个目标，过期目标不继续展示。

OOK 页面输出原始脉冲宽度及脉冲组计数，每组最多保存 64 个宽度，未加入具体厂商设备协议。改频、切换模式或停启接收后清除上一会话的脉冲结果和检测门限，IQ 丢块时丢弃未完成的组；数据发布必须匹配当前 RF generation。LCD/Web 显示距上次脉冲的时间，超过 5 秒隐藏旧脉宽，断流时显示等待，避免旧信号被误认为当前收到。RF 瀑布图支持触摸 RF 标记，设置仍由 Apply 确认。FM 页面显示实际音频波形。

IQ 录制由 `main/sdr_storage.c` 独占 SD 卡文件状态：挂载、打开、录制、完成、校验、重放、下载。存储任务优先级 3、core 1，USB 和 DSP 只提交有界队列，不执行文件 I/O。128 个 8192 字节 PSRAM 块缓存输入，实际 USB 4096 字节块可缓存约 0.91 秒；再聚合成约 64 KiB 的 POSIX 多扇区写入，避免 newlib 小缓冲拆分请求。刷新容量和目录仅在存储任务执行，LCD 读取缓存快照。

microSD 使用 BSP SDMMC slot 0（GPIO 39～44、LDO 4），在 C6 启动前挂载。Wi-Fi 的 SDIO slot 1 保留；挂载失败/刷新只清理 slot 0。挂载从不自动格式化。录制到 `/sdcard/IQ/IQ000001.TMP`，Stop 后排空队列、fsync、写入同名 `.MET`，再改名为 `.IQ`；新录制不覆盖旧文件。缓存满或 USB 中断停止并保存带丢样标记的部分文件，写入/完成失败的临时文件不列入可播放目录。列表保留最新 32 个有效文件；单文件上限 2 GiB 减 8192 字节，约 62 分钟，实际长度受卡空间和持续写速限制。

`.IQ` 是无头 CU8。`.MET` 是 36 字节大端 `SDR3`：magic、IQ 字节数、RF Hz、本振 Hz、前端、采样率、丢样次数、IQ CRC-32、前 32 字节的头 CRC-32。固定 288 kS/s，文件名严格限制为 `IQ` + 六位数字 + `.IQ`。元信息和文件长度有效的文件进入目录；开始重放前还需完整验证 IQ CRC。`.IQ` 和 `.MET` 应一起保留。旧 `SDR2` 编解码及兼容测试保留，当前 IQ 功能不再挂载原 `sdr_store` SPIFFS 分区，也不清除其内容。

重放按原采样率读取 8192 字节块，送入同一 DSP 的频谱、WFM/NFM/AM/USB/LSB 或 OOK 分析。文件 IQ 与 USB IQ 带独立来源及配置 generation，切换瞬间等待中的第一块保留到新配置生效；SD 源期间忽略 USB 数据，不调用调谐器。播放结束前等待 DSP 和监听队列排空，结束后保留标为 SD REPLAY 的频谱。重新应用接收配置才回到实时 USB。录制/完成/重放期间拒绝改频、重连和线路 TX；下载与 SD 录制/重放互斥。

网页可选择 SD 文件，分块下载标准 SigMF `cu8` 数据和 JSON 元信息，不将整个文件加载到 RAM。卡中其他目录和文件保持原样。

## 当前界面与 API（2026-09-11）

LCD 顶部为“SDR 工作台”，两页为“RTL-SDR”和“PSK Modem”。接收模式下拉框统一管理音频解调、频谱、ADS-B、Sub-GHz、扫描及 SD 功能，选项和右侧视图随模式切换；采集/保存/重放时禁止改模式。Sub-GHz 页保留频段预设、频率和 OOK 脉冲结果，不再提供重复的模式下拉框或 IQ/扫描跳转按钮。页面只由实际模式决定，不因频率落在 Sub-GHz 范围而改写顶部选项；切换到频谱、NFM、IQ 或扫描时保留当前频率和扫描范围。490 MHz 计量预设仍采用频谱分析，顶部同步显示该模式；再次选择 Sub-GHz 接收时使用 433.92 MHz 脉冲预设。界面使用 `ui_theme.c` 统一主题，`ui_zh.json` 集中管理中文，`ui_i18n.c` 处理标签和格式化文本。`tools/build_ui_fonts.py` 从 Noto Sans SC 生成 14/18/26 像素字符子集；OFL 许可证保存在 `main/NotoSansSC-OFL.txt`。新增中文后须重新生成字体。

两页瀑布图共用带深色边缘的绿色频点指示线，点击或横向拖动后松手应用频点。接收页在手势开始时固定频率范围，经 `sdr_service_configure` 提交一次调谐；频率输入框自动更新。扫描点选沿用“查看最强信号”的行为，退出扫描并进入所选频点的 IQ 查看，尚未开始录制。录制、保存和 SD 重放期间保持频率；历史重放须切回实时接收才能调谐。数字通信页的文本/图像音调可在 100–3500 Hz 内选择，沿用现有 UI 事件和 audio_task 控制；Codec2 保持固定 2000 Hz。拖动越界时限于图形边缘，外部调谐或页面/收发上下文变化会取消未完成的手势。

`main/sdr_map.c` 在世界海岸线概览上增加深圳离线道路及地名；`tools/build_regional_map.py` 从 OpenStreetMap Overpass 要素生成 `sdr_map_region.json/.inc` 和地名表，不批量下载街道瓦片。源数据时间为 2026-09-11T01:57:56Z，区域为 22.40–22.85°N、113.75–114.45°E，29,292 条要素、81,424 个简化顶点、165 个地名。数据包含来源查询和 SHA-256，按 ODbL 保留 © OpenStreetMap contributors 署名；具体许可见 https://www.openstreetmap.org/copyright 。位置为 WGS84，不加 GCJ-02 偏移。区域外不承诺街道覆盖；LCD 会显示范围提示。

手机使用 `mobile/android` 原生 Java App，直接调用设备 HTTP API；`rx_web.c`/`sdr_web.c` 历史文件名保留，但不再注册网页、JS/CSS 或地图资源，也不再把 HTML 嵌入固件。热点为 `ESP32-SDR`，默认密码 `pskmodem32`，IP `192.168.4.1`。

| 路径 | 用途 |
| --- | --- |
| `GET /api/info` | `sdr-studio` 产品标识、API 版本 1、boot id、native-app、zh-CN、只接收能力 |
| `GET /api/aircraft` | 最多 24 架飞机及报文/位置年龄、可用坐标、呼号/高度/速度/航向 |
| `GET /api/presets` | 中国大陆接收参考目录 |
| `GET /api/sdr` | 配置、实际频率、USB、IQ/PCM 丢弃、FFT、波形、扫描和 SD 状态 |
| `POST /api/sdr` | 完整接收配置，沿用固件范围校验和 TX/录制互锁 |
| `POST /api/sdr/record` | `{"action":"start"}` / `{"action":"stop"}` |
| `GET /api/sdr/record?file=IQ000001.IQ` | 指定 SD 文件的 CU8 流 |
| `GET /api/sdr/record-meta?file=IQ000001.IQ` | SigMF 元信息，含 `sdr:crc32` 和 `sdr:sample_gaps` |
| `POST /api/sdr/sd` | refresh / play（file、mode）/ stop |
| `GET /api/status`、`GET /api/modes`、`POST /api/config` | 数字通信状态、模式和解码配置 |
| `GET /api/rx?cursor=...`、`GET /api/image` | 解码内容，保留游标、内容代数和 boot id 处理 |

接收模式的线路枚举 0–9 保持兼容；“SD IQ 重放”的 10 仅为 UI 选择，不能发送为射频模式。Android HTTP 请求串行执行，失败和断开时禁用设备控制；Wi-Fi API 连接绑定覆盖设备地址的 Wi-Fi 网络，地图连接独立选择可访问互联网的网络。录制和重放在板上执行。App 的“声音监听”及下方音量控制板上扬声器；新增“手机播放 WFM”通过 UDP 8081 接收实时解调 PCM，手机用系统媒体音量调节。

`sdr_audio_stream.c` 聚合 512 个 48 kHz 单声道样本，8 帧 PSRAM 队列满时丢弃最旧帧；取帧时丢弃超过 120 ms 的数据。DSP 只在服务锁内复制 PCM，不执行网络调用，也不消费原有解码/监听队列。`sdr_audio_wifi.c` 独立低优先级任务处理单手机订阅和 UDP 发送，3 秒无续期释放；HTTP 控制不被长连接占用。发送前核对实时 WFM、配置代数、接收开关、TX 与 SD 来源；切换后不发送旧队列。API 暴露 `wifi_audio_subscribed/packets/drops/errors`；具体 WFM1 包格式、App 生命周期和延迟边界见 [Android 说明](../mobile/android/README.md#手机-wfm-播放)。

后文按日期保留旧实现与验收历史，其中 Web、旧页签和 RAM 录制说明已由本节及 SD 章节取代。

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

## 2026-09-09 首次上板与启动修复

通过 COM21 识别到 ESP32-P4 芯片 v3.2、16 MiB Flash、32 MiB PSRAM，LCD 为 1024×600 / EK79007，触摸为 GT911。使用 ESP-IDF 5.5.5 编译，并烧入 bootloader、分区表和应用；未全片擦除，也未重刷 C6。用户确认本次没有 RTL-SDR 接收棒。

原始提交 `31f2ce419dfe1d62055f31ffedfe9a49f241f389` 上板后持续重启：36.1 秒内记录到 15 次 panic，间隔约 2.4 秒，未执行到应用启动完成。基线日志为 `build/sdr-hardware-baseline-20260909.log`，ELF 标识 `debffcafd`。

修复的启动问题：

1. `content_service_init()` 在 8 KiB 主任务栈调用 `codec2_create()`；`make_analysis_window()` 的两个 512 点复数局部数组已占满 8 KiB，随后在 `kf_work()` / 中断入口触发栈保护。改由现有 24 KiB Codec2 任务创建并校验实例，通知主任务初始化结果；Codec2 原始源码未修改。
2. 内部 SRAM 被可放到外部内存的软件对象占用，导致 SDR 分配或 Wi-Fi 启动任务创建返回 `ESP_ERR_NO_MEM`。启用 Codec2 上游 `__EMBEDDED__` 选项，将约 80 KiB 只读码本留在 Flash，并用项目 allocator 将 Codec2 工作区放在 PSRAM。通用分配优先 PSRAM，应用的任务间队列显式使用 PSRAM；DMA、任务栈和热路径中显式要求内部内存的对象保持内部。启动结束后主任务返回，释放其栈。
3. P4 SPM 符合 `MALLOC_CAP_INTERNAL`，但 IDF 5.5.5 的 Flash cache guard 不接受该区域的任务栈；`rx_ap_start` 在 NVS 初始化时触发 `esp_task_stack_is_sane_cache_disabled()` 断言。Wi-Fi 初始化和 IQ 存储任务改为显式申请 DRAM 栈，不修改共享 SDK，也不关闭断言。

持续观察工具 `tools/observe_board_stability.py` 使用实际串口，保存原始日志、带相对时间的日志和 JSON 摘要；每 15 秒请求 `STATUS` / `RXDIAG`。`--exercise-codec` 在接收状态执行 36 次 Text/Image/Audio 切换；`--reset` 表示测试开始时主动复位一次，报告区分这次复位与后续异常启动。每次使用新标签以保留旧证据，例如在 IDF 环境中运行：

```powershell
python tools/observe_board_stability.py --port COM21 --seconds 600 --label sdr-stability-run1 --reset --exercise-codec
```

启动稳定性验收时烧录镜像为 `build-sdr/esp32_sdr_modem.bin`，1,797,872 字节，SHA-256 `01f77ea7e16538aba21b62605a9cc44601353bf6dcfdc06dcf486bc936e8fcc4`；设备启动报告的 ELF SHA-256 前缀为 `1b13346f7`。后续 Tx 操作修复的镜像和验证单独记录如下。

2026-09-09 10:52:20～11:02:20（UTC+8）的连续实机观察持续 600.125 秒：仅有开头的一次主动 USB 复位，应用启动一次，panic / abort / 看门狗告警 / 自发重启均为 0。36 条自动内容切换命令全部得到确认，40 次 `STATUS` / `RXDIAG` 均正常；日志另记录到 3 次非脚本配置变化，总计 40 个配置会话。Codec2 始终 ready，初始化栈余量为 14,616 字节。40 个内存样本中的内部可用堆恒为 77,651 字节，最大内部块恒为 31,744 字节；PSRAM 约 28.32 MB。

证据保存于 `build/sdr-hardware-acceptance-20260909.{log,timestamped.log,json}`；本轮最终构建与烧录日志为 `build/sdr-hardware-queues-{build,flash}-20260909.log`。`run_sdr_checks.py`、启用嵌入式码本配置的 `run_voice_checks.py --sdr`、content/rate/RsID 和生产 LVGL 主机检查均通过；这些主机检查仍不能替代物理 RF / 空口测试。

连续观察后又执行两次主动 USB 复位，各观察约 45 秒；两次均只有一次预期启动，应用和热点正常启动，各收到 3 次状态及内存诊断，工具判定通过，无 panic 或额外重启。日志与 JSON 为 `build/sdr-hardware-repeat-{1,2}-20260909.*`。这属于主动复位验证，未进行断电冷启动测试。测试结束已关闭本次串口会话，板子保留修复版固件并继续运行。

本次仍观察到两项非致命告警：`sdr_store` SPIFFS 挂载失败且分区非空，程序按设计保留内容，当前 IQ Flash 存储不可用；C6 报告版本 `0.0.0` 与主机 `2.12.0` 不匹配，但本轮热点成功启动。未进行格式化、C6 升级或真实录制验证。由于没有接收棒，持续观察中 IQ/PCM 样本数为 0，本结论仅覆盖无接收棒待机、配置切换和启动稳定性，不覆盖 USB bulk 缓冲分配、RF 持续流或满负载性能。

## Modem 页 Tx 切换修复（2026-09-09）

实机复现日志 `build/sdr-tx-baseline-20260909.log` 显示：默认 SDR 输入开启，进入 Modem 页后请求 `ROLE TX` 被旧互锁拒绝，状态仍为 Rx。服务现将显式 TX/PTT 请求视为切换到线路音频，在同一把锁内关闭 SDR 输入、递增 generation 并保留 TX 互锁；IQ 录制期间仍拒绝切换。LCD 底部增加输入来源、操作方法和录制互锁提示。

本次镜像为 1,799,168 字节，SHA-256 `bcdac072b423e6fb09ef09063b75725552939be2618cd406876f769ead9b504a`，已通过 COM21 烧录同一块 ESP32-P4 rev v3.2（MAC `e8:f6:0a:e7:72:4b`）。构建、烧录与生产 LVGL 检查日志分别为 `build/sdr-tx-{build,flash,lcd-check}-20260909.log`。LVGL 检查覆盖导航不改输入、Tx 下拉事件、输入提示更新、返回 RF 后复选框同步和录制提示；服务状态由主机桩提供，不代表真实 USB 录制。

`tools/check_board_tx.py` 通过真实串口发起与触屏共用的控制事件，检查输入来源、实际 TX 活跃状态、返回 RX 后输出停止、Codec2 会话内编码计数推进，以及 panic/异常重启/语音排空超时。每次 `--reset` 含一次主动 USB 复位，退出时恢复 Rx。例如：

```powershell
python tools/check_board_tx.py --port COM21 --content AUDIO --cycles 5 --reset --label tx-audio-run1
```

三组实机验收全部通过：Audio 5 轮 / 109.531 秒、Text 3 轮 / 69.125 秒、Image 3 轮 / 69.204 秒，共 11 轮 Tx/Rx 切换。每组仅有一次预期主动启动、一次 SDR 到线路音频切换；panic、意外重启、语音排空超时均为 0。每轮 Audio 在同一会话内编码计数由约 89～90 帧推进到 157～160 帧。33 个诊丢样本的内部空闲堆均为 77,635 字节、最大块均为 31,744 字节。测试结束状态为 Image / Rx、TX inactive、输出队列为空，串口已关闭。

验收证据为 `build/sdr-tx-audio-verified-20260909.{log,json}`、`build/sdr-tx-text-20260909.{log,json}`、`build/sdr-tx-image-20260909.{log,json}`。首次 Audio 脚本误将每会话清零的计数器当成累计计数，产生 `sdr-tx-audio-20260909` 失败记录；修正为比较同一 TX 会话内的两次计数后完成上述验收。这些测试未接 RTL-SDR 棒，也未接对端进行音频/空口互通，不能证明真实 IQ 录制、USB 满负载或端到端收发质量。

## 统一界面、ADS-B 地图与中国大陆频段（2026-09-09）

维护入口：`main/ui_theme.h/.c` 集中管理 LCD 颜色、控件、导航；`main/theme.css` 管理两个网页的公共外观，颜色与 LCD 对应。`main/display.c` 负责数字通信页，`main/sdr_display.c` 负责接收页，接收配置仍统一交给 `sdr_service_configure()`，页面导航本身不改变输入。频段元数据只写在 `main/sdr_presets.c`，LCD 直接读取，网页通过 `/api/presets` 获取。

设计参考了 [UI UX Pro Max](https://github.com/nextlevelbuilder/ui-ux-pro-max-skill) 的触控尺寸、间距和状态反馈建议，以及 [Anthropic Frontend Design](https://github.com/anthropics/skills/tree/main/skills/frontend-design) 的一致视觉语言和任务布局建议。未安装第三方 skill 或运行其安装脚本；实际界面使用项目的 LVGL 和原生 HTML/CSS/Canvas。

ADS-B 列表可点选定位，地图支持拖动、缩放和适配全部飞机。机标采用成功 CPR 解码的经纬度，15 秒无新位置即不再画点；仍有其他报文的飞机可以保留在列表，显示无新坐标。目标 60 秒没有报文后退出列表。关闭接收或离开 ADS-B 模式不再提供飞机快照，失去流时取消有效坐标，网页断线时清空机标。渲染使用带锁的有界快照，不扩大常规 SDR 状态对象，也不在音频或 USB 回调里绘图。

底图为 [Natural Earth 1:110m Coastline 4.1.0](https://www.naturalearthdata.com/downloads/110m-physical-vectors/110m-coastline/)，用于离线方位概览，仅包含海岸线和经纬网，不是街道地图。数据属于 [Public Domain](https://www.naturalearthdata.com/about/terms-of-use/)。生成工具 `tools/build_map_data.py` 从 [NACIS 数据镜像](https://naciscdn.org/naturalearth/110m/physical/ne_110m_coastline.zip) 获取 ZIP，源文件 SHA-256 为 `664449b39070027e882abb295974d182afec18ca21107273d17e9e8bf6f64817`，生成 134 条路径、5128 个顶点，量化为 0.01 度；`main/sdr_map_data.h` 与 `main/map_data.json` 应随源码保留。地图数据在 Flash，LCD 像素和飞机快照在 PSRAM。

默认中国大陆接收参考包括 433.92、315.00、431.00、490.00、868.30 MHz，展示对应的遥控或民用计量用途。依据 [工信部 2019 年第 52 号公告](https://www.miit.gov.cn/zwgk/zcwj/wjfb/gg/art/2020/art_4f2c890530cc4837b2113cf977e249c4.html) 及[技术要求目录](https://www.miit.gov.cn/cms_files/filemanager/oldfile/miit/n1146295/n1652858/n1652930/n4509607/c7545828/part/7545929.pdf)，这些范围具有具体用途及技术条件，不标成通用免许可发射频段。项目 RTL-SDR 仍为只接收；Sub-GHz 提供原始 OOK 脉冲、频谱、IQ 录制与扫描入口，尚不解码 LoRa/FSK 或具体厂商传感器数据。

新增主机验证覆盖地图投影方向、日期变更线、缩放边界、像素缓冲边界、无效/过期坐标及五组预设；实际 LVGL 覆盖飞机列表、点选定位、缩放/适配、空状态和 Sub-GHz 应用。网页检查覆盖结构化飞机、缺少坐标、断线清除、频段 POST；浏览器预览也使用标注的 HOST TEST 数据，不能作为真实空口接收证据。

本版 ESP-IDF 构建通过，镜像 1,918,928 字节，SHA-256 `d7fdabef1c819859c79a11766ad2a1e8382cfdd063aa289c07c090d0ac5302af`，经 COM21 写入并校验成功，应用分区剩余 54%。日志为 `build/ui-redesign-{build,flash,lcd,sdr-check}.log`。2026-09-09 12:21:21～12:24:21（UTC+8）的实机观察持续 180.172 秒，仅测试开始的一次主动 USB 复位，无 panic/异常重启，12 次状态响应正常；内部可用堆恒为 76,883 字节，最大内部块 31,744 字节。期间还记录到非脚本的模式、Tx 和 Image 切换。原始证据为 `build/ui-redesign-board-20260909.{log,timestamped.log,json}`。

本轮仍未接 RTL-SDR，真实飞机/传感器数据与 USB 持续流未验证；电脑也未通过板载热点完成 HTTP 联调，网页交互在本地主机 fixture 上检查。原有 SPIFFS 挂载告警仍在，未格式化数据分区。

烧录后另完成 Audio Tx/Rx 两轮实机回归，持续 61.140 秒，仅一次预期主动复位，无 panic、额外启动或语音排空超时；两轮会话的编码计数分别从 90/89 推进到 157。测试末尾恢复 Audio/Rx、TX inactive、输出队列为空，并关闭串口。证据为 `build/ui-redesign-tx-20260909.{log,json}`。

## RTL-SDR 实物接收与重启修复（2026-09-10）

本轮使用同一块 P4 rev 3.2，经 COM21 烧录；用户将接收棒插入 USB HS J18，未接低频上变频器。实际枚举为 `0bda:2838`、R820T/R820T2、bulk IN `0x81` / MPS 512。FM 按用户提供的深圳交通广播 106.2 MHz 测试；ADS-B 使用 1090 MHz、2 MS/s。以下 USB/IQ/飞机数据均来自实物，不是主机合成信号。

插棒基线在分配 8×8192 字节内部 DMA bulk 缓冲时内存不足，随后 `usbh_dev_close` 断言，32 秒内反复重启。缓冲缩减为 4×4096 字节；同时修复 USB 任务优先级倒置：IDF 5.5.5 在通知客户端后才递减 `num_ctrl_xfers_inflight`，原来的高优先级客户端可能抢先关闭设备。修改限于本项目，没有改共享 SDK 或跳过其断言。基线证据为 `build/rtlsdr-attached-baseline-20260910.*`。

另外，P4 复位而接收棒仍供电时，首次设备短描述符可能失败。已实测由驱动排空请求后重启根端口可恢复；现在等待首次枚举 6 秒仍无设备事件时自动重试一次，已见设备或正常收流后不触发此启动重试。这里是控制器根端口操作，不能等同于切断接收棒的物理 VBUS。曾试用 IDF 的 PSRAM DMA 选项，初次枚举失败在切回内部内存后同样出现，不能据此判断 PSRAM DMA 不受支持；最终配置采用已实测的 16 KiB 内部 DMA 缓冲。

ADS-B 每个 4096 字节 IQ 块约 1.024 ms 到达。原先 DSP 每处理一块就休眠一 tick，真实 2 MS/s 测试出现少量队列丢块；改为队列为空时阻塞、忙时每 20 ms 让出一 tick。FM 监听不再被 Modem 页遗留的 Audio 选择静音，只有实际数字语音播放或 SDR 数字解码时才抑制原始监听。

诊断入口和证据采集：

```powershell
python tools/probe_board_sdr.py --port COM21 --seconds 120 --label my-fm-run --step '2:SDR FM 106200000'
python tools/probe_board_sdr.py --port COM21 --seconds 300 --label my-adsb-run --step '2:SDR ADSB' --step '280:SDR AIRCRAFT'
python tools/capture_sdr_audio.py --port COM21 --frequency 106200000 --seconds 6 --label my-fm-audio
```

使用带 `pyserial`/`esptool` 的 IDF Python。每个标签必须唯一，原始串口和 JSON 保存在 `build/`。`--reset` 表示主动复位；工具区分主动启动和意外启动，退出码 0 仅表示采集正常，仍需检查 streaming、吞吐和丢块。`SDR STATUS` 提供 USB/处理字节、PCM/DAC 接收样本数、队列丢弃与内部/DMA/PSRAM 堆；`SDR FM <Hz>`、`SDR ADSB`、`SDR VOL <0..100>` 使用与界面相同的服务校验。`SDR AIRCRAFT` 只打印真实接收表，`position=0` 时即使留有上次坐标也不得画机标。PCM 采集带长度和 CRC 校验，所得 WAV 为接收棒经板上 WFM 解调后的 48 kHz 单声道，位于监听音量之前，不能替代扬声器听感验收。

最终烧录镜像为 1,922,592 字节，SHA-256 `6e86ce4187ef0e8318036672c8064c4b5f1efbe25591ad2e51131de681966b9d`。构建/Flash 校验日志为 `build/rtlsdr-final-build2-20260910.log`、`build/rtlsdr-final-flash-20260910.log`。随后三次主动整机 USB 复位各观察 35 秒，均在首次短描述符失败后由自动重试恢复，约启动后 11.2 秒开始收流；每组只有一次预期启动，无 panic/额外启动，未再要求物理重插。证据为 `build/rtlsdr-final-boot-{1,2,3}-20260910.{log,json}`。

随后同一固件完成 600.031 秒连续实机测试，采集 193 个状态快照、36 条定时命令，无 panic/自发重启，IQ/PCM/监听队列丢弃计数全程为 0。先持续 ADS-B，再每 8 秒交替配置 FM/ADS-B 共 20 次，最后返回 FM；共 23 个 RF generation 都观察到有效收流。切换通过与 LCD/Web 共用的服务执行，未冒充实际触屏操作。内部可用堆恒为 58,367 字节，内部 DMA 可用堆恒为 19,267 字节，PSRAM 为 27,869,940～27,879,396 字节。

| 实物链路 | 稳态测量 |
| --- | --- |
| ADS-B / 2 MS/s | 连续有效测量 326.622 秒，USB 和 DSP 均约 4,000,153 字节/秒，131 条有效 CRC 报文 |
| 106.2 MHz FM / 288 kS/s | 最后 77.631 秒，USB 和 DSP 均约 576,007 字节/秒，DAC 接收约 48,004 样本/秒 |
| Modem Audio 选择与 FM 共存 | 仍向 DAC 输出约 47,979 样本/秒，未因 Audio 菜单选择误静音 |

真实飞机表先后记录到 `7806E1`、`7817B5`（CSN6299）、`78138F`（CDG1183）和 `7802DD`（HBH3267）。其中 CDG1183 与 HBH3267 成功配对 CPR 并更新坐标，例如前者 `(22.505400, 113.937276)`，后者 `(22.457963, 113.877665)`；这两点为测试时的历史观测，不是当前实时位置。未配对或超过位置有效期的目标继续返回 `position=0`。

连续测试原始证据为 `build/rtlsdr-final-acceptance-20260910.{log,json}`，汇总为 `build/rtlsdr-final-acceptance-summary-20260910.json`。吞吐使用板上 uptime 和累计字节差值计算；切换时按设计排除旧 generation 和调谐稳定期，因此总 USB 字节与总 DSP 字节并非必须相等。测试末尾为 Text/Rx、WFM 106.2 MHz、监听音量 25%，串口采集已关闭。这是十分钟观察结论，不是全天候稳定性保证。

106.2 MHz PCM 导出为 `build/rtlsdr-fm1062-audio-20260910.wav`：6 秒、48 kHz、16-bit mono、288,000 样本，CRC-32 `af33096e`，长度/CRC 一致；RMS 4142.34、峰值 18816、数字削顶样本 0。对应 `.raw`/`.json` 和 `-console-` 日志保留在本地。尚未收到用户对扬声器音质的确认，不能仅凭非零 PCM 把广播可懂度或声学播放标为通过。

本轮生产 SDR/语音 DSP、content/rate/RsID、LVGL 和网页回归通过；主机 UI 覆盖双页导航、Tx 输入提示、ADS-B 地图/空状态等。主机检查不代表电脑已连接板载热点，也不能证明真实 LCD 触摸和网络满负载并行性能。原有非空 `sdr_store` 挂载告警仍在，未格式化分区或升级 C6。

## Radio 瀑布图对比度调整（2026-09-10）

LCD 和网页的信号色阶统一为深蓝、蓝/青、黄、橙、白；噪声背景压暗，峰值用白色突出，图上新增 Weak/Strong 色条。颜色仍在 `main/ui_theme.c` 集中定义，网页 `main/sdr.html` 与 `main/theme.css` 使用对应色值。

Radio 瀑布图以每行 FFT 的低 20% 分位估计噪声底，平滑跟随，以约 20 dB 的显示窗口拉开强弱信号；调谐或网页检测到设备重启后重新估计。仅改变显示颜色，不修改接收增益、原始 FFT 值或解调链路。色阶是相对强弱，不是校准后的 dBm；历史各行保留当时的映射。512 个 FFT 点缩至 LCD 452 列时保留每列覆盖范围的最大值，避免漏画单频点窄带信号。

生产 LVGL 检查增加了窄峰保留和接近实测噪声范围的强弱信号预览；网页回归通过。预览为 `docs/screenshots/lcd-sdr-contrast.png`，画面明确标注 HOST TEST IQ，不能当作现场频谱截图。构建日志为 `build/waterfall-contrast-build-20260910.log`，本版镜像 1,925,216 字节，SHA-256 `8ec219023c97d49ba1032c6dda268e01234241a21623e7fae2cbb59d08d0c8b0`。

本版已通过 COM21 烧录并校验到同一块 P4 rev 3.2（MAC `e8:f6:0a:e7:72:4b`），恢复烧录前用户正在使用的 WFM 89.8 MHz、监听音量 25%。烧录后观察 75.047 秒，24 个状态快照均正常，未观察到 panic/额外启动，最终持续收流，IQ/PCM/监听丢弃均为 0，内部可用堆 58,371 字节。证据为 `build/waterfall-contrast-{flash,board-console}-20260910.log` 和 `build/waterfall-contrast-board-20260910.{log,json}`；色彩布局通过主机预览检查，实机日志验证运行与收流，未冒充现场屏幕截图。串口观察已结束。

## IQ/Scan 接入与实物验收（2026-09-10）

LCD/Web 的 IQ/Scan 工作区改为 **Capture IQ / Scan band** 两种任务，直接提供频率或扫描起止值。应用设置会启用 RTL-SDR 并关闭音频监听/数字解码；接到实际 IQ 后才能采集。右侧显示字节进度、时长、捕获频率、丢样及 RAM/Flash 状态。扫描提供 Pause/Resume、覆盖率、完成轮数及 Inspect strongest，后者将接收器切到记录的最强频点，随后可开始 IQ 采集。

FFT 在新调谐后从第一帧实际功率初始化平均值，避免每个扫描片段都从最低显示值慢慢抬升。跳频仍经 `rtlsdr.c` 排空 USB transfer，扫描在服务层由实际 FFT 帧推进；暂停不清除测得的全景图。主机扫描检查覆盖 1 Hz 到 20 MHz 范围、非整步边界、末端覆盖、峰值保持和范围变更。生产 LVGL/Web 检查覆盖独立任务控件、暂停后保图和最强频点转 IQ；图片 `docs/screenshots/lcd-sdr-{iq,scan}.png` 标明 HOST TEST / simulated。

使用 USB HS J18 上的真实 R820T/R820T2，在 COM21 执行以下实测；未接上变频器，数据来自接收棒：

| 测试 | 结果 |
| --- | --- |
| 433.92 MHz IQ | CU8 / 288 kS/s，524,288 字节、0.910222 秒，丢样 0；串口导出的 CRC-32 `4262193f` 与板上计算一致 |
| 433.05～434.79 MHz 扫描 | 13 个调谐位置，75.032 秒混合测试中完成 6 轮，512 个显示点全部测量；暂停约 12 秒后继续 |
| 88～108 MHz 扫描 | 139 个调谐位置，150.031 秒测试中完成 2 轮，首轮约 58 秒；覆盖 100%，暂停约 14 秒后继续 |
| 录制互锁及模式回归 | 录制中改到 106.2 MHz 被拒绝；手动停止得到 266,240 字节的部分捕获，之后 FM、ADS-B、IQ 均恢复真实收流 |

两组扫描的暂停快照中，步数、轮数、覆盖率、峰值和 FFT 序号均保持不变，恢复后继续推进。两组共 111 个状态快照，panic/额外启动、IQ/PCM/监听队列丢弃均为 0；宽带扫描的内部可用堆恒为 58,247 字节。原始日志和 JSON 为 `build/iqscan-smoke-20260910.*`、`build/iqscan-wide-20260910.*`，摘要为 `build/iqscan-summary-20260910.json`；模式回归证据为 `build/iqscan-switch-20260910.*`。峰值保持不等同于同时接收全频段，最强频点也未作为广播台身份识别结果。

可通过同一服务在串口重复测试并导出 SigMF：

```powershell
python tools/probe_board_sdr.py --port COM21 --seconds 150 --label my-iq-scan --step '2:SDR SCAN 88000000 108000000'
python tools/capture_board_iq.py --port COM21 --frequency 433920000 --label my-iq-capture
```

使用 IDF Python，标签必须唯一。`capture_board_iq.py` 调谐并创建一次新捕获，检查收流、长度、CRC、频率和丢样，生成 `.sigmf-data`、`.sigmf-meta`、串口 `.raw` 和报告 `.json`。本次首份实测文件为 `build/iqscan-capture-20260910.sigmf-{data,meta}`。普通串口命令新增 `SDR IQ <Hz>`、`SDR SCAN <startHz> <stopHz>`、`SDR CAPTURE`、`SDR STOPCAP`；二进制 `SDR IQDUMP` 由工具消费。网页仍通过 `/api/sdr/record` 和 `/api/sdr/record-meta` 下载。

生产 SDR、Codec2/SDR、content/rate/RsID、LVGL 和网页检查通过。完成扫描实测后补充了“捕获频率”字段，确保实时接收改频后仍能辨认上一份文件；最终 ESP-IDF 镜像 1,936,128 字节，SHA-256 `42e9801a492afd1977ab29167afb0a34b4cba6111267bb8ae21cb64cbacd3af5`，构建日志 `build/iqscan-build4-20260910.log`，Flash 校验日志 `build/iqscan-flash2-20260910.log`。

最终镜像已写入同一块 P4 rev 3.2（MAC `e8:f6:0a:e7:72:4b`），随后观察 80.047 秒、40 个状态快照：433 MHz 扫描完成 5 轮、覆盖 100%，5 个暂停快照保留结果，恢复后继续；另验证当前改到 106.2 MHz 后，先前文件仍报告其采集频率 433.92 MHz。未观察到 panic/额外启动或 IQ/PCM/监听队列丢弃，最终内部可用堆 58,231 字节。证据为 `build/iqscan-final-20260910.{log,json}`。随后再次完成 512 KiB 实物导出，CRC-32 `fb9e53c9`、丢样 0，文件为 `build/iqscan-final-capture-20260910.sigmf-{data,meta}`；串口已关闭，板子留在 433.92 MHz IQ 模式，并保留可下载的完整捕获。

此轮旧版固件的 SPIFFS 因原有非空分区挂载问题不可用，未格式化；本轮验证的是 PSRAM 捕获及串口文件导出，没有声称 Flash 重启恢复成功。电脑未切换到板载热点，网页逻辑通过主机 fixture 检查，不能当作实际 HTTP 联调或现场触摸验收。该轮验证的是旧版 512 KiB 短捕获；后续 SD 版本及实测见本文 SD 录制章节。

## Sub-GHz 实时接收接入（2026-09-10）

Sub-GHz 的中国大陆预设现在显式启用 RTL-SDR、选择 Tuner 并关闭数字解码。LCD 点击 **Connect RTL-SDR**，网页点击应用设置，均经 `sdr_service_configure()` 和现有 USB 调谐状态机处理；即使此前 SDR input 关闭也能进入真实接收，仍受正在录制和线路音频 TX 的互锁约束。界面不直接拥有 USB 或 DAC。

LCD 将频段说明移到左侧，右侧以实际调谐频率、288 kS/s 收流及原始脉冲状态为主；提供 OOK、频谱和 NFM 三种分析。NFM 开启板载 48 kHz 单声道监听；IQ/Scanner 选择及 Open IQ / Scan 保留当前频率和预设扫描范围，进入共用的 IQ/Scan 工作区，避免留在频段说明页看不到录制/扫描进度。网页提供相同的分析选择、实际 IQ 状态和转入按钮。主机预览 `docs/screenshots/lcd-sdr-subghz.png`、`lcd-sdr-subghz-empty.png` 均有 HOST TEST 标记。

已编译并经 COM21 烧录到 P4 rev 3.2（MAC `e8:f6:0a:e7:72:4b`），接收棒仍在 USB HS J18。应用镜像 1,940,768 字节，SHA-256 `41c6550663bf78c0a9ede25d4f1a57a89a42e02c39ca3d2eaac9e96839abc28f`；构建与烧录校验日志为 `build/subghz-{build1,flash1}-20260910.log`。

烧录后先观察 35.094 秒，433.92 MHz OOK 真实收流正常；随后持续观察 120 秒、110 个状态快照。用户确认该时段按过汽车钥匙，433.92 MHz 原始脉冲组计数由启动时的 0 增至 226，并记录脉宽和更新时间。USB/DSP 稳态吞吐均约 576,020 字节/秒，FFT 持续更新；panic/额外启动与 IQ/PCM/监听丢块均为 0，内部可用堆恒为 58,215 字节。记录的是按键测试期间的 RF 活动，不是 226 条已解码的钥匙报文；没有独立确认钥匙的载频、OOK/FSK 调制或设备协议，环境信号及噪声也可能贡献原始脉冲组。

证据为 `build/subghz-startup-20260910.{log,json}`、`build/subghz-key-20260910.{log,json}`。新增串口 `SDR PRESET <0..4>` 使用与 LCD 相同的 C 预设，依次对应 433.92、315、431、490、868.30 MHz；`SDR NFM <Hz>` 用于验证该接收音频路径。`SDR STATUS` 新增 `ook_frames`、`ook_pulses`、`ook_age_ms` 及 `pulses`，年龄为 `UINT32_MAX` 表示当前会话尚无脉冲组。

随后完成 95.110 秒、47 个快照的预设和停启测试：315、431、490、868.30、433.92 MHz 均观察到实际调谐、持续 USB/DSP 收流及 FFT 更新，稳态约 575,823～576,444 字节/秒；从 SDR OFF 应用预设后自动恢复 enabled/streaming。433.92 MHz NFM 的 DAC 接收样本约 48,020 个/秒，此项只验证音频数据路径，不代表钥匙调制方式或语音可懂度。离开 OOK 或停止接收后的快照中，脉冲计数/文本均清空；本组没有 panic/额外启动或 IQ/PCM/监听丢块，内部可用堆恒为 58,215 字节。证据为 `build/subghz-presets-20260910.{log,json}`，汇总 `build/subghz-summary-20260910.json`。结束时为 433.92 MHz / Tuner / OOK，SDR 开启，数字解码及监听关闭，串口已关闭。

生产 SDR、Codec2/SDR、content/rate/RsID、LVGL、网页检查通过。主机交互检查覆盖从关闭接收的状态应用预设、脉冲等待/新鲜/过期/断流、NFM 监听标志，以及保持频率/范围转入 IQ 和扫描。网页尚未通过板载热点进行实际 HTTP 联调；当前仍未实现具体汽车钥匙、FSK/LoRa 或传感器协议解码。

## SD 卡连续 IQ 录制与重放（2026-09-10）

本轮使用 COM21 上的同一块 P4 rev 3.2、USB HS J18 的实际 RTL-SDR 和板载 SD32G 卡。卡最初容量为 31,104,958,464 字节，但自动挂载到约 200 MiB 的 FAT 分区，簇仅 512 字节，实测持续写入约 0.32 MB/s，低于 288 kS/s CU8 所需的 0.576 MB/s。初版测试触发队列满保护并保存带丢样标记的部分文件，没有崩溃；随后发现并修复数据源切换时丢弃第一块重放 IQ 的竞态。日志 `sd-iq-roundtrip1/2/3` 是问题排查记录，不能作为连续录制通过证据。

用户明确授权清空后，已将这张卡重建为覆盖整卡的单分区 FAT32、32 KiB 簇，挂载约 29,656 MiB 可用空间，SDMMC 40 MHz / 4 bit。格式化仅经一次临时维护固件执行，检查卡名 SD32G、容量和本项目 slot 0；FatFs 分区/格式化/重新挂载均返回 0。完成后已移除维护命令，并在最终应用二进制检查该命令字符串不存在。正常固件仍不自动格式化。证据 `build/sd-format-authorized-20260910.{log,json}`。

最终常规镜像为 1,973,600 字节，SHA-256 `5b5b9843df3e9658b508e0cf6c6f5a629277ee987e7b6c3fbe2eed9331db175f`，已经烧录校验，日志 `build/sd-iq-production-build2-20260910.log`、`build/sd-iq-final-flash-20260910.log`。

| 实机验证 | 结果 |
| --- | --- |
| 106.2 MHz 连续写 SD | IQ000001.IQ，34,525,184 字节，59.940 秒，CRC-32 06ae0446，丢样 0 |
| 完整 WFM 重放 | SD 全文件 CRC 通过，DSP 恰好处理 34,525,184 字节；DAC 链路接收 2,877,056 个 48 kHz PCM 样本 |
| 写入耗时 | 约 64 KiB 批量写入，累计 11.252 秒，单次最长 62.606 ms，能持续跟上 USB |
| 录制/播放互锁 | 两次尝试改变频率均被共用服务拒绝 |
| 170.063 秒稳定性观察 | 55 个快照，panic/意外启动为 0，IQ/解码 PCM/监听 PCM 丢块均为 0；内部可用堆最低 51,935 字节，DMA 最低 12,835 字节 |
| SD 文件导出 | IQ000002.IQ，2,883,584 字节，5.006 秒，CRC-32 5922578a；电脑 zlib 独立校验匹配，丢样 0 |

证据 `build/sd-iq-60s-final-20260910.{log,json,summary.json}`；导出 `build/sd-iq-5s-export-final-20260910.sigmf-{data,meta}` 及 JSON 报告。这里证明实际 USB 接收、SD 写读、DSP 和 DAC 数据路径；不是用主机合成信号冒充实物，也不能替代扬声器听感或电台身份确认。

随后完成 75.000 秒的主动复位/恢复测试：1 次预期启动、0 次额外启动/崩溃，原来的两份 SD 文件均恢复到目录；IQ000002.IQ 再次完整重放 2,883,584 字节，IQ000001.IQ 在处理 4,866,048 字节后按请求停止，再切回 106.2 MHz 实时 FM。实时接收时刷新 SD 卡成功，之后在 433.92 MHz 新建 IQ000003.IQ（1,175,552 字节、丢样 0），旧文件仍保留。全程 IQ/PCM/监听丢块为 0，最终内部堆 52,011 字节、DMA 12,911 字节。证据 `build/sd-iq-reboot-stop-final-20260910.{log,json}`。串口已释放，板子停在 433.92 MHz / Capture to SD，未继续录制；SD 卡保留 3 份测试文件。主动复位时仍见旧版已知的接收棒短描述符初次失败，现有一次启动根端口重试后自动恢复，这不属于整机重启。

使用方法见 README 的 **Capture to SD / Replay SD IQ**。串口与 LCD/Web 使用同一服务：`SDR SDREFRESH`、`SDR FILES`、`SDR PLAY <目录索引> <模式号>`、`SDR STOPPLAY`。模式号 5=频谱、3=WFM、4=NFM、2=AM、0=USB、1=LSB、8=OOK。`capture_board_iq.py --port COM21 --frequency 106200000 --seconds 5 --label UNIQUE_LABEL` 在板上完成连续 SD 录制、Stop/保存，然后从 SD 导出并做独立 CRC 校验。

SDR 算法/元信息校验、Codec2 --sdr、content/rate/RsID、生产 LVGL 交互与网页脚本检查通过。新增主机覆盖 SDR3 每一位的头损坏、边界/非法频率/路径、文件列表、重放请求/模式、停止、无卡禁用及记录源标记。`docs/screenshots/lcd-sdr-iq.png` 和 `lcd-sdr-replay.png` 为实际生产 LVGL 代码的 HOST TEST 预览，不能当作实物触屏截图。电脑未切换到板载热点，HTTP 联调仍未进行。当前支持本机保存的 288 kS/s CU8 + SDR3 文件对；2 GiB 附近、满卡、录制中拔卡/断电等尚未实测。

## WFM 监听恢复与 SPK 输出排查（2026-09-10）

IQ/Scan 或频谱重放会关闭监听，原 LCD/Web 切换到 WFM 时没有重新打开，因此即使 USB 收流、解调和电平都正常，仍可能没有声音。基线 `build/wfm-silent-baseline-20260910.{log,json}` 显示 89.8 MHz WFM 的 `monitor=0`，解调帧数增长但 `monitor_samples` 不变。现在进入 AM/WFM/NFM 时会启用接收和监听；进入后手动取消 Speaker 再 Apply 仍保持静音。该默认行为只在界面模式切换处处理，没有在服务层强制覆盖用户静音选择。

生产 LVGL 和网页交互检查均通过，覆盖 IQ/SD 频谱重放切回 Radio、AM/NFM 默认监听及手动静音保留；ESP-IDF 构建通过。镜像为 1,973,728 字节，SHA-256 `795b0df195523b7fc5a8bc7fac5348b52bc7ae68a6b648e5974d6b0c2d134c22`，COM21 烧录并校验成功。证据为 `build/wfm-audio-{build,lcd-checks,web-checks,flash}-20260910.log`。烧录后 25.047 秒观察未发现 panic 或额外启动，最终为 WFM 89.8 MHz、监听打开、音量 35，IQ/PCM/监听丢块均为 0；SD 目录有 4 份文件，本轮未清空或删除。证据 `build/wfm-audio-final-20260910.{log,json}`。用户已确认恢复声音，随后反馈噪声较大且从 SPK+/SPK− 引出。

板卡原理图中 SPK 输出来自 NS4150B 单声道 D 类功放；SPK− 为功放输出端，不是 GND。官方板卡说明将此端口用于无源扬声器，不能按普通耳机/线路输出的共地接口接线。核对资料为项目内 `ESP32-P4X-Function-EV-Board-CN/01_Schematic/SCH_ESP32-P4X_FUNCTION_EV_BOARD_V1.8_20260805.pdf` 的音频页，以及 [Espressif P4X 板卡说明](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html)。仍需确认用户的实际负载和接线，才能区分接口问题、功放底噪与 FM 接收噪声。

进一步采集实际 89.8 MHz WFM 解调后的 6 秒、48 kHz 单声道 PCM，288,000 样本、CRC-32 `ad3b2591`，峰值 19,981、削波样本 0；采集开始时 IQ/PCM/监听丢块均为 0。证据为 `build/wfm-noise-digital-20260910.{wav,json,raw}`。采样位于软件监听音量及模拟功放之前，只能验证数字数据，不能据此宣称模拟输出无噪声或噪声已修复；本轮未因该反馈盲目更改解调滤波参数。

## 尚需硬件验收

2026-09-10 已使用实物接收棒验证 FM/ADS-B 链路，范围和测量见上节。仍需指定前端的 V4/Direct-Q/变频器调谐、SSB 听感与频偏、更长时间 SD 满卡/拔卡恢复，以及 LCD/Wi-Fi/录制并行压力测试；未接上变频器的本轮结果不证明短波接收性能。

短波链路仍需真实电台和 fldigi 双向音频验证。MFSK16 完整收发、Olivia、FreeDV 700D/700E、宽带立体声/RDS、已命名传感器协议、超过 288 kS/s 的宽带 IQ 录制等不在当前已实现列表中。
