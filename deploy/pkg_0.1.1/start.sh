#!/bin/sh
set -e

BASE="/data/access"
CUR="$BASE/current"
APP="$CUR/bin/demo_rga_v4l2"

LOG_DIR="$BASE/logs"
RUN_DIR="$BASE/run"
PIDFILE="$RUN_DIR/app.pid"

mkdir -p "$LOG_DIR" "$RUN_DIR"

if [ ! -x "$APP" ]; then
  echo "[start] missing app: $APP"
  exit 1
fi

# 只从当前版本加载动态库（避免被系统库/旧库污染）
export LD_LIBRARY_PATH="$CUR/lib:$LD_LIBRARY_PATH"

# 可选：数据库路径（你如果希望固定在 /data/access 下）
if [ -z "$FACE_DB" ]; then
  export FACE_DB="$BASE/cfg/face.db"
fi

# RKNN log（无效就忽略）
export RKNN_LOG_LEVEL=1

# 设备权限（root 下才做）
if [ "$(id -u)" -eq 0 ]; then
  chmod 666 /dev/dri/* 2>/dev/null || true
  chmod 666 /dev/input/event* 2>/dev/null || true
  chmod 666 /dev/video* 2>/dev/null || true
fi

# 记录 PID：因为后面 exec，pid 不变
echo $$ > "$PIDFILE"

cd "$CUR"

# 让日志落到文件，便于现场排查 OTA/自愈
exec "$APP" >> "$LOG_DIR/app.log" 2>&1
