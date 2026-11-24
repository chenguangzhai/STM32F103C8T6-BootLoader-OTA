#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
================================================================================
 STM32 OTA 串口助手
--------------------------------------------------------------------------------
 专为 STM32F103ZET6 OTA BootLoader 工程写的串口调试工具。

 相比通用串口助手（SSCOM 等），它多做了五件事：

   1. 固定 UTF-8 解码     —— 日志里的中文不会再变 "寮曡剼"
   2. 固件分块发送        —— 块大小 / 块间延时 / 实时进度 / 随时中止
   3. 自动解析 BootLoader —— 抓 [ota] page / tail 的 sum 算真实落盘进度
   4. 字节数自动对账      —— 文件大小 vs BootLoader 回报的 sum，一眼看出丢没丢
   5. 日志按来源着色      —— [boot] 蓝 / [ota] 青 / [rst] 橙 / ok 绿 / FAIL 红

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
import struct
import subprocess
import sys
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
APP_VER = "1.0"

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

# 2026-11-24：接收区着色改为「规则表」驱动 —— 每条规则 = 一组关键词 + 一组颜色。
# 换工程时只要改规则（或整个拷走 assist_config.json），不用改代码、不用重打包。
#
# 规则字段：
#   tag       Tk text tag 名，也作字典键；默认六条的 tag 固定，自定义用 custom1...
#   name      界面上显示的规则名
#   pattern   关键词，用 | 分隔多个；命中任意一个即算匹配（子串、区分大小写）
#   fg / bg   前景/背景色，bg 为空串表示不设背景
# 2026-11-24 晚：移除 builtin 标记 —— 用户要求「全部规则都能随意改」，
# 默认六条也只是"初始值"，同样可删可改；删光后点「恢复默认」能一键找回。
# 匹配顺序 = 列表顺序，首命中即停。所以 FAIL 默认排在 ok 之前 ——
# 否则 "[ota] verify FAIL" 会先被 ok 规则染绿，失败就不显眼了。
# 顺序由用户用 ↑↓ 自行调整，代码不再强制任何规则的位置。
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
    # 用户完全可以删掉它或给它写关键词 —— 删掉后工具提示行失去灰色，
    # 显示默认字色（不会报错），这是"全部可改"的预期结果。
    {"tag": "sys",  "name": "系统提示",     "pattern": "",
     "fg": C_SUB,  "bg": ""},
]
HEX_COLOR_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")


def default_log_rules():
    """返回默认规则表的深拷贝。每次调用都新建，避免界面改动污染常量。"""
    return [dict(r) for r in DEFAULT_LOG_RULES]


def load_log_rules(cfg):
    """从配置读规则表；缺项/坏值回退默认色，绝不把非法颜色交给 Tk。

    2026-11-24：兼容上一版的 log_colors 字典 —— 老用户升级后原来改过的
    六种颜色不会丢；新的 log_rules 优先（存在且是 list 时就以它为准）。
    """
    defaults = default_log_rules()
    raw = cfg.get("log_rules")
    if isinstance(raw, list):
        # 2026-11-24 修正：配置保存的是「完整规则表」，加载以配置为唯一来源。
        # 2026-11-24 晚再修正：不再补齐缺失的默认规则 —— 用户要求全部可删，
        # 若这里把删掉的补回来，删除就形同虚设（关窗重开规则又冒出来）。
        # 只有 log_rules 整个键不存在（首次运行/老版本）才用 default_log_rules()。
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
    ("VID_1366&PID_0101", "jlink",     "J-Link"),
    ("VID_1366&PID_0105", "jlink",     "J-Link"),
]


def detect_debugger():
    """认当前插着的调试器，返回 (接口脚本名, 显示名)：('stlink', 'ST-Link V2')。

    只认「此刻真的连着」的设备。不能用注册表 Enum\\USB 判断 —— 拔掉的
    设备键还留在里面，换调试器时会认成旧的那个，然后复位必然失败。
    CFGMGR32 的 PRESENT 过滤才是准的。
    """
    if os.name != "nt":
        return None, None
    try:
        import ctypes
        from ctypes import wintypes

        cm = ctypes.windll.cfgmgr32
        CM_GETIDLIST_FILTER_PRESENT = 0x00000100

        size = wintypes.ULONG(0)
        if cm.CM_Get_Device_ID_List_SizeW(
                ctypes.byref(size), None,
                CM_GETIDLIST_FILTER_PRESENT) != 0:
            return None, None
        buf = ctypes.create_unicode_buffer(size.value)
        if cm.CM_Get_Device_ID_ListW(
                None, buf, size.value,
                CM_GETIDLIST_FILTER_PRESENT) != 0:
            return None, None
        ids = bytes(buf).decode("utf-16-le", "ignore").upper()
    except Exception:
        return None, None

    for vidpid, iface, label in DEBUGGERS:
        if vidpid in ids:
            return iface, label
    return None, None


def find_openocd():
    """返回 (openocd.exe 绝对路径, scripts 目录)；找不到返回 (None, None)"""
    pats = [
        os.path.expanduser("~/.eide/tools/openocd*/bin/openocd.exe"),
        os.path.expanduser("~/.eide/tools/openocd*/bin/openocd"),
        "C:/Program Files/OpenOCD*/bin/openocd.exe",
        "C:/Program Files (x86)/OpenOCD*/bin/openocd.exe",
        "/usr/bin/openocd",
    ]
    for pat in pats:
        for exe in sorted(glob.glob(pat), reverse=True):
            if not os.path.isfile(exe):
                continue
            root = os.path.dirname(os.path.dirname(exe))
            for sub in ("share/openocd/scripts", "share/openocd/scripts/"):
                scr = os.path.join(root, *sub.split("/"))
                if os.path.isdir(os.path.join(scr, "target")):
                    return exe, scr
            # Linux 包管理装的位置
            if os.path.isdir("/usr/share/openocd/scripts/target"):
                return exe, "/usr/share/openocd/scripts"
    return None, None


