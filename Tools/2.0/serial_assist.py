#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
================================================================================
 STM32 OTA 串口助手 2.0
--------------------------------------------------------------------------------
 2.0 相对 1.0 的变化：
   · 双通道：同时打开两个串口，左右两个接收区各收各的
   · 每个通道独立：端口 / 波特率 / 显示模式 / 时间戳 / 自动滚动 / 清空
   · 每个通道一套 SWD 复位：各自的「复位目标」下拉 + 复位/唤醒按钮
   · 日志颜色规则只有一份 —— 设置里改一次，两个接收区同时生效
   · 固件发送和文本发送各有「发送到」下拉，单选一路（不广播）

 原有能力保持不变：
   1. 固定 UTF-8 解码     —— 日志里的中文不会再变 "寮曡剼"
   2. 固件分块发送        —— 块大小 / 块间延时 / 实时进度 / 随时中止
   3. 自动解析 BootLoader —— 抓 [ota] page / tail 的 sum 算真实落盘进度
   4. 字节数自动对账      —— 文件大小 vs BootLoader 回报的 sum
   5. 日志按来源着色      —— 规则表驱动，可增删改、可调优先级

 依赖：pyserial（C:\Python311 已自带 3.5）
 运行：双击 run_serial_assist.bat
       或  C:\Python311\python.exe serial_assist.py
================================================================================
"""

import codecs
import glob
import json
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import zlib
from tkinter import colorchooser, filedialog, messagebox, ttk

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    # --noconsole 打包后 sys.stderr 是 None，直接 write 会抛
    # AttributeError 把真正的报错盖掉，所以先判空。
    _err = ("缺少 pyserial。请执行：\n"
            "    C:\\Python311\\python.exe -m pip install pyserial\n")
    if sys.stderr is not None:
        sys.stderr.write(_err)
    raise


# ==============================================================================
# 常量
# ==============================================================================

APP_TITLE = "STM32 OTA 串口助手"
APP_VER = "2.1"

# 接收区可选的文本编码（HEX 是单独模式，不走解码器）
# UTF-16 带 BOM 自动识别；UTF-16LE/BE 用于没有 BOM 的裸流。
DISPLAY_MODES = {
    "UTF-8":    "utf-8",
    "ASCII":    "ascii",
    "GBK":      "gbk",
    "GB18030":  "gb18030",
    "Big5":     "big5",
    "UTF-16":   "utf-16",
    "UTF-16LE": "utf-16-le",
    "UTF-16BE": "utf-16-be",
}
DISPLAY_OPTIONS = list(DISPLAY_MODES) + ["HEX"]

BAUD_LIST = ["9600", "19200", "38400", "57600", "115200",
             "230400", "460800", "921600"]

# 配色（浅色系，长时间看不刺眼）
C_BG      = "#F4F6F9"   # 窗口底色
C_PANEL   = "#FFFFFF"   # 面板底色
C_BORDER  = "#D9DEE6"
C_TEXT    = "#1F2733"
C_SUB     = "#6C7683"
C_ACCENT  = "#2563EB"
C_ACCENT2 = "#1D4ED8"
C_OK      = "#0F9D58"
C_WARN    = "#C77700"
C_BAD     = "#D93025"
C_BOOT    = "#7C3AED"
C_OTA     = "#0E7490"

FONT_UI   = "Microsoft YaHei UI"
FONT_MONO = "Consolas"

# 接收区着色由「规则表」驱动 —— 每条规则 = 一组关键词 + 一组颜色。
# 换工程时只要改规则（或整个拷走 assist_config.json），不用改代码、不用重打包。
#
# 规则字段：
#   tag       Tk text tag 名，也作字典键；默认六条的 tag 固定，自定义用 custom1...
#   name      界面上显示的规则名
#   pattern   关键词，用 | 分隔多个；命中任意一个即算匹配（子串、区分大小写）
#   fg / bg   前景/背景色，bg 为空串表示不设背景
# 匹配顺序 = 列表顺序，首命中即停。所以 FAIL 默认排在 ok 之前 ——
# 否则 "[ota] verify FAIL" 会先被 ok 规则染绿，失败就不显眼了。
# 顺序由用户用 ↑↓ 自行调整，代码不强制任何规则的位置。
# 2.0：这张规则表由 A/B 两个接收区共用，改一次两边同时生效。
DEFAULT_LOG_RULES = [
    {"tag": "bad",  "name": "FAIL / ERROR", "pattern": "FAIL|ERROR|error",
     "fg": C_BAD,  "bg": "#FDECEA"},
    {"tag": "boot", "name": "[boot]",       "pattern": "[boot]",
     "fg": C_BOOT, "bg": ""},
    {"tag": "ota",  "name": "[ota]",        "pattern": "[ota]",
     "fg": C_OTA,  "bg": ""},
    {"tag": "rst",  "name": "[rst]",        "pattern": "[rst]",
     "fg": C_WARN, "bg": ""},
    {"tag": "good", "name": "ok / OK",      "pattern": " ok|OK",
     "fg": C_OK,   "bg": ""},
    # sys 默认不参与文本匹配：工具提示行和 HEX 行直接挂这个 tag。
    {"tag": "sys",  "name": "系统提示",     "pattern": "",
     "fg": C_SUB,  "bg": ""},
]
HEX_COLOR_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")


def default_log_rules():
    """返回默认规则表的深拷贝。每次调用都新建，避免界面改动污染常量。"""
    return [dict(r) for r in DEFAULT_LOG_RULES]


def load_log_rules(cfg):
    """从配置读规则表；缺项/坏值回退默认色，绝不把非法颜色交给 Tk。

    兼容上一版的 log_colors 字典 —— 老用户升级后原来改过的六种颜色不会丢；
    新的 log_rules 优先（存在且是 list 时就以它为准）。
    """
    defaults = default_log_rules()
    raw = cfg.get("log_rules")
    if isinstance(raw, list):
        # 配置保存的是「完整规则表」，加载以配置为唯一来源。
        # 不补齐缺失的默认规则 —— 用户要求全部可删，
        # 若这里把删掉的补回来，删除就形同虚设（关窗重开规则又冒出来）。
        rules = []
        seen = set()
        for item in raw:
            if not isinstance(item, dict):
                continue
            tag = item.get("tag")
            if not isinstance(tag, str) or not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", tag):
                continue                    # 非法 tag 名会让 Tk 抛异常
            if tag in seen or tag == "sel":  # sel 是 Tk 内置选区标签，不能占用
                continue
            seen.add(tag)
            fg = item.get("fg")
            bg = item.get("bg")
            rules.append({
                "tag": tag,
                "name": str(item.get("name") or tag),
                "pattern": str(item.get("pattern") or ""),
                "fg": fg if isinstance(fg, str) and HEX_COLOR_RE.match(fg) else C_TEXT,
                "bg": bg if isinstance(bg, str) and HEX_COLOR_RE.match(bg) else "",
            })
        return rules
    # 没有 log_rules：兼容上一版的 log_colors 字典，老用户改过的颜色不丢
    colors = cfg.get("log_colors")
    if isinstance(colors, dict):
        for r in defaults:
            v = colors.get(r["tag"])
            if isinstance(v, str) and HEX_COLOR_RE.match(v):
                r["fg"] = v
    return defaults


# 配置（串口、波特率、块大小这些记下来，下次打开不用重选）
#
# 打包成 exe 后 __file__ 指向 PyInstaller 的临时解包目录
# （%TEMP%\_MEIxxxx），程序一退出整个目录就被删掉，配置文件跟着丢。
# 所以这里要区分「源码运行」和「exe 运行」两种情形：
#   - 源码：脚本所在目录
#   - exe ：exe 自身所在目录（用户双击的那个文件夹）
def app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


CONFIG_PATH = os.path.join(app_dir(), "assist_config.json")


def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


# ==============================================================================
# BootLoader 日志解析规则
#   全部用 search（不是 match），因为一行前面可能带别的东西，
#   例如 "发送完毕！[ota] page 0x0800B000 ok, sum: 14336"
#   2.0：解析器按通道各建一份状态，互不干扰（两块板同时升级也不会串）。
# ==============================================================================

RE_READY = re.compile(rb"\[ota\]\s+ready,\s*app=(0x[0-9A-Fa-f]+),\s*page=(\d+)B"
                      rb"(?:\s*,\s*proto=(\d+))?")
RE_PAGE  = re.compile(rb"\[ota\]\s+page\s+(0x[0-9A-Fa-f]+)\s+ok,\s*sum:\s*(\d+)")
RE_TAIL  = re.compile(rb"\[ota\]\s+tail\s+(0x[0-9A-Fa-f]+)\s+(\d+)B\s+ok,\s*sum:\s*(\d+)")
# proto=2 才开始有"尾部记录 + CRC 校验"，老版本没有这一段
RE_TAILREC = re.compile(rb"\[ota\]\s+tail:\s*len=(\d+)\s+crc=0x([0-9A-Fa-f]+)")
RE_VOK     = re.compile(rb"\[ota\]\s+verify\s+OK\s*(\d+)B")
RE_VBAD    = re.compile(rb"\[ota\]\s+verify\s+FAIL")
RE_SIZEBAD = re.compile(rb"\[ota\]\s+size\s+mismatch")


def list_serial_ports():
    """返回 [(device, 显示文本, 是否真实USB串口), ...]

    要判断"哪个是真的插着的设备"，只能看 hwid：
    蓝牙串口的 hwid 形如 BTHENUM\\{...}，是系统虚拟出来的，
    一台机器上能有七八个，而且往往按 COM 号排在前面。
    真实 USB 转串口的 hwid 里带 "USB VID:PID=xxxx:xxxx"。
    只按描述里的 CH340 / CP210 关键字判断是不可靠的 ——
    很多板子用的是通用的 "USB 串行设备" 描述。
    """
    out = []
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").strip()
        hwid = p.hwid or ""
        low = desc.lower()
        is_bluetooth = ("BTHENUM" in hwid.upper() or "蓝牙" in desc
                        or "bluetooth" in low)
        is_usb = ("USB VID" in hwid.upper() or "USB\\VID" in hwid.upper())

        if desc and low not in ("n/a", "none"):
            label = "{}  {}".format(p.device, desc)
        else:
            label = p.device

        out.append((p.device, label, is_usb and not is_bluetooth))

    # 真实 USB 串口排最前，蓝牙虚拟口排最后
    out.sort(key=lambda x: (not x[2], x[0]))
    return out


def human_bytes(n):
    """1024 -> '1.0 KB'"""
    n = float(n)
    if n < 1024:
        return "{:.0f} B".format(n)
    if n < 1024 * 1024:
        return "{:.1f} KB".format(n / 1024.0)
    return "{:.2f} MB".format(n / (1024.0 * 1024.0))


def make_check(parent, text, var, bg=None):
    """普通 Tk 复选框。

    不用 ttk 的 Card.TCheckbutton：clam 主题下选中标记会渲染成一个 ✕，
    看着像「关闭」，容易让人误判当前状态。
    """
    bg = bg or C_PANEL
    return tk.Checkbutton(
        parent, text=text, variable=var,
        bg=bg, fg=C_TEXT, activebackground=bg,
        activeforeground=C_TEXT, selectcolor="#FFFFFF",
        font=(FONT_UI, 9), bd=0, highlightthickness=0,
        anchor="w", cursor="hand2")


def fit_dropdown(cb, labels, min_chars=15):
    """让 combobox 的弹出列表按最长一项展开（收起时控件宽度不变）。

    ttk 的下拉列表默认跟控件同宽，COM 口描述一长就被截成
    "USB-SERIAL ..." / "蓝牙链接上的..."，看不出是哪个设备。
    这里先改弹出 Listbox 的 -width（按像素量最长行再折算字符数，
    中文按实际像素算）。注意光改 Listbox 还不够 —— ttk 在弹出时
    会把整个 popdown 窗口的几何强制设成输入框宽度，所以还要配合
    widen_popdown() 在弹出后把窗口本身再撑开一次。
    """
    try:
        popdown = str(cb.tk.call("ttk::combobox::PopdownWindow", str(cb)))
        lb = popdown + ".f.l"
        from tkinter import font as tkfont
        try:
            f = tkfont.Font(root=cb, font=cb.tk.call(lb, "cget", "-font"))
        except Exception:
            f = tkfont.nametofont("TkDefaultFont")
        px = max([f.measure(str(x)) for x in labels] or [0])
        unit = max(f.measure("0"), 1)
        cb.tk.call(lb, "configure", "-width",
                   max(min_chars, px // unit + 2))
    except Exception:
        pass


def widen_popdown(cb):
    """把已经弹出的下拉窗口按内容宽度重新撑开一次。

    ttk::combobox::PlacePopdown 里有一句
        wm geometry $popdown ${w}x${H}+${x}+${Y}
    w 取的是 combobox 自身宽度，所以展开列表永远和输入框一样宽，
    长设备名必被截断。这里在弹出之后按 popdown 内 frame 的
    请求宽度重设几何；超出屏幕右侧时往左挪一下，别跑到屏幕外。
    """
    try:
        pop = str(cb.tk.call("ttk::combobox::PopdownWindow", str(cb)))
        if not int(cb.tk.call("winfo", "ismapped", pop)):
            return
        need = int(cb.tk.call("winfo", "reqwidth", pop + ".f")) + 4
        geo = str(cb.tk.call("wm", "geometry", pop))     # WxH+X+Y
        wh, xs, ys = geo.split("+", 2)
        h = int(wh.split("x")[1])
        x, y = int(xs), int(ys)
        sw = int(cb.tk.call("winfo", "screenwidth", pop))
        if x + need > sw:
            x = max(sw - need, 0)
        cb.tk.call("wm", "geometry", pop,
                   "{}x{}+{}+{}".format(need, h, x, y))
    except Exception:
        pass


# ------------------------------------------------------------------ 调试器复位
# 跑 SWD 复位的不一定是 DAP-Link：ST-Link、J-Link 都能干同一件事，
# 区别只在 OpenOCD 要加载哪个 interface 脚本。下面这张表按 USB 的
# VID/PID 认设备，顺序就是优先级（同时插多个时用排在前面的）。
DEBUGGERS = [
    ("VID_1FC9&PID_5601", "cmsis-dap", "DAP-Link"),
    ("VID_1FC9&PID_0132", "cmsis-dap", "DAP-Link"),
    ("VID_0D28&PID_0204", "cmsis-dap", "DAPLink"),
    ("VID_0483&PID_3744", "stlink",    "ST-Link V1"),
    ("VID_0483&PID_3748", "stlink",    "ST-Link V2"),
    ("VID_0483&PID_374B", "stlink",    "ST-Link V2-1"),
    ("VID_0483&PID_374D", "stlink",    "ST-Link V3E"),
    ("VID_0483&PID_374E", "stlink",    "ST-Link V3"),
    ("VID_0483&PID_374F", "stlink",    "ST-Link V3"),
    ("VID_1366&PID_0100", "jlink",     "J-Link"),
    ("VID_1366&PID_0101", "jlink",     "J-Link"),
    ("VID_1366&PID_0105", "jlink",     "J-Link"),
]


def detect_debuggers():
    """枚举此刻插着的**全部**调试器，返回
    [{'iface','name','serial','vidpid'}, ...]，按 DEBUGGERS 里的优先级排序。

    序列号取自 Windows devnode id 的最后一段：
        USB\\VID_0483&PID_374B\\AE5A5BB50000000000078851   → AE5A5BB5...
    复合设备的**接口**节点长这样：
        USB\\VID_0483&PID_374B&MI_00\\8&12225435&0&0000
    末段是总线拓扑编号（同一台设备的几个接口各不同、重插还会变），拿它
    当序列号既认不准也会重复，所以必须跳过带 &MI_ 的节点。

    只认「此刻真的连着」的设备：用 CFGMGR32 的 PRESENT 过滤，不用注册表
    Enum\\USB（拔掉的键还留着，会认成旧设备）。
    """
    if os.name != "nt":
        return []
    try:
        import ctypes
        from ctypes import wintypes

        cm = ctypes.windll.cfgmgr32
        CM_GETIDLIST_FILTER_PRESENT = 0x00000100

        size = wintypes.ULONG(0)
        if cm.CM_Get_Device_ID_List_SizeW(
                ctypes.byref(size), None,
                CM_GETIDLIST_FILTER_PRESENT) != 0:
            return []
        buf = ctypes.create_unicode_buffer(size.value)
        if cm.CM_Get_Device_ID_ListW(
                None, buf, size.value,
                CM_GETIDLIST_FILTER_PRESENT) != 0:
            return []
        raw = bytes(buf).decode("utf-16-le", "ignore")
    except Exception:
        return []

    out = []
    seen = set()
    for devid in raw.split("\x00"):
        if not devid:
            continue
        up = devid.upper()
        if "&MI_" in up:
            continue                    # 复合设备的接口节点，末段不是真序列号
        for prio, (vidpid, iface, label) in enumerate(DEBUGGERS):
            if vidpid not in up:
                continue
            serial = devid.split("\\")[-1]
            key = (iface, serial.upper())
            if key in seen:             # 同一设备可能有两个设备节点，去重
                break
            seen.add(key)
            out.append({"iface": iface, "name": label,
                        "serial": serial, "vidpid": vidpid, "prio": prio})
            break
    # 排序让"第一个"与旧行为一致：优先 DEBUGGERS 表里的顺序
    out.sort(key=lambda d: (d["prio"], d["serial"].upper()))
    return out


def _openocd_scripts(exe):
    """由 openocd.exe 位置推出 scripts 目录；推不出返回 None。"""
    root = os.path.dirname(os.path.dirname(exe))
    scr = os.path.join(root, "share", "openocd", "scripts")
    if os.path.isdir(os.path.join(scr, "target")):
        return scr
    return None


def find_openocd(cfg=None, base_dir=None):
    """返回 (openocd.exe 绝对路径, scripts 目录)；找不到返回 (None, None)

    换一台电脑时 OpenOCD 不一定还在 EIDE 的目录里，所以按优先级多找几处：
      1. 配置项 openocd_path（可以是 exe，也可以是 bin 目录）
         / openocd_scripts（单独指定 scripts）
      2. 环境变量 OPENOCD_HOME / OPENOCD_SCRIPTS
      3. 程序旁边的 openocd\\bin\\openocd.exe（绿色版：整个工具链一起拷）
      4. EIDE 工具链 ~/.eide/tools/openocd*/bin
      5. PATH 里的 openocd（choco / scoop / 手动加环境变量都能命中）
      6. xPack 安装位置
      7. Program Files / C:\\OpenOCD
    """
    cfg = cfg or {}
    found = []

    def add_exe(path):
        if not path:
            return
        path = os.path.expanduser(str(path).strip().strip('"'))
        if os.path.isfile(path) and os.path.basename(path).lower().startswith(
                "openocd"):
            found.append(path)

    # 1. 配置项
    p = str(cfg.get("openocd_path") or "").strip().strip('"')
    if p:
        p = os.path.expanduser(p)
        if os.path.isdir(p):
            add_exe(os.path.join(p, "openocd.exe"))
            add_exe(os.path.join(p, "openocd"))
            add_exe(os.path.join(p, "bin", "openocd.exe"))
            add_exe(os.path.join(p, "bin", "openocd"))
        else:
            add_exe(p)

    # 2. 环境变量
    home = os.environ.get("OPENOCD_HOME", "")
    if home:
        add_exe(os.path.join(home, "bin", "openocd.exe"))
        add_exe(os.path.join(home, "bin", "openocd"))

    # 3. 程序同目录的 openocd\bin（拷 exe 时连同工具链一起拷）
    for base in filter(None, (base_dir, os.path.dirname(
            os.path.abspath(__file__)))):
        add_exe(os.path.join(base, "openocd", "bin", "openocd.exe"))
        add_exe(os.path.join(base, "openocd", "bin", "openocd"))

    # 4~7. 常见安装位置
    patterns = [
        os.path.expanduser("~/.eide/tools/openocd*/bin/openocd.exe"),
        os.path.expanduser("~/.eide/tools/openocd*/bin/openocd"),
        os.path.expanduser(
            "~/AppData/Roaming/xPacks/@xpack-dev-tools/openocd/"
            "*/.content/bin/openocd.exe"),
        os.path.expanduser(
            "~/.local/xPacks/@xpack-dev-tools/openocd/"
            "*/.content/bin/openocd.exe"),
        "C:/Program Files/OpenOCD*/bin/openocd.exe",
        "C:/Program Files (x86)/OpenOCD*/bin/openocd.exe",
        "C:/OpenOCD*/bin/openocd.exe",
        "/usr/bin/openocd",
        "/usr/local/bin/openocd",
    ]
    for pat in patterns:
        for exe in sorted(glob.glob(pat), reverse=True):
            add_exe(exe)

    # 5. PATH
    add_exe(shutil.which("openocd"))

    # scripts 目录：优先配置/环境变量，其次由 exe 位置推导
    extra_scripts = [str(cfg.get("openocd_scripts") or "").strip().strip('"'),
                     os.environ.get("OPENOCD_SCRIPTS", "").strip().strip('"')]
    for exe in found:
        for scr in extra_scripts:
            if scr:
                scr = os.path.expanduser(scr)
                if os.path.isdir(os.path.join(scr, "target")):
                    return exe, scr
        scr = _openocd_scripts(exe)
        if scr:
            return exe, scr
    return None, None


def openocd_reset(exe, scr, interface="cmsis-dap", target="stm32f1x",
                  timeout=25, serial=""):
    """让调试器复位目标板并放开运行。返回 (成功?, 说明文本)

    serial 非空时用 `adapter serial` 锁定到那个调试器实例。
    同时插两块板时，OpenOCD 默认挑第一个匹配的调试器，
    两个通道会一起复位同一块板，另一块永远不动。

    注意 `adapter serial` 必须放在 -f interface/*.cfg **之后**（那里才有
    adapter driver）、init **之前**；顺序错了会报 unknown command。
    已实测：填一个不存在的序列号，DAP-Link 报 "unable to find a matching
    CMSIS-DAP device"、ST-Link 报 "open failed"，说明该参数确实在起过滤作用。
    """
    icfg = os.path.join(scr, "interface", interface + ".cfg")
    tcfg = os.path.join(scr, "target", target + ".cfg")
    if not os.path.isfile(icfg):
        return False, "找不到接口配置：{}".format(icfg)
    if not os.path.isfile(tcfg):
        return False, "找不到芯片配置：{}".format(tcfg)

    cmd = [exe, "-f", icfg, "-f", tcfg]
    if serial:
        cmd += ["-c", "adapter serial {}".format(serial)]
    cmd += ["-c", "init; reset run; shutdown"]
    # Windows 下别弹出黑色控制台窗口
    flags = 0x08000000 if os.name == "nt" else 0
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
    except subprocess.TimeoutExpired:
        return False, "OpenOCD 超时（{}s），调试器可能没插或正被占用".format(timeout)
    except FileNotFoundError:
        return False, "OpenOCD 可执行文件不存在"
    except TypeError:
        return False, "OpenOCD 路径无效（可能没找到 OpenOCD）"

    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        brief = [l for l in out.splitlines()
                 if "Error" in l or "error" in l][-3:]
        return False, "OpenOCD 失败（exit {}）：{}".format(
            r.returncode, " | ".join(brief) or (out.splitlines() or [""])[-1])
    if "SWD" not in out and "JTAG" not in out and "DPIDR" not in out:
        return False, "调试器未响应：{}".format(
            (out.splitlines() or ["无输出"])[-1])
    return True, "调试器复位完成"


def find_jlink(cfg=None):
    """返回 JLink.exe 路径；找不到返回 None。

    没装 OpenOCD 时的兜底：J-Link 官方驱动自带 JLink.exe 就能复位。
    """
    cfg = cfg or {}
    p = str(cfg.get("jlink_path") or "").strip().strip('"')
    if p:
        p = os.path.expanduser(p)
        if os.path.isdir(p):
            p = os.path.join(p, "JLink.exe")
        if os.path.isfile(p):
            return p

    pats = [
        "C:/Program Files/SEGGER/JLink*/JLink.exe",
        "C:/Program Files (x86)/SEGGER/JLink*/JLink.exe",
        os.path.expanduser("~/AppData/Local/SEGGER/JLink*/JLink.exe"),
    ]
    for pat in pats:
        hits = [x for x in glob.glob(pat) if os.path.isfile(x)]
        if hits:
            return sorted(hits, reverse=True)[0]
    return shutil.which("JLink") or shutil.which("JLink.exe")


