#!/bin/sh

INTERFACE=usb0
CLIENT_IP="192.168.5.10"
SERVER_IP="192.168.5.5"


# Config USB
modprobe cdc_ether.ko

ifconfig ${INTERFACE} ${CLIENT_IP} netmask 255.255.255.0

#echo 256 > /sys/module/usbcore/parameters/usbfs_memory_mb

echo 7 > /sys/bus/platform/drivers/noc_qos/20550380.usb3_qos/read_priority_qos_ctrl/priority
echo 7 >/sys/bus/platform/drivers/noc_qos/20550380.usb3_qos/write_priority_qos_ctrl/priority

#echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor

ifconfig ${INTERFACE} mtu 5000



# Run App
./multi_pipe_depth2cloud -i ${INTERFACE} -s ${SERVER_IP}
