# gamepad-mode

> [English version →](README.en.md)

检测 Linux 上已连接游戏手柄的 **USB 协议模式**（HID / XInput / GIP / Switch / PlayStation）与**振动能力**。

多数手柄在 Steam 里看起来都一样（例如盖世小鸡 G7 Pro 在 GIP 和 XInput 两种模式下都显示为
"Generic X-Box pad"），但**只有 Xbox 协议模式才有振动通道**。本工具直接读内核的 USB 描述符，
告诉你手柄当前到底跑在哪个模式——这是 `jstest`、`evtest`、SDL 测试器这类"输入层"工具看不到的。

## 快速开始（推荐：直接用可执行文件）

仓库自带编译好的二进制 **`c/gamepad-mode-gui`**（约 24KB），无需安装任何依赖，只需系统有
GTK3 运行库（绝大多数 Linux 桌面已自带）：

```bash
./c/gamepad-mode-gui          # GTK3 图形窗口，1s 自动刷新，支持热插拔，q 退出
./c/gamepad-mode-gui --text   # 纯文本一次性输出
./c/gamepad-mode-gui --json   # JSON 输出（可被脚本调用）
```

示例输出（`--text`）：

```
#  名称                         VID:PID    连接   协议模式                                驱动    振动
1  Generic X-Box pad          3537:109c  USB  GIP（Xbox One / Xbox Gaming Device）  xpad  有振动
2  Microsoft X-Box 360 pad 0  28de:11ff  虚拟   虚拟手柄（uinput/Steam）              -     有振动
```

## 从源码编译

需要 `gcc` + `pkg-config` + `gtk3-devel`（Fedora：`sudo dnf install gtk3-devel`）：

```bash
cd c
make            # 生成 gamepad-mode-gui
```

## Python 版（备选，无需编译）

纯 Python 3 标准库实现（零第三方依赖，无需 root），`--text` / `--json` 输出与 C 版一致：

```bash
./gamepad-mode.py            # 一次性报告
./gamepad-mode.py -w         # curses 实时界面（含热插拔检测）
./gamepad-mode.py --json     # JSON 输出
```

## 检测原理

- 从 `/proc/bus/input/devices` 枚举手柄设备（含 FF 能力位图）。
- 经 `/sys/devices/...` 读取 USB 接口的 `bInterfaceClass / SubClass / Protocol` 及绑定驱动，判定协议模式：
  - `class ff / sub 47 / proto d0` → **GIP**（Xbox One / Xbox Gaming Device）
  - `class ff / sub 5d / proto 01` → **XInput**（Xbox 360）
  - `class 03`（HID）→ 通用 HID（安卓）——**无振动通道**
- 蓝牙手柄按驱动名区分（`hid-nintendo` / `hid-sony` / `xpadneo` 等）。
- 振动能力解析力反馈位图里的 `FF_RUMBLE`（bit 0x50），比只看"有没有 FF 行"更准。
- 内核把键盘音量旋钮也暴露为 `js*` 设备，通过 ABS 轴集合过滤掉。

## 说明

- 连接列为「**虚拟**」的条目不是第二个实体设备，而是 **Steam Input** 用 uinput 生成的虚拟
  Xbox 360 手柄（`VID 28de` = Valve）：游戏与它交互，Steam 再把输入/振动转发到你的真实手柄。
  关闭该手柄的"启用 Steam 输入"后此条目消失。
- xpad 驱动对 GIP 协议的振动上限换算只有 50%、且不驱动扳机马达；XInput 模式是全幅。振动力度偏弱是驱动限制，非硬件故障。

## 依赖

- C 版：仅需系统 GTK3 运行库（二进制已编译好，无需其他）。
- Python 版：Python ≥ 3.6，标准库。

## 许可

MIT — 见 [LICENSE](LICENSE)。