def find_stlink_cli(cfg=None):
    """返回 ST-LINK_CLI.exe 路径；找不到返回 None。"""
    cfg = cfg or {}
    p = str(cfg.get("stlink_path") or "").strip().strip('"')
    if p:
        p = os.path.expanduser(p)
        if os.path.isdir(p):
            p = os.path.join(p, "ST-LINK_CLI.exe")
        if os.path.isfile(p):
            return p

    pats = [
        "C:/Program Files (x86)/STMicroelectronics/STM32 ST-LINK Utility/"
        "ST-LINK Utility/ST-LINK_CLI.exe",
        "C:/Program Files/STMicroelectronics/STM32 ST-LINK Utility/"
        "ST-LINK Utility/ST-LINK_CLI.exe",
    ]
    for pat in pats:
        if os.path.isfile(pat):
            return pat
    return None


def find_cubeprog(cfg=None):
    """返回 STM32_Programmer_CLI.exe 路径；找不到返回 None。

    ST-Link 的第二条兜底：CubeMX / CubeIDE / CubeCLT 一般都会装它，
    不依赖已经停更的 ST-LINK Utility。
    """
    cfg = cfg or {}
    p = str(cfg.get("cubeprog_path") or "").strip().strip('"')
    if p:
        p = os.path.expanduser(p)
        if os.path.isdir(p):
            p = os.path.join(p, "STM32_Programmer_CLI.exe")
        if os.path.isfile(p):
            return p

    pats = [
        "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/"
        "bin/STM32_Programmer_CLI.exe",
        "C:/Program Files (x86)/STMicroelectronics/STM32Cube/"
        "STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe",
        "C:/Program Files/STMicroelectronics/STM32CubeCLT*/STM32CubeProgrammer/"
        "bin/STM32_Programmer_CLI.exe",
        "C:/ST/STM32CubeCLT*/STM32CubeProgrammer/bin/"
        "STM32_Programmer_CLI.exe",
        "C:/ST/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe",
    ]
    for pat in pats:
        hits = [x for x in glob.glob(pat) if os.path.isfile(x)]
        if hits:
            return sorted(hits, reverse=True)[0]
    return shutil.which("STM32_Programmer_CLI")


def jlink_reset(exe, serial="", device="STM32F103C8", speed="4000",
                timeout=25):
    """用 JLink.exe 复位并放开运行，不需要 OpenOCD。

    通过 -CommanderScript 让 J-Link Commander 执行
        r    复位（停在复位向量）
        g    放开运行
        qc   退出并关闭连接
    返回 (成功?, 说明文本)。
    """
    path = None
    try:
        fd, path = tempfile.mkstemp(suffix=".jlink", text=True)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("r\ng\nqc\n")
        cmd = [exe, "-NoGui", "1", "-Device", str(device), "-If", "SWD",
               "-Speed", str(speed), "-AutoConnect", "1"]
        if serial:
            cmd += ["-SelectEmuBySN", str(serial)]
        cmd += ["-CommanderScript", path]
        flags = 0x08000000 if os.name == "nt" else 0
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
        out = (r.stdout or "") + (r.stderr or "")
        low = out.lower()
        bad = ("cannot connect", "could not", "no emulator",
               "failed to", "unknown device")
        if r.returncode != 0 or any(b in low for b in bad):
            brief = [l for l in out.splitlines()
                     if "error" in l.lower() or "fail" in l.lower()][-3:]
            return False, "J-Link 复位失败：{}".format(
                " | ".join(brief) or (out.splitlines() or ["无输出"])[-1])
        return True, "J-Link 复位完成（{}）".format(device)
    except subprocess.TimeoutExpired:
        return False, "JLink.exe 超时（{}s），调试器可能没插或正被占用".format(
            timeout)
    except FileNotFoundError:
        return False, "JLink.exe 不存在"
    except Exception as e:
        return False, "J-Link 复位异常：{}".format(e)
    finally:
        if path:
            try:
                os.unlink(path)
            except OSError:
                pass


def stlink_reset(exe, serial="", timeout=25):
    """用 ST-LINK_CLI.exe -Rst 复位并放开运行，不需要 OpenOCD。"""
    cmd = [exe, "-c", "SWD"]
    if serial:
        cmd += ["-S", str(serial)]
    cmd += ["-Rst"]
    flags = 0x08000000 if os.name == "nt" else 0
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
    except subprocess.TimeoutExpired:
        return False, "ST-LINK_CLI 超时（{}s）".format(timeout)
    except FileNotFoundError:
        return False, "ST-LINK_CLI.exe 不存在"
    except Exception as e:
        return False, "ST-LINK 复位异常：{}".format(e)

    out = (r.stdout or "") + (r.stderr or "")
    low = out.lower()
    if r.returncode != 0 or "no st-link" in low or "can not connect" in low:
        brief = [l for l in out.splitlines()
                 if "error" in l.lower() or "fail" in l.lower()][-3:]
        return False, "ST-LINK 复位失败：{}".format(
            " | ".join(brief) or (out.splitlines() or ["无输出"])[-1])
    return True, "ST-LINK 复位完成"


