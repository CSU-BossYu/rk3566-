#!/bin/sh
# 用法：
#   cd deploy && ./run.sh
# 或者从任意目录执行 deploy/run.sh，都能保证相对路径正确

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1

# 保险：优先从同目录 lib/ 找动态库
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# 可选：你之前为了调试可能用过这些环境变量
# export LV_NO_HANDLER=1   # 跳过 lv_timer_handler 循环（不建议长期打开）
# export LV_NO_EVDEV=1     # 禁用触摸输入（调试用）

# 关闭/降低 RKNN runtime 日志（常见可用项）
export RKNN_LOG_LEVEL=0
export RKNN_LOG_LEVEL=1   # 若 0 无效，试 1（只保留 error/warn）


# 如果是 root，顺手放开设备节点权限（非 root 会失败，但不影响继续跑）
if [ "$(id -u)" -eq 0 ]; then
  chmod 666 /dev/dri/* 2>/dev/null || true
  chmod 666 /dev/input/event* 2>/dev/null || true
  chmod 666 /dev/video* 2>/dev/null || true
fi

chmod +x "$DIR/demo_rga_v4l2" 2>/dev/null || true
exec "$DIR/demo_rga_v4l2" "$@"
