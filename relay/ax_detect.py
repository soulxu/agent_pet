#!/usr/bin/env python3
# =============================================================================
# ax_detect.py - 用 macOS Accessibility (AX) 检测 Cursor 是否弹出了 "approve" UI.
#
# 这是 relay 的 *首选* 审批检测方式 (比时间启发式准): 直接看 Cursor 进程的 AX
# 树里有没有 "Run command / Accept / Run anyway ..." 这类待批准的按钮/文案.
#
# 同时它也是计划里的 `ax-spike` 验证脚本:
#   python3 ax_detect.py dump          # 打印 Cursor 的 AX 树 (人工找审批 UI 特征)
#   python3 ax_detect.py dump --grep Run
#   python3 ax_detect.py watch         # 每秒检测一次, 打印是否命中 (手动触发 approve 看)
#   python3 ax_detect.py check         # 检测一次, 退出码 0=命中 1=未命中 2=不可用
#
# 需要 "辅助功能" 权限: 系统设置 -> 隐私与安全性 -> 辅助功能, 勾选运行它的程序
# (Terminal / iTerm / python). 没权限时所有函数返回 "不可用" (None), relay 回退.
#
# 依赖 pyobjc (pip install pyobjc); 没装则不可用 (relay 回退到启发式).
# =============================================================================
from __future__ import annotations

import re
import sys
import time
from typing import List, Optional

try:
    from ApplicationServices import (
        AXUIElementCreateApplication,
        AXUIElementCopyAttributeValue,
        AXUIElementSetAttributeValue,
        AXIsProcessTrusted,
    )
    from AppKit import NSWorkspace
    _HAVE_AX = True
except Exception:  # noqa: BLE001
    _HAVE_AX = False

# check() 用: Cursor "等待人工批准" 时出现的**按钮**文案 (精确匹配, 去掉快捷键
# 括号后小写相等). 实测 Cursor 审批命令时按钮就叫 "Run". 其余是常见同类场景.
APPROVE_BUTTON_LABELS = {
    "run",
    "run command",
    "run anyway",
    "run tool",
    "accept",
    "accept all",
    "keep",
    "keep all",
    "apply",
    "approve",
    "allow",
    "resume",
    "move to background",
}

# 只在这些"可点击"角色上判按钮文案, 避免把正文里的 run 之类误判.
_CLICKABLE = ("AXButton", "AXPopUpButton", "AXRadioButton", "AXMenuButton")

# dump --grep 这类人工排查用的宽松关键词 (子串). 不参与 check().
DEFAULT_KEYWORDS = [
    "run",
    "accept",
    "approve",
    "allow",
    "keep",
    "move to background",
]


def _btn_label(s: str) -> str:
    # "Run (⌘⏎)" -> "run";  "Run" -> "run"
    return re.sub(r"\s*[\(（].*$", "", s.strip().lower()).strip()

_MAX_DEPTH = 40
_MAX_NODES = 6000


def have_ax() -> bool:
    return _HAVE_AX


def is_trusted() -> bool:
    if not _HAVE_AX:
        return False
    try:
        return bool(AXIsProcessTrusted())
    except Exception:  # noqa: BLE001
        return False


def _app_name() -> str:
    import os
    return os.environ.get("AGENT_PET_APP", "Cursor")


def _find_pid(name: str) -> Optional[int]:
    # Cursor 有一堆同名进程 (主程序 + Helper + CursorUIViewService ...).
    # 只要"常规带界面的那个" (activationPolicy == Regular = 0), 否则会撞上
    # 无窗口的 helper/service, 拿到一棵空树.
    try:
        regular = None
        fallback = None
        nm = name.lower()
        for app in NSWorkspace.sharedWorkspace().runningApplications():
            ln = app.localizedName()
            if not ln or nm not in str(ln).lower():
                continue
            pid = int(app.processIdentifier())
            try:
                policy = int(app.activationPolicy())
            except Exception:  # noqa: BLE001
                policy = -1
            if policy == 0:  # NSApplicationActivationPolicyRegular
                if str(ln).lower() == nm:   # 精确名优先 ("Cursor")
                    return pid
                if regular is None:
                    regular = pid
            elif fallback is None:
                fallback = pid
        return regular if regular is not None else fallback
    except Exception:  # noqa: BLE001
        return None


def _attr(el, name: str):
    try:
        err, val = AXUIElementCopyAttributeValue(el, name, None)
        if err == 0:
            return val
    except Exception:  # noqa: BLE001
        pass
    return None


def _node_strings(el) -> List[str]:
    out = []
    for a in ("AXTitle", "AXDescription", "AXValue", "AXHelp", "AXLabel"):
        v = _attr(el, a)
        if isinstance(v, str) and v.strip():
            out.append(v.strip())
    return out


def _walk(el, depth, counter, visit):
    if depth > _MAX_DEPTH or counter[0] > _MAX_NODES:
        return
    counter[0] += 1
    role = _attr(el, "AXRole")
    visit(el, depth, str(role) if role else "")
    children = _attr(el, "AXChildren")
    if children:
        for c in children:
            _walk(c, depth + 1, counter, visit)


