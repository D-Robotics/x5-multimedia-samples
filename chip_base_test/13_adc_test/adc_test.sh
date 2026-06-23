#!/bin/sh

usage()
{
    echo "usage: ./adc_test.sh <channel{3-7}> [-m mode] [-n count] [-i interval]"
    echo "  channel      ADC channel number (3-7)"
    echo "  -m mode      sample mode: single (default) / continuous"
    echo "  -n count     sample count, 0=infinite (default: 0)"
    echo "  -i interval  sample interval in seconds for single mode (default: 1)"
    echo ""
    echo "examples:"
    echo "  ./adc_test.sh 3                              # single mode, infinite"
    echo "  ./adc_test.sh 3 -n 10 -i 0.5                # single mode, 10 samples, 0.5s"
    echo "  ./adc_test.sh 3 -m continuous -n 100         # continuous mode, 100 samples"
    echo "  ./adc_test.sh 3 -m continuous                # continuous mode, infinite (Ctrl+C to stop)"
}

# ---- Parse parameters ----
channel=""
mode="single"
count=0
interval=1

if [ $# -lt 1 ]; then
    usage; exit 0
fi
channel=$1
shift

while [ $# -gt 0 ]; do
    case $1 in
        -m) mode=$2; shift 2 ;;
        -n) count=$2; shift 2 ;;
        -i) interval=$2; shift 2 ;;
        -h) usage; exit 0 ;;
        *)  echo "error: unknown option '$1'"; usage; exit 1 ;;
    esac
done

# ---- Validate ----
case $channel in
    3|4|5|6|7) ;;
    *) echo "error: channel must be 3-7, got '$channel'"; usage; exit 1 ;;
esac

case $mode in
    single|continuous) ;;
    *) echo "error: mode must be single or continuous, got '$mode'"; exit 1 ;;
esac

# ---- Device paths ----
iio_base=/sys/bus/iio/devices/iio\:device0
in_voltage=${iio_base}/in_voltage${channel}_raw
scale_file=${iio_base}/in_voltage_scale

# ---- Check common device files ----
if [ ! -f "$in_voltage" ]; then
    echo "error: $in_voltage not found"; exit 1
fi
if [ ! -f "$scale_file" ]; then
    echo "error: $scale_file not found"; exit 1
fi

# ---- Continuous mode specific checks ----
if [ "$mode" = "continuous" ]; then
    buffer_path=${iio_base}/buffer0
    iio_dev=/dev/iio\:device0
    if [ ! -d "$buffer_path" ]; then
        echo "error: $buffer_path not found, continuous mode not supported"; exit 1
    fi
    if [ ! -c "$iio_dev" ]; then
        echo "error: $iio_dev not found"; exit 1
    fi
fi

# ---- Read scale once ----
# vol_scale is in mV/LSB (e.g. 1.757812500), so mV = raw * vol_scale
vol_scale=$(cat "$scale_file")

# ---- Read system info (tolerant: N/A if unavailable) ----
iio_name=$(cat ${iio_base}/name 2>/dev/null || echo "N/A")
adc_clk=$(cat /sys/kernel/debug/clk/adc_clk/clk_rate 2>/dev/null || echo "N/A")
adc_pclk=$(cat /sys/kernel/debug/clk/adc_pclk/clk_rate 2>/dev/null || echo "N/A")

# ---- Print test info ----
echo "========================================"
echo "ADC Test Info"
echo "========================================"
echo "Channel     : $channel"
echo "Mode        : $mode"
echo "IIO Device  : $iio_name"
echo "Scale       : $vol_scale"
echo "ADC Clock   : ${adc_clk} Hz"
echo "ADC APB Clk : ${adc_pclk} Hz"
count_str=$([ $count -eq 0 ] && echo "infinite" || echo $count)
echo "Count       : $count_str"
if [ "$mode" = "single" ]; then
    echo "Interval    : ${interval}s"
fi
echo "========================================"
echo "Press Ctrl+C for quit"
echo ""

