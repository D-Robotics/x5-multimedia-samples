#!/bin/sh
#Copyright: D-Robotic
#Function: X5 PMIC RTC (hpu3501-rtc) test script

RTC_DEV="/dev/rtc0"
RTC_SYSFS="/sys/class/rtc/rtc0"
WRITE_SETTLE_SEC=2
DEFAULT_ALARM_SEC=10
ALARM_TIMEOUT_MARGIN=5
ALARM_POLL_INTERVAL=0.1

usage()
{
    echo "usage: $0 <command>"
    echo ""
    echo "commands:"
    echo "  info                      show RTC hardware information"
    echo "  read                      read hardware RTC time"
    echo "  setsys <datetime>         set system time"
    echo "                              example: $0 setsys \"2025-06-09 14:30:00\""
    echo "  setrtc <datetime>         set RTC time (system time also updated)"
    echo "                              example: $0 setrtc \"2025-06-09 14:30:00\""
    echo "  alarm [-s seconds]        set RTC alarm and verify by interrupt count"
    echo "                              default: ${DEFAULT_ALARM_SEC}s"
    echo ""
    echo "examples:"
    echo "  $0 info"
    echo "  $0 read"
    echo "  $0 setsys \"2025-06-09 14:30:00\""
    echo "  $0 setrtc \"2025-06-09 14:30:00\""
    echo "  $0 alarm -s 10"
}

log_err()
{
    echo "error: $*" >&2
}

log_ok()
{
    echo "PASS: $*"
}

log_fail()
{
    echo "FAIL: $*" >&2
}

require_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        log_err "please run as root"
        exit 1
    fi
}

find_rtc_dev()
{
    if [ -c /dev/rtc0 ]; then
        RTC_DEV="/dev/rtc0"
    elif [ -c /dev/rtc ]; then
        RTC_DEV="/dev/rtc"
    else
        log_err "no /dev/rtc0 or /dev/rtc, is rtc driver loaded?"
        exit 1
    fi
}

check_sysfs()
{
    if [ ! -d "$RTC_SYSFS" ]; then
        log_err "$RTC_SYSFS not found, is rtc driver loaded?"
        exit 1
    fi
}

read_sysfs()
{
    attr=$1
    default=${2:-N/A}
    val=$(cat "${RTC_SYSFS}/${attr}" 2>/dev/null) || val="$default"
    echo "$val"
}

has_sysfs_attr()
{
    [ -r "${RTC_SYSFS}/$1" ]
}

read_hwclock()
{
    hwclock -r -f "$RTC_DEV" 2>/dev/null || hwclock -r
}

write_to_hwclock()
{
    hwclock -w -f "$RTC_DEV" 2>/dev/null || hwclock -w
}

validate_datetime()
{
    datetime=$1

    if date -d "$datetime" +%s >/dev/null 2>&1; then
        return 0
    fi

    log_err "invalid datetime '$datetime'"
    return 1
}

epoch_now()
{
    date +%s
}

# Return timestamp in milliseconds (best effort)
get_time_ms()
{
    if date +%s%3N >/dev/null 2>&1; then
        date +%s%3N
    elif date +%s.%N >/dev/null 2>&1; then
        date +%s.%N | awk -F. '{printf "%d%03d", $1, substr($2 "000", 1, 3)}'
    else
        epoch_now | awk '{printf "%d000", $1}'
    fi
}

# latency_ms = end_ms - start_ms, print seconds with 3 decimal places
calc_latency_sec()
{
    start_ms=$1
    end_ms=$2
    echo "$start_ms $end_ms" | awk '{printf "%.3f", ($2 - $1) / 1000}'
}

calc_error_sec()
{
    expected=$1
    actual=$2
    echo "$expected $actual" | awk '{printf "%+.3f", $2 - $1}'
}

# Clear leftover chars when overwriting a \r progress line
print_countdown()
{
    remaining=$1
    irq_delta=$2

    printf "\r  countdown: %2ds left | IRQ +%-2d          " \
        "$remaining" "$irq_delta"
}

