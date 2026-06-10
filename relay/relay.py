#!/usr/bin/env python3
# =============================================================================
# agent_pet relay - Mac 上常驻的小中枢 (纯 HTTP, 不碰蓝牙).
#
# 新架构: StickS3 是个纯蓝牙键盘 (按键直接敲回车/Esc), agent 状态走 WiFi.
# 所以 relay 只干两件事:
#   1) 收 Cursor/Claude/Codex 的 hook 上报 (回环 HTTP), 聚合每个 agent 的状态;
#      StickS3 通过 WiFi 轮询 GET /state 拿聚合状态驱动眼睛颜色.
#   2) 审批检测器 (AX 优先 / 回退启发式): 判断是否卡在原生/企业强制 approve,
#      判到了把状态标成 wait (眼睛变红) 并把 Cursor 切到前台, 你按 StickS3 的
#      回车键即可批准 (敲键这步纯由蓝牙 HID 本地完成, 不经过 relay).
#
# 依赖: pyobjc (AX 检测, 可选). 没有也能跑, 回退启发式. 兼容 Python 3.9+.
# =============================================================================
from __future__ import annotations

import argparse
import json
import os
import ssl
import subprocess
import threading
import time
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional

CERTS_DIR = Path(__file__).resolve().parent / "certs"

try:
    import ax_detect
except Exception:  # noqa: BLE001
    ax_detect = None

CONFIG_PATH = Path(
    os.environ.get("AGENT_PET_CONFIG") or "~/.config/agent_pet/config.json"
).expanduser()

DEFAULTS = {
    "app_name": "Cursor",
    "detect_mode": "auto",        # "auto" | "ax" | "heuristic" | "off"
    "wait_threshold_ms": 2500,    # 启发式: before 后多久没 after 判等待
    "activate_on_wait": True,     # 判为等待时把 Cursor 切前台
    "ax_keywords": [],            # 额外的审批按钮文案 (精确匹配)
}

MAX_AGENTS = 16


def _ts() -> str:
    return time.strftime("%H:%M:%S")


def log(msg: str) -> None:
    print(f"[{_ts()}] {msg}", flush=True)


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


CFG = load_config()


def activate_app() -> None:
    if not CFG.get("activate_on_wait", True):
        return
    app = str(CFG.get("app_name", "Cursor"))
    try:
        subprocess.run(["osascript", "-e", f'tell application "{app}" to activate'],
                       timeout=2.0, capture_output=True)
    except Exception as e:  # noqa: BLE001
        log(f"activate {app} failed: {e}")