# ---- Stats helper functions ----
min_raw=""
max_raw=""
sum_raw=0
samples=0
min_vol=""
max_vol=""
sum_vol="0"

update_stats()
{
    raw=$1
    vol=$2

    if [ -z "$min_raw" ] || [ "$raw" -lt "$min_raw" ]; then
        min_raw=$raw; min_vol=$vol
    fi
    if [ -z "$max_raw" ] || [ "$raw" -gt "$max_raw" ]; then
        max_raw=$raw; max_vol=$vol
    fi
    sum_raw=$((sum_raw + raw))
    sum_vol=$(echo "scale=4; $sum_vol + $vol" | bc)
    samples=$((samples + 1))
}

print_stats()
{
    if [ $samples -eq 0 ]; then
        echo "no samples collected"; return
    fi
    avg_raw=$((sum_raw / samples))
    avg_vol=$(echo "scale=4; $sum_vol / $samples" | bc)
    echo "=== Statistics ==="
    echo "Samples: $samples"
    echo "Raw   - min: $min_raw  max: $max_raw  avg: $avg_raw"
    echo "Volt  - min: ${min_vol}  max: ${max_vol}  avg: ${avg_vol} mV"
}

# ---- Single mode: read sysfs one by one ----
if [ "$mode" = "single" ]; then
    if [ $count -eq 0 ]; then
        trap 'echo ""; print_stats; exit 0' INT
    fi

    while true; do
        vol_raw=$(cat "$in_voltage")
        vol=$(echo "scale=2; $vol_raw * $vol_scale / 1" | bc)
        echo "[$samples] vol_raw:${vol_raw} vol:${vol} mV"
        update_stats "$vol_raw" "$vol"

        if [ $count -ne 0 ] && [ $samples -ge $count ]; then
            break
        fi

        sleep "$interval"
    done

    print_stats
    exit 0
fi

# ---- Continuous mode: IIO buffer + hexdump ----

# Buffer helper functions
enable_buffer()
{
    echo 1 > "${buffer_path}/in_voltage${channel}_en"
    echo 1 > "${buffer_path}/enable"
}

disable_buffer()
{
    echo 0 > "${buffer_path}/enable" 2>/dev/null
    echo 0 > "${buffer_path}/in_voltage${channel}_en" 2>/dev/null
}

# Continuous finite: read all data at once, then process
if [ $count -ne 0 ]; then
    enable_buffer

    raw_output=$(dd if="$iio_dev" bs=2 count=$count 2>/dev/null | hexdump -d -v | awk '{for(i=2;i<=NF;i++) printf "%d ", $i}')

    disable_buffer

    for val in $raw_output; do
        [ -z "$val" ] && continue
        vol=$(echo "scale=2; $val * $vol_scale / 1" | bc)
        echo "[$samples] vol_raw:${val} vol:${vol} mV"
        update_stats "$val" "$vol"
    done

    print_stats
    exit 0
fi

# Continuous infinite: stream via fifo for real-time display + stats
fifo=/tmp/adc_fifo_$$
mkfifo "$fifo" || { echo "error: failed to create fifo"; exit 1; }

trap 'kill $stream_pid 2>/dev/null; rm -f "$fifo"; disable_buffer; echo ""; print_stats; exit 0' INT

enable_buffer

hexdump -d -v "$iio_dev" | awk '{for(i=2;i<=NF;i++) printf "%d\n", $i}' > "$fifo" &
stream_pid=$!

while read vol_raw; do
    [ -z "$vol_raw" ] && continue
    vol=$(echo "scale=2; $vol_raw * $vol_scale / 1" | bc)
    echo "[$samples] vol_raw:${vol_raw} vol:${vol} mV"
    update_stats "$vol_raw" "$vol"
done < "$fifo"

# Cleanup (reached if hexdump exits on its own)
kill $stream_pid 2>/dev/null
rm -f "$fifo"
disable_buffer
print_stats

exit 0