def cubeprog_reset(exe, serial="", timeout=25):
    """用 STM32_Programmer_CLI.exe -c port=SWD -rst 复位并放开运行。"""
    conn = "port=SWD"
    if serial:
        conn += " sn={}".format(serial)
    cmd = [exe, "-c", conn, "-rst"]
    flags = 0x08000000 if os.name == "nt" else 0
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
    except subprocess.TimeoutExpired:
        return False, "STM32_Programmer_CLI 超时（{}s）".format(timeout)
    except FileNotFoundError:
        return False, "STM32_Programmer_CLI.exe 不存在"
    except Exception as e:
        return False, "CubeProgrammer 复位异常：{}".format(e)

    out = (r.stdout or "") + (r.stderr or "")
    low = out.lower()
    bad = ("error", "no st-link", "not detected", "failed")
    if r.returncode != 0 or any(b in low for b in bad):
        brief = [l for l in out.splitlines()
                 if "error" in l.lower() or "fail" in l.lower()][-3:]
        return False, "CubeProgrammer 复位失败：{}".format(
            " | ".join(brief) or (out.splitlines() or ["无输出"])[-1])
    return True, "CubeProgrammer 复位完成"


# ==============================================================================
# 协议打包（GUI 与无界面调试端 ota_headless.py 共用）
# ==============================================================================
# 抽成模块级函数，是因为字节定义必须和 C 端逐字节一致：
#   老协议尾部（12 字节，'OTAE' + len + crc32）
# 一旦 GUI 里写一份、调试脚本里再写一份，两边漂移的症状是"发送说成功、
# 板子报 crc mismatch"，而且不报任何错 —— 这类坑排查成本极高。

OTA_TAIL_MAGIC = b"OTAE"


def build_ota_tail(fw_len, fw_crc):
    """老协议（BootLoader 串口收固件）的 12 字节尾部。"""
    return struct.pack("<4sII", OTA_TAIL_MAGIC, fw_len, fw_crc)