get_alarm_irq_line()
{
    grep -iE 'hpu3501|alarm' /proc/interrupts 2>/dev/null | head -1
}

get_alarm_irq_count()
{
    line=$(get_alarm_irq_line)
    if [ -z "$line" ]; then
        line=$(grep -i 'aon_gpio' /proc/interrupts 2>/dev/null | head -1)
    fi
    if [ -z "$line" ]; then
        echo ""
        return 1
    fi
    echo "$line" | awk '{print $2}'
}

show_alarm_irq_line()
{
    line=$(get_alarm_irq_line)
    if [ -z "$line" ]; then
        line=$(grep -i 'aon_gpio' /proc/interrupts 2>/dev/null | head -1)
    fi
    if [ -n "$line" ]; then
        echo "$line"
    else
        echo "(no matching IRQ line found)"
    fi
}

cmd_info()
{
    echo "========================================"
    echo "RTC Hardware Info"
    echo "========================================"
    echo "Device node : $RTC_DEV"
    echo "Sysfs path  : $RTC_SYSFS"
    echo "Name        : $(read_sysfs name)"
    echo "Date        : $(read_sysfs date)"
    echo "Time        : $(read_sysfs time)"

    if has_sysfs_attr since_epoch; then
        echo "Since epoch : $(read_sysfs since_epoch) sec"
    fi

    if has_sysfs_attr wakealarm; then
        echo "Wakealarm   : $(read_sysfs wakealarm)"
    fi

    driver=$(readlink -f "${RTC_SYSFS}/device/driver" 2>/dev/null || echo "N/A")
    echo "Driver      : $(basename "$driver" 2>/dev/null || echo "$driver")"
    echo "Hwclock     : $(read_hwclock)"
    echo "System date : $(date)"
    echo "Alarm IRQ   : $(show_alarm_irq_line)"
    echo "========================================"
}

cmd_read()
{
    echo "Hardware RTC: $(read_hwclock)"
}

cmd_setsys()
{
    datetime=$1

    if [ -z "$datetime" ]; then
        log_err "setsys requires datetime, e.g. $0 setsys \"2025-06-09 14:30:00\""
        exit 1
    fi

    if ! validate_datetime "$datetime"; then
        exit 1
    fi

    if ! date -s "$datetime" >/dev/null 2>&1; then
        log_err "failed to set system time"
        exit 1
    fi

    echo "Set datetime : $datetime"
    echo "System time  : $(date)"
    log_ok "system time set"
}

cmd_setrtc()
{
    datetime=$1

    if [ -z "$datetime" ]; then
        log_err "setrtc requires datetime, e.g. $0 setrtc \"2025-06-09 14:30:00\""
        exit 1
    fi

    if ! validate_datetime "$datetime"; then
        exit 1
    fi

    if ! date -s "$datetime" >/dev/null 2>&1; then
        log_err "failed to set system time"
        exit 1
    fi

    write_to_hwclock
    sleep "$WRITE_SETTLE_SEC"

    echo "Set datetime : $datetime"
    echo "Hardware RTC : $(read_hwclock)"
    echo "System time  : $(date)"
    log_ok "RTC time set"
}

