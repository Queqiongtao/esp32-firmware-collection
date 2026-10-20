#!/usr/bin/env bash
set -euo pipefail

DEFAULT_PORT="/dev/ttyACM0"
PORT="${PORT:-$DEFAULT_PORT}"
PORT_WAS_SET=0
EIM_LOG_FILE="${EIM_LOG_FILE:-/tmp/eim-clock.log}"

usage() {
    cat <<'EOF'
Usage:
  ./dev.sh [options] <command>
  ./dev.sh <command> [options]

Options:
  -p, --port <port>    Serial port for flash/monitor commands
                      Default: /dev/ttyACM0, or PORT env var
  -h, --help           Show this help

Commands:
  build                Build firmware
  flash                Flash firmware to the board
  download             Alias for flash
  monitor              Start serial monitor
  flash-monitor        Flash firmware and start serial monitor
  ports                List visible serial ports
  clean                Clean build outputs
  fullclean            Fully clean generated build/config outputs
  menuconfig           Open ESP-IDF menuconfig
  help                 Show this help

Examples:
  ./dev.sh build
  ./dev.sh flash
  ./dev.sh monitor
  ./dev.sh flash-monitor
  ./dev.sh -p /dev/ttyUSB0 flash
  PORT=/dev/ttyUSB0 ./dev.sh monitor
EOF
}

die() {
    echo "Error: $*" >&2
    echo >&2
    usage >&2
    exit 1
}

run_idf() {
    eim --log-file "$EIM_LOG_FILE" run "idf.py $*"
}

list_ports() {
    local found=0

    for port in /dev/serial/by-id/* /dev/ttyACM* /dev/ttyUSB*; do
        if [[ -e "$port" ]]; then
            found=1
            ls -l "$port"
        fi
    done

    if ((found == 0)); then
        echo "No serial ports found under /dev/serial/by-id, /dev/ttyACM*, or /dev/ttyUSB*."
    fi
}

require_port() {
    if [[ -e "$PORT" ]]; then
        return
    fi

    echo "Serial port not found: $PORT" >&2
    echo >&2
    list_ports >&2
    echo >&2
    if ((PORT_WAS_SET == 0)); then
        echo "Connect the board, then retry. If it appears on another port, use:" >&2
        echo "  ./dev.sh -p /dev/ttyUSB0 $command" >&2
        echo "or:" >&2
        echo "  PORT=/dev/ttyUSB0 ./dev.sh $command" >&2
    fi
    exit 1
}

command=""

while (($# > 0)); do
    case "$1" in
        -p|--port)
            shift
            (($# > 0)) || die "missing value for --port"
            PORT="$1"
            PORT_WAS_SET=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        help)
            usage
            exit 0
            ;;
        build|flash|download|monitor|flash-monitor|ports|clean|fullclean|menuconfig)
            [[ -z "$command" ]] || die "multiple commands provided: '$command' and '$1'"
            command="$1"
            ;;
        *)
            die "unknown option or command: $1"
            ;;
    esac
    shift
done

case "$command" in
    "")
        usage
        ;;
    build)
        run_idf build
        ;;
    flash|download)
        require_port
        run_idf -p "$PORT" flash
        ;;
    monitor)
        require_port
        run_idf -p "$PORT" monitor
        ;;
    flash-monitor)
        require_port
        run_idf -p "$PORT" flash monitor
        ;;
    ports)
        list_ports
        ;;
    clean)
        run_idf clean
        ;;
    fullclean)
        run_idf fullclean
        ;;
    menuconfig)
        run_idf menuconfig
        ;;
esac