# ==============================================================================
# 单路串口通道
# ==============================================================================
class SerialChannel(object):
    """一路串口 + 一块接收区 + 一套 SWD 复位（目标是它对应的那块板）。

    每路自己维护：串口对象/读线程/队列/解码器/HEX 缓冲/跨行缓冲/OTA 解析
    状态/复位目标序列号。日志颜色规则不在这里，统一由 OtaSerialApp 管理。
    """

    _RX_NAV_KEYS = frozenset((
        "Left", "Right", "Up", "Down", "Home", "End", "Prior", "Next",
        "Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R",
        "Caps_Lock", "Num_Lock", "Escape",
    ))

    def __init__(self, app, parent, idx, name):
        self.app = app
        self.idx = idx
        self.name = name            # "A" / "B"

        # ---- 串口与线程 ----
        self.ser = None
        self.reader_alive = False
        self.reader_thread = None
        self.q = queue.Queue()      # (kind, payload) 主线程消费

        # ---- 解码器（增量式，解决多字节字符被切开变乱码；
        #      具体用哪个 codec 由「显示」下拉决定，见 _reset_decoder）----
        self.decoder = None

        # ---- HEX 显示缓冲 ----
        self.hex_buf = b""
        self.hex_off = 0
        self.hex_last_rx = 0.0

        # ---- 解析行缓冲 ----
        self.line_buf = b""

        # ---- OTA 解析状态（发送流程依赖；每路独立）----
        self.ota_base = ""
        self.ota_page = ""
        self.ota_saved = 0
        self.ota_proto = 1
        self.ota_verify = None      # None / ("ok", 长度) / ("fail", 0)
        self.ota_declared = 0
        self.ota_crc = ""
        self.ota_done_ts = 0.0

        # ---- 接收区最大保留行数 ----
        self.rx_max_lines = 5000

        # ---- SWD 复位（每路一套）----
        self.ocd_serial = ""        # 空 = 自动（第一个在位调试器）
        self.ocd_iface = "cmsis-dap"
        self.ocd_name = "DAP-Link"
        self.ocd_serial_missing = False
        self.ocd_iface_ok = False
        self.ocd_ready = False
        self.rst_backend = None     # openocd / jlink / stlink
        self.debuggers = []

        self._build(parent)
        self._load_cfg()
        self.apply_debugger(app.ocd_debuggers, app.cfg.get("ocd_iface") or "")
        self.poll()

    # ------------------------------------------------------------------ 配置
    def _cfg_key(self, base):
        return "{}{}".format(base, self.idx + 1)

    def _cfg_get(self, base, legacy, default):
        """先读新键（如 baud1），没有再退回 1.0 的老键（如 baud）。"""
        v = self.app.cfg.get(self._cfg_key(base))
        if v is None and self.idx == 0:
            v = self.app.cfg.get(legacy)
        return default if v is None else v

    def _load_cfg(self):
        self.saved_port = str(self._cfg_get("port", "port", "") or "")
        self.ocd_serial = str(self._cfg_get("ocd_serial", "ocd_serial", "") or "")
        self.baud_var.set(str(self._cfg_get("baud", "baud", "115200")))
        mode = str(self._cfg_get("disp", "disp_mode", "UTF-8"))
        if mode not in DISPLAY_OPTIONS:
            mode = "UTF-8"
        self.disp_mode.set(mode)
        self.timestamp.set(bool(self._cfg_get("ts", "timestamp", False)))
        self.autoscroll.set(bool(self._cfg_get("scroll", "autoscroll", True)))
        self._reset_decoder()

    def _reset_decoder(self):
        """按当前「显示」模式重建增量解码器；HEX 模式不需要解码器。"""
        codec = DISPLAY_MODES.get(self.disp_mode.get())
        if not codec:
            self.decoder = None
            return
        self.decoder = codecs.getincrementaldecoder(codec)("replace")

    def save_cfg(self, cfg):
        """把本路的设置写进配置字典（键带 1/2 后缀）。"""
        cfg[self._cfg_key("port")] = self.port_var.get()
        cfg[self._cfg_key("baud")] = self.baud_var.get()
        cfg[self._cfg_key("disp")] = self.disp_mode.get()
        cfg[self._cfg_key("ts")] = self.timestamp.get()
        cfg[self._cfg_key("scroll")] = self.autoscroll.get()
        cfg[self._cfg_key("ocd_serial")] = self.ocd_serial

    # ------------------------------------------------------------------ 界面
    def _build(self, parent):
        self.box = ttk.Frame(parent)
        self.box.columnconfigure(0, weight=1)
        self.box.rowconfigure(1, weight=1)

        head = tk.Frame(self.box, bg=C_BG)
        head.grid(row=0, column=0, sticky="ew", pady=(0, 4))

        # ---- 第一行：端口 / 波特率 / 打开 ----
        # 用 grid 而不是 pack：pack 在空间不足时会优先裁掉后放入的控件
        # （高 DPI / 窗口偏窄时「打开」按钮就被挤没了）。
        # grid 里给端口框 weight=1，空间不足时先压缩它，按钮始终完整。
        r1 = tk.Frame(head, bg=C_BG)
        r1.pack(fill="x")
        r1.columnconfigure(1, weight=1)
        tk.Label(r1, text="串口 " + self.name, bg=C_BG, fg=C_TEXT,
                 font=(FONT_UI, 9, "bold")).grid(
            row=0, column=0, sticky="w", padx=(0, 6))

        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(r1, textvariable=self.port_var, width=15,
                                    state="readonly", font=(FONT_MONO, 9))
        self.port_cb.grid(row=0, column=1, sticky="ew")

        # ttk 弹出时会把下拉窗口压成输入框宽度（combobox.tcl 的
        # PlacePopdown），只改列表宽度没用 —— 点开后再把窗口撑宽一次。
        # 用 after(30) 而不是 after_idle：Post 内部会先跑 update idletasks，
        # 用 idle 会在 PlacePopdown 之前执行，宽度又被压回去。
        def _fit_port_popdown(_evt=None):
            self.port_cb.after(30,
                               lambda: widen_popdown(self.port_cb))
        self.port_cb.bind("<Button-1>", _fit_port_popdown, add="+")
        self.port_cb.bind("<F4>", _fit_port_popdown, add="+")

        tk.Label(r1, text="波特率", bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=0, column=2, padx=(6, 2))
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(r1, textvariable=self.baud_var, width=7,
                     values=BAUD_LIST, state="readonly",
                     font=(FONT_MONO, 9)).grid(row=0, column=3)

        self.btn_open = ttk.Button(r1, text="打开", width=6,
                                   style="Accent.TButton",
                                   command=self.toggle_port)
        self.btn_open.grid(row=0, column=4, padx=(6, 0))

        # ---- 第二行：显示模式 / 时间戳 / 自动滚动 / 清空 ----
        r2 = tk.Frame(head, bg=C_BG)
        r2.pack(fill="x", pady=(4, 0))
        tk.Label(r2, text="显示", bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=0, column=0)
        self.disp_mode = tk.StringVar(value="UTF-8")
        cb_mode = ttk.Combobox(r2, textvariable=self.disp_mode, width=9,
                               values=DISPLAY_OPTIONS, state="readonly",
                               font=(FONT_MONO, 8))
        cb_mode.grid(row=0, column=1, padx=(4, 8))
        cb_mode.bind("<<ComboboxSelected>>", lambda e: self.on_disp_mode_change())

        self.timestamp = tk.BooleanVar(value=False)
        make_check(r2, "时间戳", self.timestamp, bg=C_BG).grid(
            row=0, column=2, padx=(0, 8))
        self.autoscroll = tk.BooleanVar(value=True)
        make_check(r2, "自动滚动", self.autoscroll, bg=C_BG).grid(
            row=0, column=3, padx=(0, 8))
        r2.columnconfigure(4, weight=1)     # 弹性空列：空间不够先挤这里
        ttk.Button(r2, text="清空", width=5,
                   command=self.clear_rx).grid(row=0, column=5, sticky="e")

        # ---- 接收区 ----
        rw = ttk.Frame(self.box)
        rw.grid(row=1, column=0, sticky="nsew")
        rw.columnconfigure(0, weight=1)
        rw.rowconfigure(0, weight=1)

        # 接收区必须保持 normal 才能用鼠标/键盘选中文字复制。
        # Tk 的 disabled Text 连选中都不允许（这正是红字日志复制不了的根因），
        # 所以用 normal + 按键白名单：只放行导航/复制键，编辑键一律吞掉。
        self.rx = tk.Text(rw, wrap="char", font=(FONT_MONO, 10),
                          bg="#FBFCFE", fg=C_TEXT, insertbackground=C_TEXT,
                          relief="solid", borderwidth=1,
                          padx=8, pady=6, spacing1=1, spacing3=1,
                          undo=False, state="normal")
        self.rx.grid(row=0, column=0, sticky="nsew")
        self.rx.bind("<Key>", self._on_rx_key)

        # 右键菜单：复制选中 / 复制全部日志 / 全选 / 保存本区日志
        self.rx_menu = tk.Menu(self.app.root, tearoff=0, font=(FONT_UI, 9))
        self.rx_menu.add_command(label="复制选中", command=self._copy_rx_selection)
        self.rx_menu.add_command(label="复制全部日志", command=self._copy_all_rx)
        self.rx_menu.add_separator()
        self.rx_menu.add_command(label="全选", command=self._select_all_rx)
        self.rx_menu.add_command(label="保存本区日志", command=self.save_log)
        self.rx.bind("<Button-3>",
                     lambda e: (self.rx_menu.tk_popup(e.x_root, e.y_root), "break")[1])

        sb = ttk.Scrollbar(rw, orient="vertical", command=self.rx.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.rx.configure(yscrollcommand=sb.set)

        # 着色规则（红字只是文本 tag，不影响选中与复制）
        self.apply_colors()

        # sel 必须显式提权：Tk 的 tag 优先级按创建顺序递增，内置的 sel 排在
        # 用户标签之前，bad 的浅红背景会盖住选区高亮 —— 红字行"看着没选中"
        # 的根因就在这里（选区其实已包含它，剪贴板能拿到）。
        self.rx.tag_configure("sel", background="#0078D8")
        self.rx.tag_raise("sel")

        # ---- SWD 复位（本路专用）----
        sf = tk.Frame(self.box, bg=C_BG)
        sf.grid(row=2, column=0, sticky="ew", pady=(4, 0))

        tk.Label(sf, text="复位目标", bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 8)).pack(side="left")
        self.dbg_sel_var = tk.StringVar()
        self.cb_dbg = ttk.Combobox(sf, textvariable=self.dbg_sel_var, width=17,
                                   state="readonly", font=(FONT_UI, 8))
        self.cb_dbg.pack(side="left", padx=(4, 4))
        self.cb_dbg.bind("<<ComboboxSelected>>", self._on_pick_debugger)
        # 点开下拉前先轻量重扫一遍 USB：刚插上的板子不必先点「刷新软件」
        self.cb_dbg.bind("<Button-1>", self._rescan_debuggers)

        self.btn_rst = ttk.Button(sf, text="SWD 复位", width=9,
                                  command=self.reset_swd)
        self.btn_rst.pack(side="left", padx=(0, 4))
        ttk.Button(sf, text="唤醒", width=5,
                   command=self.wake_ota).pack(side="left")

        self.ocd_hint = tk.StringVar()
        self.lb_ocd_hint = tk.Label(self.box, textvariable=self.ocd_hint, bg=C_BG,
                                    font=(FONT_UI, 7), anchor="w")
        self.lb_ocd_hint.grid(row=3, column=0, sticky="ew", pady=(2, 0))

    # ------------------------------------------------------------ 调试器状态
    def apply_debugger(self, debuggers, cfg_iface=""):
        """按在位调试器清单重算本路的 iface / 名字 / 就绪状态并刷 UI。

        cfg_iface 非空 = 用户在配置里手写了 ocd_iface（逃生口），
        优先于现场探测，两个通道都尊重它。
        """
        self.debuggers = list(debuggers)
        sel = None
        if self.ocd_serial:
            for d in self.debuggers:
                if d["serial"].upper() == self.ocd_serial.upper():
                    sel = d
                    break
        else:
            sel = self.debuggers[0] if self.debuggers else None

        probed_iface = sel["iface"] if sel else None
        self.ocd_iface = cfg_iface or probed_iface or "cmsis-dap"
        self.ocd_name = (sel["name"] if sel else None) or {
            "cmsis-dap": "DAP-Link",
            "stlink":    "ST-Link",
            "jlink":     "J-Link",
        }.get(self.ocd_iface, self.ocd_iface.upper())
        self.ocd_serial_missing = bool(self.ocd_serial and sel is None)

        self._refresh_ready()
        self._sync_combo()
        self._apply_ui(cfg_iface)

    def _refresh_ready(self):
        """按当前 ocd_iface 重算「接口脚本存在 + 整体就绪」。"""
        scr = self.app.ocd_scr
        self.ocd_iface_ok = bool(
            scr and os.path.isfile(os.path.join(scr, "interface",
                                                self.ocd_iface + ".cfg")))
        # 一台调试器都没认出来时不能算就绪：否则按钮可点、点了必失败
        # （OpenOCD 报 "unable to find a matching CMSIS-DAP device"）。
        # 手工在配置里写了 ocd_iface 视为逃生口，允许继续尝试。
        self.no_debugger = not self.debuggers \
            and not (self.app.cfg.get("ocd_iface") or "")

        # 复位后端选择：优先 OpenOCD（interface 脚本齐全），
        # 没有 OpenOCD 时 J-Link / ST-Link 用官方 CLI 兜底。
        self.rst_backend = None
        if not self.no_debugger:
            if bool(self.app.ocd_exe) and self.ocd_iface_ok:
                self.rst_backend = "openocd"
            elif self.ocd_iface == "jlink" and getattr(
                    self.app, "jlink_exe", None):
                self.rst_backend = "jlink"
            elif self.ocd_iface == "stlink" and (
                    getattr(self.app, "stlink_cli", None)
                    or getattr(self.app, "cubeprog_exe", None)):
                self.rst_backend = "stlink"
        self.ocd_ready = bool(self.rst_backend)

    def _current_debugger(self):
        """本路当前选中的调试器字典；自动模式取第一台，没有则 None。"""
        if self.ocd_serial:
            for d in self.debuggers:
                if d["serial"].upper() == self.ocd_serial.upper():
                    return d
            return None                 # 锁定的那台不在位
        return self.debuggers[0] if self.debuggers else None

    def _dbg_labels(self):
        """下拉项：第 0 项自动，之后每台调试器一项（序列号只显示末 8 位）。"""
        labels = ["自动（第一个）"]
        for d in self.debuggers:
            sn = d["serial"]
            labels.append("{} · {}".format(d["name"],
                                           sn[-8:] if sn else "无序列号"))
        return labels

    def _sync_combo(self):
        """把已锁定的序列号反映到本路下拉框。"""
        self.cb_dbg["values"] = self._dbg_labels()
        idx = 0
        if self.ocd_serial:
            for i, d in enumerate(self.debuggers, start=1):
                if d["serial"].upper() == self.ocd_serial.upper():
                    idx = i
                    break
        self.cb_dbg.current(idx)

    def _apply_ui(self, cfg_iface=""):
        """同步复位按钮/提示文字的颜色与可用性。"""
        ok_color = False
        if not self.debuggers and not (cfg_iface or ""):
            hint = ("未检测到调试器（DAP-Link / ST-Link / J-Link），"
                    "插好后点「刷新软件」")
        elif self.ocd_serial_missing:
            hint = "锁定的调试器不在位（{}），复位会失败".format(
                self.ocd_serial[-8:])
        elif self.rst_backend == "openocd":
            cur = self._current_debugger()
            if cfg_iface and cur and cfg_iface != cur["iface"]:
                hint = "注意：配置锁定 interface/{}，与当前设备不符".format(
                    cfg_iface)
            else:
                hint = "SWD 复位就绪 · {}（OpenOCD）".format(self.ocd_name)
                ok_color = True
        elif self.rst_backend == "jlink":
            hint = "SWD 复位就绪 · {}（JLink.exe，无需 OpenOCD）".format(
                self.ocd_name)
            ok_color = True
        elif self.rst_backend == "stlink":
            tool = ("ST-LINK_CLI" if getattr(self.app, "stlink_cli", None)
                    else "STM32_Programmer_CLI")
            hint = "SWD 复位就绪 · {}（{}）".format(self.ocd_name, tool)
            ok_color = True
        elif not self.app.ocd_exe:
            if self.ocd_iface == "jlink":
                hint = ("未找到 OpenOCD / JLink.exe；装 J-Link 驱动，"
                        "或填 openocd_path")
            elif self.ocd_iface == "stlink":
                hint = ("未找到 OpenOCD / ST-LINK_CLI.exe / "
                        "STM32_Programmer_CLI.exe；可填 openocd_path")
            else:
                hint = ("未找到 OpenOCD（可在 assist_config.json 填 "
                        "openocd_path），请手动复位")
        elif not self.ocd_iface_ok:
            hint = ("OpenOCD 缺 interface/{}.cfg，且没有可用的 CLI 兜底，"
                    "请手动复位".format(self.ocd_iface))
        else:
            hint = "复位不可用，请手动按板子复位键"

        self.ocd_hint.set(hint)
        self.lb_ocd_hint.configure(fg=C_OK if ok_color else C_BAD)
        self.btn_rst.configure(state="normal" if self.ocd_ready else "disabled")

    def _rescan_debuggers(self, _evt=None):
        """点开本路下拉前，轻量重扫一遍在位调试器（不动串口、不清日志）。"""
        self.app.rediscover_debuggers()

    def _on_pick_debugger(self, _evt=None):
        """用户在本路下拉里换「复位目标」：立刻落盘并同步状态。"""
        idx = self.cb_dbg.current()
        if idx <= 0:
            self.ocd_serial = ""
            label = "自动（第一个）"
            cur = self.debuggers[0] if self.debuggers else None
            if cur:
                self.ocd_name = cur["name"]
                if not self.app.cfg.get("ocd_iface"):
                    self.ocd_iface = cur["iface"]
        else:
            d = self.debuggers[idx - 1]
            self.ocd_serial = d["serial"]
            self.ocd_name = d["name"]
            label = "{} · {}".format(d["name"], d["serial"][-8:] if d["serial"]
                                     else "无序列号")
            if not self.app.cfg.get("ocd_iface"):
                self.ocd_iface = d["iface"]

        self.ocd_serial_missing = bool(
            self.ocd_serial and self._current_debugger() is None)
        self._refresh_ready()
        self._sync_combo()
        self._apply_ui(self.app.cfg.get("ocd_iface") or "")

        # 内存里的 cfg 也要同步：「刷新软件」从 cfg 恢复 ocd_serial，
        # 只写磁盘不更新内存的话，刚选好的目标会被旧配置覆盖回去。
        self.app.cfg[self._cfg_key("ocd_serial")] = self.ocd_serial
        cfg = load_config()
        cfg[self._cfg_key("ocd_serial")] = self.ocd_serial
        save_config(cfg)
        self._sys_line("--- 复位目标：{}{} ---".format(
            label, "" if self.ocd_serial else "（不指定序列号）"))

    def reset_blocking(self):
        """在后台线程里同步执行一次复位（发送流程用）。

        按 rst_backend 选后端：OpenOCD 优先，其次 J-Link / ST-Link CLI。
        """
        if not self.ocd_ready:
            return False, "复位后端不可用（缺 OpenOCD / JLink.exe / ST-LINK_CLI）"
        if self.rst_backend == "jlink":
            return jlink_reset(
                self.app.jlink_exe, self.ocd_serial,
                self.app.cfg.get("jlink_device", "STM32F103C8"),
                str(self.app.cfg.get("jlink_speed", "4000")))
        if self.rst_backend == "stlink":
            if getattr(self.app, "stlink_cli", None):
                return stlink_reset(self.app.stlink_cli, self.ocd_serial)
            return cubeprog_reset(self.app.cubeprog_exe, self.ocd_serial)
        return openocd_reset(self.app.ocd_exe, self.app.ocd_scr,
                             self.ocd_iface, self.app.ocd_target,
                             serial=self.ocd_serial)

    def reset_swd(self):
        """手动 SWD 复位：只复位本路对应的那块板。"""
        if not self.ocd_ready:
            if self.no_debugger:
                msg = ("未检测到调试器。请插好 DAP-Link / ST-Link / J-Link，"
                       "再点「刷新软件」重扫。")
            elif self.ocd_iface == "jlink":
                msg = ("没有可用的复位后端：既没找到 OpenOCD，"
                       "也没找到 JLink.exe。\n安装 J-Link 驱动，或在 "
                       "assist_config.json 里填 openocd_path。")
            elif self.ocd_iface == "stlink":
                msg = ("没有可用的复位后端：既没找到 OpenOCD，也没找到 "
                       "ST-LINK_CLI.exe / STM32_Programmer_CLI.exe。\n"
                       "装 STM32CubeProgrammer（带 CLI），或在 "
                       "assist_config.json 里填 openocd_path。")
            else:
                msg = ("OpenOCD 里缺 interface/{}.cfg，认不出当前调试器。"
                       "请手动按板子复位键。".format(self.ocd_iface))
            messagebox.showwarning("无调试器", msg)
            return

        self._sys_line("--- 正在通过 SWD 复位（串口 {} 的目标板）… ---".format(
            self.name))

        def work():
            ok, info = self.reset_blocking()
            # 后台线程不直接碰 Tk 控件，结果走队列回主线程
            self.q.put(("sys", "--- SWD 复位 {}：{} ---".format(
                "成功" if ok else "失败", info)))

        threading.Thread(target=work, daemon=True).start()

    # ------------------------------------------------------------------ 串口
    def toggle_port(self):
        if self.ser and self.ser.is_open:
            self.close_port()
        else:
            self.open_port()

    def open_port(self):
        sel = self.port_var.get().strip()
        if not sel:
            messagebox.showwarning("没有可用串口", "请先插好 USB 转串口，再点刷新。")
            return
        dev = sel.split()[0]

        # 同一个 COM 不能被两路同时打开 —— 第二个 open 会报"拒绝访问"，
        # 提前拦下来给一句能看懂的话。
        other = self.app.other_channel(self)
        if other and other.ser and other.ser.is_open:
            if other.port_var.get().strip().split()[:1] == [dev]:
                messagebox.showwarning(
                    "端口重复",
                    "{} 已被「串口 {}」占用。\n请为两路选择不同的端口。".format(
                        dev, other.name))
                return

        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("波特率无效", "请选择或输入一个数字波特率。")
            return

        # 先构造但不 open，把 DTR/RTS 置 False 再 open
        # 否则 open 瞬间会拉一次 DTR —— 很多板子 DTR 接 NRST，会白复位一次
        ser = serial.Serial()
        ser.port = dev
        ser.baudrate = baud
        ser.bytesize = serial.EIGHTBITS
        ser.parity = serial.PARITY_NONE
        ser.stopbits = serial.STOPBITS_ONE
        ser.timeout = 0.05
        ser.write_timeout = None        # 大块写时阻塞等待，不丢数据
        ser.dtr = False
        ser.rts = False

        try:
            ser.open()
        except serial.SerialException as e:
            msg = str(e)
            if "PermissionError" in msg or "拒绝访问" in msg or "Access is denied" in msg:
                messagebox.showerror(
                    "串口被占用",
                    "{} 已被其他程序占用。\n\n"
                    "常见原因：SSCOM / XCOM / VSCode 串口监视器 还开着。\n"
                    "请先关掉它们再试。".format(dev))
            else:
                messagebox.showerror("打开失败", msg)
            return

        self.ser = ser
        self.reader_alive = True
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

        # 复位解码器与 HEX 偏移
        self._reset_decoder()
        self.hex_buf = b""
        self.hex_off = 0
        self.line_buf = b""

        self.btn_open.configure(text="关闭")
        self.app.update_conn()
        self._sys_line("--- 已连接 {} @ {} ---".format(dev, baud))

    def close_port(self, quiet=False):
        self.reader_alive = False

        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=0.6)

        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

        self.btn_open.configure(text="打开")
        self.app.update_conn()
        if not quiet:
            self._sys_line("--- 已断开 ---")

    def _reader_loop(self):
        """后台读串口，只做一件事：把字节丢进队列。UI 一律主线程改。"""
        while self.reader_alive:
            ser = self.ser
            if ser is None:
                break
            try:
                n = ser.in_waiting
                data = ser.read(n if n > 0 else 1)
            except Exception as e:
                self.q.put(("error", str(e)))
                break
            if data:
                self.q.put(("data", data))

    # ==========================================================================
    # 主线程轮询：把本路队列里的东西刷到界面
    # ==========================================================================

    def poll(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "data":
                    self._on_rx(payload)
                elif kind == "sys":
                    self._sys_line(payload)
                elif kind == "error":
                    self._on_serial_error(payload)
        except queue.Empty:
            pass

        # HEX 模式下，不足 16 字节的尾巴要定时冲出来，否则要等到下一批数据
        if (self.disp_mode.get() == "HEX" and self.hex_buf
                and time.time() - self.hex_last_rx > 0.15):
            self._flush_hex()

        self.box.after(30, self.poll)

    def _on_serial_error(self, msg):
        self._sys_line("--- 串口异常: {} ---".format(msg))
        self.close_port(quiet=True)

    def _on_rx(self, data):
        self.app.rx_count += len(data)
        self.app.update_counts()
        self._feed_parser(data)         # 解析永远基于原始字节，与显示模式无关
        self._render(data)

    # ==========================================================================
    # 渲染
    # ==========================================================================

    def _render(self, data):
        mode = self.disp_mode.get()

        if mode == "HEX":
            self.hex_last_rx = time.time()
            self.hex_buf += data
            while len(self.hex_buf) >= 16:
                chunk, self.hex_buf = self.hex_buf[:16], self.hex_buf[16:]
                self._insert("{:08X}  {}\n".format(
                    self.hex_off, " ".join("{:02X}".format(b) for b in chunk)), "sys")
                self.hex_off += 16
            return

        dec = self.decoder
        if dec is None:                 # 理论不会：模式切换时已重建
            self._reset_decoder()
            dec = self.decoder
            if dec is None:
                return
        text = dec.decode(data)
        if not text:
            return

        if self.timestamp.get():
            now = time.time()
            ms = int((now - int(now)) * 1000)
            text = time.strftime("[%H:%M:%S") + ".{:03d}] ".format(ms) + text

        start_line = int(self.rx.index("end-1c").split(".")[0])
        self._insert(text)
        end_line = int(self.rx.index("end-1c").split(".")[0])

        # 逐行着色
        for ln in range(start_line, end_line + 1):
            try:
                line = self.rx.get("{}.0".format(ln), "{}.end".format(ln))
            except tk.TclError:
                continue
            tag = self.app.classify(line)
            if tag:
                self.rx.tag_add(tag, "{}.0".format(ln), "{}.end".format(ln))

    def _flush_hex(self):
        if not self.hex_buf:
            return
        chunk = self.hex_buf
        self.hex_buf = b""
        self._insert("{:08X}  {}\n".format(
            self.hex_off, " ".join("{:02X}".format(b) for b in chunk)), "sys")
        self.hex_off += len(chunk)

    def _insert(self, s, tag=None):
        at_end = self.rx.yview()[1] > 0.999
        # 不切 state：Text 保持 normal 才能选中复制，编辑键由 _on_rx_key 拦截。
        self.rx.insert("end", s, tag or ())
        # 超量裁剪，保证长时间开着也不吃内存
        total = int(self.rx.index("end-1c").split(".")[0])
        if total > self.rx_max_lines:
            self.rx.delete("1.0", "{}.0".format(total - self.rx_max_lines + 500))
        if self.autoscroll.get() or at_end:
            self.rx.see("end")

    def _sys_line(self, s):
        # 工具提示行固定挂 "sys" tag。若用户在设置里删掉了 sys 规则，
        # Tk 对未配置的 tag 名只是不着色（不会抛异常），显示默认字色。
        self._insert(s + "\n", "sys")

    # 接收区只读但可选中：按键白名单 + 右键复制
    def _on_rx_key(self, event):
        """接收区保持 normal 可复制，同时不让用户改动日志。

        返回 "break" 会让 Tk 停止继续处理该按键。导航键（含 Shift+方向
        扩展选区）永远放行；Control 只放行复制/全选/清屏和 Ctrl+Insert，
        其余包括 BackSpace、Delete、Ctrl+V 一律吞掉。
        """
        if event.keysym in self._RX_NAV_KEYS:
            return None
        if event.state & 0x0004:            # Control 修饰位
            if event.keysym.lower() in ("c", "a", "l") or event.keysym == "Insert":
                return None
            return "break"
        return "break"

    def _copy_rx_selection(self):
        """右键「复制选中」：没有选区时静默返回，不弹框打扰。"""
        try:
            txt = self.rx.get("sel.first", "sel.last")
        except tk.TclError:
            return
        self.app.root.clipboard_clear()
        self.app.root.clipboard_append(txt)

    def _copy_all_rx(self):
        """右键「复制全部日志」：整屏文本进剪贴板。"""
        txt = self.rx.get("1.0", "end-1c")
        if not txt:
            return
        self.app.root.clipboard_clear()
        self.app.root.clipboard_append(txt)

    def _select_all_rx(self):
        self.rx.tag_add("sel", "1.0", "end")
        self.rx.mark_set("insert", "end")

    # ---------------------------------------------------------------- 着色
    def apply_colors(self):
        """把（共用的）规则表颜色应用到本接收区的所有 tag。"""
        for r in self.app.log_rules:
            opts = {"foreground": r["fg"]}
            if r["bg"]:
                opts["background"] = r["bg"]
            self.rx.tag_configure(r["tag"], **opts)
        self.rx.tag_raise("sel")

    def _clear_all_log_tags(self):
        """清掉接收区所有日志 tag（保留 sel 选区）。"""
        for r in self.app.log_rules:
            self.rx.tag_remove(r["tag"], "1.0", "end")
        for t in self.rx.tag_names():
            if t != "sel":
                self.rx.tag_remove(t, "1.0", "end")

    def recolor_existing(self):
        """按当前规则表重新给本区已有行着色（确定设置后调用）。"""
        self._clear_all_log_tags()
        total = int(self.rx.index("end-1c").split(".")[0])
        for ln in range(1, total + 1):
            a, b = "{}.0".format(ln), "{}.end".format(ln)
            try:
                line = self.rx.get(a, b)
            except tk.TclError:
                continue
            if not line:
                continue
            if line.startswith("--- ") or line.startswith(">>> "):
                self.rx.tag_add("sys", a, b)
                continue
            tag = self.app.classify(line)
            if tag and tag != "sys":
                self.rx.tag_add(tag, a, b)

    def on_disp_mode_change(self):
        self._reset_decoder()
        self.hex_buf = b""
        self.hex_off = 0
        self.clear_rx()
        self._sys_line("--- 显示模式: {} ---".format(self.disp_mode.get()))

    def clear_rx(self):
        """清空接收区显示 —— 只动界面，不碰串口一个字节

        做的事只有三件：清 Text 内容、丢掉 HEX 模式下攒着不满 16 字节的
        尾巴、偏移归零。

        明确不清的东西（这是刻意留着的）：
          · 驱动层 in_waiting —— 板子刚发来、驱动已收到但还没读的字节
          · 本路队列 —— 已读到、还没渲染的块
          · line_buf —— 跨块的半行，解析靠它拼整行
          · 增量解码器 —— 可能存着半个多字节字符
          · 全局收发计数

        副作用：HEX 模式下偏移从 00000000 重新起算。
        """
        self.rx.delete("1.0", "end")
        self.hex_buf = b""
        self.hex_off = 0

    def reset_runtime(self):
        """把本路运行时状态恢复成"刚打开软件"（刷新软件时用）。"""
        self.line_buf = b""
        self._reset_decoder()
        self.hex_buf = b""
        self.hex_off = 0
        self.ota_base = ""
        self.ota_page = ""
        self.ota_saved = 0
        self.ota_proto = 1
        self.ota_verify = None
        self.ota_declared = 0
        self.ota_crc = ""
        self.ota_done_ts = 0.0

    def save_log(self):
        """保存本接收区的日志。"""
        txt = self.rx.get("1.0", "end-1c")
        if not txt.strip():
            messagebox.showinfo("没有内容", "接收区还是空的。")
            return
        path = filedialog.asksaveasfilename(
            title="保存串口 {} 的接收日志".format(self.name),
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")],
            initialfile=time.strftime(
                "serial_{}_%Y%m%d_%H%M%S.txt".format(self.name)))
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(txt)
            messagebox.showinfo("已保存", path)
        except Exception as e:
            messagebox.showerror("保存失败", str(e))

    # ==========================================================================
    # BootLoader 日志解析 → 本路内部状态
    # ==========================================================================

    def _feed_parser(self, data):
        """按行切分后交给 _handle_line。跨数据块的行用 line_buf 拼起来。"""
        self.line_buf += data
        while b"\n" in self.line_buf:
            line, self.line_buf = self.line_buf.split(b"\n", 1)
            self._handle_line(line.strip(b"\r"))
        # 长时间没有换行的异常数据（比如二进制）不要无限累积
        if len(self.line_buf) > 4096:
            self.line_buf = self.line_buf[-1024:]

    def _handle_line(self, line):
        """从串口日志里提取发送流程需要的状态（每路各一份）。"""
        if not line:
            return

        m = RE_READY.search(line)
        if m:
            # 升级完成后会自动复位，重启的 BootLoader 又打一遍 [ota] ready。
            # 这一次不能清 ota_saved —— 老协议的"等 sum 对齐再复位"会永远等不到。
            just_done = (time.time() - self.ota_done_ts) < 15.0
            self.ota_base = m.group(1).decode()
            self.ota_page = m.group(2).decode()
            # proto 是可选组：老 BootLoader 不打这个字段，就是 1
            self.ota_proto = int(m.group(3)) if m.group(3) else 1
            if not just_done:
                self.ota_saved = 0
            return

        m = RE_PAGE.search(line)
        if m:
            self.ota_saved = int(m.group(2))
            return

        m = RE_TAIL.search(line)
        if m:
            self.ota_saved = int(m.group(3))
            self.ota_done_ts = time.time()      # 后面重启的 ready 不会清统计
            return

        m = RE_TAILREC.search(line)
        if m:
            self.ota_declared = int(m.group(1))
            self.ota_crc = m.group(2).decode().upper()
            return

        m = RE_VOK.search(line)
        if m:
            n = int(m.group(1))
            self.ota_verify = ("ok", n)
            self.ota_saved = n
            self.ota_done_ts = time.time()
            return

        if RE_VBAD.search(line) or RE_SIZEBAD.search(line):
            # 校验没过就别指望跳转了，板子还停在 OTA 里等重传
            self.ota_verify = ("fail", 0)
            return

    # ---------------------------------------------------------------- 普通发送
    def send_bytes(self, payload):
        """把一段字节写进本路串口。失败返回 False 并提示。"""
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开",
                                   "请先打开「串口 {}」。".format(self.name))
            return False
        try:
            self.ser.write(payload)
        except Exception as e:
            messagebox.showerror("发送失败", str(e))
            return False
        self.app.tx_count += len(payload)
        self.app.update_counts()
        return True

    def wake_ota(self):
        """发一个 0x00 触发 BootLoader 的 OTA 窗口。"""
        if self.send_bytes(b"\x00"):
            self._sys_line(">>> 唤醒字节 0x00（触发 OTA 窗口）")


