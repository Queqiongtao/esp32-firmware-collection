#!/usr/bin/env bash
set -euo pipefail

PROJECT_NAME="companion_display"
DEFAULT_PORT="/dev/ttyACM0"
PORT="${PORT:-$DEFAULT_PORT}"
PORT_WAS_SET=0
EIM_LOG_FILE="${EIM_LOG_FILE:-/tmp/eim-${PROJECT_NAME}.log}"

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
  idf <args...>        Pass arbitrary arguments to idf.py
  help                 Show this help

Examples:
  ./dev.sh build
  ./dev.sh flash
  ./dev.sh monitor
  ./dev.sh flash-monitor
  ./dev.sh -p /dev/ttyUSB0 flash
  PORT=/dev/ttyUSB0 ./dev.sh monitor
  ./dev.sh idf --version

Environment:
  EIM_LOG_FILE=/tmp/custom-eim.log ./dev.sh build
EOF
}

die() {
    echo "Error: $*" >&2
    echo >&2
    usage >&2
    exit 1
}

require_eim() {
    if ! command -v eim >/dev/null 2>&1; then
        echo "error: eim is not installed or not in PATH" >&2
        echo "Install/select ESP-IDF with eim first, then rerun this script." >&2
        exit 127
    fi
}

run_idf() {
    local command_line

    printf -v command_line "%q " "$@"
    eim --log-file "$EIM_LOG_FILE" run "$command_line"
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
idf_args=()

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
        idf)
            [[ -z "$command" ]] || die "multiple commands provided: '$command' and '$1'"
            command="$1"
            shift
            idf_args=("$@")
            break
            ;;
        *)
            die "unknown option or command: $1"
            ;;
    esac
    shift
done

require_eim

case "$command" in
    "")
        usage
        ;;
    build)
        run_idf idf.py build
        ;;
    flash|download)
        require_port
        run_idf idf.py -p "$PORT" flash
        ;;
    monitor)
        require_port
        run_idf idf.py -p "$PORT" monitor
        ;;
    flash-monitor)
        require_port
        run_idf idf.py -p "$PORT" flash monitor
        ;;
    ports)
        list_ports
        ;;
    clean)
        run_idf idf.py clean
        ;;
    fullclean)
        run_idf idf.py fullclean
        ;;
    menuconfig)
        run_idf idf.py menuconfig
        ;;
    idf)
        ((${#idf_args[@]} > 0)) || die "'./dev.sh idf' requires idf.py arguments"
        run_idf idf.py "${idf_args[@]}"
        ;;
esac
