#!/usr/bin/env python3
"""检测已连接游戏手柄的 USB 协议模式（HID / XInput / GIP 等）与振动能力。

零第三方依赖（仅标准库）。无需 root（只读 /proc、/sys）。

用法:
    gamepad-mode.py            # 一次性打印报告
    gamepad-mode.py -w         # 进入 curses 实时界面（含热插拔检测）
    gamepad-mode.py --json     # 以 JSON 输出
"""

import argparse
import json
import os
import sys
import textwrap

INPUT_DEVICES = "/proc/bus/input/devices"

DRIVER_HINTS = {
    "hid-nintendo": ("Switch（HID）", "hid-nintendo 驱动，振动需额外工具"),
    "hid-sony": ("PlayStation（HID）", "DS4/DualSense"),
    "hid-playstation": ("PlayStation（HID）", "DS4/DualSense"),
    "xpadneo": ("Xbox（蓝牙）", "xpadneo 驱动"),
    "xpad": ("Xbox", "xpad 驱动"),
    "xone-dongle": ("Xbox（GIP）", "xone 驱动"),
    "hid-generic": ("HID（通用/安卓）", "无专属驱动，通常无振动"),
    "usbhid": ("HID（通用/安卓）", "无专属驱动，通常无振动"),
}


def _read(path):
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except (OSError, IOError):
        return None


def _readlink(path):
    try:
        return os.path.basename(os.path.realpath(path))
    except OSError:
        return None


def parse_input_devices():
    blocks = []
    cur = None
    with open(INPUT_DEVICES) as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("I: "):
                cur = {"I": line[3:]}
                blocks.append(cur)
                continue
            if cur is None:
                continue
            if line.startswith("B: "):
                key, val = line[3:].split("=", 1)
                cur.setdefault("B", {})[key] = val.split()
            elif ": " in line:
                k, v = line.split(": ", 1)
                for key in ("Name", "Phys", "Sysfs", "Handlers", "Uniq"):
                    if v.startswith(key + "="):
                        v = v[len(key) + 1:]
                        break
                cur[k] = v.strip()
    return blocks


def _has_bit(words, bit):
    total = 0
    for w in words:
        total = (total << 64) | int(w, 16)
    return (total >> bit) & 1


def _is_gamepad(blk):
    handlers = blk.get("H", "")
    if "js" not in handlers:
        return False
    b = blk.get("B", {})
    abs_words = b.get("ABS", [])
    if any(_has_bit(abs_words, c) for c in (2, 3, 4, 5, 16, 17)):
        return True
    ff = b.get("FF")
    if ff and any(int(w, 16) for w in ff):
        return True
    name = blk.get("N", "").lower()
    return any(k in name for k in ("pad", "gamepad", "controller", "joystick", "手柄"))


def _interface_attrs(sysfs_path):
    p = sysfs_path
    while p and p != "/":
        parent = os.path.dirname(p)
        c = os.path.join(parent, "bInterfaceClass")
        if os.path.exists(c):
            s = os.path.join(parent, "bInterfaceSubClass")
            pr = os.path.join(parent, "bInterfaceProtocol")
            drv = os.path.join(parent, "driver")
            return {
                "cls": _read(c),
                "sub": _read(s),
                "proto": _read(pr),
                "driver": _readlink(drv),
            }
        p = parent
    return {}


def _usb_device_attrs(sysfs_path):
    p = sysfs_path
    while p and p != "/":
        parent = os.path.dirname(p)
        if os.path.exists(os.path.join(parent, "idVendor")):
            return {
                "vendor": _read(os.path.join(parent, "idVendor")),
                "product": _read(os.path.join(parent, "idProduct")),
                "manufacturer": _read(os.path.join(parent, "manufacturer")),
                "dev_product": _read(os.path.join(parent, "product")),
            }
        p = parent
    return {}