def _root() -> Optional[object]:
    name = _app_name()
    pid = _find_pid(name)
    if pid is None:
        return None
    app = AXUIElementCreateApplication(pid)
    # Cursor 是 Electron/Chromium, AX 树默认不暴露. 主动设这两个属性唤醒它,
    # 否则只能看到一个空壳窗口 (0 个文本节点).
    for attr in ("AXManualAccessibility", "AXEnhancedUserInterface"):
        try:
            AXUIElementSetAttributeValue(app, attr, True)
        except Exception:  # noqa: BLE001
            pass
    return app


# relay 用: 检测 Cursor 是否有 approve UI (有"Run"等审批按钮).
#   返回 True=有, False=没有, None=不可用 (没装 pyobjc / 没权限 / 找不到 Cursor)
#   extra_labels: 额外的审批按钮文案 (来自 config.ax_keywords), 精确匹配.
def check(extra_labels: Optional[List[str]] = None) -> Optional[bool]:
    if not _HAVE_AX or not is_trusted():
        return None
    root = _root()
    if root is None:
        return None
    labels = set(APPROVE_BUTTON_LABELS)
    for x in (extra_labels or []):
        labels.add(str(x).strip().lower())
    hit = [False]
    counter = [0]

    def visit(el, depth, role):
        if hit[0] or role not in _CLICKABLE:
            return
        for s in _node_strings(el):
            if _btn_label(s) in labels:
                hit[0] = True
                return

    try:
        _walk(root, 0, counter, visit)
    except Exception:  # noqa: BLE001
        return None
    return hit[0]


# spike: 收集 Cursor AX 树里所有文本节点 (role: text), 给人工分析审批 UI 特征.
def collect(grep: Optional[str] = None) -> List[str]:
    if not _HAVE_AX:
        print("[ax] pyobjc 不可用: pip3 install pyobjc", file=sys.stderr)
        return []
    if not is_trusted():
        print("[ax] 未获辅助功能权限: 系统设置 -> 隐私与安全性 -> 辅助功能", file=sys.stderr)
        return []
    root = _root()
    if root is None:
        print(f"[ax] 找不到进程 '{_app_name()}' (Cursor 没开?)", file=sys.stderr)
        return []
    pid = _find_pid(_app_name())
    top = _attr(root, "AXChildren") or []
    print(f"[ax] app={_app_name()} pid={pid} trusted={is_trusted()} "
          f"顶层元素={len(top)} (顶层=0 通常是没授权或 Electron AX 没唤醒)",
          file=sys.stderr)
    lines: List[str] = []
    counter = [0]

    def visit(el, depth, role):
        strs = _node_strings(el)
        if not strs:
            return
        text = " | ".join(strs)
        if grep and grep.lower() not in text.lower():
            return
        lines.append(f"{'  ' * depth}[{role}] {text}")

    _walk(root, 0, counter, visit)
    print(f"[ax] 共遍历 {counter[0]} 个节点", file=sys.stderr)
    return lines


def collect_clickable() -> List[str]:
    if not _HAVE_AX or not is_trusted():
        print("[ax] 不可用 (没装 pyobjc 或没辅助功能权限)", file=sys.stderr)
        return []
    root = _root()
    if root is None:
        print(f"[ax] 找不到进程 '{_app_name()}'", file=sys.stderr)
        return []
    out: List[str] = []
    counter = [0]
    roles = ("AXButton", "AXPopUpButton", "AXRadioButton", "AXMenuButton", "AXCheckBox")

    def visit(el, depth, role):
        if role in roles:
            ss = _node_strings(el)
            if ss:
                out.append(f"[{role}] " + " | ".join(ss))

    _walk(root, 0, counter, visit)
    return sorted(set(out))


def _main(argv: List[str]) -> int:
    cmd = argv[0] if argv else "check"
    if cmd == "buttons":
        for ln in collect_clickable():
            print(ln)
        return 0
    if cmd == "dump":
        grep = None
        if "--grep" in argv:
            i = argv.index("--grep")
            grep = argv[i + 1] if i + 1 < len(argv) else None
        lines = collect(grep)
        print(f"[ax] {len(lines)} 个文本节点" + (f" (grep={grep!r})" if grep else ""))
        for ln in lines:
            print(ln)
        return 0
    if cmd == "watch":
        if not is_trusted():
            print("[ax] 不可用 (没装 pyobjc 或没辅助功能权限)")
            return 2
        print("[ax] 每秒检测 Cursor approve UI (Ctrl-C 退出). 手动触发一次 approve 看是否命中:")
        try:
            while True:
                r = check()
                print(f"  {time.strftime('%H:%M:%S')}  approve_ui={r}")
                time.sleep(1.0)
        except KeyboardInterrupt:
            return 0
    # check
    r = check()
    if r is None:
        print("unavailable")
        return 2
    print("approval" if r else "none")
    return 0 if r else 1


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
