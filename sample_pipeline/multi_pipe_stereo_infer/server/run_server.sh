#!/bin/sh

USB_NET_INF=usb0
USB_SERVER_IP=192.168.5.5


ops_module=("20510500.sif0_qos" "7" "7"
	"20510580.sif1_qos" "7" "7"
	"20510600.sif2_qos" "7" "7"
	"20510680.sif3_qos" "7" "7"
	"20510280.isp_axi5_hdr_qos" "4" "4"
	"20510300.isp_axi4_mcm_qos" "4" "4"
	"20510380.isp_axi3_sp2_qos" "4" "4"
	"20510480.isp_axi1_mp_qos" "4" "4"
	"20510100.dw230_gdc_qos" "4" "4"
	"20510180.dw230_scalar2_qos" "4" "4"
	"20510200.dw230_scalar3_qos" "4" "4"
	"20520000.bpu_qos" "0" "0"
	"20530000.video_qos" "0" "0"
	"20530080.jpeg_qos" "0" "0")

function write_read_check_list() {
	local _i=0
	local _read_v=
	local _base_path="/sys/bus/platform/drivers/noc_qos"
	local _readp_path="read_priority_qos_ctrl/priority"
	local _writep_path="write_priority_qos_ctrl/priority"

	for (( _i=0;_i<${#ops_module[@]};_i+=3 )); do
		local _module=${ops_module[$_i]}
		local _read_qos=${ops_module[$_i+1]}
		local _write_qos=${ops_module[$_i+2]}

		if [ ! -f ${_base_path}/${_module}/${_readp_path} ] ||
		   [ ! -f ${_base_path}/${_module}/${_writep_path} ];then
			echo "${_module} QoS not initialized! Skipping!"
			continue
		fi

		echo ${_read_qos} > ${_base_path}/${_module}/${_readp_path}
		_read_v=$(cat ${_base_path}/${_module}/${_readp_path})
		if [[ "${_read_qos}" != "${_read_v: -1}" ]]; then
			echo "Config ${_module} read qos fail(${_read_qos} != ${_read_v: -1}), please check it"
		fi

		echo ${_write_qos} > ${_base_path}/${_module}/${_writep_path}
		_read_v=$(cat ${_base_path}/${_module}/${_writep_path})
		if [[ "${_write_qos}" != "${_read_v: -1}" ]]; then
			echo "Config ${_module} write qos fail(${_write_qos} != ${_read_v: -1}), please check it"
		fi

		echo "Config ${_module} qos read: ${_read_qos} write: ${_write_qos} done"
	done
}

function config_usb()
{
	# USB3.0 Device Mode
	echo device > /sys/class/usb_role/35100000.usb-role-switch/role

	/etc/init.d/usb3.0-gadget.sh restart ecm

	ifconfig ${USB_NET_INF} ${USB_SERVER_IP} netmask 255.255.255.0

	#echo 256 > /sys/module/usbcore/parameters/usbfs_memory_mb

	echo 7 > /sys/bus/platform/drivers/noc_qos/20550380.usb3_qos/read_priority_qos_ctrl/priority
	echo 7 >/sys/bus/platform/drivers/noc_qos/20550380.usb3_qos/write_priority_qos_ctrl/priority


	ifconfig ${INTERFACE} mtu 5000

	echo "Configure USB Network."
}

function config_drm()
{
	modprobe panel-jc-050hd134
	modprobe galcore
	modprobe vio_n2d
	modprobe lontium_lt8618
	modprobe vs-x5-syscon-bridge
	modprobe vs_drm
	sleep 2
	echo 1 > /sys/kernel/debug/csi-bridge-3e020000.mipi_csi0/clk_continuous
	echo "Configure DRM."
}

function config_common()
{
	# Performance Mode
	echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
	write_read_check_list

	# Clear GDC Bin
	rm /tmp/left_camera_gdc.bin
	rm /tmp/right_camera_gdc.bin

	# OpenCV LIB
	export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/userdata/multi_pipe_stereo_infer/3rdparty/lib_opencv4.5.4/lib

	echo "Configure Common."
}

config_common

if [ -z "$1" ] || [ "$1" = "local" ]; then
	INTERFACE=local
	echo "Runing on local debug mode."
elif [ "$1" = "help" ]; then
	./multi_pipe_stereo_infer -h
	exit 0
elif [ "$1" = "mipi" ]; then
	echo "Runing on MIPI-CSI TX mode."
	INTERFACE=mipi
	CFG_LOCK_PATH="/var/lock/drm-lock"
	if [ -f "$CFG_LOCK_PATH" ]; then
		echo "$CFG_LOCK_PATH Exist. No need to configure DRM repeatedly."
	else
		touch "$CFG_LOCK_PATH"
		config_drm
	fi
elif [ "$1" = "usb" ]; then
	echo "Runing on USB mode."
	INTERFACE=${USB_NET_INF}
	CFG_LOCK_PATH="/var/lock/usb-lock"
	if [ -f "$CFG_LOCK_PATH" ]; then
		echo "$CFG_LOCK_PATH Exist. No need to configure USB repeatedly."
	else
		touch "$CFG_LOCK_PATH"
		config_usb
	fi
else
	echo "Unknow Mode: $1"
	exit 0
fi


# Run App
./multi_pipe_stereo_infer -i ${INTERFACE} -c "sensor=52" -c "sensor=53" -b -g 1

