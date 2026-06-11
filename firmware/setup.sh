#!/usr/bin/env bash
# =============================================================================
# agent_pet 工具链一键安装 (自包含, 跨 clone 可复现).
#
# 干的事:
#   1) 准备 arduino-cli (优先复用已有的; 没有就按版本下载到 .toolchain/)
#   2) 写一份仓库内的 arduino-cli.yaml (board URLs + 数据目录都在 .toolchain/)
#   3) 装 ESP32(m5stack) core 和固件依赖库 (M5Unified / M5GFX / ArduinoJson)
#   4) 生成 .toolchain/acli 包装脚本, 供 flash.sh 调用
#
# 全部装进 <仓库>/.toolchain/, 不污染系统, 也不动你 ~/.arduino15.
# 幂等: 重复跑会跳过已装好的部分.
#
#   firmware/setup.sh            # 安装/补齐工具链
#   ACLI_VER=1.4.0 firmware/setup.sh
#   ARDUINO_CLI=/usr/local/bin/arduino-cli firmware/setup.sh   # 复用现成 cli
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # firmware/
REPO="$(cd "$HERE/.." && pwd)"                         # 仓库根
TC="$REPO/.toolchain"
CFG="$TC/arduino-cli.yaml"
DATA="$TC/data"

# 版本锁定 (与开发机一致, 保证可复现)
ACLI_VER="${ACLI_VER:-1.4.0}"
CORE="m5stack:esp32@3.3.7"
LIBS=("M5Unified@0.2.14" "M5GFX@0.2.20" "ArduinoJson@7.4.3")

ESP32_URL="https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
M5_URL="https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json"

mkdir -p "$TC" "$DATA"

# --- 1) 找/下 arduino-cli --------------------------------------------------
locate_cli() {
  # 顺序: 显式指定 > 已下好的 > 开发机父目录 > 系统 PATH
  if [[ -n "${ARDUINO_CLI:-}" && -x "${ARDUINO_CLI}" ]]; then echo "$ARDUINO_CLI"; return; fi
  if [[ -x "$TC/arduino-cli" ]]; then echo "$TC/arduino-cli"; return; fi
  local c
  for c in "$REPO/../tools/arduino-cli" "$REPO/../../tools/arduino-cli"; do
    [[ -x "$c" ]] && { echo "$c"; return; }
  done
  command -v arduino-cli 2>/dev/null || true
}

download_cli() {
  local os arch asset url tmp
  case "$(uname -s)" in
    Darwin) os="macOS" ;;
    Linux)  os="Linux" ;;
    *) echo "[setup] 不支持的系统 $(uname -s); 请手动安装 arduino-cli 后用 ARDUINO_CLI= 指定" >&2; exit 1 ;;
  esac
  case "$(uname -m)" in
    arm64|aarch64) arch="ARM64" ;;
    x86_64|amd64)  arch="64bit" ;;
    armv7l)        arch="ARMv7" ;;
    *) echo "[setup] 不支持的架构 $(uname -m)" >&2; exit 1 ;;
  esac
  asset="arduino-cli_${ACLI_VER}_${os}_${arch}.tar.gz"
  url="https://downloads.arduino.cc/arduino-cli/${asset}"
  tmp="$(mktemp -d)"
  echo "[setup] 下载 arduino-cli ${ACLI_VER} (${os}_${arch})"
  curl -fsSL "$url" -o "$tmp/cli.tgz"
  tar -xzf "$tmp/cli.tgz" -C "$tmp" arduino-cli
  mv "$tmp/arduino-cli" "$TC/arduino-cli"
  chmod +x "$TC/arduino-cli"
  rm -rf "$tmp"
}

CLI="$(locate_cli)"
if [[ -z "$CLI" ]]; then
  command -v curl >/dev/null || { echo "[setup] 需要 curl 下载 arduino-cli" >&2; exit 1; }
  download_cli
  CLI="$TC/arduino-cli"
fi
echo "[setup] arduino-cli = $CLI ($("$CLI" version 2>/dev/null | head -1))"

# --- 2) 仓库内配置文件 ------------------------------------------------------
cat > "$CFG" <<YAML
board_manager:
  additional_urls:
    - $ESP32_URL
    - $M5_URL
build_cache:
  path: $DATA/cache
directories:
  data: $DATA
  downloads: $DATA/staging
  user: $TC
network:
  # m5stack 工具链有 300MB+, 默认 1m 读超时在慢网会断; 放宽到 30m.
  connection_timeout: 1800s
YAML
echo "[setup] 写入 $CFG"

ACLI=("$CLI" --config-file "$CFG")

# --- 3) core + 依赖库 ------------------------------------------------------
echo "[setup] 更新 board index ..."
"${ACLI[@]}" core update-index

if "${ACLI[@]}" core list 2>/dev/null | grep -q "^m5stack:esp32[[:space:]].*3\.3\.7"; then
  echo "[setup] core $CORE 已装, 跳过"
else
  echo "[setup] 安装 core $CORE (体积较大, 含编译器, 请耐心) ..."
  "${ACLI[@]}" core install "$CORE"
fi

echo "[setup] 安装依赖库 ${LIBS[*]} ..."
"${ACLI[@]}" lib install "${LIBS[@]}"

# --- 4) acli 包装脚本 ------------------------------------------------------
cat > "$TC/acli" <<SH
#!/usr/bin/env bash
# 自动生成: 固定使用本仓库 .toolchain 的配置与工具链.
exec "$CLI" --config-file "$CFG" "\$@"
SH
chmod +x "$TC/acli"

cat <<EOF

[setup] 工具链就绪 ✅  全部位于 $TC
  arduino-cli : $CLI
  core        : $CORE
  libs        : ${LIBS[*]}

下一步编译/烧录:
  firmware/flash.sh                 # StickS3 编译+烧录
  TARGET=atom firmware/flash.sh     # Atom Lite 编译+烧录
  firmware/flash.sh build           # 只编译
EOF
