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

echo "Build successful!"
echo ""

# Get current IP address
IP_ADDR=$(hostname -I | awk '{print $1}')
if [ -z "$IP_ADDR" ]; then
    IP_ADDR="127.0.0.1"
fi

echo "========================================="
echo "Starting CADBay Web Server"
echo "========================================="
echo "Server URL: http://$IP_ADDR:3000"
echo "Port: 3000"
echo ""
echo "Features:"
echo "  - Dark mode toggle button"
echo "  - Responsive design"
echo "  - Static file serving"
echo ""
echo "Press Ctrl+C to stop the server"
echo "========================================="
echo ""

# Run the webserver
./webserver
