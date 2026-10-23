#!/bin/bash

# CADBay Web Server Startup Script
# This script simplifies starting and managing the web server

COLOR_GREEN='\033[0;32m'
COLOR_YELLOW='\033[1;33m'
COLOR_RED='\033[0;31m'
COLOR_BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

show_help() {
    echo "CADBay Web Server Manager"
    echo ""
    echo "Usage: $0 [COMMAND]"
    echo ""
    echo "Commands:"
    echo "  start      Start the web server (default)"
    echo "  stop       Stop the web server"
    echo "  restart    Restart the web server"
    echo "  status     Check if web server is running"
    echo "  clean      Force kill all webserver instances"
    echo "  help       Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 start      - Start the server"
    echo "  $0 stop       - Gracefully stop the server"
    echo "  $0 clean      - Force kill and clean ports"
}

check_binary() {
    if [ ! -x "$1" ]; then
        echo -e "${COLOR_RED}✗ Error: $1 not found${NC}"
        echo "Please compile first: gcc -o $(basename $1) $(basename $1).c"
        exit 1
    fi
}

start_server() {
    check_binary "./webserver"
    
    # look specifically for the webserver binary or the renamed process
    if pgrep -x webserver > /dev/null || pgrep -f "Server Launcher" > /dev/null; then
        echo -e "${COLOR_YELLOW}⚠ Webserver is already running${NC}"
        echo "Reloading file state instead of starting a new instance."

        main_pid=$(ps -C webserver -o pid=,etimes= | awk '{print $1 " " $2}' | sort -k2n | head -1 | awk '{print $1}')
        if [ -n "$main_pid" ]; then
            kill -HUP "$main_pid" 2>/dev/null || true
            echo "Reload signal sent to PID $main_pid."
        else
            echo "Could not determine the main webserver PID; the running server will continue serving files."
        fi
        exit 0
    fi
    
    echo -e "${COLOR_BLUE}Starting CADBay Web Server...${NC}"
    # redirect output to log so we can inspect errors
    LOGFILE="webserver.log"
    ./webserver >"$LOGFILE" 2>&1 &
    SERVER_PID=$!
    sleep 1
    
    if kill -0 $SERVER_PID 2>/dev/null; then
        echo -e "${COLOR_GREEN}✓ Server started successfully (PID: $SERVER_PID)${NC}"
        echo "(log file: $LOGFILE)"
    else
        echo -e "${COLOR_RED}✗ Failed to start server${NC}"
        echo "--- server log follows ---"
        cat "$LOGFILE" 2>/dev/null || echo "(no log available)"
        echo "---------------------------"
        rm -f "$LOGFILE"
        exit 1
    fi
}

stop_server() {
    echo -e "${COLOR_BLUE}Stopping CADBay Web Server...${NC}"
    ./kill-server-instances.sh
}

clean_server() {
    echo -e "${COLOR_BLUE}Force killing webserver processes...${NC}"
    ./kill-server-instances.sh --force
    
    echo -e "${COLOR_GREEN}✓ Cleanup complete${NC}"
}

status_server() {
    if pgrep -x webserver > /dev/null || pgrep -f "Server Launcher" > /dev/null; then
        echo -e "${COLOR_GREEN}✓ Web server is RUNNING${NC}"
        pgrep -af "webserver" | head -1
    else
        echo -e "${COLOR_RED}✗ Web server is NOT RUNNING${NC}"
    fi
}

# Main logic
COMMAND="${1:-start}"

case "$COMMAND" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    restart)
        stop_server
        sleep 1
        start_server
        ;;
    status)
        status_server
        ;;
    clean)
        clean_server
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo -e "${COLOR_RED}Unknown command: $COMMAND${NC}"
        show_help
        exit 1
        ;;
esac
