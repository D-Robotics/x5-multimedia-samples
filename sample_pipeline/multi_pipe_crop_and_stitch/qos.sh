#!/bin/sh

set_qos() {
	local _module=$1
	local _read_qos=$2
	local _write_qos=$3

	local _base_path="/sys/bus/platform/drivers/noc_qos"
	local _readp_path="read_priority_qos_ctrl/priority"
	local _writep_path="write_priority_qos_ctrl/priority"

	if [ ! -f ${_base_path}/${_module}/${_readp_path} ] ||
	   [ ! -f ${_base_path}/${_module}/${_writep_path} ];then
		echo "${_module} QoS not initialized! Skipping!"
		return
	fi

	echo ${_read_qos} > ${_base_path}/${_module}/${_readp_path}
	local _read_v=$(cat ${_base_path}/${_module}/${_readp_path})
	local _read_v_last=$(printf '%s' "$_read_v" | tail -c 1)
	if [ "${_read_qos}" != "${_read_v_last}" ]; then
		echo "Config ${_module} read qos fail(${_read_qos} != ${_read_v_last}), please check it"
	fi

	echo ${_write_qos} > ${_base_path}/${_module}/${_writep_path}
	local _read_v=$(cat ${_base_path}/${_module}/${_writep_path})
	local _read_v_last=$(printf '%s' "$_read_v" | tail -c 1)
	if [ "${_write_qos}" != "${_read_v_last}" ]; then
		echo "Config ${_module} write qos fail(${_write_qos} != ${_read_v_last}), please check it"
	fi

	echo "Config ${_module} qos read: ${_read_qos} write: ${_write_qos} done"
}

set_qos "20510500.sif0_qos" "7" "7"
set_qos "20510580.sif1_qos" "7" "7"
set_qos "20510600.sif2_qos" "7" "7"
set_qos "20510680.sif3_qos" "7" "7"
set_qos "20510280.isp_axi5_hdr_qos" "4" "4"
set_qos "20510300.isp_axi4_mcm_qos" "4" "4"
set_qos "20510380.isp_axi3_sp2_qos" "4" "4"
set_qos "20510480.isp_axi1_mp_qos" "4" "4"
set_qos "20510100.dw230_gdc_qos" "4" "4"
set_qos "20510180.dw230_scalar2_qos" "4" "4"
set_qos "20510200.dw230_scalar3_qos" "4" "4"
set_qos "20520000.bpu_qos" "0" "0"
set_qos "20510000.bt1120_qos" "7" "7"
set_qos "20510080.dc8000_qos" "7" "7"
set_qos "20530000.video_qos" "0" "0"
set_qos "20530080.jpeg_qos" "0" "0"

echo ""
echo ""
echo ""

# 配置cpu bpu ddr 降频的结温温度
echo 105000 > /sys/class/thermal/thermal_zone0/trip_point_0_temp
echo 105000 > /sys/class/thermal/thermal_zone1/trip_point_1_temp

# 设置cpu运行在高性能模式
echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor


modprobe panel-jc-050hd134
modprobe galcore
modprobe vio_n2d
modprobe lontium_lt8618
modprobe vs-x5-syscon-bridge
modprobe vs_drm
