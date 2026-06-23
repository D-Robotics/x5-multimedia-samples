#!/bin/sh
#Copyright: D-Robotic
#Function: X5 hardware watchdog test script

WDOG_SYSFS="/sys/class/watchdog/watchdog0"
WDOG_DEV=""

usage()
{
    echo "usage: $0 <command>"
    echo ""
    echo "commands:"
    echo "  info      show hardware watchdog information"
    echo "  timeout   show configured watchdog timeout (seconds)"
    echo "  feeder    show whether kernel or userspace is feeding"
    echo "  timeleft  show remaining timeout (seconds)"
    echo "  feed      feed watchdog N times: feed <count> [-i interval_sec]"
    echo "  disable   stop watchdog (requires nowayout=0)"
    echo ""
    echo "examples:"
    echo "  $0 info"
    echo "  $0 feeder"
    echo "  $0 feed 1"
    echo "  $0 feed 6 -i 2"
}

log_err()
{
    echo "error: $*" >&2
}

require_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        log_err "please run as root"
        exit 1
    fi
}

find_watchdog_dev()
{
    if [ -c /dev/watchdog0 ]; then
        WDOG_DEV="/dev/watchdog0"
    elif [ -c /dev/watchdog ]; then
        WDOG_DEV="/dev/watchdog"
    else
        log_err "no /dev/watchdog or /dev/watchdog0"
        exit 1
    fi
}

check_sysfs()
{
    if [ ! -d "$WDOG_SYSFS" ]; then
        log_err "$WDOG_SYSFS not found, is watchdog driver loaded?"
        exit 1
    fi
}

read_sysfs()
{
    attr=$1
    default=${2:-N/A}
    val=$(cat "${WDOG_SYSFS}/${attr}" 2>/dev/null) || val="$default"
    echo "$val"
}

has_sysfs_attr()
{
    [ -r "${WDOG_SYSFS}/$1" ]
}

cmd_info()
{
    echo "========================================"
    echo "Watchdog Hardware Info"
    echo "========================================"
    echo "Device node : $WDOG_DEV"
    echo "Sysfs path  : $WDOG_SYSFS"

    if has_sysfs_attr identity; then
        echo "Identity    : $(read_sysfs identity)"
        echo "State       : $(read_sysfs state)"
        echo "Nowayout    : $(read_sysfs nowayout)"
    else
        echo "Identity    : (sysfs disabled, enable CONFIG_WATCHDOG_SYSFS)"
    fi

    driver=$(readlink -f "${WDOG_SYSFS}/device/driver" 2>/dev/null || echo "N/A")
    echo "Driver      : $(basename "$driver" 2>/dev/null || echo "$driver")"

    echo "========================================"
}

cmd_timeout()
{
    if ! has_sysfs_attr timeout; then
        log_err "timeout sysfs not available"
        exit 1
    fi

    timeout=$(read_sysfs timeout)
    echo "Watchdog timeout: ${timeout} sec"
}

userspace_holds_device()
{
    if command -v fuser >/dev/null 2>&1; then
        fuser -s "$WDOG_DEV" 2>/dev/null
        return $?
    fi

    if command -v lsof >/dev/null 2>&1; then
        lsof "$WDOG_DEV" >/dev/null 2>&1
        return $?
    fi

    return 1
}

cmd_feeder()
{
    state="N/A"
    if has_sysfs_attr state; then
        state=$(read_sysfs state)
    fi

    echo "========================================"
    echo "Watchdog Feeding Source"
    echo "========================================"
    echo "Sysfs state : $state"

    if userspace_holds_device; then
        echo "Feed source : userspace"
        echo "Reason      : a process is holding $WDOG_DEV open"
        if command -v fuser >/dev/null 2>&1; then
            echo "Process     :"
            fuser -v "$WDOG_DEV" 2>&1 | sed 's/^/  /'
        fi
        echo "========================================"
        return 0
    fi

    if [ "$state" = "active" ]; then
        echo "Feed source : userspace (expected)"
        echo "Reason      : state=active (userspace opened watchdog before)"
        echo "Note        : no process currently holds $WDOG_DEV;"
        echo "              system may reset soon if nobody feeds"
        echo "========================================"
        return 0
    fi

    if has_sysfs_attr timeleft; then
        timeout=$(read_sysfs timeout 10)
        samples=""
        prev=""
        kicked=0
        i=1

        echo "Sampling timeleft (1s x 7, kernel feeds about every $((timeout / 2))s):"

        while [ "$i" -le 7 ]; do
            cur=$(read_sysfs timeleft)
            samples="${samples}${cur} "
            echo "  [${i}] timeleft=${cur}s"

            if [ -n "$prev" ] && [ "$cur" -gt "$prev" ] 2>/dev/null; then
                kicked=1
            fi
            prev=$cur
            if [ "$i" -lt 7 ]; then
                sleep 1
            fi
            i=$((i + 1))
        done

        echo "Timeleft seq : ${samples}"

        if [ "$kicked" -eq 1 ]; then
            echo "Feed source : kernel"
            echo "Reason      : state=inactive, timeleft increased during sampling"
            echo "              (kernel watchdog worker kicked the hardware WDT)"
        else
            echo "Feed source : none (warning)"
            echo "Reason      : timeleft kept decreasing for 7s without refresh"
            echo "Warning     : nobody feeding, system may reset in ~${prev}s"
            echo "Note        : if kernel should feed, check HORIZON_WATCHDOG_ENABLE"
        fi
    else
        echo "Feed source : kernel (likely)"
        echo "Reason      : state=inactive, no userspace open (typical Release config)"
    fi

    echo "========================================"
}

