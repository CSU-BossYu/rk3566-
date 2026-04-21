#!/bin/sh
# 用法：
#   sh ota_apply.sh [server_url]
# 为空则读取 /data/access/cfg/ota.conf

SERVER="$1"
BIN=/data/access/current/bin/demo_rga_v4l2

if [ ! -x "$BIN" ]; then
  echo "[ota_apply] missing: $BIN"
  exit 1
fi

exec "$BIN" --ota "$SERVER"
