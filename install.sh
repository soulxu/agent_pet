#!/usr/bin/env bash
# =============================================================================
# agent_pet 安装脚本 (macOS).
#
#   ./install.sh              装 Cursor hooks + 依赖 + 默认配置
#   ./install.sh --launchd    额外把 relay 装成 launchd (开机自启)
#   ./install.sh --uninstall  移除本项目装进 ~/.cursor/hooks.json 的 hook + launchd
#
# 幂等: 重复跑只刷新自己的条目, 不动你已有的其它 hooks.
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOOK="$HERE/hooks/hook.py"
RELAY="$HERE/relay/relay.py"
PYTHON="$(command -v python3)"
CURSOR_HOOKS="$HOME/.cursor/hooks.json"
CFG_DIR="$HOME/.config/agent_pet"
CFG="$CFG_DIR/config.json"
LAUNCH_PLIST="$HOME/Library/LaunchAgents/com.agentpet.relay.plist"
LOG="$CFG_DIR/relay.log"

uninstall() {
  echo "[uninstall] 从 $CURSOR_HOOKS 移除 agent_pet hook"
  if [[ -f "$CURSOR_HOOKS" ]]; then
    HOOK="$HOOK" "$PYTHON" - "$CURSOR_HOOKS" <<'PY'
import json, os, sys
path = sys.argv[1]; hook = os.environ["HOOK"]
data = json.load(open(path))
for ev, arr in list(data.get("hooks", {}).items()):
    arr = [h for h in arr if hook not in (h.get("command") or "")]
    if arr: data["hooks"][ev] = arr
    else:   del data["hooks"][ev]
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print("  done")
PY
  fi
  if [[ -f "$LAUNCH_PLIST" ]]; then
    launchctl unload "$LAUNCH_PLIST" 2>/dev/null || true
    rm -f "$LAUNCH_PLIST"
    echo "[uninstall] 已移除 launchd"
  fi
  echo "[uninstall] 完成 (配置 $CFG 保留)"
  exit 0
}

[[ "${1:-}" == "--uninstall" ]] && uninstall

[[ -n "$PYTHON" ]] || { echo "需要 python3"; exit 1; }
chmod +x "$HOOK" "$RELAY" "$HERE/firmware/flash.sh" 2>/dev/null || true

# 0) 依赖: pyobjc (AX 检测, 可选但推荐; 缺了自动回退启发式)
echo "[deps] 安装 relay 依赖 (pyobjc) ..."
"$PYTHON" -m pip install --user -r "$HERE/relay/requirements.txt" || \
  echo "[deps] !! 安装失败, 请手动: $PYTHON -m pip install -r $HERE/relay/requirements.txt"

# 1) 默认配置
mkdir -p "$CFG_DIR"
if [[ ! -f "$CFG" ]]; then
  cp "$HERE/hooks/config.example.json" "$CFG"
  echo "[config] 写入默认配置 $CFG"
else
  echo "[config] 已存在 $CFG, 保留"
fi

# 2) 合并 hooks 到 ~/.cursor/hooks.json
mkdir -p "$HOME/.cursor"
[[ -f "$CURSOR_HOOKS" ]] && cp "$CURSOR_HOOKS" "$CURSOR_HOOKS.bak.$(date +%s)" && echo "[hooks] 备份旧 hooks.json"

HOOK="$HOOK" TEMPLATE="$HERE/hooks/hooks.template.json" "$PYTHON" - "$CURSOR_HOOKS" <<'PY'
import json, os, sys
path = sys.argv[1]; hook = os.environ["HOOK"]
tmpl = json.load(open(os.environ["TEMPLATE"]))
try:
    data = json.load(open(path))
except Exception:
    data = {"version": 1, "hooks": {}}
data.setdefault("version", 1); data.setdefault("hooks", {})
for ev, arr in tmpl["hooks"].items():
    existing = data["hooks"].get(ev, [])
    existing = [h for h in existing if hook not in (h.get("command") or "")]
    for h in arr:
        h = dict(h); h["command"] = h["command"].replace("__HOOK__", hook)
        existing.append(h)
    data["hooks"][ev] = existing
json.dump(data, open(path, "w"), indent=2, ensure_ascii=False)
print(f"[hooks] 已写入 {path}")
for ev in tmpl["hooks"]:
    print(f"  + {ev}")
PY

# 3) (可选) launchd 自启
if [[ "${1:-}" == "--launchd" ]]; then
  mkdir -p "$HOME/Library/LaunchAgents"
  sed -e "s#__PYTHON__#$PYTHON#g" \
      -e "s#__RELAY__#$RELAY#g" \
      -e "s#__LOG__#$LOG#g" \
      "$HERE/relay/com.agentpet.relay.plist" > "$LAUNCH_PLIST"
  launchctl unload "$LAUNCH_PLIST" 2>/dev/null || true
  launchctl load "$LAUNCH_PLIST"
  echo "[launchd] 已加载, relay 会开机自启 (日志: $LOG)"
fi

cat <<EOF

安装完成. 下一步:
  1) 生成 mTLS 证书 (必须在烧固件前):  $HERE/relay/gen_certs.sh
     会生成 relay/certs/ 和 firmware/certs.h (都不入库).
  2) 烧固件:
       StickS3:    $HERE/firmware/flash.sh            (有屏+联网, 蓝牙名 AgentPet)
       Atom Lite:  TARGET=atom $HERE/firmware/flash.sh (纯键盘, 蓝牙名 AgentPet-AtomLite)
     按键: StickS3 BtnA=回车/长按Shift+回车, BtnB=Esc/长按配网;
           Atom 单键 单击=回车/长按Shift+回车/双击=Esc.
     (Atom Lite 不联网, 不需要 gen_certs.sh / 配网, 烧完直接连蓝牙用.)
  3) macOS 蓝牙里把设备当键盘连接 (AgentPet / AgentPet-AtomLite).
  4) [仅 StickS3] 启动 relay (没用 --launchd 的话):  $PYTHON $RELAY
     - HTTP 回环 8799 收 hook; HTTPS+mTLS 8443 给 StickS3 拉状态.
     - 想用 AX 精准检测审批: 系统设置 -> 隐私与安全性 -> 辅助功能,
       勾选运行 relay 的程序 (Terminal / iTerm / python). 没勾就自动回退启发式.
  5) [仅 StickS3] 配网 (首次自动开热点 AgentPet-XXXX, http://192.168.4.1):
     填 WiFi + Relay 地址 = relay 打印的 "Mac IP:8443".
  6) 重启 Cursor 让 hooks 生效 (设置 -> Hooks 里能看到).

状态页: http://127.0.0.1:8799/   |   设备状态: https://<MacIP>:8443/state (需客户端证书)
EOF