cmd_alarm()
{
    seconds=$DEFAULT_ALARM_SEC
    wakealarm="${RTC_SYSFS}/wakealarm"

    while [ $# -gt 0 ]; do
        case "$1" in
            -s)
                if [ $# -lt 2 ]; then
                    log_err "alarm -s requires seconds value"
                    exit 1
                fi
                seconds=$2
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_err "unknown alarm option '$1'"
                usage
                exit 1
                ;;
        esac
    done

    case "$seconds" in
        ''|*[!0-9]*)
            log_err "alarm seconds must be a positive integer, got '$seconds'"
            exit 1
            ;;
    esac

    if [ "$seconds" -le 0 ]; then
        log_err "alarm seconds must be >= 1"
        exit 1
    fi

    if [ ! -w "$wakealarm" ]; then
        log_err "$wakealarm not available, alarm not supported?"
        exit 1
    fi

    irq_before=$(get_alarm_irq_count)
    if [ -z "$irq_before" ]; then
        log_err "cannot find RTC alarm IRQ in /proc/interrupts"
        exit 1
    fi

    echo "========================================"
    echo "RTC Alarm Test"
    echo "========================================"
    echo "Alarm delay : ${seconds}s"
    echo "IRQ line    : $(show_alarm_irq_line)"
    echo "IRQ before  : $irq_before"

    if ! echo "+${seconds}" > "$wakealarm"; then
        log_err "failed to set wakealarm"
        exit 1
    fi

    set_ms=$(get_time_ms)
    set_epoch=$(epoch_now)

    echo "Set at      : $(date -d "@$set_epoch" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date)"

    timeout=$((seconds + ALARM_TIMEOUT_MARGIN))
    trigger_ms=""
    trigger_epoch=""

    echo "Waiting for alarm (delay ${seconds}s, max wait ${timeout}s) ..."

    while true; do
        now_ms=$(get_time_ms)
        now_epoch=$(epoch_now)
        elapsed_sec=$(( (now_ms - set_ms) / 1000 ))

        irq_now=$(get_alarm_irq_count)
        irq_delta=$((irq_now - irq_before))
        wake_now=$(cat "$wakealarm" 2>/dev/null)

        if [ "$elapsed_sec" -ge "$timeout" ]; then
            break
        fi

        remaining=$((seconds - elapsed_sec))
        [ "$remaining" -lt 0 ] && remaining=0
        print_countdown "$remaining" "$irq_delta"

        if [ "$irq_delta" -ge 1 ]; then
            trigger_ms=$now_ms
            trigger_epoch=$now_epoch
            break
        fi

        if [ -z "$wake_now" ] && [ "$elapsed_sec" -ge "$seconds" ]; then
            trigger_ms=$now_ms
            trigger_epoch=$now_epoch
            break
        fi

        sleep "$ALARM_POLL_INTERVAL" 2>/dev/null || sleep 1
    done
    echo ""

    irq_after=$(get_alarm_irq_count)
    irq_delta=$((irq_after - irq_before))

    if [ -n "$trigger_ms" ]; then
        latency_sec=$(calc_latency_sec "$set_ms" "$trigger_ms")
        error_sec=$(calc_error_sec "$seconds" "$latency_sec")
        trigger_time=$(date -d "@$trigger_epoch" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date)
    else
        latency_sec="N/A"
        error_sec="N/A"
        trigger_time="N/A"
    fi

    echo "Trigger at  : $trigger_time"
    echo "Latency     : ${latency_sec}s (set -> trigger)"
    echo "Error       : ${error_sec}s"
    echo "IRQ after   : $irq_after"
    echo "========================================"

    if [ "$irq_delta" -ge 1 ] && [ "$latency_sec" != "N/A" ]; then
        log_ok "RTC alarm interrupt detected (+${irq_delta}), latency ${latency_sec}s"
        exit 0
    fi

    if [ "$irq_delta" -ge 1 ]; then
        log_ok "RTC alarm interrupt detected (+${irq_delta})"
        exit 0
    fi

    wake_now=$(cat "$wakealarm" 2>/dev/null)
    if [ -z "$wake_now" ]; then
        log_fail "alarm cleared but IRQ count unchanged (check IRQ trigger edge config)"
    else
        log_fail "alarm not triggered after ${timeout}s"
    fi
    exit 1
}

main()
{
    require_root
    find_rtc_dev
    check_sysfs

    case "${1:-}" in
        info)   cmd_info ;;
        read)   cmd_read ;;
        setsys)
            shift
            cmd_setsys "$*"
            ;;
        setrtc)
            shift
            cmd_setrtc "$*"
            ;;
        alarm)
            shift
            cmd_alarm "$@"
            ;;
        -h|--help|help|"")
            usage
            [ -n "${1:-}" ] || exit 0
            exit 0
            ;;
        *)
            log_err "unknown command '$1'"
            usage
            exit 1
            ;;
    esac
}

main "$@"
