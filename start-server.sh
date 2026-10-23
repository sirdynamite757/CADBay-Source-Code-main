#!/bin/bash

# CADBay Web Server Launcher Script

echo "========================================="
echo "  CADBay Web Server"
echo "========================================="
echo ""

# Check if make is installed
if ! command -v make &> /dev/null; then
    echo "Error: 'make' is not installed."
    echo "Please install build-essential or gcc and make."
    exit 1
fi

# Check if gcc is installed
if ! command -v gcc &> /dev/null; then
    echo "Error: 'gcc' is not installed."
    echo "Please install a C compiler (gcc)."
    exit 1
fi

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

cd "$SCRIPT_DIR"

# Build the webserver
echo "Building webserver..."
make clean > /dev/null 2>&1
if ! make build; then
    echo "Error: Failed to compile webserver"
    exit 1
fi

if [[ "${TUNNEL_ENABLED:-0}" == "1" ]]; then
    echo "Building raw-IP tunnel..."
    if ! make tunnel; then
        echo "Error: Failed to compile tunnel"
        exit 1
    fi
fi

echo "Build successful!"
echo ""

# Get current local IP address
LOCAL_IP=$(hostname -I | awk '{print $1}')
if [ -z "$LOCAL_IP" ]; then
    LOCAL_IP="127.0.0.1"
fi

# Attempt to determine public IP for terminal display
PUBLIC_IP="unknown"
if command -v curl >/dev/null 2>&1; then
    PUBLIC_IP=$(curl -s --connect-timeout 5 https://api.ipify.org)
elif command -v wget >/dev/null 2>&1; then
    PUBLIC_IP=$(wget -qO- --timeout=5 https://api.ipify.org)
fi
if [ -z "$PUBLIC_IP" ]; then
    PUBLIC_IP="unknown"
fi

echo "========================================="
echo "Starting CADBay Web Server"
echo "========================================="
echo "Local URL: http://$LOCAL_IP:4000"
echo "Public URL: http://$PUBLIC_IP:4000"
echo "Port: 4000"
echo ""
echo "Features:"
echo "  - Dark mode toggle button"
echo "  - Responsive design"
echo "  - Static file serving"
echo ""
echo "Press Ctrl+C to stop the server"
echo "========================================="
echo ""

# The tunnel is opt-in and forwards an externally reachable port to the webserver.
TUNNEL_PID=""
if [[ "${TUNNEL_ENABLED:-0}" == "1" ]]; then
    TUNNEL_BIND="${TUNNEL_BIND:-0.0.0.0:8080}"
    TUNNEL_TARGET="${TUNNEL_TARGET:-127.0.0.1:4000}"
    echo "Raw-IP tunnel: $TUNNEL_BIND -> $TUNNEL_TARGET"
    ./cadbay-tunnel --listen "$TUNNEL_BIND" "$TUNNEL_TARGET" &
    TUNNEL_PID=$!
    trap 'kill "$TUNNEL_PID" 2>/dev/null' EXIT INT TERM
fi

WATCHDOG_PID=""
if [[ "${PREMIUM_WATCHDOG_ENABLED:-0}" == "1" ]]; then
    make watchdog
    ./premium_watchdog "${PREMIUM_WATCHDOG_INTERVAL:-60}" &
    WATCHDOG_PID=$!
    trap 'kill "$WATCHDOG_PID" 2>/dev/null' EXIT INT TERM
fi

# Run the webserver
./webserver
