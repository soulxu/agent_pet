#!/usr/bin/env python3
# =============================================================================
# agent_pet hook - Cursor (以及 claude-code / codex) 的 hook 入口.
#
# 关键设计 (和 agent_approver 不同):
#   - 所有 permission hook 一律 **放行 (allow)**, 不拦截, 不替 Cursor 决定.
#     企业/原生强制 approve 不经过 hook, allow 也绕不过它, 由 stick HID 敲快捷键批.
#   - hook 只负责把 "agent 在干啥" 上报给 relay (驱动眼睛颜色), 并给 before/after
#     带 in-flight 标记, 供 relay 启发式判断 "是否卡在原生审批".
#
# 用法 (由 hooks.template.json 指定):
#   hook.py shell        beforeShellExecution -> allow + 上报 before(shell)
#   hook.py shell_done   afterShellExecution  -> 上报 after(shell)
#   hook.py mcp          beforeMCPExecution   -> allow + 上报 before(mcp)
#   hook.py mcp_done     afterMCPExecution    -> 上报 after(mcp)
#   hook.py status <lbl> 其它所有 hook        -> 上报活动 (lbl 见 STATUS_SPEC)
#
# relay 不可用时静默放行, 绝不卡住 agent. 只用标准库, 兼容 Python 3.9.
# =============================================================================
from __future__ import annotations

import hashlib
import json
import os
import sys
import urllib.request
from pathlib import Path

CONFIG_PATH = Path(
    os.environ.get("AGENT_PET_CONFIG") or "~/.config/agent_pet/config.json"
).expanduser()

DEFAULTS = {
    "url": "http://127.0.0.1:8799",
    "activity": True,
}

# 各 status 事件 -> (state, response)
#   state    : 上报给眼睛的状态 busy/idle/end
#   response : 该 hook 要不要回点东西以免挡住 agent
#              "allow"=回 {permission:allow}; "continue"=回 {continue:true}; "none"=不回
STATUS_SPEC = {
    "session_start":  ("idle", "none"),
    "session_end":    ("end",  "none"),
    "prompt":         ("busy", "continue"),
    "shell_done":     ("busy", "none"),
    "mcp_done":       ("busy", "none"),
    "read":           ("busy", "allow"),
    "edit":           ("busy", "none"),
    "response":       ("busy", "none"),
    "thought":        ("busy", "none"),
    "compact":        ("busy", "none"),
    "subagent_start": ("busy", "allow"),
    "subagent_stop":  ("busy", "none"),
    "tool":           ("busy", "allow"),
    "tool_done":      ("busy", "none"),
    "tool_fail":      ("busy", "none"),
    "stop":           ("idle", "none"),
}


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    try:
        if CONFIG_PATH.is_file():
            user = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if isinstance(user, dict):
                cfg.update(user)
    except Exception:  # noqa: BLE001
        pass
    return cfg


def source_name() -> str:
    return os.environ.get("AGENT_PET_AGENT", "cursor")


def read_event() -> dict:
    try:
        raw = sys.stdin.read()
        return json.loads(raw) if raw.strip() else {}
    except Exception:  # noqa: BLE001
        return {}


def event_cwd(ev: dict) -> str:
    for k in ("cwd", "workspace", "workspaceRoot"):
        v = ev.get(k)
        if isinstance(v, str) and v:
            return v
    roots = ev.get("workspace_roots") or ev.get("workspaceRoots")
    if isinstance(roots, list) and roots:
        first = roots[0]
        if isinstance(first, str):
            return first
        if isinstance(first, dict):
            return str(first.get("path") or first.get("uri") or "")
    return os.getcwd()


def agent_id(ev: dict) -> str:
    for k in ("conversation_id", "session_id", "parent_conversation_id", "generation_id"):
        v = ev.get(k)
        if isinstance(v, str) and v:
            return v
    cwd = event_cwd(ev)
    return "ws:" + cwd if cwd else "default"


def agent_label(ev: dict) -> str:
    cwd = event_cwd(ev)
    name = ""
    if cwd:
        name = os.path.basename(cwd.rstrip("/")) or cwd
    if not name:
        name = source_name()
    return name[:24]