def openocd_reset(exe, scr, interface="cmsis-dap", target="stm32f1x",
                  timeout=25):
    """让调试器复位目标板并放开运行。返回 (成功?, 说明文本)"""
    icfg = os.path.join(scr, "interface", interface + ".cfg")
    tcfg = os.path.join(scr, "target", target + ".cfg")
    if not os.path.isfile(icfg):
        return False, "找不到接口配置：{}".format(icfg)
    if not os.path.isfile(tcfg):
        return False, "找不到芯片配置：{}".format(tcfg)

    cmd = [exe, "-f", icfg, "-f", tcfg,
           "-c", "init; reset run; shutdown"]
    # Windows 下别弹出黑色控制台窗口
    flags = 0x08000000 if os.name == "nt" else 0
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
    except subprocess.TimeoutExpired:
        return False, "OpenOCD 超时（{}s），调试器可能没插或正被占用".format(timeout)
    except FileNotFoundError:
        return False, "OpenOCD 可执行文件不存在"

    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        brief = [l for l in out.splitlines()
                 if "Error" in l or "error" in l][-3:]
        return False, "OpenOCD 失败（exit {}）：{}".format(
            r.returncode, " | ".join(brief) or out.splitlines()[-1:] or "")
    if "SWD" not in out and "JTAG" not in out and "DPIDR" not in out:
        return False, "调试器未响应：{}".format(
            (out.splitlines() or ["无输出"])[-1])
    return True, "调试器复位完成"


# ==============================================================================
# 协议打包（GUI 与无界面调试端 ota_headless.py 共用）
# ==============================================================================
# 抽成模块级函数，是因为字节定义必须和 C 端逐字节一致：
#   老协议尾部（12 字节，'OTAE' + len + crc32）
# 一旦 GUI 里写一份、调试脚本里再写一份，两边漂移的症状是"发送说成功、
# 板子报 crc mismatch"，而且不报任何错 —— 这类坑排查成本极高。
# 2026-11-24：按用户要求移除「新架构」（App 直收 + FWUP 32 字节槽头）整条路径，
# 连同 ota_headless.py 的 app-ota 模式一起删除；本工具不再有 FWUP 字节定义。

OTA_TAIL_MAGIC = b"OTAE"


def build_ota_tail(fw_len, fw_crc):
    """老协议（BootLoader 串口收固件）的 12 字节尾部。"""
    return struct.pack("<4sII", OTA_TAIL_MAGIC, fw_len, fw_crc)


# ==============================================================================
# 主窗口
# ==============================================================================

