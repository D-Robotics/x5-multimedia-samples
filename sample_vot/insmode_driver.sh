#!/bin/sh
systemctl stop lightdm

rmmod vs_drm
rmmod vs-x5-syscon-bridge
rmmod sii902x
rmmod drm_kms_helper

modprobe sii902x
modprobe vs-x5-syscon-bridge
modprobe drm_kms_helper
modprobe vs_drm
