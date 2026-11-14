# T-2CAN 打灯自动变道精简版

## 硬件购买

### 1. T-2CAN 开发板

本固件需要 **LILYGO T-2CAN ESP32-S3 普通版**（MCP2515），购买时注意区分 CAN FD 版本。

> **重要提示：本接线方案下，下单时必须联系卖家客服，要求拆掉 T-2CAN 开发板上的 120 欧姆电阻。**

- 参考到手价：**168.91 元**
- [淘宝下单链接](https://m.tb.cn/h.jjwbm4Z)

### 2. 线束

线束可以按自己的动手需求选择：

**TITA 家线束**：约 **60 元**，需要另外接延长线。

<img src="docs/images/tita-harness.png" alt="TITA 线束客服报价参考" width="240">

**成品延长线束**：约 **150 元**，想少折腾可以选成品。

<img src="docs/images/harness-shop.png" alt="成品线束店铺参考" width="240">

下单前请向商家确认车型、接口和线束规格。以上价格仅供参考，实际以商家报价为准；店铺截图中不同商品的价格以对应商品页面为准。

## HW4 车型：TITA 线束接线参考

以下记录的是提供的 HW4 TITA 线束示例；线色、插头和针脚可能因车型、年款或线束版本不同而变化，动手前请与卖家核对实际线束，不能只凭颜色判断。

> **请先断电并将线束与车辆和开发板断开，再进行剪线与接线。** 当前固件会在 CAN A 上主动发送帧，接线、车辆兼容性和功能效果尚未完成台架或实车验证，请先在隔离 CAN 台架验证，不要用于道路行驶。

### 1. 处理线束插头

按下图橙色标记的位置，剪掉 TITA 转接线束支线末端的小插头，预留足够长度接延长线；不要剪断车辆原车线束。剪线前确认标记位置和各根线的用途；未使用的线芯分别做好绝缘，避免短路。

<img src="docs/images/hw4-tita-cut.png" alt="TITA 线束剪线位置参考" width="320">

### 2. 接延长线并连接 T-2CAN

按下面的线色对应关系接线，CAN H 接 H、CAN L 接 L。接头应牢固并分别绝缘；T-2CAN 端以板上 CAN A、CAN B 及 H/L 实际标识为准。

| TITA 线束线色 | 用户提供的信号 | T-2CAN 端口 |
| --- | --- | --- |
| 橙色 | Chassis CAN H | CAN A 的 H |
| 绿色 | Chassis CAN L | CAN A 的 L |
| 黄色 | CAN H | CAN B 的 H |
| 白色 | CAN L | CAN B 的 L |

线束接延长线的参考照片：

<img src="docs/images/hw4-tita-extension.png" alt="TITA 线束接延长线参考" width="480">

T-2CAN 接线参考照片：

<img src="docs/images/hw4-t2can-connection.png" alt="T-2CAN 接线参考" width="480">

**固件说明：** 当前精简版只初始化并使用 CAN A；黄色和白色线即使按上表接到 CAN B，当前固件也不会使用 CAN B。

### 3. 插头针脚参考图

下面的图是所提供的车辆侧插头参考，仅用于核对资料；图中颜色和编号不能直接替代对具体车型、线束与插头朝向的核实。

<img src="docs/images/hw4-connector-reference.png" alt="车辆侧插头针脚参考图" width="480">

## 固件刷写

### 方式一：使用 AI 提示词协助刷写

适合不熟悉编译和刷写的用户。使用能够操作电脑文件和终端的 AI 工具，将 T-2CAN 用 USB 数据线连接电脑，并保持设备与车辆断开，然后复制下面的提示词发送给 AI：

```text
请帮我通过 USB 给 LILYGO T-2CAN ESP32-S3 普通版刷入这个项目的精简版固件：
https://github.com/emo666/t2can-lane-change

请先确认设备已与车辆断开，并根据我的操作系统检查 USB 连接、串口、Python 和 PlatformIO；缺少工具时指导我从官方来源安装。不要猜测串口：如果有多个候选设备或无法确认 T-2CAN，请让我确认。

请下载项目，检查 platformio.ini。将 src/credentials.example.h 复制为 src/credentials.h，并让我在本地文件中设置热点名称 SSID 和热点密码 PASSWORD；热点密码至少 8 位。保留示例中默认的网页登录密码 TOKEN。不要让我把个人热点密码发到聊天中，也不要输出或上传它。

在项目目录编译固件。如果设备上已有需要保留的固件或数据，请先帮助我备份并确认设备分区；备份或设备状态无法确认时先停下。确认目标设备和刷写风险后，使用 PlatformIO 的 USB 上传方式刷写，刷写可能覆盖已有固件和分区。

刷写结束后报告结果、生成的固件位置和访问网页的方法；如果失败，请根据实际错误帮助我排查。不要声称已经验证车辆效果。
```

刷写成功后，连接配置的 Wi-Fi 热点，打开 `http://192.168.4.1/`；默认网页登录用户名为 `admin`，密码为 `88888888`。设备每次重启后开关默认开启。

### 方式二：网页一键刷写

不需要安装编译工具。先下载项目准备好的[完整固件包](https://github.com/emo666/t2can-lane-change/raw/refs/heads/main/release/t2can-complete.bin)，再用乐鑫官方的 [ESP Tool 网页刷写工具](https://espressif.github.io/esptool-js/)刷写。

1. 将 T-2CAN 与车辆断开，用 USB 数据线连接电脑，在 Chrome 或 Edge 打开 [ESP Tool](https://espressif.github.io/esptool-js/)。刷写前请备份需要保留的固件和数据。
2. 在 **Program** 区域点击 **Connect**，选择开发板串口，确认识别到 ESP32-S3。
3. 选择下载的 `t2can-complete.bin`，将 **Flash Address 改为 `0x0`**，再点击 **Program**。不要随意选择 Erase all。
4. 刷写完成后设备重启。手机连接热点 `T2CAN-Monitor`，热点密码为 `t2can888`；浏览器访问 `http://192.168.4.1/`，登录用户名 `admin`、密码 `88888888`。

此完整镜像会覆盖其写入范围内的数据，包括 NVS；即使没有勾选 Erase all 也可能丢失该范围内原有数据。**不要把单独的应用文件 `firmware.bin` 当作完整镜像按 `0x0` 刷写。**该刷写流程尚未在实际 T-2CAN 设备上验证。

> 成品包的热点和登录密码公开且相同，附近知道密码的人可能连接并控制设备；HTTP 通信也未加密，请仅在可信环境使用。要使用自己的密码，需要修改配置并重新编译。

### 方式三：PlatformIO USB 刷写

适合熟悉电脑开发工具的用户。下载本仓库，在项目目录中将 `src/credentials.example.h` 复制为 `src/credentials.h`，填写热点名称和热点密码；网页登录默认用户名 `admin`、密码 `88888888`。连接开发板后，在项目目录运行：

```sh
pio run -t upload
```

PlatformIO 会编译并通过 USB 上传。若有多个串口，请先用 `pio device list` 确认设备，再用 `pio run -t upload --upload-port COM3` 指定端口（将 `COM3` 换成实际端口）。刷写可能覆盖已有固件和分区，操作前请确认设备并备份。

### 后续更新：设备网页 OTA

设备已经安装支持 OTA 的兼容固件且分区布局兼容时，可以连接设备热点，在网页中选择“OTA 固件更新”并上传应用固件 `firmware.bin`。空白或原厂设备不能直接使用本项目 OTA。上传时先使用当前固件的登录密码；成功重启后使用新固件的密码。

请先备份现有固件和数据。自行编译的固件如果包含个人热点或网页登录密码，请不要公开分享。