class OtaSerialApp(object):

    # ------------------------------------------------------------------ 初始化
    def __init__(self, root):
        self.root = root
        self.cfg = load_config()

        # 2026-11-24：规则表从配置读，非法颜色/非法 tag 名在 load_log_rules
        # 里就挡掉了 —— Tk 收到坏颜色会在 tag_configure 抛 TclError，
        # 界面直接起不来，不能让手改 json 把软件弄崩。
        self.log_rules = load_log_rules(self.cfg)

        root.title("{}  v{}".format(APP_TITLE, APP_VER))
        root.geometry(self.cfg.get("geometry") or "1280x900")
        root.minsize(1040, 840)
        root.configure(bg=C_BG)

        # ---- 串口对象与线程状态 ----
        self.ser = None
        self.reader_alive = False
        self.reader_thread = None
        self.rx_queue = queue.Queue()        # (kind, payload) 主线程消费

        # 注：这里曾经有个 rx_epoch 分代号，用来在"清空缓冲"时丢弃
        # reader 手上正在读的那一块。后来确认需求只是清屏 —— 清屏不该
        # 影响接收，丢掉的是板子刚发来的数据 —— 所以连同代号一起撤了。

        # ---- 发送线程 ----
        self.tx_thread = None
        self.abort_evt = threading.Event()

        # ---- 解码器（增量式，解决多字节字符被切开变乱码）----
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")

        # ---- HEX 显示缓冲 ----
        self.hex_buf = b""
        self.hex_off = 0
        self.hex_last_rx = 0.0

        # ---- 解析行缓冲 ----
        self.line_buf = b""

        # ---- OTA 解析状态（原先挂在右侧 OTA 状态卡片上，卡片已移除）----
        # 只保留发送流程真正依赖的数据：ota_saved / ota_proto / ota_verify
        # 决定尾部记录发不发、何时复位；ota_done_ts 用来识别重启后重复的 ready。
        # 纯显示用的字段与标签全部删掉，不再构造那块面板。
        self.ota_base = ""          # App 基址，如 0x08008000
        self.ota_page = ""          # 页大小，如 2048
        self.ota_saved = 0          # BootLoader 已确认落盘的字节数（sum）
        self.ota_proto = 1          # 协议版本：1 = 老版（无校验），2 = 支持尾部 + CRC
        self.ota_verify = None      # None 未出结果 / ("ok", 长度) / ("fail", 0)
        self.ota_declared = 0       # 尾部记录里声明的固件长度
        self.ota_crc = ""           # 尾部记录里带的 CRC（字符串）
        self.fw_size = 0            # 当前选中的固件文件大小
        # 2026-11-24：选文件时记下 mtime，供「检查更新」按钮判断是否重新编译过。
        # 初值 0 表示还没选文件；按钮此时是 disabled，不会被误用。
        self.fw_mtime = 0.0
        self.ota_done_ts = 0.0      # 最近一次升级完成的时刻（0 = 还没跑过）

        # ---- 调试器（DAP-Link / ST-Link 经 OpenOCD 复位）----
        # 2026-11-24：探测逻辑抽成 _probe_debugger()，「刷新软件」按钮复用。
        # 原先写死在 __init__ 里，换调试器只能关窗重开才生效。
        self._probe_debugger()

        # ---- 收发计数 ----
        self.rx_count = 0
        self.tx_count = 0

        # ---- 接收区最大保留行数 ----
        self.rx_max_lines = 5000

        self._init_style()
        self._build_ui()
        self.refresh_ports()

        # Ctrl+L 清空接收区，和终端里清屏的习惯一致。
        # 绑在 root 上而不是 Text 上：焦点在输入框时也想能按。
        # 末尾返回 "break" 阻止 Tk 继续往上冒泡（否则某些主题会吃掉按键）。
        root.bind("<Control-l>",
                  lambda e: (self.clear_rx(), "break")[1])

        # 关闭窗口时先收拾线程，否则 python 会卡在 join
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self._poll()

    def _probe_debugger(self):
        """重新探测 OpenOCD 与当前插着的调试器，只更新 self.ocd_* 数据。

        2026-11-24：从 __init__ 抽出，供「刷新软件」复用。
        注意配置里手写的 ocd_iface 优先于现场探测 —— 那是给"认错了"
        留的逃生口，刷新时不能悄悄覆盖用户明确写下的选择。
        """
        self.ocd_exe, self.ocd_scr = find_openocd()

        # 调试器类型：配置里显式指定 > 现场探测 > 默认按 DAP-Link 猜
        _probed_iface, _probed_name = detect_debugger()
        self.ocd_iface = (self.cfg.get("ocd_iface") or _probed_iface
                          or "cmsis-dap")
        self.ocd_target = self.cfg.get("ocd_target", "stm32f1x")
        self.ocd_name = _probed_name or {
            "cmsis-dap": "DAP-Link",
            "stlink":    "ST-Link",
            "jlink":     "J-Link",
        }.get(self.ocd_iface, self.ocd_iface.upper())

        # 接口脚本必须真的存在，否则 OpenOCD 一启动就报错，不如提前置灰
        self.ocd_iface_ok = bool(
            self.ocd_scr
            and os.path.isfile(os.path.join(self.ocd_scr, "interface",
                                            self.ocd_iface + ".cfg")))
        self.ocd_ready = bool(self.ocd_exe) and self.ocd_iface_ok
        return _probed_iface, _probed_name

    def _apply_debugger_ui(self):
        """把 ocd_* 状态同步到复位按钮和底部提示（探测完必须调一次）。

        2026-11-24：从 _build_fw_card 抽出，刷新后不用重建控件就能更新。
        """
        if not self.ocd_exe:
            self.ocd_hint.set("未找到 OpenOCD，请手动按板子复位")
        elif not self.ocd_iface_ok:
            self.ocd_hint.set("OpenOCD 缺 interface/{}.cfg，请手动复位".format(
                self.ocd_iface))
        else:
            self.ocd_hint.set("SWD 复位就绪 · " + self.ocd_name)
        self.lb_ocd_hint.configure(fg=C_OK if self.ocd_ready else C_BAD)
        self.btn_rst.configure(state="normal" if self.ocd_ready else "disabled")

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
        self._build_middle()      # row 1  接收区 + 右侧面板
        self._build_sendbar()     # row 2
        self._build_statusbar()   # row 3

    # ---- 顶部：串口配置 ----
    def _build_topbar(self):
        bar = ttk.Frame(self.root)
        bar.grid(row=0, column=0, sticky="ew", padx=12, pady=(10, 6))

        ttk.Label(bar, text="端口").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(bar, textvariable=self.port_var,
                                    width=26, state="readonly",
                                    font=(FONT_MONO, 9))
        self.port_cb.pack(side="left", padx=(6, 4))

        ttk.Button(bar, text="刷新", width=6,
                   command=self.refresh_ports).pack(side="left", padx=(0, 14))

        ttk.Label(bar, text="波特率").pack(side="left")
        self.baud_var = tk.StringVar(value=str(self.cfg.get("baud", "115200")))
        ttk.Combobox(bar, textvariable=self.baud_var, width=9,
                     values=BAUD_LIST, state="readonly",
                     font=(FONT_MONO, 9)).pack(side="left", padx=(6, 14))

        self.btn_open = ttk.Button(bar, text="打开串口", width=11,
                                   style="Accent.TButton",
                                   command=self.toggle_port)
        self.btn_open.pack(side="left")

        # 2026-11-24：软件内"软重启"，换调试器不用关窗再开。
        # 和左边那个「刷新」区分开：那个只重扫串口，这个会断串口+清屏+
        # 重新认调试器，等价于重开软件。
        ttk.Button(bar, text="刷新软件", width=9,
                   command=self.refresh_software).pack(side="left", padx=(10, 0))

        # 2026-11-24：日志字色设置窗口，和「刷新软件」一样放左侧按钮组
        ttk.Button(bar, text="设置", width=6,
                   command=self.open_color_settings).pack(side="left", padx=(6, 0))

        # ---- 右侧显示选项（pack side=right 是从右往左排，注意顺序反着写）----
        self.disp_mode = tk.StringVar(value=self.cfg.get("disp_mode", "UTF-8"))
        cb_mode = ttk.Combobox(bar, textvariable=self.disp_mode, width=7,
                               values=["UTF-8", "GBK", "HEX"], state="readonly",
                               font=(FONT_MONO, 9))
        cb_mode.pack(side="right")
        cb_mode.bind("<<ComboboxSelected>>", lambda e: self.on_disp_mode_change())
        ttk.Label(bar, text="显示").pack(side="right", padx=(0, 6))

        self.autoscroll = tk.BooleanVar(value=self.cfg.get("autoscroll", True))
        self._chk(bar, "自动滚动", self.autoscroll, bg=C_BG).pack(
            side="right", padx=(0, 14))

        self.timestamp = tk.BooleanVar(value=self.cfg.get("timestamp", False))
        self._chk(bar, "时间戳", self.timestamp, bg=C_BG).pack(
            side="right", padx=(0, 14))

    # ---- 中部：接收区 + 右侧固件发送面板 ----
    # OTA 状态卡片已按用户要求移除；BootLoader 日志仍由 _handle_line 解析，
    # 发送流程依赖的状态照常维护，只是不再画到界面上。
    def _build_middle(self):
        mid = ttk.Frame(self.root)
        mid.grid(row=1, column=0, sticky="nsew", padx=12, pady=(0, 6))
        mid.columnconfigure(0, weight=1)
        mid.rowconfigure(0, weight=1)

        # ------- 左：接收区 -------
        left = ttk.Frame(mid)
        left.grid(row=0, column=0, sticky="nsew")
        left.columnconfigure(0, weight=1)
        left.rowconfigure(0, weight=1)

        # 接收区必须保持 normal 才能用鼠标/键盘选中文字复制。
        # Tk 的 disabled Text 连选中都不允许（这正是红字日志复制不了的根因），
        # 所以用 normal + 按键白名单：只放行导航/复制键，编辑键一律吞掉。
        self.rx = tk.Text(left, wrap="char", font=(FONT_MONO, 10),
                          bg="#FBFCFE", fg=C_TEXT, insertbackground=C_TEXT,
                          relief="solid", borderwidth=1,
                          padx=8, pady=6, spacing1=1, spacing3=1,
                          undo=False, state="normal")
        self.rx.grid(row=0, column=0, sticky="nsew")
        self.rx.bind("<Key>", self._on_rx_key)

        # 右键菜单：复制选中 / 复制全部日志，省得给智能体贴日志还要先手动全选
        self.rx_menu = tk.Menu(self.root, tearoff=0, font=(FONT_UI, 9))
        self.rx_menu.add_command(label="复制选中", command=self._copy_rx_selection)
        self.rx_menu.add_command(label="复制全部日志", command=self._copy_all_rx)
        self.rx_menu.add_separator()
        self.rx_menu.add_command(label="全选", command=self._select_all_rx)
        self.rx.bind("<Button-3>",
                     lambda e: (self.rx_menu.tk_popup(e.x_root, e.y_root), "break")[1])

        sb = ttk.Scrollbar(left, orient="vertical", command=self.rx.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.rx.configure(yscrollcommand=sb.set)

        # 着色规则（红字只是文本 tag，不影响选中与复制）
        # 2026-11-24：字色改为可配置，初值和「设置」保存走同一个函数。
        self._apply_log_colors()

        # sel 必须显式提权：Tk 的 tag 优先级按创建顺序递增，内置的 sel 排在
        # 用户标签之前，bad 的浅红背景会盖住选区高亮 —— 红字行"看着没选中"
        # 的根因就在这里（选区其实已包含它，剪贴板能拿到）。
        # 只设 background、不设 foreground：选中后仍按来源着色，便于分辨日志类型。
        self.rx.tag_configure("sel", background="#0078D8")
        self.rx.tag_raise("sel")

        # ------- 右：只保留固件发送面板 -------
        # 不写死宽度，让右栏按内容自身需求占位。
        # 之前锁死 344px + grid_propagate(False)，一旦内容最小宽度超过它，
        # 控件就会被画到边界外、被窗口裁掉，而且极难察觉。
        right = ttk.Frame(mid)
        right.grid(row=0, column=1, sticky="ns", padx=(10, 0))
        self._build_fw_card(right)

    def _apply_log_colors(self):
        """把规则表里的颜色应用到接收区所有 tag。

        2026-11-24：改为遍历 self.log_rules（含用户自定义规则）。
        sel 每次都重新 tag_raise —— 新建 tag 会改变优先级顺序，
        不提权的话坏行背景又会盖住选区高亮，回到"选中看不出来"的老问题。
        """
        for r in self.log_rules:
            opts = {"foreground": r["fg"]}
            if r["bg"]:
                opts["background"] = r["bg"]
            self.rx.tag_configure(r["tag"], **opts)
        self.rx.tag_raise("sel")

    def _clear_all_log_tags(self):
        """清掉接收区所有日志 tag（保留 sel 选区）。

        改规则后必须先把旧 tag 摘干净再按新规则重打 ——
        否则删掉的规则残留、顺序变了的行还挂旧颜色，越改越乱。
        """
        for r in self.log_rules:
            self.rx.tag_remove(r["tag"], "1.0", "end")
        # 旧配置里可能有现在已删掉的 tag，也一并清（tag_names() 全量扫）
        for t in self.rx.tag_names():
            if t != "sel":
                self.rx.tag_remove(t, "1.0", "end")

    def _recolor_existing(self):
        """按当前规则表重新给屏幕上已有行着色（确定设置后调用）。

        工具提示行（sys）不参与匹配，靠文本前缀 "--- " 识别后直接挂 sys。
        这是保守做法：用户日志里极少以 "--- " 开头，误判代价很低。
        """
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
            tag = self._classify(line)
            if tag and tag != "sys":
                self.rx.tag_add(tag, a, b)

    def open_color_settings(self):
        """规则设置窗口：增删改颜色规则、调优先级、实时预览、确定才落盘。

        2026-11-24 重写。从"只能改六种固定字色"升级成规则表编辑器 ——
        换工程时改规则即可，不用改代码重打包。
        pending 是深拷贝的规则列表，取消/关窗不碰主界面；
        确定后一次性应用（含给已显示的行重新着色）+ 写 json。
        """
        win = tk.Toplevel(self.root)
        win.title("日志颜色规则")
        win.configure(bg=C_BG)
        win.resizable(False, False)
        win.transient(self.root)
        pending = [dict(r) for r in self.log_rules]   # 深拷贝，取消不影响主界面

        # ---- 标题与说明 ----
        head = tk.Frame(win, bg=C_BG)
        head.pack(fill="x", padx=14, pady=(10, 6))
        tk.Label(head, text="日志颜色规则（自上而下匹配，首命中即停）",
                 bg=C_BG, fg=C_TEXT, font=(FONT_UI, 11, "bold")).pack(anchor="w")
        tk.Label(head, text="关键词用 | 分隔多个，含任意一段即命中；顺序决定优先级，用 ↑↓ 调整",
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
            """删除选中规则 —— 2026-11-24 晚：全部规则一视同仁，包括默认六条。

            不需要确认弹窗：取消/关窗都不会落盘，误删只要不点确定就没影响；
            真删错了点「恢复默认」能一键找回（或按原来重新加一条）。
            """
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
            # 确定才动主界面：换规则表 → 重设 tag → 给已显示的行重着色 → 落盘
            self.log_rules = [dict(r) for r in pending]
            self._apply_log_colors()
            self._recolor_existing()
            self.cfg["log_rules"] = [dict(r) for r in pending]
            save_config(self.cfg)
            self._sys_line("--- 日志颜色规则已更新 ---")
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

    def _chk(self, parent, text, var, bg=None):
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
        # OTA 状态卡片移除后，固件发送面板上移到第 0 行。
        c = self._card(parent, "固件发送", 0)

        self.fw_path = tk.StringVar(value="")
        self.fw_name = tk.StringVar(value="（未选择文件）")

        # 2026-11-24：选文件 + 检查更新并排。检查按钮只对比当前选中 bin 的
        # mtime/size，变了就自动重新装填（用户确认过不要弹窗打断）。
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

        # 块大小
        tk.Label(c, text="块大小", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=2, column=0, sticky="w", pady=1)
        self.chunk_var = tk.StringVar(value=str(self.cfg.get("chunk", "256")))
        ttk.Combobox(c, textvariable=self.chunk_var, width=7, state="readonly",
                     values=["64", "128", "256", "512", "1024", "2048"],
                     font=(FONT_MONO, 9)).grid(row=2, column=1, sticky="e", pady=1)

        # 块延时
        tk.Label(c, text="块延时 (ms)", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=3, column=0, sticky="w", pady=1)
        self.delay_var = tk.StringVar(value=str(self.cfg.get("delay", "1")))
        ttk.Combobox(c, textvariable=self.delay_var, width=7, state="readonly",
                     values=["0", "1", "2", "5", "10", "20"],
                     font=(FONT_MONO, 9)).grid(row=3, column=1, sticky="e", pady=1)

        self.auto_reset = tk.BooleanVar(
            value=self.cfg.get("auto_reset", self.ocd_ready))
        self.chk_reset = self._chk(c, "发送前调试器复位（抢 OTA 窗口）",
                                   self.auto_reset)
        self.chk_reset.grid(row=4, column=0, columnspan=2, sticky="w",
                            pady=(6, 2))

        # 2026-11-24：按用户要求移除「新架构」开关及其固件版本号输入。
        # 发送现在固定走 BootLoader 老协议：SWD 抢窗口 → 裸流 → OTAE 尾 → 等板子校验跳转。
        # 副作用：不再支持 App 直收 FWUP 槽头的路径，相关配置键也不再写入。

        # 发送 / 中止
        bf = tk.Frame(c, bg=C_PANEL)
        bf.grid(row=5, column=0, columnspan=2, sticky="ew")
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
                 font=(FONT_MONO, 8), anchor="w").grid(
            row=6, column=0, columnspan=2, sticky="ew", pady=(5, 0))

        # 复位：DAP-Link 的串口 DTR 不接 NRST，必须走 SWD。
        # width 单位是字符，必须限死，否则 ttk 按钮按文字撑出 147px 最小宽度、
        # 把卡片顶宽，右对齐的控件会被画到窗口外且 winfo 量不出来。
        rf = tk.Frame(c, bg=C_PANEL)
        rf.grid(row=7, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        rf.columnconfigure(0, weight=2)
        rf.columnconfigure(1, weight=1)

        self.btn_rst = ttk.Button(rf, text="SWD 复位", width=9,
                                  command=self.reset_via_debugger)
        self.btn_rst.grid(row=0, column=0, sticky="ew", padx=(0, 3))
        if not self.ocd_ready:
            self.btn_rst.configure(state="disabled")

        ttk.Button(rf, text="唤醒", width=5, command=self.wake_ota).grid(
            row=0, column=1, sticky="ew")

        # 调试器状态一行说明，找不到时告诉用户原因。
        # 2026-11-24：label 存引用，刷新后由 _apply_debugger_ui 改文字/颜色；
        # 复位按钮的可用性也归它管，两处状态不能各写一份。
        self.ocd_hint = tk.StringVar()
        self.lb_ocd_hint = tk.Label(c, textvariable=self.ocd_hint, bg=C_PANEL,
                                    font=(FONT_UI, 7), anchor="w")
        self.lb_ocd_hint.grid(row=8, column=0, columnspan=2, sticky="ew",
                              pady=(3, 0))
        self._apply_debugger_ui()

    # ---- 底部：发送区 ----
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
        opt.pack(anchor="w", pady=(0, 4))

        self.hex_tx = tk.BooleanVar(value=False)
        self._chk(opt, "HEX", self.hex_tx).pack(side="left")

        self.crlf = tk.BooleanVar(value=True)
        self._chk(opt, "\\r\\n", self.crlf).pack(side="left", padx=(10, 0))

        ttk.Button(side, text="发送", style="Accent.TButton",
                   command=self.send_text).pack(fill="x")

        tk.Label(wrap, text="Ctrl+Enter 快捷发送", bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=1, column=0, sticky="w", pady=(2, 0))

    # ---- 最底：状态栏 ----
    def _build_statusbar(self):
        bar = tk.Frame(self.root, bg="#E9EDF2", height=24)
        bar.grid(row=3, column=0, sticky="ew")
        bar.grid_propagate(False)

        self.var_conn = tk.StringVar(value="未连接")
        tk.Label(bar, textvariable=self.var_conn, bg="#E9EDF2", fg=C_SUB,
                 font=(FONT_UI, 8)).pack(side="left", padx=12)

        self.var_count = tk.StringVar(value="RX 0 B   |   TX 0 B")
        tk.Label(bar, textvariable=self.var_count, bg="#E9EDF2", fg=C_SUB,
                 font=(FONT_MONO, 8)).pack(side="right", padx=12)

        # 文案回到"清空接收"：它清的是接收区这一屏字，
        # 串口缓冲、待渲染队列、解析状态全都原样保留，名字得说清楚
        tk.Button(bar, text="清空接收", relief="flat", bg="#E9EDF2", fg=C_SUB,
                  font=(FONT_UI, 8), bd=0, activebackground="#DCE2E9",
                  cursor="hand2", command=self.clear_rx).pack(side="right", padx=4)

        tk.Button(bar, text="保存日志", relief="flat", bg="#E9EDF2", fg=C_SUB,
                  font=(FONT_UI, 8), bd=0, activebackground="#DCE2E9",
                  cursor="hand2", command=self.save_log).pack(side="right", padx=4)

    # ==========================================================================
    # 串口开关
    # ==========================================================================

    def refresh_software(self):
        """软件内"软重启"：断串口、清屏、重新认调试器、重扫串口。

        2026-11-24 新增。起因：调试器只在 __init__ 探测一次，换 DAP-Link /
        ST-Link 后必须关窗重开，否则 SWD 复位还按旧 interface 脚本走。
        这里把重开窗口才会重做的事做一遍，省掉一次完整重启。

        副作用（和真重启一致，用户已确认要"整个软件刷新"）：
          · 已连接的串口会被断开（换调试器通常也要换线，留着没意义）
          · 接收区日志、OTA 解析状态、收发计数全部清零
          · 正在发送的固件会被中止
        保留：固件路径、窗口位置、所有配置勾选 —— 真重启也是从 json 恢复它们。
        """
        # 1) 先停发送和接收线程，再断串口；顺序反了线程会对着 None 写
        if self.tx_thread and self.tx_thread.is_alive():
            self.abort_evt.set()
        if self.ser and self.ser.is_open:
            self.close_port()
        self.btn_go.configure(state="normal")
        self.btn_stop.configure(state="disabled")

        # 2) 清屏并重置所有和"新开一个窗口"等价的运行时状态
        self.clear_rx()
        self.line_buf = b""
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")
        self.rx_count = 0
        self.tx_count = 0
        self._update_counts()
        self.var_tx.set("")
        self.ota_base = ""
        self.ota_page = ""
        self.ota_saved = 0
        self.ota_proto = 1
        self.ota_verify = None
        self.ota_declared = 0
        self.ota_crc = ""
        self.ota_done_ts = 0.0

        # 3) 重新探测调试器并同步按钮/提示 —— 这是本功能的核心目的
        probed_iface, probed_name = self._probe_debugger()
        self._apply_debugger_ui()

        # 4) 串口列表也重扫一遍（换调试器时 COM 号可能跟着变）
        self.refresh_ports()

        # 5) 写一条可见的回执，免得用户不确定到底刷没刷
        self._sys_line("--- 已刷新软件：串口已断开，日志与状态已重置 ---")
        if self.cfg.get("ocd_iface"):
            self._sys_line("--- 调试器：配置锁定 interface/{}（改配置才会变）---".format(
                self.ocd_iface))
        elif probed_name:
            self._sys_line("--- 调试器：{} → interface/{}.cfg，SWD 复位{} ---".format(
                probed_name, self.ocd_iface,
                "就绪" if self.ocd_ready else "不可用（检查 OpenOCD/脚本）"))
        else:
            self._sys_line("--- 调试器：未识别到在位的 DAP/ST/J-Link，"
                           "SWD 复位不可用，可手动按复位键 ---")

    def refresh_ports(self):
        ports = list_serial_ports()
        values = [d for _, d, _ in ports]
        self.port_cb["values"] = values
        if not values:
            self.port_var.set("")
            return

        cur = self.port_var.get()
        if cur in values:
            return
        # 上次用的那个口还插着，就接着用它
        saved = self.cfg.get("port")
        if saved in values:
            self.port_var.set(saved)
            return
        # 列表已经按"真实 USB 串口优先、蓝牙虚拟口靠后"排好序，取第一个即可
        self.port_var.set(values[0])

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
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")
        self.hex_buf = b""
        self.hex_off = 0
        self.line_buf = b""

        self.btn_open.configure(text="关闭串口")
        self.var_conn.set("已连接  {}  {}".format(dev, baud))
        self._sys_line("--- 已连接 {} @ {} ---".format(dev, baud))

    def close_port(self):
        # 先让发送线程停下
        self.abort_evt.set()
        self.reader_alive = False

        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=0.6)

        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

        self.btn_open.configure(text="打开串口")
        self.var_conn.set("未连接")
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
                self.rx_queue.put(("error", str(e)))
                break
            if data:
                self.rx_queue.put(("data", data))

    # ==========================================================================
    # 主线程轮询：把队列里的东西刷到界面
    # ==========================================================================

    def _poll(self):
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "data":
                    self._on_rx(payload)
                elif kind == "error":
                    self._on_serial_error(payload)
                elif kind == "tx":
                    self._on_tx_progress(*payload)
        except queue.Empty:
            pass

        # HEX 模式下，不足 16 字节的尾巴要定时冲出来，否则要等到下一批数据
        if (self.disp_mode.get() == "HEX" and self.hex_buf
                and time.time() - self.hex_last_rx > 0.15):
            self._flush_hex()

        self.root.after(30, self._poll)

    def _on_serial_error(self, msg):
        self._sys_line("--- 串口异常: {} ---".format(msg))
        self.close_port()

    def _on_rx(self, data):
        self.rx_count += len(data)
        self._update_counts()
        self._feed_parser(data)         # 解析永远基于原始字节，与显示模式无关
        self._render(data)

    def _update_counts(self):
        self.var_count.set("RX {}   |   TX {}".format(
            human_bytes(self.rx_count), human_bytes(self.tx_count)))

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

        dec = self.dec_utf8 if mode == "UTF-8" else self.dec_gbk
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
            tag = self._classify(line)
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
        # 不再切 state：Text 保持 normal 才能选中复制，编辑键由 _on_rx_key 拦截。
        self.rx.insert("end", s, tag or ())
        # 超量裁剪，保证长时间开着也不吃内存
        total = int(self.rx.index("end-1c").split(".")[0])
        if total > self.rx_max_lines:
            self.rx.delete("1.0", "{}.0".format(total - self.rx_max_lines + 500))
        if self.autoscroll.get() or at_end:
            self.rx.see("end")

    def _sys_line(self, s):
        # 工具提示行固定挂 "sys" tag。若用户在设置里删掉了 sys 规则，
        # Tk 对未配置的 tag 名只是不着色（不会抛异常），显示默认字色 ——
        # 这是"全部规则可删"的预期结果，不必在这里补规则。
        self._insert(s + "\n", "sys")

    # 接收区只读但可选中：按键白名单 + 右键复制
    # ------------------------------------------------------------------
    _RX_NAV_KEYS = frozenset((
        "Left", "Right", "Up", "Down", "Home", "End", "Prior", "Next",
        "Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R",
        "Caps_Lock", "Num_Lock", "Escape",
    ))

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
        self.root.clipboard_clear()
        self.root.clipboard_append(txt)

    def _copy_all_rx(self):
        """右键「复制全部日志」：整屏文本进剪贴板，方便直接贴给智能体分析。"""
        txt = self.rx.get("1.0", "end-1c")
        if not txt:
            return
        self.root.clipboard_clear()
        self.root.clipboard_append(txt)

    def _select_all_rx(self):
        """全选接收区，供用户自行决定复制哪一段。"""
        self.rx.tag_add("sel", "1.0", "end")
        self.rx.mark_set("insert", "end")

    def _classify(self, line):
        """按规则表给一行日志选 tag：从上到下，首命中即停。

        2026-11-24：从硬编码六种改成规则驱动。pattern 里的 | 是「或」，
        每段作为一个子串去找；sys 规则的 pattern 为空，不参与匹配
        （工具提示行是 _sys_line 直接挂 tag 的）。

        注意顺序敏感：FAIL 必须排在 ok 前面，否则 "verify FAIL"
        会先被 " OK" 之类命中染绿。规则表支持上下移动就是为了调这个。
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

    def on_disp_mode_change(self):
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")
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
          · rx_queue —— 已读到、还没渲染的块
          · line_buf —— 跨块的半行，OTA 面板靠它拼整行
          · 增量解码器 —— 可能存着半个多字节字符
          · rx_count / tx_count —— 累计收发统计
        这些都属于"数据"，清屏不该把它们扔掉。所以清完之后只要板子还在
        发，屏幕会接着往下滚 —— 这是对的，清屏不等于停收。

        副作用：HEX 模式下偏移从 00000000 重新起算。不归零的话清屏后
        第一行会接着上一屏的地址，看起来像丢了一段，反而更迷惑。
        """
        self.rx.delete("1.0", "end")
        self.hex_buf = b""
        self.hex_off = 0

    def save_log(self):
        txt = self.rx.get("1.0", "end-1c")
        if not txt.strip():
            messagebox.showinfo("没有内容", "接收区还是空的。")
            return
        path = filedialog.asksaveasfilename(
            title="保存接收日志",
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")],
            initialfile=time.strftime("serial_%Y%m%d_%H%M%S.txt"))
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(txt)
            messagebox.showinfo("已保存", path)
        except Exception as e:
            messagebox.showerror("保存失败", str(e))

    # ==========================================================================
    # BootLoader 日志解析 → 内部状态（OTA 面板已移除）
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
        """从串口日志里提取发送流程需要的状态，不再更新任何界面控件。

        OTA 状态卡片移除后，这里只保留会影响发送决策的数据：
        ota_saved（老协议等落盘）、ota_proto（决定发不发尾部）、
        ota_verify / ota_done_ts（校验结论与重启识别）。其它日志行保持
        原样显示在接收区，用户仍可选中复制。
        """
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
            self.ota_done_ts = time.time()      # 记时刻，后面重启的 ready 不会清统计
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

    # ==========================================================================
    # 普通发送
    # ==========================================================================

    def send_text(self):
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开", "请先点「打开串口」。")
            return

        raw = self.tx.get("1.0", "end-1c")
        if not raw:
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

        try:
            self.ser.write(payload)
            self.tx_count += len(payload)
            self._update_counts()
            self._sys_line(">>> {} B: {}".format(
                len(payload),
                payload[:48].hex(" ").upper() + (" ..." if len(payload) > 48 else "")))
        except Exception as e:
            messagebox.showerror("发送失败", str(e))

    def wake_ota(self):
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开", "请先点「打开串口」。")
            return
        try:
            self.ser.write(b"\x00")
            self.tx_count += 1
            self._update_counts()
            self._sys_line(">>> 唤醒字节 0x00（触发 OTA 窗口）")
        except Exception as e:
            messagebox.showerror("发送失败", str(e))

    def reset_via_debugger(self, silent=False, done_cb=None):
        """经 SWD 让 DAP-Link 复位目标板，MCU 从头跑、进入 OTA 窗口。

        silent=True 时（发送流程内调用）不弹框，只写日志。
        done_cb(成功?) 在后台线程结束后回调，用于串起"复位完立刻发数据"。
        """
        if not self.ocd_ready:
            if not self.ocd_exe:
                msg = "未找到 OpenOCD，无法通过调试器复位。请手动按板子复位键。"
            else:
                msg = ("OpenOCD 里缺 interface/{}.cfg，认不出当前调试器。"
                       "请手动按板子复位键。".format(self.ocd_iface))
            if silent:
                self._sys_line("!!! " + msg)
            else:
                messagebox.showwarning("无调试器", msg)
            if done_cb:
                done_cb(False)
            return

        def work():
            ok, info = openocd_reset(self.ocd_exe, self.ocd_scr,
                                     self.ocd_iface, self.ocd_target)
            self.rx_queue.put(("sys",
                               "--- SWD 复位 {}：{} ---".format(
                                   "成功" if ok else "失败", info)))
            if done_cb:
                done_cb(ok)

        if not silent:
            self._sys_line("--- 正在通过 SWD 复位 … ---")
        threading.Thread(target=work, daemon=True).start()

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

        2026-11-24 新增。抽成函数是因为 pick_file 和 check_fw_update 都要走
        同一套装填逻辑 —— 两处各写一份的话，检查更新后大小显示忘了刷新
        这类不一致迟早出现。
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
        # 只重置解析状态；原先同步 OTA 卡片标签/进度条的调用随卡片一起移除。
        self.ota_saved = 0
        self.var_tx.set("")
        if announce:
            self._sys_line("--- 已装填最新固件：{}（{:,} B）---".format(
                os.path.basename(path), self.fw_size))
        return True

    def check_fw_update(self):
        """检查当前选中的 bin 有没有被重新编译，变了就自动重新装填。

        2026-11-24 新增。判定依据 = mtime 或 size 变化：
          · mtime 变 —— 重新编译过（哪怕大小没变）
          · size 变  —— 文件被替换或截断
        不读文件内容算 CRC：几十 KB 的 bin 读一遍虽然不慢，但发送时本来
        就会读，检查按钮没必要重复做这件事。
        """
        path = self.fw_path.get()
        if not path:
            return
        if not os.path.isfile(path):
            self._sys_line("--- 检查更新：固件文件已不存在：{} ---".format(path))
            self.btn_chk_fw.configure(state="disabled")
            return
        try:
            st = os.stat(path)
        except OSError as e:
            self._sys_line("--- 检查更新失败：{} ---".format(e))
            return
        if abs(st.st_mtime - self.fw_mtime) < 1e-6 and st.st_size == self.fw_size:
            self._sys_line("--- 检查更新：固件未变化（{:,} B）---".format(st.st_size))
            return
        old_size = self.fw_size
        if self._adopt_fw(path, announce=True):
            self._sys_line("--- 检测到固件已更新：{:,} B → {:,} B ---".format(
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
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开", "请先点「打开串口」。")
            return
        path = self.fw_path.get()
        if not path or not os.path.isfile(path):
            messagebox.showwarning("没有固件", "请先点「选择 .bin 文件」。")
            return
        if self.tx_thread and self.tx_thread.is_alive():
            return

        self.abort_evt.clear()
        self.ota_saved = 0
        self.ota_done_ts = 0.0
        self.ota_verify = None       # 清掉上一轮的校验结论，否则会误判这次成功
        self.ota_declared = 0
        self.ota_crc = ""

        self.btn_go.configure(state="disabled")
        self.btn_stop.configure(state="normal")

        chunk = int(self.chunk_var.get())
        delay = float(self.delay_var.get())

        # 2026-11-24：发送固定走 BootLoader 老协议，不再按开关分叉。
        self._sys_line("--- 开始发送 {} （{} B，块 {}，延时 {}ms）---".format(
            os.path.basename(path), self.fw_size, chunk, delay))
        self.tx_thread = threading.Thread(
            target=self._tx_worker, args=(path, chunk, delay), daemon=True)
        self.tx_thread.start()

    def abort_file_send(self):
        if self.tx_thread and self.tx_thread.is_alive():
            self.abort_evt.set()
            self._sys_line("--- 用户中止发送 ---")

    def _tx_worker(self, path, chunk, delay_ms):
        """后台线程：读文件 → 分块写串口。所有 UI 更新一律走队列。"""
        total = os.path.getsize(path)
        sent = 0
        crc = 0                  # 边发边算，省一次读盘。和 MCU 那边用同一套 CRC32
        t0 = time.time()
        last_ui = 0.0
        aborted = False

        try:
            if self.auto_reset.get():
                # 走 SWD 复位。DAP-Link 的 CDC 串口只透传 TX/RX，
                # DTR/RTS 不接目标板 NRST，串口侧脉冲对它是空操作。
                ok, info = openocd_reset(self.ocd_exe, self.ocd_scr,
                                         self.ocd_iface, self.ocd_target)
                self.rx_queue.put(("sys", "--- SWD 复位 {}：{} ---".format(
                    "成功" if ok else "失败", info)))
                if ok:
                    # BootLoader 复位后先打 [ota] ready 再进窗口，等它打印完
                    time.sleep(0.3)
                else:
                    self.rx_queue.put(("sys",
                        "!!! 复位失败，OTA 窗口可能没打开（可手动按板子复位后重发）"))

            with open(path, "rb") as f:
                while sent < total:
                    if self.abort_evt.is_set():
                        aborted = True
                        break
                    block = f.read(chunk)
                    if not block:
                        break
                    self.ser.write(block)
                    crc = zlib.crc32(block, crc)
                    sent += len(block)

                    now = time.time()
                    if now - last_ui > 0.08 or sent >= total:
                        speed = sent / max(now - t0, 1e-6)
                        self.rx_queue.put(("tx", (sent, total, speed, False)))
                        last_ui = now

                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)

        except Exception as e:
            self.rx_queue.put(("error", "发送中断: {}".format(e)))
            self.rx_queue.put(("tx", (sent, total, 0.0, True, True)))
            return

        speed = sent / max(time.time() - t0, 1e-6)
        self.rx_queue.put(("tx", (sent, total, speed, True, aborted)))

        # ---------------- 收尾 ----------------
        # 新协议（proto=2）：把 {长度, CRC32} 作为 12 字节尾部发下去。
        # BootLoader 收齐后把 Flash 读回来重算 CRC，对得上就自己跳转 ——
        # 这一步不需要我们再做复位，也就不存在"复位时机早了把 tail 截掉"的问题。
        #
        # 老协议（proto=1）：没有校验能力，只能沿用"等 sum 对齐 → 复位"的老路。
        # 关键：老协议绝对不能发尾部，那 12 字节会被当成固件写进 Flash。
        if aborted or sent < total:
            return

        if self.ota_proto >= 2:
            crc32 = crc & 0xFFFFFFFF
            # 停一下再发尾部：让 MCU 先把最后一块固件消化掉，
            # 尾部这 12 字节才更可能单独成块，不会被拆到两个读周期里
            time.sleep(0.05)
            try:
                self.ser.write(build_ota_tail(total, crc32))
            except Exception as e:
                self.rx_queue.put(("error", "尾部记录发送失败: {}".format(e)))
                return

            self.rx_queue.put(("sys",
                "--- 已发尾部记录 len={:,} crc=0x{:08X}，等待校验 … ---".format(
                    total, crc32)))

            # 15KB 的 CRC 只要几毫秒，5 秒是留给串口日志和擦参数页的余量
            deadline = time.time() + 5.0
            while time.time() < deadline:
                if self.ota_verify is not None:
                    break
                if self.abort_evt.is_set():
                    return
                time.sleep(0.05)

            if self.ota_verify and self.ota_verify[0] == "ok":
                self.rx_queue.put(("sys",
                    "--- 校验通过，BootLoader 已自行跳转，无需复位 ---"))
            elif self.ota_verify and self.ota_verify[0] == "fail":
                self.rx_queue.put(("sys",
                    "!!! 校验失败，固件没有跳转。板子停在 OTA，可以直接重发"))
            else:
                self.rx_queue.put(("sys",
                    "!!! 5 秒内没等到校验结果。板子可能还停在 OTA，看看日志"))
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
        if self.ocd_ready and self.auto_reset.get():
            self.rx_queue.put(("sys", "--- 等待 BootLoader 落盘确认 … ---"))
            deadline = time.time() + 8.0
            while time.time() < deadline:
                if self.ota_saved >= total:
                    break
                if self.abort_evt.is_set():
                    return
                time.sleep(0.1)

            landed = self.ota_saved
            if landed < total:
                self.rx_queue.put(("sys",
                    "!!! 只确认落盘 {:,} / {:,} B，仍执行复位（请核对日志）".format(
                        landed, total)))
            else:
                self.rx_queue.put(("sys",
                    "--- 已确认落盘 {:,} B，复位跳新固件 ---".format(landed)))

            time.sleep(0.3)          # 让 tail 那页的擦写彻底结束
            ok, info = openocd_reset(self.ocd_exe, self.ocd_scr,
                                     self.ocd_iface, self.ocd_target)
            self.rx_queue.put(("sys",
                "--- 自动复位 {}：{} ---".format(
                    "成功" if ok else "失败", info)))

    def _on_tx_progress(self, sent, total, speed, done, aborted=False):
        pct = sent * 100.0 / total if total else 0
        self.var_tx.set("已发 {:,} / {:,} B  ({:.1f}%)   {:.1f} KB/s".format(
            sent, total, pct, speed / 1024.0))

        if not done:
            return

        # done 只来一次，这里的 sent 就是本次总共写出去的字节数
        self.tx_count += sent
        self._update_counts()

        self.btn_go.configure(state="normal")
        self.btn_stop.configure(state="disabled")

        if aborted:
            self._sys_line("--- 发送已中止（{} B）---".format(sent))
        elif sent >= total:
            self._sys_line("--- 发送完毕 {} B，用时 {:.2f}s ---".format(
                sent, total / max(speed, 1e-6) if speed else 0))

    # ==========================================================================
    # 收尾
    # ==========================================================================

    def on_close(self):
        self.reader_alive = False
        self.abort_evt.set()
        save_config({
            "port": self.port_var.get(),
            "baud": self.baud_var.get(),
            "disp_mode": self.disp_mode.get(),
            "autoscroll": self.autoscroll.get(),
            "timestamp": self.timestamp.get(),
            "chunk": self.chunk_var.get(),
            "delay": self.delay_var.get(),
            "auto_reset": self.auto_reset.get(),
            "last_file": self.fw_path.get(),
            # 2026-11-24：颜色规则表随配置持久化，拷走 json 就等于换工程带走规则
            "log_rules": [dict(r) for r in self.log_rules],
            "geometry": self.root.geometry(),
        })
        try:
            if self.ser:
                self.ser.close()
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
    # 打包后用它确认 pyserial 的 Windows 后端没被打包器漏掉。
    if "--check" in sys.argv:
        root.update_idletasks()
        root.update()
        msg = "UI self-check OK: {}x{}".format(root.winfo_width(),
                                               root.winfo_height())
        if sys.stdout is not None:   # --noconsole 打包后 stdout 可能为 None
            print(msg)
            ports = list_serial_ports()
            print("serial ports({}): {}".format(
                len(ports), ", ".join(p[0] for p in ports) or "-"))
            print("debugger: {} -> interface/{}.cfg [{}]".format(
                app.ocd_name, app.ocd_iface,
                "ready" if app.ocd_ready else "unavailable"))
            print("config: {}".format(CONFIG_PATH))
        root.destroy()
        return

    root.mainloop()


if __name__ == "__main__":
    main()
