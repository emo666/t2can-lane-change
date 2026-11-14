#!/usr/bin/env python3
"""Build a personalized complete image for ESP Tool (contains credentials)."""
from pathlib import Path
import json
import subprocess
import sys

project = Path(__file__).resolve().parent.parent
credentials = project / 'src' / 'credentials.h'
if not credentials.is_file():
    sys.exit('请先复制 src/credentials.example.h 为 src/credentials.h，并填写自己的配置。')
if any(value in credentials.read_text(encoding='utf-8-sig') for value in
       ('YOUR_WIFI_SSID', 'YOUR_WIFI_PASSWORD', 'YOUR_CONTROL_TOKEN')):
    sys.exit('请先将 src/credentials.h 中的示例占位符替换为自己的设置。')

try:
    subprocess.run([sys.executable, '-m', 'platformio', 'run', '-e', 't2can-lane-change'], cwd=project, check=True)
except FileNotFoundError:
    sys.exit('未找到 Python 或 PlatformIO。')
except subprocess.CalledProcessError:
    sys.exit('PlatformIO 构建失败，请检查错误信息。')

build = project / '.pio' / 'build' / 't2can-lane-change'
try:
    system_info = subprocess.check_output(
        [sys.executable, '-m', 'platformio', 'system', 'info', '--json-output'], cwd=project)
    core_dir = Path(json.loads(system_info)['core_dir']['value'])
except (subprocess.CalledProcessError, KeyError, ValueError):
    sys.exit('无法获取 PlatformIO Core 目录，请检查 PlatformIO 安装。')
boot_app = core_dir / 'packages' / 'framework-arduinoespressif32' / 'tools' / 'partitions' / 'boot_app0.bin'
segments = [(0x0, build / 'bootloader.bin'), (0x8000, build / 'partitions.bin'),
            (0xe000, boot_app), (0x10000, build / 'firmware.bin')]
missing = [str(path) for _, path in segments if not path.is_file()]
if missing:
    sys.exit('缺少合并所需文件：' + ', '.join(missing) + '。请检查 PlatformIO 安装和 PLATFORMIO_CORE_DIR。')

output = build / 't2can-complete.bin'
command = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', 'merge_bin', '-o', str(output)]
for offset, path in segments:
    command.extend([hex(offset), str(path)])
try:
    subprocess.run(command, cwd=project, check=True)
except subprocess.CalledProcessError:
    sys.exit('镜像合并失败。请安装 esptool：python -m pip install esptool')
print('完整固件已生成：' + str(output))
print('该文件包含个人密码，请勿公开分享。')
