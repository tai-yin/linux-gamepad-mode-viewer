# gamepad-mode

Detect the USB protocol mode (HID / XInput / GIP / Switch / PlayStation) and
rumble capability of connected game controllers on Linux.

Zero dependencies — pure Python 3 standard library, read-only access to
`/proc` and `/sys` (no root required).

> 检测 Linux 上已连接游戏手柄的 USB 协议模式与振动能力。纯标准库、零依赖、无需 root。

## Why

Most gamepads support several modes that look identical in Steam (e.g. the
GameSir G7 Pro shows up as "Generic X-Box pad" in both GIP and XInput modes),
but only the Xbox-protocol modes expose force feedback (rumble). This tool tells
you which mode a controller is actually in, straight from the kernel's USB
descriptors — something input-level testers (`jstest`, `evtest`, SDL testers)
cannot see.

## Usage

```bash
./gamepad-mode.py            # one-shot report
./gamepad-mode.py -w         # live curses TUI (1 s refresh, hotplug-aware, q to quit)
./gamepad-mode.py --json     # JSON output for scripting
./gamepad-mode.py --no-color # disable ANSI colors (for logs / non-tty)
```

Example output:

```
#  名称                         VID:PID    连接   协议模式                驱动    振动
1  Generic X-Box pad          3537:100a  USB  XInput（Xbox 360）    xpad  有振动 ✅
2  Microsoft X-Box 360 pad 0  28de:11ff  虚拟   虚拟手柄（uinput/Steam）  -  有振动 ✅
```

## How it works

- Enumerates joystick devices from `/proc/bus/input/devices`.
- Resolves each device's USB interface
  `bInterfaceClass / bInterfaceSubClass / bInterfaceProtocol` and the bound
  driver from `/sys/devices/...` to classify the protocol mode:
  - `class ff / sub 47 / proto d0` → GIP (Xbox One / Xbox Gaming Device)
  - `class ff / sub 5d / proto 01` → XInput (Xbox 360)
  - `class 03` (HID) → generic HID (Android) — no rumble channel
- Bluetooth controllers are classified by driver name
  (`hid-nintendo`, `hid-sony`, `xpadneo`, ...).
- Rumble support is read from the force-feedback capability bitmap
  (`FF_RUMBLE`, bit `0x50`) instead of just "has an FF line".
- Keyboard knobs / dials that the kernel also exposes as `js*` devices are
  filtered out via their ABS axis set.

## Requirements

- Linux
- Python ≥ 3.6 (standard library only)

## License

MIT — see [LICENSE](LICENSE).