def detect(blk):
    sysfs_path = blk.get("S", "")
    if sysfs_path.startswith("/devices"):
        sysfs_path = "/sys" + sysfs_path
    iface = _interface_attrs(sysfs_path)
    usb = _usb_device_attrs(sysfs_path)

    i_fields = blk["I"].split()
    bus = i_fields[0].split("=")[1] if i_fields else ""
    vendor = i_fields[1].split("=")[1] if len(i_fields) > 1 else ""
    product = i_fields[2].split("=")[1] if len(i_fields) > 2 else ""
    if not vendor and usb.get("vendor"):
        vendor = usb["vendor"]
    if not product and usb.get("product"):
        product = usb["product"]

    cls = iface.get("cls")
    sub = iface.get("sub")
    proto = iface.get("proto")
    driver = iface.get("driver") or ""

    b = blk.get("B", {})
    ff_words = b.get("FF", [])
    has_ff = bool(ff_words) and any(int(w, 16) for w in ff_words)
    has_rumble = has_ff and _has_bit(ff_words, 0x50) if ff_words else False

    transport = {"0003": "USB", "0005": "蓝牙", "0006": "蓝牙(uhid)"}.get(
        bus, "总线 " + bus if bus else "?"
    )
    is_virtual = "virtual" in (blk.get("S", "")) and not blk.get("P", "")
    if is_virtual:
        transport = "虚拟"

    if is_virtual:
        mode, note = "虚拟手柄（uinput/Steam）", "软件创建的虚拟设备"
    else:
        mode, note = _classify(bus, cls, sub, proto, driver)

    name = blk.get("N", "")
    if name.startswith('"') and name.endswith('"'):
        name = name[1:-1]

    return {
        "name": name,
        "vendor": vendor,
        "product": product,
        "transport": transport,
        "mode": mode,
        "driver": driver,
        "rumble": bool(has_rumble),
        "ff": has_ff,
        "note": note,
        "phys": blk.get("P", ""),
        "handlers": blk.get("H", ""),
        "vendor_name": usb.get("manufacturer", ""),
        "dev_product": usb.get("dev_product", ""),
    }


def _classify(bus, cls, sub, proto, driver):
    if bus == "0005" or bus == "0006":
        label, note = DRIVER_HINTS.get(driver, ("HID（蓝牙）", ""))
        return label, note
    if cls == "03":
        label, note = DRIVER_HINTS.get(driver, ("HID（通用/安卓）", "无专属驱动"))
        return label, note
    if cls == "ff":
        if sub == "47" and proto == "d0":
            return "GIP（Xbox One / Xbox Gaming Device）", "xpad/xone 驱动"
        if sub == "5d" and proto == "01":
            return "XInput（Xbox 360）", "xpad 驱动，回报率 1000Hz 推荐"
        return "Xbox（厂商协议）", f"class ff sub {sub} proto {proto}"
    return f"其他（class {cls} sub {sub} proto {proto}）", ""


def _rumble_label(info):
    if info["rumble"]:
        return "有振动 ✅"
    if info["ff"]:
        return "有 FF（无 RUMBLE）⚠️"
    return "无振动 ❌"


def report():
    infos = [detect(b) for b in parse_input_devices() if _is_gamepad(b)]
    infos.sort(key=lambda x: x["transport"] == "虚拟")
    return infos


LEGEND = ("提示：连接列为「虚拟」的条目是 Steam Input 用 uinput 生成的虚拟手柄"
          "（VID 28de=Valve），游戏通过它转发输入/振动到真实手柄；"
          "在 Steam 里关掉「启用 Steam 输入」后该条目会消失。")