# ==============================================================================
# 主窗口
# ==============================================================================
class OtaSerialApp(object):

    # ------------------------------------------------------------------ 初始化
    def __init__(self, root):
        self.root = root
        self.cfg = load_config()

        # 规则表从配置读，非法颜色/非法 tag 名在 load_log_rules
        # 里就挡掉了 —— Tk 收到坏颜色会在 tag_configure 抛 TclError，
        # 界面直接起不来，不能让手改 json 把软件弄崩。
        self.log_rules = load_log_rules(self.cfg)

        root.title("{}  v{}".format(APP_TITLE, APP_VER))
        root.geometry(self.cfg.get("geometry") or "1500x900")
        root.minsize(1280, 840)
        root.configure(bg=C_BG)

        # ---- 固件发送线程（全局只有一个，发送目标由下拉决定）----
        self.tx_thread = None
        self.abort_evt = threading.Event()
        self.tx_q = queue.Queue()        # (kind, payload) 主线程消费

        # ---- 收发计数（两路合计）----
        self.rx_count = 0
        self.tx_count = 0

        # ---- 固件文件 ----
        self.fw_size = 0
        self.fw_mtime = 0.0

        # ---- 调试器（OpenOCD 路径与设备清单两路共用，目标各自选）----
        self._probe_debugger()

        self._init_style()
        self._build_ui()
        self.refresh_ports()
        self._fit_window()

        # Ctrl+L 清空两个接收区，和终端里清屏的习惯一致。
        # 绑在 root 上而不是 Text 上：焦点在输入框时也想能按。
        root.bind("<Control-l>",
                  lambda e: (self.clear_all_rx(), "break")[1])

        # 关闭窗口时先收拾线程，否则 python 会卡在 join
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self._poll()

    # ------------------------------------------------------------ 调试器探测
    def _fit_window(self):
        """启动时把窗口收进屏幕可视范围，并保证不小于舒适尺寸。

        旧配置里的 geometry 可能是 "1x1+907+207"（最小化时保存的），
        直接套用会得到一个贴边的小窗口：高 DPI 下右侧按钮被裁掉，
        或者整个窗口跑到屏幕外只露出一个角。这里统一处理：
          · 尺寸至少 1500x900，但不超过屏幕（纵向给任务栏留 60px）
          · 位置夹在屏幕内，负坐标 / 越界坐标都拉回来
          · minsize 跟随屏幕收缩，小屏笔记本也能把窗口缩小
        """
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
            gw = gh = 0
            gx = gy = None
            m = re.match(r"^(\d+)x(\d+)\+(-?\d+)\+(-?\d+)$",
                         str(self.cfg.get("geometry") or ""))
            if m:
                gw, gh, gx, gy = (int(v) for v in m.groups())

            w = min(max(gw, 1500), sw)
            # sh 是整块屏幕高度，还要给标题栏 + 任务栏留位置，
            # 否则窗口底部（状态栏）会被任务栏压住
            h = min(max(gh, 900), max(sh - 100, 600))
            if gx is None:
                x = max((sw - w) // 2, 0)
                y = max((sh - h) // 3, 0)
            else:
                x = min(max(gx, 0), max(sw - w, 0))
                y = min(max(gy, 0), max(sh - h, 0))
            self.root.geometry("{}x{}+{}+{}".format(w, h, x, y))
            self.root.minsize(min(1280, sw), min(840, max(sh - 100, 600)))
        except Exception:
            pass

    def _probe_debugger(self):
        self.ocd_exe, self.ocd_scr = find_openocd(self.cfg, app_dir())
        self.ocd_debuggers = detect_debuggers()
        self.ocd_target = self.cfg.get("ocd_target", "stm32f1x")
        # 没 OpenOCD 时的兜底工具（各自独立找，找不到就是 None）
        self.jlink_exe = find_jlink(self.cfg)
        self.stlink_cli = find_stlink_cli(self.cfg)
        self.cubeprog_exe = find_cubeprog(self.cfg)

    def rediscover_debuggers(self):
        """轻量重扫：只重认 OpenOCD 与 USB 调试器，并把结果同步到两路。

        点开任一「复位目标」下拉或点「刷新软件」时调用。
        """
        self._probe_debugger()
        cfg_iface = self.cfg.get("ocd_iface") or ""
        for ch in self.channels:
            ch.apply_debugger(self.ocd_debuggers, cfg_iface)

    def other_channel(self, ch):
        for c in self.channels:
            if c is not ch:
                return c
        return None

    # ---------------------------------------------------------------- ttk 主题
    def _init_style(self):
        st = ttk.Style()
        try:
            st.theme_use("clam")            # clam 才允许改背景色
        except tk.TclError:
            pass

        st.configure("TFrame", background=C_BG)
        st.configure("Card.TFrame", background=C_PANEL,
                     relief="solid", borderwidth=1)
        st.configure("TLabel", background=C_BG, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("Card.TLabel", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("Hint.TLabel", background=C_PANEL, foreground=C_SUB,
                     font=(FONT_UI, 8))
        st.configure("Val.TLabel", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_MONO, 10))
        st.configure("Big.TLabel", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_MONO, 13, "bold"))
        st.configure("TLabelframe", background=C_BG, bordercolor=C_BORDER,
                     relief="solid", borderwidth=1)
        st.configure("TLabelframe.Label", background=C_BG, foreground=C_SUB,
                     font=(FONT_UI, 9, "bold"))
        st.configure("TCheckbutton", background=C_BG, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("Card.TCheckbutton", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("TButton", font=(FONT_UI, 9), padding=(9, 4))
        st.configure("Accent.TButton", background=C_ACCENT, foreground="#FFFFFF",
                     font=(FONT_UI, 9, "bold"), padding=(12, 5), borderwidth=0)
        st.map("Accent.TButton",
               background=[("active", C_ACCENT2), ("disabled", "#A9C0F2")])
        st.configure("Go.TButton", background=C_OK, foreground="#FFFFFF",
                     font=(FONT_UI, 9, "bold"), padding=(10, 6), borderwidth=0)
        st.map("Go.TButton",
               background=[("active", "#0B8049"), ("disabled", "#A8D5BE")])
        st.configure("Stop.TButton", background="#F1F3F6", foreground=C_BAD,
                     font=(FONT_UI, 9, "bold"), padding=(10, 6), borderwidth=1)
        st.map("Stop.TButton",
               background=[("active", "#FBE4E2"), ("disabled", "#F0F0F0")])

    # ------------------------------------------------------------------- 布局
    def _build_ui(self):
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        self._build_topbar()      # row 0
        self._build_middle()      # row 1  两个接收区 + 右侧固件发送
        self._build_sendbar()     # row 2
        self._build_statusbar()   # row 3

        # 按配置恢复「双串口 / 单串口」布局（不重复写盘）
        self._apply_dual_mode(persist=False)

    # ---- 顶部：全局按钮 ----
    def _build_topbar(self):
        bar = ttk.Frame(self.root)
        bar.grid(row=0, column=0, sticky="ew", padx=12, pady=(10, 6))

        tk.Label(bar, text="{} v{}".format(APP_TITLE, APP_VER),
                 bg=C_BG, fg=C_TEXT,
                 font=(FONT_UI, 10, "bold")).pack(side="left", padx=(0, 14))

        ttk.Button(bar, text="刷新端口", width=9,
                   command=self.refresh_ports).pack(side="left")
        ttk.Button(bar, text="刷新软件", width=9,
                   command=self.refresh_software).pack(side="left", padx=(6, 0))
        ttk.Button(bar, text="设置", width=6,
                   command=self.open_color_settings).pack(side="left", padx=(6, 0))

        # 双串口开关：关掉后隐藏 B 区、断开 B 口，窗口退化成单串口布局；
        # 再勾上恢复双通道。状态记在配置里，下次打开保持一致。
        self.dual_var = tk.BooleanVar(value=bool(self.cfg.get("dual_mode", True)))
        chk_dual = make_check(bar, "双串口（显示串口 B）", self.dual_var, bg=C_BG)
        chk_dual.configure(command=self.on_toggle_dual)
        chk_dual.pack(side="left", padx=(10, 0))

        tk.Label(bar, text="双通道：两路各自独立接收；发送目标见「固件发送」和底部发送栏",
                 bg=C_BG, fg=C_SUB, font=(FONT_UI, 8)).pack(side="right")

    # ---- 中部：左 A / 中 B / 右固件发送 ----
    def _build_middle(self):
        mid = ttk.Frame(self.root)
        self.mid = mid
        mid.grid(row=1, column=0, sticky="nsew", padx=12, pady=(0, 6))
        mid.columnconfigure(0, weight=1)
        mid.columnconfigure(1, weight=1)
        mid.rowconfigure(0, weight=1)

        self.ch_a = SerialChannel(self, mid, 0, "A")
        self.ch_b = SerialChannel(self, mid, 1, "B")
        self.channels = [self.ch_a, self.ch_b]

        self.ch_a.box.grid(row=0, column=0, sticky="nsew")
        self.ch_b.box.grid(row=0, column=1, sticky="nsew", padx=(10, 0))

        right = ttk.Frame(mid)
        right.grid(row=0, column=2, sticky="ns", padx=(10, 0))
        self._build_fw_card(right)

    def _card(self, parent, title, row):
        """带标题的卡片外壳，返回内部 content frame"""
        outer = tk.Frame(parent, bg=C_BG)
        outer.grid(row=row, column=0, sticky="ew", pady=(0, 6))
        outer.columnconfigure(0, weight=1)

        head = tk.Frame(outer, bg=C_BG)
        head.grid(row=0, column=0, sticky="ew", pady=(0, 3))
        tk.Label(head, text=title, bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 9, "bold")).pack(side="left")

        box = tk.Frame(outer, bg=C_PANEL, highlightbackground=C_BORDER,
                       highlightthickness=1)
        box.grid(row=1, column=0, sticky="ew")
        box.columnconfigure(1, weight=1)

        content = tk.Frame(box, bg=C_PANEL)
        content.grid(row=0, column=0, sticky="nsew", padx=10, pady=8)
        content.columnconfigure(1, weight=1)
        return content

    def _build_fw_card(self, parent):
        c = self._card(parent, "固件发送", 0)

        self.fw_path = tk.StringVar(value="")
        self.fw_name = tk.StringVar(value="（未选择文件）")

        # 选文件 + 检查更新并排。检查按钮只对比当前选中 bin 的
        # mtime/size，变了就自动重新装填（不弹窗打断）。
        bf_pick = tk.Frame(c, bg=C_PANEL)
        bf_pick.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 5))
        bf_pick.columnconfigure(0, weight=2)
        bf_pick.columnconfigure(1, weight=1)
        ttk.Button(bf_pick, text="选择 .bin 文件",
                   command=self.pick_file).grid(row=0, column=0, sticky="ew",
                                                padx=(0, 3))
        self.btn_chk_fw = ttk.Button(bf_pick, text="检查更新", width=8,
                                     command=self.check_fw_update)
        self.btn_chk_fw.grid(row=0, column=1, sticky="ew")
        self.btn_chk_fw.configure(state="disabled")   # 没选文件时不可用

        tk.Label(c, textvariable=self.fw_name, bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8), wraplength=288, justify="left",
                 anchor="w").grid(row=1, column=0, columnspan=2,
                                  sticky="ew", pady=(0, 7))

        # 发送目标：两路里单选一路
        tk.Label(c, text="发送到", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=2, column=0, sticky="w", pady=1)
        self.fw_target = ttk.Combobox(c, width=7, state="readonly",
                                      values=["串口 A", "串口 B"],
                                      font=(FONT_UI, 8))
        self.fw_target.grid(row=2, column=1, sticky="e", pady=1)
        self.fw_target.current(int(self.cfg.get("send_target", 0)) % 2)

        # 块大小
        tk.Label(c, text="块大小", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=3, column=0, sticky="w", pady=1)
        self.chunk_var = tk.StringVar(value=str(self.cfg.get("chunk", "256")))
        ttk.Combobox(c, textvariable=self.chunk_var, width=7, state="readonly",
                     values=["64", "128", "256", "512", "1024", "2048"],
                     font=(FONT_MONO, 9)).grid(row=3, column=1, sticky="e", pady=1)

        # 块延时
        tk.Label(c, text="块延时 (ms)", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=4, column=0, sticky="w", pady=1)
        self.delay_var = tk.StringVar(value=str(self.cfg.get("delay", "1")))
        ttk.Combobox(c, textvariable=self.delay_var, width=7, state="readonly",
                     values=["0", "1", "2", "5", "10", "20"],
                     font=(FONT_MONO, 9)).grid(row=4, column=1, sticky="e", pady=1)

        self.auto_reset = tk.BooleanVar(
            value=self.cfg.get("auto_reset", True))
        self.chk_reset = make_check(
            c, "发送前调试器复位（抢 OTA 窗口）", self.auto_reset,
            bg=C_PANEL)
        self.chk_reset.grid(row=5, column=0, columnspan=2, sticky="w",
                            pady=(6, 2))

        # 发送 / 中止
        bf = tk.Frame(c, bg=C_PANEL)
        bf.grid(row=6, column=0, columnspan=2, sticky="ew")
        bf.columnconfigure(0, weight=2)
        bf.columnconfigure(1, weight=1)

        self.btn_go = ttk.Button(bf, text="▶ 开始发送", style="Go.TButton",
                                 command=self.start_file_send)
        self.btn_go.grid(row=0, column=0, sticky="ew", padx=(0, 4))

        self.btn_stop = ttk.Button(bf, text="■ 中止", style="Stop.TButton",
                                   command=self.abort_file_send,
                                   state="disabled")
        self.btn_stop.grid(row=0, column=1, sticky="ew")

        # 进度细节：已发 / 速度
        self.var_tx = tk.StringVar(value="")
        tk.Label(c, textvariable=self.var_tx, bg=C_PANEL, fg=C_SUB,
                 font=(FONT_MONO, 8), anchor="w", wraplength=288,
                 justify="left").grid(row=7, column=0, columnspan=2,
                                      sticky="ew", pady=(5, 0))

        tk.Label(c, text="注：SWD 复位与复位目标已移到每个接收区下方，各自对应本路板子",
                 bg=C_PANEL, fg=C_SUB, font=(FONT_UI, 7), wraplength=288,
                 justify="left", anchor="w").grid(
            row=8, column=0, columnspan=2, sticky="ew", pady=(6, 0))

    # ---- 底部：发送区（一个，选口发送）----
    def _build_sendbar(self):
        wrap = ttk.Frame(self.root)
        wrap.grid(row=2, column=0, sticky="ew", padx=12, pady=(0, 6))
        wrap.columnconfigure(0, weight=1)

        box = tk.Frame(wrap, bg=C_PANEL, highlightbackground=C_BORDER,
                       highlightthickness=1)
        box.grid(row=0, column=0, sticky="ew")
        box.columnconfigure(0, weight=1)

        self.tx = tk.Text(box, height=3, wrap="char", font=(FONT_MONO, 10),
                          bg=C_PANEL, fg=C_TEXT, insertbackground=C_TEXT,
                          relief="flat", borderwidth=0,
                          padx=8, pady=6, undo=True)
        self.tx.grid(row=0, column=0, sticky="ew", padx=(0, 0), pady=0)
        self.tx.bind("<Control-Return>", lambda e: (self.send_text(), "break")[1])

        side = tk.Frame(box, bg=C_PANEL)
        side.grid(row=0, column=1, sticky="ns", padx=8, pady=6)

        # HEX / \r\n 横排，否则这一列会比文本框还高，把发送框顶高
        opt = tk.Frame(side, bg=C_PANEL)
        opt.pack(anchor="w", pady=(0, 3))
        self.hex_tx = tk.BooleanVar(value=False)
        make_check(opt, "HEX", self.hex_tx).pack(side="left")
        self.crlf = tk.BooleanVar(value=True)
        make_check(opt, "\\r\\n", self.crlf).pack(side="left", padx=(10, 0))

        tgt = tk.Frame(side, bg=C_PANEL)
        tgt.pack(anchor="w", pady=(0, 3))
        tk.Label(tgt, text="发送到", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).pack(side="left", padx=(0, 4))
        self.txt_target = ttk.Combobox(tgt, width=7, state="readonly",
                                       values=["串口 A", "串口 B"],
                                       font=(FONT_UI, 8))
        self.txt_target.pack(side="left")
        self.txt_target.current(int(self.cfg.get("txt_target", 0)) % 2)

        ttk.Button(side, text="发送", style="Accent.TButton",
                   command=self.send_text).pack(fill="x")

        tk.Label(wrap, text="Ctrl+Enter 快捷发送    Ctrl+L 清空两个接收区",
                 bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=1, column=0, sticky="w", pady=(2, 0))

    # ---- 最底：状态栏 ----
    def _build_statusbar(self):
        bar = tk.Frame(self.root, bg="#E9EDF2", height=24)
        bar.grid(row=3, column=0, sticky="ew")
        bar.grid_propagate(False)

        self.var_conn = tk.StringVar(value="串口 A: 未连接   |   串口 B: 未连接")
        tk.Label(bar, textvariable=self.var_conn, bg="#E9EDF2", fg=C_SUB,
                 font=(FONT_UI, 8)).pack(side="left", padx=12)

        self.var_count = tk.StringVar(value="RX 0 B   |   TX 0 B")
        tk.Label(bar, textvariable=self.var_count, bg="#E9EDF2", fg=C_SUB,
                 font=(FONT_MONO, 8)).pack(side="right", padx=12)

    # ==========================================================================
    # 双串口 / 单串口切换
    # ==========================================================================

    def on_toggle_dual(self):
        self._apply_dual_mode(persist=True)

    def _apply_dual_mode(self, persist=False):
        """按「双串口」勾选切换布局。

        关掉时：
          · 串口 B 若开着先断开（隐藏的接收区不该继续占着 COM）
          · B 区隐藏，A 区跨两列占满，窗口退化成单串口
          · 两个「发送到」下拉只剩「串口 A」，避免把固件发去不存在的 B
        勾上时反向恢复，B 区回到右侧，A 区恢复单列。
        """
        dual = self.dual_var.get()
        values = ["串口 A", "串口 B"] if dual else ["串口 A"]
        for cb in (self.fw_target, self.txt_target):
            cb["values"] = values
            if not dual:
                cb.current(0)           # 强制回到 A，避免残留 B 的选择

        if dual:
            self.mid.columnconfigure(1, weight=1)
            self.ch_a.box.grid_configure(columnspan=1)
            self.ch_b.box.grid()
            self.ch_b.box.grid_configure(columnspan=1)
        else:
            if self.ch_b.ser and self.ch_b.ser.is_open:
                self.ch_b.close_port(quiet=True)
            self.ch_b.box.grid_remove()
            self.ch_a.box.grid_configure(columnspan=2)
            self.mid.columnconfigure(1, weight=0)

        self.cfg["dual_mode"] = dual
        if persist:
            cfg = load_config()
            cfg["dual_mode"] = dual
            save_config(cfg)
            self.ch_a._sys_line("--- 已切换到{}串口模式 ---".format(
                "双" if dual else "单"))
            if not dual:
                self.ch_a._sys_line("--- 串口 B 已断开；发送目标仅保留串口 A ---")

    # ==========================================================================
    # 全局操作
    # ==========================================================================

    def update_conn(self):
        parts = []
        for ch in self.channels:
            if ch.ser and ch.ser.is_open:
                parts.append("串口 {}: {} @ {} 已连接".format(
                    ch.name, ch.port_var.get().split()[0], ch.baud_var.get()))
            else:
                parts.append("串口 {}: 未连接".format(ch.name))
        self.var_conn.set("   |   ".join(parts))

    def update_counts(self):
        self.var_count.set("RX {}   |   TX {}".format(
            human_bytes(self.rx_count), human_bytes(self.tx_count)))

    def classify(self, line):
        """按规则表给一行日志选 tag：从上到下，首命中即停。

        pattern 里的 | 是「或」，每段作为一个子串去找；sys 规则的 pattern
        为空，不参与匹配（工具提示行是 _sys_line 直接挂 tag 的）。

        注意顺序敏感：FAIL 必须排在 ok 前面，否则 "verify FAIL"
        会先被 "OK" 之类命中染绿。规则表支持上下移动就是为了调这个。
        """
        s = line.strip()
        if not s:
            return None
        for r in self.log_rules:
            pat = r.get("pattern", "")
            if not pat:
                continue                     # sys 或空规则
            for kw in pat.split("|"):
                kw = kw.strip()
                if kw and kw in s:
                    return r["tag"]
        return None

    def clear_all_rx(self):
        for ch in self.channels:
            ch.clear_rx()

    def _sys_line_all(self, s):
        for ch in self.channels:
            ch._sys_line(s)

    def refresh_software(self):
        """软件内"软重启"：断两路串口、清屏、重新认调试器、重扫串口。

        副作用（和真重启一致）：
          · 两路已连接的串口都会被断开
          · 两个接收区日志、OTA 解析状态、收发计数全部清零
          · 正在发送的固件会被中止
        保留：固件路径、窗口位置、所有配置勾选 —— 真重启也是从 json 恢复它们。
        """
        # 1) 先停发送线程，再断串口；顺序反了线程会对着 None 写
        if self.tx_thread and self.tx_thread.is_alive():
            self.abort_evt.set()

        # 2) 断开并重置两路
        for ch in self.channels:
            if ch.ser and ch.ser.is_open:
                ch.close_port(quiet=True)
            ch.clear_rx()
            ch.reset_runtime()
        self.btn_go.configure(state="normal")
        self.btn_stop.configure(state="disabled")
        self.var_tx.set("")
        self.rx_count = 0
        self.tx_count = 0
        self.update_counts()
        self.update_conn()

        # 3) 重新探测调试器并同步两路按钮/提示 —— 这是本功能的核心目的
        self._probe_debugger()
        cfg_iface = self.cfg.get("ocd_iface") or ""
        for ch in self.channels:
            ch.apply_debugger(self.ocd_debuggers, cfg_iface)

        # 4) 串口列表也重扫一遍（换调试器时 COM 号可能跟着变）
        self.refresh_ports()

        # 5) 写回执，免得用户不确定到底刷没刷
        self._sys_line_all("--- 已刷新软件：串口已断开，日志与状态已重置 ---")
        if cfg_iface:
            self._sys_line_all(
                "--- 调试器：配置锁定 interface/{}（改配置才会变）---".format(
                    cfg_iface))
        for ch in self.channels:
            if ch.ocd_ready:
                ch._sys_line("--- 串口 {} 调试器：{} → interface/{}.cfg，"
                             "SWD 复位就绪 ---".format(ch.name, ch.ocd_name,
                                                       ch.ocd_iface))
            elif ch.ocd_name:
                ch._sys_line("--- 串口 {} 调试器：{} → interface/{}.cfg，"
                             "复位不可用（检查 OpenOCD/脚本）---".format(
                                 ch.name, ch.ocd_name, ch.ocd_iface))

        # 6) 多台在位时把清单打出来，方便确认「复位目标」该选哪个
        if len(self.ocd_debuggers) > 1:
            self.channels[0]._sys_line("--- 在位调试器 {} 台：{} ---".format(
                len(self.ocd_debuggers),
                "，".join("{}·{}".format(
                    d["name"], d["serial"][-8:] if d["serial"] else "无序列号")
                    for d in self.ocd_debuggers)))

    def refresh_ports(self):
        ports = list_serial_ports()
        values = [d for _, d, _ in ports]

        # 默认优先真正的 USB 串口：Python 枚举会把蓝牙虚拟串口
        # （"COM13 蓝牙链接上的标准串行"）排在前面，新电脑上直接
        # 打开往往就是它 —— 表现为收不到数据、看起来"功能怪怪的"。
        def _is_bluetooth(desc, hwid):
            return ("蓝牙" in desc or "bluetooth" in desc.lower()
                    or "BTHENUM" in (hwid or "").upper())
        usb_first = [d for _, d, h in ports if not _is_bluetooth(d, h)]
        order = usb_first + [v for v in values if v not in usb_first]

        chosen = []
        for ch in self.channels:
            cur = ch.port_var.get()
            pick = ""
            if cur in values and cur not in chosen:
                pick = cur
            elif ch.saved_port in values and ch.saved_port not in chosen:
                pick = ch.saved_port
            else:
                pick = next((v for v in order if v not in chosen),
                            values[0] if values else "")
            ch.port_cb["values"] = values
            ch.port_var.set(pick)
            fit_dropdown(ch.port_cb, values)
            chosen.append(pick)

    def open_color_settings(self):
        """规则设置窗口：增删改颜色规则、调优先级、实时预览、确定才落盘。

        pending 是深拷贝的规则列表，取消/关窗不碰主界面；
        确定后一次性应用（两个接收区 + 已显示的行）并写 json。
        """
        win = tk.Toplevel(self.root)
        win.title("日志颜色规则（两路共用）")
        win.configure(bg=C_BG)
        win.resizable(False, False)
        win.transient(self.root)
        pending = [dict(r) for r in self.log_rules]   # 深拷贝，取消不影响主界面

        # ---- 标题与说明 ----
        head = tk.Frame(win, bg=C_BG)
        head.pack(fill="x", padx=14, pady=(10, 6))
        tk.Label(head, text="日志颜色规则（自上而下匹配，首命中即停）",
                 bg=C_BG, fg=C_TEXT, font=(FONT_UI, 11, "bold")).pack(anchor="w")
        tk.Label(head,
                 text="关键词用 | 分隔多个，含任意一段即命中；顺序决定优先级，"
                      "用 ↑↓ 调整。两份接收区共用这套规则",
                 bg=C_BG, fg=C_SUB, font=(FONT_UI, 8)).pack(anchor="w", pady=(2, 0))

        # ---- 规则列表（Treeview）----
        tw = ttk.Treeview(win, columns=("name", "pattern", "color"),
                          show="headings", height=8)
        tw.heading("name", text="规则名")
        tw.heading("pattern", text="关键词（| 分隔）")
        tw.heading("color", text="字色 / 底色")
        tw.column("name", width=110, anchor="w")
        tw.column("pattern", width=190, anchor="w")
        tw.column("color", width=130, anchor="w")
        tw.pack(fill="x", padx=14)
        for r in pending:
            tw.insert("", "end", values=(r["name"], r["pattern"] or "（不匹配，仅提示色）",
                                         r["fg"] + ((" / " + r["bg"]) if r["bg"] else "")))

        pv = tk.Text(win, height=5, wrap="none", font=(FONT_MONO, 9),
                     bg="#FBFCFE", fg=C_TEXT, relief="solid", borderwidth=1,
                     padx=8, pady=4, state="normal")
        pv.pack(fill="x", padx=14, pady=(8, 2))

        def paint_preview():
            """预览区：每条规则给一行示例文字，按 pending 的颜色着色。"""
            pv.configure(state="normal")
            pv.delete("1.0", "end")
            for r in pending:
                # 示例行：取第一个关键词拼一句，sys 用工具提示示例
                kw = (r["pattern"].split("|")[0].strip() if r["pattern"] else "")
                sample = ("--- 系统提示：已连接 COM25 @ 115200 ---" if r["tag"] == "sys"
                          else "示例 [{}] {}".format(r["tag"], kw or "日志内容"))
                pv.insert("end", sample + "\n")
                pv.tag_configure(r["tag"], foreground=r["fg"],
                                 background=r["bg"] if r["bg"] else "")
                pv.tag_add(r["tag"], "end-2l", "end-1l")
            pv.tag_configure("sel", background="#0078D8")
            pv.tag_raise("sel")
            pv.configure(state="disabled")

        def selected_idx():
            sel = tw.selection()
            if not sel:
                return None
            return tw.index(sel[0])

        def refresh_list(keep=None):
            tw.delete(*tw.get_children())
            for r in pending:
                tw.insert("", "end", values=(
                    r["name"], r["pattern"] or "（不匹配，仅提示色）",
                    r["fg"] + ((" / " + r["bg"]) if r["bg"] else "")))
            kids = tw.get_children()
            if kids:
                idx = keep if keep is not None and keep < len(kids) else 0
                tw.selection_set(kids[idx])
            paint_preview()

        def edit_selected():
            """编辑选中的规则：名称/关键词/颜色/底色。"""
            i = selected_idx()
            if i is None:
                messagebox.showinfo("未选中", "请先在列表里点一条规则。", parent=win)
                return
            r = pending[i]
            dlg = tk.Toplevel(win)
            dlg.title("编辑规则 · " + r["name"])
            dlg.configure(bg=C_BG)
            dlg.resizable(False, False)
            dlg.transient(win)
            f = tk.Frame(dlg, bg=C_BG)
            f.pack(padx=16, pady=14)
            tk.Label(f, text="规则名", bg=C_BG, fg=C_TEXT,
                     font=(FONT_UI, 9)).grid(row=0, column=0, sticky="w", pady=3)
            name_e = tk.Entry(f, font=(FONT_UI, 9), width=26)
            name_e.insert(0, r["name"])
            name_e.grid(row=0, column=1, pady=3)
            tk.Label(f, text="关键词", bg=C_BG, fg=C_TEXT,
                     font=(FONT_UI, 9)).grid(row=1, column=0, sticky="w", pady=3)
            pat_e = tk.Entry(f, font=(FONT_MONO, 9), width=26)
            pat_e.insert(0, r["pattern"])
            pat_e.grid(row=1, column=1, pady=3)
            tk.Label(f, text="（sys 规则留空，不参与匹配）", bg=C_BG, fg=C_SUB,
                     font=(FONT_UI, 8)).grid(row=2, column=1, sticky="w")

            fg_var = tk.StringVar(value=r["fg"])
            bg_var = tk.StringVar(value=r["bg"] or "（无）")
            tk.Label(f, text="字色", bg=C_BG, fg=C_TEXT,
                     font=(FONT_UI, 9)).grid(row=3, column=0, sticky="w", pady=3)
            fg_btn = tk.Button(f, text="   ", bg=r["fg"], relief="ridge", bd=1,
                               width=3, cursor="hand2")
            fg_btn.grid(row=3, column=1, sticky="w")
            tk.Label(f, textvariable=fg_var, bg=C_BG, fg=C_SUB,
                     font=(FONT_MONO, 9)).grid(row=3, column=2, sticky="w", padx=(6, 0))
            tk.Label(f, text="底色", bg=C_BG, fg=C_TEXT,
                     font=(FONT_UI, 9)).grid(row=4, column=0, sticky="w", pady=3)
            bg_btn = tk.Button(f, text="   ", bg=r["bg"] or C_PANEL, relief="ridge",
                               bd=1, width=3, cursor="hand2")
            bg_btn.grid(row=4, column=1, sticky="w")

            def pick(which):
                cur = r["fg"] if which == "fg" else (r["bg"] or C_PANEL)
                c = colorchooser.askcolor(initialcolor=cur, parent=dlg,
                                          title="选择" + ("字色" if which == "fg" else "底色"))
                if c and c[1]:
                    if which == "fg":
                        fg_var.set(c[1].upper()); fg_btn.configure(bg=c[1])
                    else:
                        bg_var.set(c[1].upper()); bg_btn.configure(bg=c[1])

            fg_btn.configure(command=lambda: pick("fg"))
            bg_btn.configure(command=lambda: pick("bg"))

            def no_bg():
                bg_var.set("（无）"); bg_btn.configure(bg=C_PANEL)

            tk.Button(f, text="底色设为无", font=(FONT_UI, 8),
                      command=no_bg).grid(row=4, column=2, sticky="w", padx=(6, 0))

            def save_edit():
                r["name"] = name_e.get().strip() or r["tag"]
                r["pattern"] = pat_e.get().strip()
                r["fg"] = fg_var.get() if HEX_COLOR_RE.match(fg_var.get()) else r["fg"]
                r["bg"] = bg_var.get() if HEX_COLOR_RE.match(bg_var.get()) else ""
                dlg.destroy()
                refresh_list(i)

            btns = tk.Frame(f, bg=C_BG)
            btns.grid(row=5, column=0, columnspan=3, pady=(12, 0))
            ttk.Button(btns, text="取消", command=dlg.destroy).pack(side="right", padx=(6, 0))
            ttk.Button(btns, text="保存", style="Accent.TButton",
                       command=save_edit).pack(side="right")
            dlg.grab_set()

        def add_rule():
            n = 1
            used = {r["tag"] for r in pending}
            while "custom{}".format(n) in used:
                n += 1
            pending.append({"tag": "custom{}".format(n), "name": "新规则{}".format(n),
                            "pattern": "", "fg": C_ACCENT, "bg": ""})
            refresh_list(len(pending) - 1)

        def del_rule():
            """删除选中规则 —— 全部规则一视同仁，包括默认六条。"""
            i = selected_idx()
            if i is None:
                return
            del pending[i]
            refresh_list(min(i, len(pending) - 1) if pending else None)

        def move(delta):
            i = selected_idx()
            if i is None:
                return
            j = i + delta
            if j < 0 or j >= len(pending):
                return
            pending[i], pending[j] = pending[j], pending[i]
            refresh_list(j)

        # ---- 操作按钮行 ----
        ops = tk.Frame(win, bg=C_BG)
        ops.pack(fill="x", padx=14, pady=(6, 2))
        ttk.Button(ops, text="新增", command=add_rule).pack(side="left")
        ttk.Button(ops, text="编辑", command=edit_selected).pack(side="left", padx=(6, 0))
        ttk.Button(ops, text="删除", command=del_rule).pack(side="left", padx=(6, 0))
        ttk.Button(ops, text="↑ 上移", command=lambda: move(-1)).pack(side="left", padx=(6, 0))
        ttk.Button(ops, text="↓ 下移", command=lambda: move(1)).pack(side="left", padx=(6, 0))
        tw.bind("<Double-1>", lambda e: edit_selected())

        # ---- 底部按钮 ----
        bar = tk.Frame(win, bg=C_BG)
        bar.pack(fill="x", padx=14, pady=(6, 12))

        def restore_defaults():
            pending[:] = default_log_rules()
            refresh_list(0)

        def apply_and_close():
            # 确定才动主界面：换规则表 → 两区重设 tag → 已显示行重着色 → 落盘
            self.log_rules = [dict(r) for r in pending]
            for ch in self.channels:
                ch.apply_colors()
                ch.recolor_existing()
            self.cfg["log_rules"] = [dict(r) for r in pending]
            save_config(self.cfg)
            self._sys_line_all("--- 日志颜色规则已更新（两区共用）---")
            win.destroy()

        ttk.Button(bar, text="恢复默认", command=restore_defaults).pack(side="left")
        ttk.Button(bar, text="取消", command=win.destroy).pack(side="right", padx=(6, 0))
        ttk.Button(bar, text="确定", style="Accent.TButton",
                   command=apply_and_close).pack(side="right")

        paint_preview()
        win.update_idletasks()
        x = self.root.winfo_x() + (self.root.winfo_width() - win.winfo_width()) // 2
        y = self.root.winfo_y() + (self.root.winfo_height() - win.winfo_height()) // 2
        win.geometry("+{}+{}".format(max(x, 0), max(y, 0)))
        win.grab_set()

    # ==========================================================================
    # 普通发送（底部发送栏）
    # ==========================================================================

    def send_text(self):
        raw = self.tx.get("1.0", "end-1c")
        if not raw:
            return

        ch = self.channels[self.txt_target.current() % 2]
        if not (ch.ser and ch.ser.is_open):
            messagebox.showwarning("串口未打开",
                                   "请先打开「串口 {}」。".format(ch.name))
            return

        if self.hex_tx.get():
            try:
                payload = bytes.fromhex(
                    raw.replace(",", " ").replace("0x", " ").replace("0X", " "))
            except ValueError:
                messagebox.showerror("HEX 格式错误",
                                     "HEX 模式只能填十六进制数，例如：01 A0 FF")
                return
        else:
            payload = raw.encode("utf-8")
            if self.crlf.get():
                payload += b"\r\n"

        if ch.send_bytes(payload):
            ch._sys_line(">>> {} B: {}".format(
                len(payload),
                payload[:48].hex(" ").upper()
                + (" ..." if len(payload) > 48 else "")))

    # ==========================================================================
    # 固件发送
    # ==========================================================================

    def pick_file(self):
        path = filedialog.askopenfilename(
            title="选择固件",
            filetypes=[("固件 bin", "*.bin"), ("所有文件", "*.*")],
            initialdir=self._guess_fw_dir())
        if not path:
            return
        self._adopt_fw(path, announce=False)

    def _adopt_fw(self, path, announce=False):
        """装填一个 bin 文件：更新显示、记基准时间戳、存路径、重置状态。

        抽成函数是因为 pick_file 和 check_fw_update 都要走同一套装填逻辑 ——
        两处各写一份的话，检查更新后大小显示忘了刷新这类不一致迟早出现。
        """
        try:
            st = os.stat(path)
        except OSError as e:
            messagebox.showerror("读取失败", str(e))
            return False
        self.fw_path.set(path)
        self.fw_size = st.st_size
        # mtime 作为"有没有重新编译"的基准。只比 size 会漏掉"大小没变但
        # 内容变了"（改个常量再编译），所以两个一起记。
        self.fw_mtime = st.st_mtime
        self.fw_name.set("{}\n{:,} B  ({})".format(
            os.path.basename(path), self.fw_size, human_bytes(self.fw_size)))
        self.btn_chk_fw.configure(state="normal")
        # 路径选完立刻落盘，不再等关窗 —— 程序崩了/强杀进程也不丢上次路径
        self.cfg["last_file"] = path
        save_config(self.cfg)
        # 只重置解析状态
        for ch in self.channels:
            ch.ota_saved = 0
        self.var_tx.set("")
        if announce:
            self._sys_line_all("--- 已装填最新固件：{}（{:,} B）---".format(
                os.path.basename(path), self.fw_size))
        return True

    def check_fw_update(self):
        """检查当前选中的 bin 有没有被重新编译，变了就自动重新装填。

        判定依据 = mtime 或 size 变化：
          · mtime 变 —— 重新编译过（哪怕大小没变）
          · size 变  —— 文件被替换或截断
        不读文件内容算 CRC：发送时本来就会读，检查按钮没必要重复做。
        """
        path = self.fw_path.get()
        if not path:
            return
        if not os.path.isfile(path):
            self._sys_line_all("--- 检查更新：固件文件已不存在：{} ---".format(path))
            self.btn_chk_fw.configure(state="disabled")
            return
        try:
            st = os.stat(path)
        except OSError as e:
            self._sys_line_all("--- 检查更新失败：{} ---".format(e))
            return
        if abs(st.st_mtime - self.fw_mtime) < 1e-6 and st.st_size == self.fw_size:
            self._sys_line_all("--- 检查更新：固件未变化（{:,} B）---".format(
                st.st_size))
            return
        old_size = self.fw_size
        if self._adopt_fw(path, announce=True):
            self._sys_line_all("--- 检测到固件已更新：{:,} B → {:,} B ---".format(
                old_size, self.fw_size))

    def _guess_fw_dir(self):
        """打开目录优先用上次选过的固件所在目录；文件被删则退到其父目录"""
        last = self.cfg.get("last_file")
        if last:
            if os.path.isfile(last):
                return os.path.dirname(last)
            d = os.path.dirname(last)
            if os.path.isdir(d):
                return d
        here = app_dir()
        cand = os.path.abspath(os.path.join(here, "..", "..",
                                            "Application", "build", "Debug"))
        return cand if os.path.isdir(cand) else here

    def start_file_send(self):
        ch = self.channels[self.fw_target.current() % 2]
        if not (ch.ser and ch.ser.is_open):
            messagebox.showwarning("串口未打开",
                                   "请先打开「串口 {}」，或在「发送到」里换一路。".format(
                                       ch.name))
            return
        path = self.fw_path.get()
        if not path or not os.path.isfile(path):
            messagebox.showwarning("没有固件", "请先点「选择 .bin 文件」。")
            return
        if self.tx_thread and self.tx_thread.is_alive():
            return

        self.abort_evt.clear()
        ch.ota_saved = 0
        ch.ota_done_ts = 0.0
        ch.ota_verify = None       # 清掉上一轮的校验结论，否则会误判这次成功
        ch.ota_declared = 0
        ch.ota_crc = ""

        self.btn_go.configure(state="disabled")
        self.btn_stop.configure(state="normal")

        chunk = int(self.chunk_var.get())
        delay = float(self.delay_var.get())

        ch._sys_line("--- 开始发送 {} 到串口 {}（{} B，块 {}，延时 {}ms）---".format(
            os.path.basename(path), ch.name, self.fw_size, chunk, delay))
        self.tx_thread = threading.Thread(
            target=self._tx_worker, args=(path, chunk, delay, ch), daemon=True)
        self.tx_thread.start()

    def abort_file_send(self):
        if self.tx_thread and self.tx_thread.is_alive():
            self.abort_evt.set()
            self._sys_line_all("--- 用户中止发送 ---")

    def _tx_worker(self, path, chunk, delay_ms, ch):
        """后台线程：读文件 → 分块写目标串口。UI 更新一律走队列。"""
        total = os.path.getsize(path)
        sent = 0
        crc = 0                  # 边发边算，省一次读盘。和 MCU 那边用同一套 CRC32
        t0 = time.time()
        last_ui = 0.0
        aborted = False

        def say(txt):
            self.tx_q.put(("sys", (ch, txt)))

        try:
            if self.auto_reset.get():
                # 走 SWD 复位，且只复位"发送目标那一路"的板子。
                # 调试器不可用时不中断发送，只提示一句 —— 用户可以手动复位。
                if ch.ocd_ready:
                    ok, info = ch.reset_blocking()
                    say("--- SWD 复位 {}：{} ---".format(
                        "成功" if ok else "失败", info))
                    if ok:
                        # BootLoader 复位后先打 [ota] ready 再进窗口，等它打印完
                        time.sleep(0.3)
                    else:
                        say("!!! 复位失败，OTA 窗口可能没打开（可手动按板子复位后重发）")
                else:
                    say("!!! 串口 {} 没有可用的调试器，跳过复位直接发送".format(
                        ch.name))

            with open(path, "rb") as f:
                while sent < total:
                    if self.abort_evt.is_set():
                        aborted = True
                        break
                    block = f.read(chunk)
                    if not block:
                        break
                    ch.ser.write(block)
                    crc = zlib.crc32(block, crc)
                    sent += len(block)

                    now = time.time()
                    if now - last_ui > 0.08 or sent >= total:
                        speed = sent / max(now - t0, 1e-6)
                        self.tx_q.put(("tx", (sent, total, speed, False,
                                              False, ch)))
                        last_ui = now

                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)

        except Exception as e:
            self.tx_q.put(("error", (ch, "发送中断: {}".format(e))))
            self.tx_q.put(("tx", (sent, total, 0.0, True, True, ch)))
            return

        speed = sent / max(time.time() - t0, 1e-6)
        self.tx_q.put(("tx", (sent, total, speed, True, aborted, ch)))

        # ---------------- 收尾 ----------------
        # 新协议（proto=2）：把 {长度, CRC32} 作为 12 字节尾部发下去。
        # BootLoader 收齐后把 Flash 读回来重算 CRC，对得上就自己跳转 ——
        # 这一步不需要我们再做复位，也就不存在"复位时机早了把 tail 截掉"的问题。
        #
        # 老协议（proto=1）：没有校验能力，只能沿用"等 sum 对齐 → 复位"的老路。
        # 关键：老协议绝对不能发尾部，那 12 字节会被当成固件写进 Flash。
        if aborted or sent < total:
            return

        if ch.ota_proto >= 2:
            crc32 = crc & 0xFFFFFFFF
            # 停一下再发尾部：让 MCU 先把最后一块固件消化掉，
            # 尾部这 12 字节才更可能单独成块，不会被拆到两个读周期里
            time.sleep(0.05)
            try:
                ch.ser.write(build_ota_tail(total, crc32))
            except Exception as e:
                self.tx_q.put(("error", (ch, "尾部记录发送失败: {}".format(e))))
                return

            say("--- 已发尾部记录 len={:,} crc=0x{:08X}，等待校验 … ---".format(
                total, crc32))

            # 15KB 的 CRC 只要几毫秒，5 秒是留给串口日志和擦参数页的余量
            deadline = time.time() + 5.0
            while time.time() < deadline:
                if ch.ota_verify is not None:
                    break
                if self.abort_evt.is_set():
                    return
                time.sleep(0.05)

            if ch.ota_verify and ch.ota_verify[0] == "ok":
                say("--- 校验通过，BootLoader 已自行跳转，无需复位 ---")
            elif ch.ota_verify and ch.ota_verify[0] == "fail":
                say("!!! 校验失败，固件没有跳转。板子停在 OTA，可以直接重发")
            else:
                say("!!! 5 秒内没等到校验结果。板子可能还停在 OTA，看看日志")
            # 新协议下不再补复位：校验失败时复位也没用（它会再验一次，还是失败），
            # 校验成功时 BootLoader 已经自己跳走了。
            return

        # ---------------- 老协议兼容路径 ----------------
        # BootLoader 写完固件后停在 OTA 里等人按键，不会自己跳。
        # 这里补一次复位，让新固件直接跑起来。
        #
        # 时机不能靠固定延时：最后不足一页的 tail 要等 BootLoader 的空闲超时
        # 才落盘，这个时间不受上位机控制。发完立刻复位会把 tail 截掉，
        # 导致 Flash 里是半个固件（表现为 sum 停在最后一页边界）。
        # 正确做法是盯它自己报的 sum，够了才复位。
        if ch.ocd_ready and self.auto_reset.get():
            say("--- 等待 BootLoader 落盘确认 … ---")
            deadline = time.time() + 8.0
            while time.time() < deadline:
                if ch.ota_saved >= total:
                    break
                if self.abort_evt.is_set():
                    return
                time.sleep(0.1)

            landed = ch.ota_saved
            if landed < total:
                say("!!! 只确认落盘 {:,} / {:,} B，仍执行复位（请核对日志）".format(
                    landed, total))
            else:
                say("--- 已确认落盘 {:,} B，复位跳新固件 ---".format(landed))

            time.sleep(0.3)          # 让 tail 那页的擦写彻底结束
            ok, info = ch.reset_blocking()
            say("--- 自动复位 {}：{} ---".format(
                "成功" if ok else "失败", info))

    def _on_tx_progress(self, sent, total, speed, done, aborted=False, ch=None):
        pct = sent * 100.0 / total if total else 0
        self.var_tx.set("已发 {:,} / {:,} B  ({:.1f}%)   {:.1f} KB/s".format(
            sent, total, pct, speed / 1024.0))

        if not done:
            return

        # done 只来一次，这里的 sent 就是本次总共写出去的字节数
        self.tx_count += sent
        self.update_counts()

        self.btn_go.configure(state="normal")
        self.btn_stop.configure(state="disabled")

        if ch is None:
            return
        if aborted:
            ch._sys_line("--- 发送已中止（{} B）---".format(sent))
        elif sent >= total:
            ch._sys_line("--- 发送完毕 {} B，用时 {:.2f}s ---".format(
                sent, total / max(speed, 1e-6) if speed else 0))

    # ==========================================================================
    # 主线程轮询：固件发送线程的回执
    # ==========================================================================

    def _poll(self):
        try:
            while True:
                kind, payload = self.tx_q.get_nowait()
                if kind == "tx":
                    self._on_tx_progress(*payload)
                elif kind == "sys":
                    ch, txt = payload
                    ch._sys_line(txt)
                elif kind == "error":
                    ch, msg = payload
                    ch._on_serial_error(msg)
        except queue.Empty:
            pass

        self.root.after(30, self._poll)

    # ==========================================================================
    # 收尾
    # ==========================================================================

    def on_close(self):
        self.abort_evt.set()
        for ch in self.channels:
            ch.reader_alive = False

        # 读旧配置再覆盖本次要写的键，不再整份替换。
        # 手写的 ocd_iface / ocd_target 不由界面维护，整份替换会把它们丢掉。
        cfg = load_config()
        for ch in self.channels:
            ch.save_cfg(cfg)
        cfg.update({
            "chunk": self.chunk_var.get(),
            "delay": self.delay_var.get(),
            "auto_reset": self.auto_reset.get(),
            "last_file": self.fw_path.get(),
            # 颜色规则表随配置持久化，拷走 json 就等于换工程带走规则
            "log_rules": [dict(r) for r in self.log_rules],
            "send_target": self.fw_target.current(),
            "txt_target": self.txt_target.current(),
            "dual_mode": self.dual_var.get(),
            "geometry": self.root.geometry(),
        })
        save_config(cfg)

        for ch in self.channels:
            try:
                if ch.ser:
                    ch.ser.close()
            except Exception:
                pass
        self.root.destroy()


