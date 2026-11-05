# ESP32-S31-Korvo-1 网络收音机

这是面向 **ESP32-S31-Korvo-1 V1.1** 的独立 ESP-IDF 工程，硬件定义来自配套原理图和乐鑫官方 `esp32_s31_korvo_1` BSP。

新的 Codex 对话开始维护本工程前应先阅读 [`AGENTS.md`](AGENTS.md)，其中记录了硬件约束、软件架构、构建烧录流程、当前版本、验证清单和最近一次实机状态。版本化的单一合并固件位于 [`firmware/`](firmware/)，配套原理图位于 [`docs/hardware/`](docs/hardware/)。

## 已实现

- 800×480 RGB565 屏幕与 GT1151 电容触摸
- LVGL 固定为 9.4.x，主标题为 `WebRadio By Wireless-Tag`
- ES8389 Codec、双路 NS4150 功放和板载扬声器接口
- MP3/AAC 网络电台、ICY 曲目信息和 HLS 播放列表
- 8 个中国大陆 CDN 直连 MP3 电台置顶，默认启动中国之声
- 无 GMF pipeline：HTTP 数据直接送入轻量 `esp_audio_codec` 解码器
- 启动时固定播放 2 秒 1 kHz 扬声器测试音
- 触摸选台、左右滑动切台、播放/停止、音量、重新配网和测试音
- 与指定参考工程相同的 `78/esp-wifi-connect` Web 配网方案
- UART0 双向控制和 115200 baud 日志
- Flash Download Tool 可直接烧录的单文件合并固件

## 流畅度设计

- 网络读取任务固定在 CPU0；LVGL、应用控制和音频解码运行在 CPU1，并按优先级隔离
- 256 KiB 编码流缓冲显式分配在 PSRAM，让网络下载和解码/I2S 输出并行执行
- 首播只预缓存约 2 秒以缩短切台等待；256 KiB 缓冲继续在后台蓄水，低水位恢复仍按约 5 秒容量执行
- HTTP 短暂无数据会继续等待，不再误判为播放结束并反复重连
- 关闭 Wi-Fi 省电模式，并停用高 CPU 占用的 RGB bounce buffer
- UI 只在状态改变时刷新控件，不再以固定周期重复设置全部标签
- 长文本取消持续滚动，频谱更新限制在低负载的 10 FPS
- 首页频谱柱以 10 FPS 读取真实 PCM 峰值，在兼顾流畅度的同时恢复动态效果
- 音频链不创建 GMF pipeline，因而不会再出现 `ESP_GMF_PIPELINE` 空指针日志

## 硬件通道

工程不复用旧 ESP32-S3 Nano 的引脚定义，所有屏幕、触摸和音频通道均由官方 Korvo-1 BSP 初始化：

- 触摸 I2C：SDA GPIO0，SCL GPIO1，GT1151 地址 `0x14`
- 音频 I2S：BCLK GPIO3，LRCK GPIO4，DOUT GPIO5
- 扬声器功放使能：GPIO7
- 显示：800×480 RGB565，并行 RGB 接口
- 日志：板载 USB-UART 桥连接 UART0，115200 8-N-1

该板的 ES8389 没有连接 MCLK，因此播放器统一把解码结果转换为 **16 kHz、双声道、16 bit** 再输出。扬声器接口是 NS4150 的差分功率输出，应连接配套无源扬声器；不要把普通三段式耳机或电脑有源音箱的 3.5 mm 插头当作等价负载，也不要把任一 `SPK+`/`SPK-` 端接地。

## 构建与合并

```bash
cd /home/biubiu/work/tmp/ESP32S31_KORVO1_RADIO
./tools/build_merged.sh
```

默认使用：

```text
/home/biubiu/.espressif/master/esp-idf
```

需要切换 IDF 时可设置 `ESP_IDF_ROOT`。ESP32-S31 在当前 IDF 中仍是 preview target，脚本已经带上所需的 `--preview` 参数。

## Flash Download Tool

生成文件：

```text
build/esp32s31-korvo1-webradio-merged.bin
```

设置：

| 选项 | 值 |
| --- | --- |
| Chip | ESP32-S31 |
| 文件地址 | `0x0` |
| SPI Mode | DIO |
| SPI Speed | 80 MHz |
| Flash Size | 16 MB |

合并镜像内含位于 `0x2000` 的 bootloader、位于 `0x8000` 的分区表，以及位于 `0x10000` 的应用。建议首次烧录前执行一次整片擦除，合并文件中的空白区也会清除旧 NVS，之后需重新配网。

## 使用

1. 上电后，UART0 会输出日志，扬声器自动播放 2 秒测试音。
2. 没有已保存网络时，设备进入 Web 配网模式；连接屏幕提示的 `WEBRADIO...` AP，打开 `http://192.168.4.1`。
3. 点击主电台卡片打开全部电台列表，或在卡片上左右滑动切台。
4. 右侧可播放/停止、重新配网或再次播放测试音；底部滑条控制 ES8389 硬件音量。

UART0 命令：`r` 重试、`n/p` 切台、`+/-` 调音量、`w` 配网、`t` 测试音、`?` 状态。
