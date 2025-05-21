#!/bin/sh

set -e

. /etc/profile.d/environment.sh

local_path=$(dirname "$(readlink -f "$0")")

# 配置cpu bpu ddr 降频的结温温度
echo 105000 > /sys/class/thermal/thermal_zone0/trip_point_0_temp
echo 105000 > /sys/class/thermal/thermal_zone1/trip_point_1_temp

# 设置cpu运行在高性能模式
echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor

BOARD_ID=$(cat /sys/class/socinfo/board_id)

case "$BOARD_ID" in
	0x03*|0x05*)
		#RDK
		modprobe sii902x
		modprobe panel-wh-cm480
		modprobe ft5406
		modprobe hb_bl
		modprobe vio_n2d
		modprobe vs-x5-syscon-bridge
		modprobe vs_drm
		;;
	*)
		#EVB
		modprobe panel-jc-050hd134
		modprobe galcore
		modprobe vio_n2d
		modprobe lontium_lt8618
		modprobe vs-x5-syscon-bridge
		modprobe vs_drm
esac

cd "${local_path}"/sunrise_camera/bin || exit 1
echo "============= Start Sunrise Camera ==============="
export LD_LIBRARY_PATH=../bin:"${LD_LIBRARY_PATH}"
if [ "$#" -eq 0 ]; then
	./sunrise_camera
else
	gdb -ex "handle SIGUSR2 nostop" -ex "handle SIGPIPE nostop" -ex "run"  ./sunrise_camera
fi
