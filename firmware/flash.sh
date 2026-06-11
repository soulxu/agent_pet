#!/usr/bin/env bash
# agent_pet 固件编译 + 烧录脚本. 支持两种设备, 用 TARGET 选择:
#
#   firmware/flash.sh                       # 默认 sticks3, 自动找端口, 编译并烧录
#   TARGET=atom firmware/flash.sh           # 烧录 M5Atom Lite
#   firmware/flash.sh build                 # 只编译
#   TARGET=atom firmware/flash.sh build     # 只编译 atom
#   firmware/flash.sh upload                # 只烧录 (用上次编译产物)
#   firmware/flash.sh monitor               # 只串口监视
#   firmware/flash.sh all                   # 编译 + 烧录 + 监视
#   PORT=/dev/cu.usbmodem11201 firmware/flash.sh   # 强制指定端口
#
# 进入下载模式:
#   StickS3: 长按机身侧面 reset 键约 2 秒, 等绿色 LED 闪烁后松开.
#   Atom Lite: 一般免手动, 插上即可; 若失败, 按住主键再插 USB.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"   # 仓库根 (有 acli + .arduino15)
SKETCH="$HERE"
ACLI="$ROOT/acli"
TARGET="${TARGET:-sticks3}"
BAUD="${BAUD:-115200}"
EXTRA_BUILD=()
HIDE_LOCAL_PARTITIONS=0

case "$TARGET" in
  sticks3|stick|s3)
    # 8MB flash + custom 分区表 (partitions.csv)
    FQBN="m5stack:esp32:m5stack_sticks3:FlashSize=8M,PartitionScheme=custom"
    ;;
  atom|atomlite|lite)
    # M5Atom Lite (ESP32, 4MB flash). huge_app 给到 3MB app.
    # compiler.cpp.extra_flags 追加 -DAGENTPET_ATOM 让固件走单键+LED 分支.
    FQBN="m5stack:esp32:m5stack_atom:PartitionScheme=huge_app"
    EXTRA_BUILD=(--build-property "compiler.cpp.extra_flags=-DAGENTPET_ATOM")
    # sketch 本地 partitions.csv 是给 StickS3 的 8MB 表, 会无视上面的 PartitionScheme,
    # 烧进 4MB 的 Atom 会 boot 失败. 编译时临时隐藏它, 改用内置 huge_app.
    HIDE_LOCAL_PARTITIONS=1
    ;;
  *)
    echo "[flash] 未知 TARGET=$TARGET (可选: sticks3 | atom)" >&2
    exit 2
    ;;
esac
echo "[flash] TARGET=$TARGET FQBN=$FQBN"

if [[ ! -x "$ACLI" ]]; then
  echo "[flash] cannot find acli at $ACLI" >&2
  exit 1
fi

find_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  local p
  # StickS3 = usbmodem (原生 USB); Atom Lite = usbserial / wchusbserial (CH9102 等).
  p="$(ls /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB* 2>/dev/null | head -n 1 || true)"
  if [[ -z "$p" ]]; then
    echo "[flash] 没找到串口设备, 插上设备 (StickS3 长按侧面 reset 进下载模式) 或用 PORT= 指定" >&2
    exit 1
  fi
  echo "$p"
}

do_build() {
  echo "[flash] compile $SKETCH"
  local pcsv="$SKETCH/partitions.csv" hidden="$SKETCH/partitions.csv.hidden"
  local clean=()
  if [[ "$HIDE_LOCAL_PARTITIONS" == "1" && -f "$pcsv" ]]; then
    mv "$pcsv" "$hidden"
    trap 'mv -f "'"$hidden"'" "'"$pcsv"'" 2>/dev/null || true' EXIT
    # 增量编译会复用上次(可能含 8MB 分区)的 partitions.bin, 必须 --clean 重生成.
    clean=(--clean)
  fi
  "$ACLI" compile "${clean[@]+"${clean[@]}"}" --fqbn "$FQBN" ${EXTRA_BUILD[@]+"${EXTRA_BUILD[@]}"} "$SKETCH"
  if [[ -f "$hidden" ]]; then mv -f "$hidden" "$pcsv"; trap - EXIT; fi
}
do_upload() { echo "[flash] upload -> $1"; "$ACLI" upload --fqbn "$FQBN" -p "$1" "$SKETCH"; }
do_monitor(){ echo "[flash] monitor $1 @ $BAUD (Ctrl-C to exit)"; "$ACLI" monitor --port "$1" -c "baudrate=$BAUD"; }

cmd="${1:-flash}"
case "$cmd" in
  build)   do_build ;;
  upload)  do_upload "$(find_port)" ;;
  monitor) do_monitor "$(find_port)" ;;
  all)     port="$(find_port)"; do_build; do_upload "$port"; do_monitor "$port" ;;
  flash|"") port="$(find_port)"; do_build; do_upload "$port" ;;
  *) echo "usage: $0 [build|upload|monitor|all|flash]" >&2; exit 2 ;;
esac
