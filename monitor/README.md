# T-2CAN 双路监控版

本目录是独立于仓库根目录精简版的 PlatformIO 工程。页面提供 CAN A/B 接收状态、AP 红绿灯识别及高速/NOA 暂停、打灯自动变道、AP 免打扰、维护模式和 OTA 更新。五个功能开关的请求状态保存在设备 NVS，重启后恢复。

AP 免打扰开启且收到新鲜的 CAN A 中立滚轮模板时，右滚轮按 `+1 → 0 → -1 → 0` 循环，相邻步骤约 100 毫秒，每组结束后等待约 2 秒。手动操作或模板过期会暂停循环；重启后若开关之前保持开启，会在条件满足时继续运行。

## 编译和连接

1. 准备 LILYGO T-2CAN ESP32-S3 普通版（MCP2515），确认实际板型、接线及 CAN 速率。当前配置使用经典 CAN 500 kbit/s：CAN A 为 MCP2515（CS 10、RESET 9、SCK 12、MISO 13、MOSI 11），CAN B 为 ESP32 TWAI（TX 7、RX 6）。
2. 将 `t2can_monitor/credentials.example.h` 复制为同目录 `credentials.h`，在本机设置热点名称、热点密码和 OTA 密码。`credentials.h` 已由仓库忽略，不要上传。
3. 在本目录运行 `python -m platformio run -e t2can`；连接目标板后按实际串口选择运行 `python -m platformio run -e t2can -t upload --upload-port COM端口`。
4. 连接设备热点，访问 `http://192.168.4.1/`。首页及控制接口当前无需认证；`/update` 使用用户名 `admin` 和本机配置的 `TOKEN` 进行 OTA 认证。

开关表示设备请求状态。该监控版尚未经过本固件的真实 CAN 台架或车辆验证；发送提交不代表总线或对端已确认，网页处理和总线状态也会影响实际时序。CAN A/B 使用普通模式。已提交给控制器的帧在关闭或中断后仍可能发出，滚轮序列中断时不保证补发归零。

## 测试

`tlssc_pure_test.cpp` 覆盖红绿灯识别策略，`tests/dnd_scroll_cycle_test.cpp` 覆盖滚轮编码与调度，`tests/persistence_handler_host_test.py` 使用宿主替身测试实际处理函数的配置保存和重启逻辑。宿主测试不替代主板掉电、CAN 抓包或车辆验证。
