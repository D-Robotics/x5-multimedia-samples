#!/bin/sh
cd /sys/bus/platform/drivers/noc_qos || exit
for dir in */; do
	case "$dir" in
		*qos*)
			cd "$dir" || continue
			val=$(cat write_priority_qos_ctrl/priority | awk '{print $NF}')
			echo "$dir write priority: $val"
			val=$(cat read_priority_qos_ctrl/priority | awk '{print $NF}')
			echo "$dir read priority: $val"
			cd ..
			;;
	esac
done