cmd_timeleft()
{
    if ! has_sysfs_attr timeleft; then
        log_err "timeleft sysfs not available"
        exit 1
    fi

    timeleft=$(read_sysfs timeleft)
    timeout=$(read_sysfs timeout)
    echo "Watchdog timeleft: ${timeleft} sec (timeout=${timeout} sec)"
}

cmd_feed()
{
    count=$1
    interval=$2

    case "$count" in
        ''|*[!0-9]*)
            log_err "feed count must be a positive integer, got '$count'"
            exit 1
            ;;
    esac

    if [ "$count" -eq 0 ]; then
        log_err "feed count must be >= 1"
        exit 1
    fi

    case "$interval" in
        ''|*[!0-9]*)
            log_err "feed interval must be a non-negative integer, got '$interval'"
            exit 1
            ;;
    esac

    echo "Feeding watchdog on $WDOG_DEV: ${count} time(s), interval=${interval}s"

    exec 3>"$WDOG_DEV" 2>/dev/null || {
        log_err "failed to open $WDOG_DEV (device busy?)"
        exit 1
    }

    i=1
    while [ "$i" -le "$count" ]; do
        if ! printf '1' >&3; then
            exec 3>&-
            log_err "feed #$i failed"
            exit 1
        fi

        if has_sysfs_attr timeleft; then
            tl=$(read_sysfs timeleft)
            echo "  [$i/$count] keepalive ok, timeleft=${tl}s"
        else
            echo "  [$i/$count] keepalive ok"
        fi

        if [ "$i" -lt "$count" ] && [ "$interval" -gt 0 ]; then
            sleep "$interval"
        fi
        i=$((i + 1))
    done

    exec 3>&-

    echo "Feed done (${count} times)"
    echo "Note: userspace took over (state may become active);"
    echo "      kernel auto-feed pauses until reboot or disable"
}

cmd_disable()
{
    nowayout=0
    if has_sysfs_attr nowayout; then
        nowayout=$(read_sysfs nowayout 0)
    fi

    if [ "$nowayout" = "1" ]; then
        log_err "nowayout=1, watchdog cannot be disabled"
        exit 1
    fi

    echo "Stopping watchdog via magic close on $WDOG_DEV ..."

    if ! printf 'V' > "$WDOG_DEV" 2>/dev/null; then
        log_err "failed to write magic close to $WDOG_DEV"
        exit 1
    fi

    sleep 1

    if has_sysfs_attr state; then
        echo "State after disable: $(read_sysfs state)"
    fi
    if has_sysfs_attr timeleft; then
        echo "Timeleft after disable: $(read_sysfs timeleft) sec"
    fi

    echo "Watchdog disable command sent (magic close 'V')"
    echo "Note: reboot or re-probe driver to re-enable if needed"
}

main()
{
    require_root
    find_watchdog_dev
    check_sysfs

    case "${1:-}" in
        info)     cmd_info ;;
        timeout)  cmd_timeout ;;
        feeder)   cmd_feeder ;;
        timeleft) cmd_timeleft ;;
        feed)
            count=""
            interval=1
            shift
            while [ $# -gt 0 ]; do
                case "$1" in
                    -i)
                        if [ $# -lt 2 ]; then
                            log_err "feed -i requires interval value"
                            exit 1
                        fi
                        interval=$2
                        shift 2
                        ;;
                    -h|--help)
                        usage
                        exit 0
                        ;;
                    -*)
                        log_err "unknown feed option '$1'"
                        usage
                        exit 1
                        ;;
                    *)
                        if [ -n "$count" ]; then
                            log_err "unexpected argument '$1'"
                            usage
                            exit 1
                        fi
                        count=$1
                        shift
                        ;;
                esac
            done
            if [ -z "$count" ]; then
                log_err "feed requires count, e.g. $0 feed 5"
                usage
                exit 1
            fi
            cmd_feed "$count" "$interval"
            ;;
        disable)  cmd_disable ;;
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