def render_text(infos, color=True):
    if not infos:
        return "未检测到游戏手柄（检查 /proc/bus/input/devices）"
    lines = []
    widths = {}
    rows = []
    for i, info in enumerate(infos, 1):
        vidpid = f"{info['vendor']}:{info['product']}" if info["vendor"] else "?"
        rows.append([str(i), info["name"], vidpid, info["transport"],
                     info["mode"], info["driver"] or "-", _rumble_label(info)])
    headers = ["#", "名称", "VID:PID", "连接", "协议模式", "驱动", "振动"]
    for c in range(len(headers)):
        widths[c] = max(len(headers[c]), max(len(r[c]) for r in rows))
    fmt = "  ".join("{%d:<%d}" % (c, widths[c]) for c in range(len(headers)))
    lines.append(fmt.format(*headers))
    for r in rows:
        lines.append(fmt.format(*r))
    if color:
        out = []
        for ln in lines:
            if "无振动" in ln:
                ln = "\033[31m" + ln + "\033[0m"
            elif "有振动" in ln:
                ln = "\033[32m" + ln + "\033[0m"
            out.append(ln)
        lines = out
    try:
        term_width = os.get_terminal_size().columns
    except OSError:
        term_width = 80
    legend = textwrap.fill(LEGEND, width=max(20, min(term_width, 100)))
    return "\n".join(lines) + "\n\n" + legend


def watch_tui():
    import curses
    import time

    def main(stdscr):
        curses.curs_set(0)
        stdscr.nodelay(True)
        try:
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_GREEN, -1)
            curses.init_pair(2, curses.COLOR_RED, -1)
            curses.init_pair(3, curses.COLOR_CYAN, -1)
        except curses.error:
            pass
        while True:
            stdscr.erase()
            h, w = stdscr.getmaxyx()
            infos = report()
            footer = textwrap.wrap(LEGEND, width=max(20, w - 1)) if w > 1 else [LEGEND]
            nfooter = len(footer)
            stdscr.addstr(0, 0, "游戏手柄模式检测  (q 退出)", curses.A_BOLD)
            try:
                stdscr.addstr(1, 3, "名称", curses.A_UNDERLINE)
                stdscr.addstr(1, 44, "连接", curses.A_UNDERLINE)
                stdscr.addstr(1, 51, "协议模式", curses.A_UNDERLINE)
                stdscr.addstr(1, 84, "VID:PID", curses.A_UNDERLINE)
                stdscr.addstr(1, 97, "振动", curses.A_UNDERLINE)
            except curses.error:
                pass
            if not infos:
                stdscr.addstr(3, 0, "未检测到游戏手柄，正在等待插入……")
            y = 3
            limit = max(3, h - nfooter)
            for i, info in enumerate(infos, 1):
                if y >= limit:
                    break
                vidpid = f"{info['vendor']}:{info['product']}" if info["vendor"] else "?"
                try:
                    stdscr.addstr(y, 0, f"{i}. ", curses.color_pair(3))
                    stdscr.addstr(y, 3, info["name"][:40])
                    stdscr.addstr(y, 44, f"{info['transport']:<6}")
                    stdscr.addstr(y, 51, info["mode"][:32])
                    stdscr.addstr(y, 84, f"{vidpid:<12}")
                    rumble = _rumble_label(info)
                    attr = curses.color_pair(1) if info["rumble"] else curses.color_pair(2)
                    stdscr.addstr(y, 97, rumble, attr)
                    if info["driver"]:
                        stdscr.addstr(y, 108, f"[{info['driver']}]", curses.A_DIM)
                except curses.error:
                    pass
                y += 1
            for j, ln in enumerate(footer):
                fy = h - nfooter + j
                if fy < 0:
                    continue
                try:
                    stdscr.addstr(fy, 0, ln, curses.A_DIM)
                except curses.error:
                    pass
            stdscr.refresh()
            try:
                c = stdscr.getch()
            except curses.error:
                c = -1
            if c in (ord("q"), ord("Q"), 27):
                break
            time.sleep(0.8)

    curses.wrapper(main)


def main():
    ap = argparse.ArgumentParser(description="检测游戏手柄协议模式与振动能力")
    ap.add_argument("-w", "--watch", action="store_true", help="进入实时 TUI 界面")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--no-color", action="store_true", help="禁用颜色")
    args = ap.parse_args()

    if args.watch:
        watch_tui()
        return

    infos = report()
    if args.json:
        print(json.dumps(infos, ensure_ascii=False, indent=2))
        return
    use_color = not args.no_color and sys.stdout.isatty()
    print(render_text(infos, color=use_color))


if __name__ == "__main__":
    main()
