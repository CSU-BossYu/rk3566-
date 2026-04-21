#!/bin/sh
set -e

BASE="/data/access"
APP="$BASE/current/bin/demo_rga_v4l2"
LOG_DIR="$BASE/logs"
RUN_DIR="$BASE/run"
PIDFILE="$RUN_DIR/app.pid"

mkdir -p "$LOG_DIR" "$RUN_DIR"

# 动态库优先从当前版本加载
export LD_LIBRARY_PATH="$BASE/current/lib:$LD_LIBRARY_PATH"
export RKNN_LOG_LEVEL=1

# 设备权限（root 下有效）
if [ "$(id -u)" -eq 0 ]; then
  chmod 666 /dev/dri/* 2>/dev/null || true
  chmod 666 /dev/input/event* 2>/dev/null || true
  chmod 666 /dev/video* 2>/dev/null || true
fi

STOP=0
trap 'STOP=1; if [ -f "$PIDFILE" ]; then kill "$(cat "$PIDFILE")" 2>/dev/null || true; fi; exit 0' INT TERM

echo "[start] wrapper pid=$$" >> "$LOG_DIR/app.log"

while [ $STOP -eq 0 ]; do
  if [ ! -x "$APP" ]; then
    echo "[start] missing app: $APP" >> "$LOG_DIR/app.log"
    sleep 2
    continue
  fi

  echo "[start] launching: $APP (current=$(readlink $BASE/current 2>/dev/null || echo N/A))" >> "$LOG_DIR/app.log"
  "$APP" >> "$LOG_DIR/app.log" 2>&1 &
  CHILD=$!
  echo "$CHILD" > "$PIDFILE"

  wait "$CHILD"
  RC=$?
  rm -f "$PIDFILE"

  echo "[start] exit rc=$RC at $(date '+%F %T')" >> "$LOG_DIR/app.log"

  # backoff，避免疯狂刷屏；同时给 BootReconcile/回滚留窗口
  sleep 1
done

exit 0