# =============================================================================
# 共享状态
# =============================================================================
class State:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.agents: "OrderedDict[str, dict]" = OrderedDict()  # id -> dict
        self.inflight: "dict[str, dict]" = {}                  # inflight_id -> info
        self.last_event: "dict[str, float]" = {}
        self.approving: Optional[str] = None

    def _upsert_locked(self, agent_id: str, **fields) -> dict:
        a = self.agents.pop(agent_id, None) or {
            "id": agent_id, "label": "agent", "base_state": "busy",
            "kind": "", "text": "", "cwd": "",
        }
        a.update({k: v for k, v in fields.items() if v is not None})
        a["ts"] = time.time()
        self.agents[agent_id] = a
        while len(self.agents) > MAX_AGENTS:
            old, _ = self.agents.popitem(last=False)
            if self.approving == old:
                self.approving = None
        return a

    def set_status(self, payload: dict) -> None:
        agent_id = str(payload.get("agent_id") or "default")
        state = str(payload.get("state") or "busy")
        phase = str(payload.get("phase") or "activity")
        now = time.time()
        with self.lock:
            self.last_event[agent_id] = now
            if state == "end":
                self.agents.pop(agent_id, None)
                if self.approving == agent_id:
                    self.approving = None
                for fid in [k for k, v in self.inflight.items() if v["agent_id"] == agent_id]:
                    self.inflight.pop(fid, None)
                return

            self._upsert_locked(
                agent_id,
                label=(str(payload.get("label")) if payload.get("label") else None),
                base_state=state,
                kind=str(payload.get("kind") or ""),
                text=str(payload.get("text") or ""),
                cwd=str(payload.get("cwd") or ""),
            )

            fid = str(payload.get("inflight_id") or "")
            if phase == "before" and fid:
                self.inflight[fid] = {
                    "agent_id": agent_id, "ts": now,
                    "command": str(payload.get("command") or ""),
                }
            elif phase == "after" and fid:
                self.inflight.pop(fid, None)
                if self.approving == agent_id:
                    self.approving = None

    # ---- 审批检测器 ----
    def evaluate(self) -> None:
        mode = str(CFG.get("detect_mode", "auto"))
        if mode == "off":
            return
        now = time.time()
        threshold = float(CFG.get("wait_threshold_ms", 2500)) / 1000.0

        ax_result = None
        if mode in ("auto", "ax") and ax_detect is not None:
            try:
                ax_result = ax_detect.check(CFG.get("ax_keywords") or [])
            except Exception:  # noqa: BLE001
                ax_result = None

        with self.lock:
            target = None
            if (mode == "ax") or (mode == "auto" and ax_result is not None):
                if ax_result:
                    target = self._pick_pending_locked(now, threshold) or \
                             self._most_recent_busy_locked()
            if target is None and mode in ("auto", "heuristic") and ax_result is None:
                target = self._pick_pending_locked(now, threshold)
            self._apply_approving_locked(target)

    def _pick_pending_locked(self, now, threshold) -> Optional[str]:
        best, best_ts = None, None
        for v in self.inflight.values():
            aid = v["agent_id"]
            if now - v["ts"] < threshold:
                continue
            if self.last_event.get(aid, 0) > v["ts"] + 0.05:
                continue
            if best_ts is None or v["ts"] < best_ts:
                best_ts, best = v["ts"], aid
        return best

    def _most_recent_busy_locked(self) -> Optional[str]:
        cand = [a for a in self.agents.values() if a["base_state"] == "busy"]
        if not cand:
            return None
        cand.sort(key=lambda x: x["ts"], reverse=True)
        return cand[0]["id"]

    def _apply_approving_locked(self, target) -> None:
        if target == self.approving:
            return
        self.approving = target
        if target and target in self.agents:
            log(f"approval WAIT agent={self.agents[target].get('label')} ({target[:8]})")
            activate_app()

    # ---- 给 StickS3 (WiFi) 的聚合视图 ----
    def global_view(self) -> dict:
        with self.lock:
            if self.approving and self.approving in self.agents:
                a = self.agents[self.approving]
                return {"state": "wait", "label": a.get("label", ""), "text": a.get("text", "")}
            ordered = sorted(self.agents.values(), key=lambda x: x["ts"], reverse=True)
            for a in ordered:
                if a["base_state"] == "busy":
                    return {"state": "busy", "label": a.get("label", ""), "text": a.get("text", "")}
            if ordered:
                a = ordered[0]
                return {"state": "idle", "label": a.get("label", ""), "text": a.get("text", "")}
            return {"state": "idle", "label": "", "text": ""}

    def snapshot(self) -> dict:
        with self.lock:
            agents = []
            for a in sorted(self.agents.values(), key=lambda x: x["ts"], reverse=True):
                eff = "wait" if self.approving == a["id"] else a["base_state"]
                agents.append({**a, "eff_state": eff})
            return {"approving": self.approving, "inflight": len(self.inflight),
                    "agents": agents}


STATE = State()


