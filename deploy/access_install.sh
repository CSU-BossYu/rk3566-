#!/bin/sh
set -e

# 用法：
#   sh access_install.sh <version>
# 若不传 version：
#   1) 优先读 ./VERSION
#   2) 否则用时间戳作为版本号

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

VER="${1:-}"
if [ -z "$VER" ] && [ -f "$SELF_DIR/VERSION" ]; then
  VER="$(cat "$SELF_DIR/VERSION" | tr -d '\r\n')"
fi
if [ -z "$VER" ]; then
  VER="$(date +%Y%m%d_%H%M%S)"
fi

APP_BASE=/data/access
DST="$APP_BASE/versions/$VER"

echo "[install] version=$VER"
echo "[install] dst=$DST"

mkdir -p "$DST/bin" "$DST/lib" "$DST/models"
mkdir -p "$APP_BASE/cfg" "$APP_BASE/ota/logs" "$APP_BASE/logs" "$APP_BASE/run"

# 复制可执行文件与依赖
cp -f "$SELF_DIR/demo_rga_v4l2" "$DST/bin/"
chmod 755 "$DST/bin/demo_rga_v4l2"

if [ -d "$SELF_DIR/lib" ]; then
  cp -af "$SELF_DIR/lib/." "$DST/lib/"
fi

if [ -d "$SELF_DIR/models" ]; then
  cp -af "$SELF_DIR/models/." "$DST/models/"
fi

# 版本内启动脚本（供 init.d 调用）
cp -f "$SELF_DIR/start.sh" "$DST/start.sh"
chmod 755 "$DST/start.sh"

# 复制（可选）ota_apply 工具脚本到 base（方便手工调试）
if [ -f "$SELF_DIR/ota_apply.sh" ]; then
  cp -f "$SELF_DIR/ota_apply.sh" "$APP_BASE/ota_apply.sh"
  chmod 755 "$APP_BASE/ota_apply.sh"
fi

# 初始化 OTA 配置
if [ ! -f "$APP_BASE/cfg/ota.conf" ]; then
  cat > "$APP_BASE/cfg/ota.conf" <<EOF
# OTA server base url
# example: OTA_SERVER=http://192.168.1.10:5000
OTA_SERVER=http://127.0.0.1:5000
EOF
  chmod 644 "$APP_BASE/cfg/ota.conf"
fi

# 初始化 current / last_good
ln -sfn "$DST" "$APP_BASE/current"
if [ ! -L "$APP_BASE/last_good" ]; then
  ln -s "$DST" "$APP_BASE/last_good"
fi

# 安装 init.d 脚本
cp -f "$SELF_DIR/S90access" /etc/init.d/S90access
chmod 755 /etc/init.d/S90access

sync

echo "[install] restart service..."
/etc/init.d/S90access restart || true

echo "[install] done."