# ==============================================================================
# 入口
# ==============================================================================

def main():
    # DPI 感知必须在创建任何窗口之前设置。
    # 顺序反了的话，125% 缩放的屏幕上整个界面会被系统拉伸，字发虚。
    try:
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    app = OtaSerialApp(root)

    # 自检模式：验证界面能建起来 + 串口枚举是否正常，不进入事件循环。
    # 打包后用它确认 pyserial 的 Windows 后端没被漏掉。
    if "--check" in sys.argv:
        root.update_idletasks()
        root.update()
        if sys.stdout is not None:   # --noconsole 打包后 stdout 可能为 None
            print("UI self-check OK: {}x{}".format(root.winfo_width(),
                                                   root.winfo_height()))
            ports = list_serial_ports()
            print("serial ports({}): {}".format(
                len(ports), ", ".join(p[0] for p in ports) or "-"))
            for ch in app.channels:
                print("channel {}: port={} baud={} debugger={} -> interface/{}.cfg [{}]".format(
                    ch.name, ch.port_var.get() or "-", ch.baud_var.get(),
                    ch.ocd_name, ch.ocd_iface,
                    "ready" if ch.ocd_ready else "unavailable"))
            print("debuggers({}): {}".format(
                len(app.ocd_debuggers),
                ", ".join("{}@{}".format(d["name"], d["serial"] or "-")
                          for d in app.ocd_debuggers) or "-"))
            print("config: {}".format(CONFIG_PATH))
        root.destroy()
        return

    root.mainloop()


if __name__ == "__main__":
    main()
