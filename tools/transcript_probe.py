#!/usr/bin/env python3
# =============================================================================
# transcript_probe.py - 计划里的 `transcript-spike` 兜底验证脚本.
#
# 目的: 当 AX 检测不可行时, 看能否靠 Cursor 的 transcript JSONL 判断 "卡在审批":
#   思路是找 "有 tool_use / tool_call 但还没有对应 tool_result" 的悬空调用.
#
# 用法:
#   python3 transcript_probe.py <path-to-transcript.jsonl>   # 分析一次
#   python3 transcript_probe.py --auto                       # 自动找最近的 transcript
#   python3 transcript_probe.py --watch <path>               # 每秒刷新, 等审批时观察
#
# transcript 路径从哪来: hook 事件里带 transcript_path, relay 会记到日志; 或用
# --auto 在 ~/.cursor / ~/.config/cursor 等目录里找最近修改的 .jsonl.
#
# 这是探测脚本, 格式因 Cursor 版本而异; 输出供人工判断该信号是否可靠.
# 只用标准库.
# =============================================================================
from __future__ import annotations

import glob
import json
import os
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

CALL_KEYS = ("tool_use", "toolUse", "tool_call", "toolCall", "function_call", "functionCall")
RESULT_KEYS = ("tool_result", "toolResult", "tool_results", "function_result")


def find_recent_transcripts(limit: int = 5) -> List[str]:
    roots = [
        "~/.cursor", "~/.config/cursor", "~/.config/Cursor",
        "~/Library/Application Support/Cursor",
    ]
    found: List[Tuple[float, str]] = []
    for r in roots:
        base = os.path.expanduser(r)
        if not os.path.isdir(base):
            continue
        for p in glob.glob(os.path.join(base, "**", "*.jsonl"), recursive=True):
            try:
                found.append((os.path.getmtime(p), p))
            except OSError:
                pass
    found.sort(reverse=True)
    return [p for _, p in found[:limit]]


def _walk(obj, hit):
    if isinstance(obj, dict):
        hit(obj)
        for v in obj.values():
            _walk(v, hit)
    elif isinstance(obj, list):
        for v in obj:
            _walk(v, hit)


def _id_of(d: dict) -> str:
    for k in ("id", "tool_use_id", "toolUseId", "call_id", "callId", "tool_call_id"):
        if k in d and isinstance(d[k], (str, int)):
            return str(d[k])
    return ""


def analyze(path: str) -> dict:
    calls: dict = {}     # id -> name
    results: set = set()
    last_lines: List[str] = []
    n = 0
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                n += 1
                last_lines.append(line[:160])
                try:
                    obj = json.loads(line)
                except Exception:  # noqa: BLE001
                    continue

                def hit(d: dict):
                    t = str(d.get("type") or d.get("role") or "")
                    keys = set(d.keys())
                    is_call = any(k in keys for k in CALL_KEYS) or any(
                        c in t for c in ("tool_use", "tool_call", "function_call"))
                    is_res = any(k in keys for k in RESULT_KEYS) or any(
                        c in t for c in ("tool_result", "function_result"))
                    if is_call:
                        cid = _id_of(d)
                        name = str(d.get("name") or d.get("tool_name") or d.get("toolName") or "?")
                        if cid:
                            calls[cid] = name
                    if is_res:
                        rid = _id_of(d)
                        if rid:
                            results.add(rid)

                _walk(obj, hit)
    except FileNotFoundError:
        return {"error": f"file not found: {path}"}

    pending = {cid: name for cid, name in calls.items() if cid not in results}
    return {
        "path": path, "lines": n,
        "calls": len(calls), "results": len(results), "pending": pending,
        "tail": last_lines[-4:],
    }


def report(res: dict) -> None:
    if res.get("error"):
        print("  " + res["error"])
        return
    print(f"  file   : {res['path']}")
    print(f"  lines  : {res['lines']}   tool_calls={res['calls']}  results={res['results']}")
    if res["pending"]:
        print(f"  PENDING (call 无 result) x{len(res['pending'])}:")
        for cid, name in res["pending"].items():
            print(f"     - {name}  (id={cid})")
        print("  => 可能正卡在审批/执行中 (需结合 before/after 与时间判断)")
    else:
        print("  pending: none (没有悬空 tool 调用)")
    print("  tail:")
    for t in res["tail"]:
        print("     " + t)


def main(argv: List[str]) -> int:
    watch = "--watch" in argv
    argv = [a for a in argv if a != "--watch"]

    if "--auto" in argv or not argv:
        recent = find_recent_transcripts()
        if not recent:
            print("没找到 transcript .jsonl; 请把路径作为参数传入 (从 relay 日志里的 transcript_path 拿).")
            return 2
        path = recent[0]
        print(f"[auto] 选最近修改的: {path}")
        if len(recent) > 1:
            print("[auto] 其它候选:")
            for p in recent[1:]:
                print("   " + p)
    else:
        path = argv[0]

    if watch:
        print("每秒刷新 (Ctrl-C 退出). 触发一次审批, 看 PENDING 是否稳定出现:")
        try:
            while True:
                print("-" * 60, time.strftime("%H:%M:%S"))
                report(analyze(path))
                time.sleep(1.0)
        except KeyboardInterrupt:
            return 0
    report(analyze(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