def inflight_id(ev: dict, key: str) -> str:
    raw = (agent_id(ev) + "|" + key).encode("utf-8")
    return hashlib.sha1(raw).hexdigest()[:16]


def post_json(url: str, obj: dict, timeout: float) -> "dict | None":
    data = json.dumps(obj).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception:  # noqa: BLE001
        return None


def send_status(cfg: dict, ev: dict, state: str, kind: str, text: str,
                phase: str = "activity", flight: str = "",
                command: str = "", summary: str = "") -> None:
    if not cfg.get("activity", True):
        return
    payload = {
        "agent_id": agent_id(ev),
        "label": agent_label(ev),
        "source": source_name(),
        "state": state,
        "kind": kind,
        "text": text,
        "cwd": event_cwd(ev),
        "phase": phase,  # "before" | "after" | "activity"
    }
    if flight:
        payload["inflight_id"] = flight
    if command:
        payload["command"] = command[:400]
    tp = ev.get("transcript_path")
    if isinstance(tp, str) and tp:
        payload["transcript_path"] = tp
    if summary:
        payload["summary"] = summary
    post_json(cfg["url"].rstrip("/") + "/hook/status", payload, timeout=1.0)


def emit(permission: str) -> None:
    print(json.dumps({"permission": permission}))


# =============================================================================
# 文本工具
# =============================================================================
def _short_path(p: str) -> str:
    if not p:
        return "(file)"
    parts = p.replace("\\", "/").split("/")
    return "/".join(parts[-2:]) if len(parts) > 2 else p


def _clip(s: str, n: int) -> str:
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "\u2026"


def status_text(label: str, ev: dict) -> str:
    if label == "prompt":
        return "\u4efb\u52a1: " + _clip(ev.get("prompt") or ev.get("text") or "", 120)
    if label == "thought":
        return "\u601d\u8003: " + _clip(ev.get("text") or "", 100)
    if label == "response":
        return "\u56de\u590d: " + _clip(ev.get("text") or "", 100)
    if label == "edit":
        f = ev.get("file_path") or ev.get("filePath") or ev.get("path") or ""
        return "\u7f16\u8f91 " + _short_path(str(f))
    if label == "read":
        f = ev.get("file_path") or ev.get("filePath") or ev.get("path") or ""
        return "\u8bfb\u53d6 " + _short_path(str(f))
    if label == "shell_done":
        return "\u5b8c\u6210\u547d\u4ee4 " + _clip(ev.get("command") or "", 60)
    if label == "mcp_done":
        return "\u5b8c\u6210 MCP " + str(ev.get("tool_name") or "")
    if label == "tool":
        return "\u8c03\u7528 " + str(ev.get("tool_name") or "tool")
    if label == "tool_done":
        return "\u5b8c\u6210 " + str(ev.get("tool_name") or "tool")
    if label == "tool_fail":
        return "\u5931\u8d25 " + str(ev.get("tool_name") or "tool")
    if label == "compact":
        return "\u538b\u7f29\u4e0a\u4e0b\u6587"
    if label == "subagent_start":
        return "\u5b50\u4efb\u52a1: " + _clip(ev.get("task") or "", 80)
    if label == "subagent_stop":
        return "\u5b50\u4efb\u52a1\u5b8c\u6210"
    if label == "session_start":
        return "\u4f1a\u8bdd\u5f00\u59cb"
    if label == "stop":
        return "\u5b8c\u6210, \u7a7a\u95f2\u4e2d"
    return label


def shell_title(command: str) -> str:
    toks = command.split()
    if not toks:
        return "\u547d\u4ee4"
    idx = 0
    while idx < len(toks) and "=" in toks[idx] and not toks[idx].startswith("-"):
        idx += 1
    head = toks[idx:idx + 2] if idx < len(toks) else toks[:2]
    return (" ".join(head) if head else "\u547d\u4ee4")[:48]


