#!/bin/sh
# 板端运行脚本：指定 EGLFS + HDMI1 输出
cd "$(dirname "$0")"
export QT_QPA_PLATFORM=eglfs
export QT_QPA_EGLFS_KMS_CONFIG="$(pwd)/eglfs_kms_config.json"
export QT_QPA_EGLFS_HIDECURSOR=0
exec ./sample_qt "$@"
