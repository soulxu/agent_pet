#!/usr/bin/env bash
# =============================================================================
# gen_certs.sh - 给 StickS3 <-> relay 的 mTLS 生成自签证书 (EC P-256).
#
# 产物:
#   relay/certs/server.key  server.crt   relay 的 TLS 服务器证书 (StickS3 setInsecure 不强验它)
#   relay/certs/client.key  client.crt   StickS3 的客户端证书 (relay 用它的公钥验证设备身份)
#   firmware/certs.h                      把 client.crt + client.key (+ server.crt) 嵌入固件
#
# StickS3 出示 client 证书 -> relay 强制验证 (CERT_REQUIRED), 即"公钥认证".
# 私钥写进 firmware/certs.h 烧入设备; 默认会保留 relay/certs/client.key 以便重生成 certs.h,
# 想更安全可加 --wipe-client-key 在生成后删掉本地 client.key 副本.
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERTS_DIR="$HERE/certs"
FW_HEADER="$HERE/../firmware/certs.h"
DAYS=3650
WIPE_CLIENT_KEY=0

for arg in "$@"; do
  case "$arg" in
    --wipe-client-key) WIPE_CLIENT_KEY=1 ;;
    *) echo "未知参数: $arg"; exit 1 ;;
  esac
done

command -v openssl >/dev/null || { echo "需要 openssl"; exit 1; }
mkdir -p "$CERTS_DIR"

# 探测本机所有局域网 IPv4, 写进 server 证书 SAN. 设备按 IP 连时要靠 SAN 过校验.
detect_ips() {
  local ips=""
  for iface in en0 en1 en2 en3 bridge100; do
    local ip
    ip="$(ipconfig getifaddr "$iface" 2>/dev/null || true)"
    [ -n "$ip" ] && ips="$ips $ip"
  done
  echo "$ips"
}

SAN="DNS:agentpet.local,DNS:localhost,IP:127.0.0.1"
for ip in $(detect_ips); do
  SAN="$SAN,IP:$ip"
done
echo "[certs] server 证书 SAN: $SAN"

gen_client() {
  openssl ecparam -name prime256v1 -genkey -noout -out "$CERTS_DIR/client.key"
  openssl req -new -x509 -key "$CERTS_DIR/client.key" \
    -out "$CERTS_DIR/client.crt" -days "$DAYS" -subj "/CN=agentpet-stick" >/dev/null 2>&1
  chmod 600 "$CERTS_DIR/client.key"
  echo "[certs] client.crt / client.key  (CN=agentpet-stick)"
}

gen_server() {
  openssl ecparam -name prime256v1 -genkey -noout -out "$CERTS_DIR/server.key"
  openssl req -new -x509 -key "$CERTS_DIR/server.key" \
    -out "$CERTS_DIR/server.crt" -days "$DAYS" -subj "/CN=agentpet-relay" \
    -addext "subjectAltName=$SAN" >/dev/null 2>&1
  chmod 600 "$CERTS_DIR/server.key"
  # 校验 SAN 真写进去了 (LibreSSL/openssl 都支持 -addext, 没写进去就报警)
  if ! openssl x509 -in "$CERTS_DIR/server.crt" -noout -text 2>/dev/null | grep -q "Subject Alternative Name"; then
    echo "[certs] !! 警告: server 证书没写入 SAN, 设备按 IP 连会校验失败 (升级 openssl?)"
  fi
  echo "[certs] server.crt / server.key  (CN=agentpet-relay)"
}

gen_server
gen_client

emit() {  # emit <symbol> <file>
  echo "static const char $1[] PROGMEM = R\"CERT("
  cat "$2"
  echo ")CERT\";"
  echo
}

{
  echo "// 自动生成, 勿手改. 由 relay/gen_certs.sh 生成于 $(date '+%Y-%m-%d %H:%M:%S')"
  echo "// 含 StickS3 的客户端私钥, 不要提交到公开仓库."
  echo "#pragma once"
  echo
  emit CLIENT_CERT "$CERTS_DIR/client.crt"
  emit CLIENT_KEY  "$CERTS_DIR/client.key"
  emit RELAY_CERT  "$CERTS_DIR/server.crt"
} > "$FW_HEADER"
echo "[certs] 写入 $FW_HEADER"

if [[ "$WIPE_CLIENT_KEY" == "1" ]]; then
  rm -f "$CERTS_DIR/client.key"
  echo "[certs] 已删除本地 client.key (私钥只剩固件里那份)"
fi

echo "[certs] 完成. relay 用 certs/ 下的 server.* 和 client.crt; 固件用 certs.h"