# =============================================================================
# 模式: 全部放行, 只上报
# =============================================================================
def mode_shell(cfg: dict) -> None:
    ev = read_event()
    command = str(ev.get("command") or ev.get("commandLine") or "").strip()
    fid = inflight_id(ev, "shell:" + command)
    send_status(cfg, ev, "busy", "shell", "$ " + shell_title(command),
                phase="before", flight=fid, command=command)
    emit("allow")


def mode_shell_done(cfg: dict) -> None:
    ev = read_event()
    command = str(ev.get("command") or "").strip()
    fid = inflight_id(ev, "shell:" + command)
    send_status(cfg, ev, "busy", "shell_done", status_text("shell_done", ev),
                phase="after", flight=fid)


def mode_mcp(cfg: dict) -> None:
    ev = read_event()
    tool = str(ev.get("tool_name") or ev.get("toolName") or ev.get("name") or "mcp")
    fid = inflight_id(ev, "mcp:" + tool)
    send_status(cfg, ev, "busy", "mcp", "MCP " + tool, phase="before", flight=fid)
    emit("allow")


def mode_mcp_done(cfg: dict) -> None:
    ev = read_event()
    tool = str(ev.get("tool_name") or "")
    fid = inflight_id(ev, "mcp:" + tool)
    send_status(cfg, ev, "busy", "mcp_done", status_text("mcp_done", ev),
                phase="after", flight=fid)


def mode_status(cfg: dict, label: str) -> None:
    ev = read_event()
    state, response = STATUS_SPEC.get(label, ("busy", "none"))
    summary = ""
    if label == "response":
        summary = _clip(ev.get("text") or "", 1200)
    send_status(cfg, ev, state, label, status_text(label, ev), summary=summary)
    if response == "allow":
        emit("allow")
    elif response == "continue":
        print(json.dumps({"continue": True}))


# =============================================================================
# Claude / Codex (CLI): 只上报状态驱动眼睛, 不做审批 (CLI 无 GUI approve 框).
# =============================================================================
def claude_kind(tool: str) -> str:
    if tool == "Bash":
        return "shell"
    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit", "Update", "apply_patch"):
        return "edit"
    if tool == "Read":
        return "read"
    if tool.startswith("mcp__"):
        return "mcp"
    return "tool"


def claude_status(cfg: dict, label: str) -> None:
    ev = read_event()
    if label == "prompt":
        send_status(cfg, ev, "busy", "prompt",
                    "\u4efb\u52a1: " + _clip(ev.get("prompt") or ev.get("user_prompt") or "", 120))
    elif label == "pretool":
        tool = str(ev.get("tool_name") or "")
        send_status(cfg, ev, "busy", claude_kind(tool), "\u8c03\u7528 " + (tool or "tool"))
    elif label == "posttool":
        tool = str(ev.get("tool_name") or "")
        send_status(cfg, ev, "busy", claude_kind(tool), "\u5b8c\u6210 " + (tool or "tool"))
    elif label == "stop":
        send_status(cfg, ev, "idle", "stop", "\u5b8c\u6210, \u7a7a\u95f2\u4e2d",
                    summary=_clip(ev.get("last_assistant_message") or "", 1200))
    elif label == "session_end":
        send_status(cfg, ev, "end", "session_end", "\u4f1a\u8bdd\u7ed3\u675f")
    else:
        send_status(cfg, ev, "busy", label, label)


def main() -> int:
    cfg = load_config()
    args = sys.argv[1:]
    mode = args[0] if args else ""
    try:
        if mode == "shell":
            mode_shell(cfg)
        elif mode == "shell_done":
            mode_shell_done(cfg)
        elif mode == "mcp":
            mode_mcp(cfg)
        elif mode == "mcp_done":
            mode_mcp_done(cfg)
        elif mode == "status":
            mode_status(cfg, args[1] if len(args) > 1 else "")
        elif mode == "claude":
            claude_status(cfg, args[1] if len(args) > 1 else "")
        else:
            emit("allow")
    except Exception:  # noqa: BLE001 - 任何异常都别挡住 agent
        if mode in ("shell", "mcp"):
            emit("allow")
    return 0


if __name__ == "__main__":
    sys.exit(main())