# =============================================================================
# HTTP
# =============================================================================
class Handler(BaseHTTPRequestHandler):
    server_version = "agent_pet_relay/2.0"

    def log_message(self, fmt, *args):  # noqa: N802
        pass

    def _read_json(self, max_len=256 * 1024):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0 or length > max_len:
            self._json(400, {"err": "bad length"})
            return None
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception as e:  # noqa: BLE001
            self._json(400, {"err": f"bad json: {e}"})
            return None

    def _json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/state":
            self._json(200, STATE.global_view())
        elif path == "/healthz":
            self._json(200, {"ok": True})
        elif path == "/":
            self._status_page()
        else:
            self._json(404, {"err": "not found"})

    def do_POST(self):  # noqa: N802
        if self.path == "/hook/status":
            req = self._read_json()
            if req is None:
                return
            STATE.set_status(req)
            self._json(200, {"ok": True})
        else:
            self._json(404, {"err": "not found"})

    def _status_page(self):
        snap = STATE.snapshot()
        def esc(s):
            return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        rows = "".join(
            f"<tr><td>{time.strftime('%H:%M:%S', time.localtime(a['ts']))}</td>"
            f"<td>{esc(a['label'])}</td><td>{esc(a['eff_state'])}</td>"
            f"<td>{esc(a['text'])}</td></tr>"
            for a in snap["agents"]
        )
        gv = STATE.global_view()
        html = (
            "<!doctype html><meta charset=utf-8><title>agent_pet relay</title>"
            "<meta http-equiv=refresh content=2>"
            "<style>body{font-family:-apple-system,system-ui,sans-serif;max-width:760px;"
            "margin:20px auto;padding:0 12px}td,th{border-bottom:1px solid #eee;padding:4px 6px;"
            "font-size:13px;text-align:left}</style><h2>agent_pet relay</h2>"
            f"<p>global=<b>{esc(gv['state'])}</b> &nbsp; approving={esc(snap['approving'])}"
            f" &nbsp; inflight={snap['inflight']}</p>"
            "<table><tr><th>updated</th><th>agent</th><th>state</th><th>doing</th></tr>"
            f"{rows}</table>"
        )
        body = html.encode("utf-8")
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class QuietHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    # TLS 握手失败 / 无证书客户端乱连时, 别刷一大堆 traceback.
    def handle_error(self, request, client_address):
        import sys
        exc = sys.exc_info()[1]
        # TLS 握手失败 / 非证书客户端乱连, 静默 (设 AGENT_PET_DEBUG=1 可看)
        if isinstance(exc, (ssl.SSLError, OSError)):
            if os.environ.get("AGENT_PET_DEBUG"):
                log(f"TLS握手失败 {client_address}: {exc}")
            return
        super().handle_error(request, client_address)


class TLSServer(QuietHTTPServer):
    """关键: accept() 不在主线程做 TLS 握手, 否则一个握手慢/不握手的连接会卡死
    整个 accept 循环, 后续连接被拒 (表现为客户端 connection refused).
    这里 accept 立刻返回, 握手推迟到每连接的工作线程里完成."""
    ssl_ctx: Optional[ssl.SSLContext] = None

    def get_request(self):
        sock, addr = self.socket.accept()
        ssock = self.ssl_ctx.wrap_socket(sock, server_side=True,
                                         do_handshake_on_connect=False)
        return ssock, addr


def make_ssl_context() -> Optional[ssl.SSLContext]:
    server_crt = CERTS_DIR / "server.crt"
    server_key = CERTS_DIR / "server.key"
    client_crt = CERTS_DIR / "client.crt"
    if not (server_crt.is_file() and server_key.is_file() and client_crt.is_file()):
        return None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(server_crt), str(server_key))
    ctx.load_verify_locations(str(client_crt))  # 拿 StickS3 的证书(公钥)当受信 CA
    ctx.verify_mode = ssl.CERT_REQUIRED          # 强制客户端出示证书 = 公钥认证
    return ctx


def detect_loop():
    while True:
        try:
            STATE.evaluate()
        except Exception as e:  # noqa: BLE001
            log(f"detect error: {e}")
        time.sleep(0.7)


def lan_ip() -> str:
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:  # noqa: BLE001
        return "127.0.0.1"


