# SDR 工作台 Android

原生 Java / Android Views 客户端，无 WebView。Android 8.0（API 26）及以上，当前实测为荣耀 BVL-AN16 / Android 16。

## 安装和连接

构建产物：项目根目录 `build/android/sdr-studio-0.1.0-debug.apk`。本机已通过 ADB 安装测试版。

1. 手机连接开发板热点 **ESP32-SDR**，密码 **pskmodem32**。提示 WLAN 无互联网时选择继续使用。
2. 打开“SDR 工作台”，默认设备地址 **192.168.4.1**，也可在“连接”对话框填写设备的局域网 IPv4 地址。
3. “接收模式”选择后直接应用到板子；单独编辑频率后点击“应用频率”。“声音监听”和下方音量控制板上扬声器。WFM 下勾选“手机播放 WFM”，通过 Wi-Fi 实时收听解调音频；手机音量使用系统媒体音量键，与板子独立。
4. IQ 采集、保存和重放都在板上 SD 卡执行。重放前校验全文件 CRC，校验时显示进度；停止或播放完毕后再切换实时接收模式。
5. “导出 IQ”选择 SD 文件，使用 Android 文件选择器保存 ZIP，包含同名 SigMF CU8 数据和 JSON 元信息。导出检查长度、CRC，SD 源文件保留。
6. 数字通信页提供解码设置和文本/图像/Codec2 状态。当前发送/PTT 在 LCD 操作，RTL-SDR 仅接收。

顶部页签与板子统一为 **RTL-SDR / PSK Modem**，当前页使用绿色高亮。标题、连接状态和页签固定在独立底色的顶部区域；下方参数、状态提示、瀑布图与接收结果属于滚动区，以分隔线、常显滚动条和滚动边缘淡出区分。两个页面分别保留当前会话内的滚动位置。

## 手机 WFM 播放

WFM 实时 USB IQ → 板上解调 → 48 kHz 单声道 S16LE → Wi-Fi UDP → Android AudioTrack。需要更新板子固件和 APK，当前支持一台手机监听；不传原始 IQ，不消耗 SD 卡空间。进入非 WFM 模式、关闭接收或使用 SD 重放时暂停手机音频，恢复实时 WFM 后可继续。换台清理旧配置的音频。

播放器只在 App 前台运行；退出前台会停止音频，返回后按勾选状态恢复。其他应用取得音频焦点或耳机断开时取消勾选，避免意外外放。手机端启动预缓冲约 64 ms；设备、Wi-Fi 与系统音频输出还会增加延迟，此数值不是实测端到端延迟。网络和播放缓冲均有上限，卡顿时舍弃旧数据并恢复，界面显示网络丢包及缓冲丢弃。

设备 UDP 8081 接收 `WFM-LISTEN 1\n`，仅向请求的来源地址/端口发流，订阅每 900 ms 续期、3 秒无续期停止；同一时间只接受一个订阅者。`WFM-STOP 1\n` 可提前停止当前订阅。每帧固定 32 字节小端头：`WFM1`、配置代数 u32、音频序号 u32、RF Hz u32、设备时间 ms u32、样本数 u16、标志 u16、设备缓冲丢弃 u32、本次服务启动标识 u32。实时帧含 512 个 S16LE 样本，标志 1；等待帧样本数/标志均为 0。最大 UDP 负载 1056 字节，音频净流量 96 kB/s。HTTP 控制与音频独立执行。

## 地图

离线区域为深圳核心及周边，边界 **22.40–22.85°N、113.75–114.45°E**。含主要道路、部分次级道路、河流、机场和地名，区域外不保证道路覆盖。与 LCD 共用 `main/sdr_map_region.json`，不把模拟飞机显示为实物接收。

有互联网连接时按可见视野请求 OpenStreetMap 街道瓦片；没有互联网或服务不可达时使用缓存和区域离线底图。地图与设备 Wi-Fi 连接分别选路，加载在线地图可能使用移动数据。当前网络的在线瓦片未通过实机验收，离线区域已验证。

坐标采用 WGS84 / Web Mercator，不做 GCJ-02 偏移。在线瓦片 URL 位于 `AircraftMapView.java`；遵循可见视野加载、明确 User-Agent、至少 7 天缓存、不预取，RAM 12 MiB、磁盘 100 MiB 上限。离线要素经简化，地图用于接收观察。

© [OpenStreetMap contributors](https://www.openstreetmap.org/copyright)，数据库许可 [ODbL 1.0](https://opendatacommons.org/licenses/odbl/1-0/)。离线文件内保留查询、时间和源数据 SHA-256；生成器为 `tools/build_regional_map.py`。瓦片使用规范：[OSM Tile Usage Policy](https://operations.osmfoundation.org/policies/tiles/)。

## 构建和维护

工具链：Gradle **8.7**、Android Gradle Plugin **8.5.2**、Android SDK **34**、Java **17** 源码。Windows 构建脚本使用 Android Studio 的 JBR，检测本机缓存的 Gradle 8.7 和已安装的 SDK 34。

在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_android.ps1 -Test
```

也可用 Android Studio 打开本目录，选择 Gradle 8.7，执行 `assembleDebug testDebugUnitTest`。仓库未存放 Gradle wrapper 二进制，需本机提供对应 Gradle。`preBuild` 自动将项目的离线地图和 `ui_zh.json` 复制到生成资源目录，避免维护重复数据。

- `MainActivity`：两页界面、设备状态同步、命令/轮询调度、系统文件导出。
- `ReceiverConfig`：无线枚举与模式默认设置，10 仅代表 UI 的 SD 重放，不发送为无线模式。
- `DeviceApi`：串行 HTTP、设备网络选路、限长读取、SD 数据流与 CRC。
- `WfmAudioPlayer` / `WfmAudioBuffer`：绑定设备 Wi-Fi 的 UDP 接收、有限抖动缓冲、AudioTrack 输出、生命周期与音频焦点。
- `SpectrumView` / `AircraftMapView`：原生绘图及真实/历史/断开状态。
- `UiText`：复用 LCD 中文目录翻译设备状态，保留收到的原始数字通信内容。

测试使用生产 Activity、绘图和 HTTP 客户端，连接标明 **HOST TEST** 的本机协议夹具；测试截图不能证明真实 RF 性能。测试覆盖模式切换、音频恢复/静音、SD 入口保留、指令期间轮询恢复、设备拒绝/断开、地图坐标，以及数字通信初始状态和 HTTP 202 响应。

当前为开发调试 APK；发布签名和应用商店分发尚未配置。设备控制 API 只在当前热点提供，不提供旧网页。
