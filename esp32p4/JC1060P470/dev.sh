#!/usr/bin/env bash
set -euo pipefail

PROJECT_NAME="JC1060P470"
EIM_LOG_FILE="${EIM_LOG_FILE:-/tmp/eim-${PROJECT_NAME}.log}"

usage() {
    cat <<'EOF'
Usage:
  ./dev.sh <command> [idf.py options]

Commands:
  build                 Build the project
  flash -p PORT         Flash the project
  monitor -p PORT       Open serial monitor
  flash-monitor -p PORT Flash and then open serial monitor
  menuconfig            Open ESP-IDF menuconfig
  clean                 Remove build outputs for normal rebuilds
  fullclean             Remove the whole build directory
  idf <args...>         Pass arbitrary arguments to idf.py

Examples:
  ./dev.sh build
  ./dev.sh flash -p /dev/ttyUSB0
  ./dev.sh monitor -p /dev/ttyUSB0
  ./dev.sh flash-monitor -p /dev/ttyUSB0
  ./dev.sh idf -p /dev/ttyUSB0 build flash monitor

Environment:
  EIM_LOG_FILE=/tmp/custom-eim.log ./dev.sh build

Notes:
  This script runs idf.py through eim using the currently selected ESP-IDF
  installation. To exit idf.py monitor, press Ctrl-].
EOF
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

main() {
    if [[ $# -eq 0 ]]; then
        usage
        exit 2
    fi

    case "$1" in
    -h|--help|help)
        usage
        ;;
    build)
        shift
        require_eim
        run_idf idf.py "$@" build
        ;;
    flash)
        shift
        require_eim
        run_idf idf.py "$@" flash
        ;;
    monitor)
        shift
        require_eim
        run_idf idf.py "$@" monitor
        ;;
    flash-monitor|fm)
        shift
        require_eim
        run_idf idf.py "$@" flash monitor
        ;;
    menuconfig)
        shift
        require_eim
        run_idf idf.py "$@" menuconfig
        ;;
    clean)
        shift
        require_eim
        run_idf idf.py "$@" clean
        ;;
    fullclean)
        shift
        require_eim
        run_idf idf.py "$@" fullclean
        ;;
    idf)
        shift
        if [[ $# -eq 0 ]]; then
            echo "error: './dev.sh idf' requires idf.py arguments" >&2
            exit 2
        fi
        require_eim
        run_idf idf.py "$@"
        ;;
    *)
        echo "error: unknown command '$1'" >&2
        echo >&2
        usage >&2
        exit 2
        ;;
    esac
}

main "$@"