def all_ipv4() -> list:
    """列出本机所有非回环 IPv4 (多网卡时帮用户挑对的填给 StickS3)."""
    import socket
    ips = set()
    primary = lan_ip()
    if primary != "127.0.0.1":
        ips.add(primary)
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                ips.add(ip)
    except Exception:  # noqa: BLE001
        pass
    # macOS 上再补一手常见网卡
    for iface in ("en0", "en1", "en2", "bridge100"):
        try:
            out = subprocess.run(["ipconfig", "getifaddr", iface],
                                 capture_output=True, text=True, timeout=1.0)
            ip = out.stdout.strip()
            if ip and not ip.startswith("127."):
                ips.add(ip)
        except Exception:  # noqa: BLE001
            pass
    # 主网卡排前面
    ordered = ([primary] if primary in ips else []) + sorted(ips - {primary})
    return ordered


def parse_args():
    ap = argparse.ArgumentParser(description="agent_pet relay (HTTP loopback + HTTPS mTLS)")
    ap.add_argument("--host", default=os.environ.get("AGENT_PET_HOST", "0.0.0.0"),
                    help="HTTPS(mTLS) 对外监听地址")
    ap.add_argument("--port", type=int, default=int(os.environ.get("AGENT_PET_PORT", "8799")),
                    help="本地回环 HTTP 端口 (给 Cursor hook 上报)")
    ap.add_argument("--https-port", type=int,
                    default=int(os.environ.get("AGENT_PET_HTTPS_PORT", "8443")),
                    help="对外 HTTPS(mTLS) 端口 (给 StickS3 拉 /state)")
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    mode = str(CFG.get("detect_mode", "auto"))
    if ax_detect is None:
        log("AX 模块不可用 (缺 pyobjc?) -> 检测回退启发式")
    elif mode in ("auto", "ax"):
        if ax_detect.is_trusted():
            log("AX 可用 (已授辅助功能权限), 作首选审批检测")
        else:
            log("AX 未授权 (系统设置->隐私与安全性->辅助功能 勾选本程序) -> 暂回退启发式")

    threading.Thread(target=detect_loop, daemon=True).start()

    def make_server(addr, label):
        try:
            return QuietHTTPServer(addr, Handler)
        except OSError as e:  # noqa: BLE001
            log(f"!! 绑定 {label} {addr[0]}:{addr[1]} 失败: {e}")
            log("   端口可能被占用 -> 是不是已经有一个 relay 在跑? "
                "(lsof -nP -iTCP:%d)" % addr[1])
            raise SystemExit(2)

    # 1) 本地回环 HTTP: Cursor hook 上报 + 状态页 (不出网卡, 无需 TLS)
    http_server = make_server(("127.0.0.1", args.port), "HTTP")
    threading.Thread(target=http_server.serve_forever, daemon=True).start()
    log(f"hook 上报 -> http://127.0.0.1:{args.port}/hook/status")
    log(f"状态页 -> http://127.0.0.1:{args.port}/")

    # 2) 对外 HTTPS + mTLS: StickS3 拉 /state (强制客户端证书 = 公钥认证)
    ctx = make_ssl_context()
    https_server = None
    if ctx is None:
        log(f"!! 未找到证书 {CERTS_DIR}/  -> 先跑 ./gen_certs.sh, 否则 StickS3 连不上")
    else:
        TLSServer.ssl_ctx = ctx
        try:
            https_server = TLSServer((args.host, args.https_port), Handler)
        except OSError as e:  # noqa: BLE001
            log(f"!! 绑定 HTTPS {args.host}:{args.https_port} 失败: {e}")
            raise SystemExit(2)
        log(f"HTTPS(mTLS) 监听 {args.host}:{args.https_port}")
        ips = all_ipv4()
        if ips:
            log("StickS3 配网里 Relay 地址填下面【与 StickS3 同一 WiFi 网段】那个:")
            for ip in ips:
                log(f"    {ip}:{args.https_port}")
        else:
            log(f"StickS3 配网填: <Mac局域网IP>:{args.https_port}")

    try:
        if https_server is not None:
            https_server.serve_forever()
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        http_server.server_close()
        if https_server is not None:
            https_server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
