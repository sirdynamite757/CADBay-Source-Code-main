#!/bin/bash

################################################################################
# CADBay Server Launcher with On-Boot Health Check Integration
# 
# Purpose:
#   This enhanced launcher script starts the CADBay webserver and automatically
#   runs comprehensive health checks once the server signals it is live and ready
#   to accept connections.
#
# Features:
#   - Starts the CADBay webserver
#   - Automatically runs on-boot health checks
#   - Captures server output for log file analysis
#   - Graceful error handling and recovery
#   - Integration with the server startup workflow
#
# Usage:
#   ./start-server-with-tests.sh
#
# Behavior:
#   1. Builds the webserver (if needed)
#   2. Starts the webserver in background with output captured to log file
#   3. Runs on-boot health check tests once server signals ready
#   4. Reports test results
#
################################################################################

set -eu  # Exit on error, treat undefined variables as errors

# Color codes for output formatting
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'  # No Color

# Get the script directory (where CADBay source is located)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Configuration paths
CONFIG_FILE="$SCRIPT_DIR/monitor.conf"
LOG_DIR="$SCRIPT_DIR/logs"
SERVER_LOG_FILE="$LOG_DIR/server.log"
TEST_SCRIPT="$SCRIPT_DIR/test-server-onboot.sh"

################################################################################
# HELPER FUNCTION: Setup logging directory
# 
# Creates the logs directory if it doesn't exist.
# This is needed to capture server output for analysis by the health check.
# 
# Returns: 0 on success, 1 on failure
################################################################################
setup_logging() {
    echo -e "${BLUE}[*] Setting up logging...${NC}"
    
    # Create logs directory if it doesn't exist
    # -p flag ensures parent directories are created if needed
    mkdir -p "$LOG_DIR"
    
    if [ ! -d "$LOG_DIR" ]; then
        echo -e "${RED}[ERROR] Failed to create logs directory: $LOG_DIR${NC}"
        return 1
    fi
    
    echo -e "${GREEN}[✓] Logs directory: $LOG_DIR${NC}"
    return 0
}

################################################################################
# HELPER FUNCTION: Verify test script exists
# 
# Ensures that the on-boot test script is available before attempting to run it.
# 
# Returns: 0 if script exists and is executable, 1 otherwise
################################################################################
verify_test_script() {
    if [ ! -f "$TEST_SCRIPT" ]; then
        echo -e "${RED}[ERROR] Test script not found: $TEST_SCRIPT${NC}"
        return 1
    fi
    
    if [ ! -x "$TEST_SCRIPT" ]; then
        echo -e "${YELLOW}[WARN] Test script not executable, fixing permissions...${NC}"
        chmod +x "$TEST_SCRIPT"
    fi
    
    echo -e "${GREEN}[✓] Test script verified: $TEST_SCRIPT${NC}"
    return 0
}

################################################################################
# HELPER FUNCTION: Verify configuration file
# 
# Ensures that monitor.conf exists with required PUBLIC_IP and SERVER_PORT settings.
# These are used by the health check test to know what server to test.
# 
# Returns: 0 if config is valid, 1 otherwise
################################################################################
verify_config() {
    echo -e "${BLUE}[*] Verifying configuration...${NC}"
    
    if [ ! -f "$CONFIG_FILE" ]; then
        echo -e "${RED}[ERROR] Configuration file not found: $CONFIG_FILE${NC}"
        return 1
    fi
    
    # Source the config to check that required variables are set
    source "$CONFIG_FILE" || {
        echo -e "${RED}[ERROR] Failed to parse configuration file${NC}"
        return 1
    }
    
    # Verify required variables
    if [ -z "${PUBLIC_IP:-}" ] || [ -z "${SERVER_PORT:-}" ]; then
        echo -e "${RED}[ERROR] PUBLIC_IP or SERVER_PORT not configured in $CONFIG_FILE${NC}"
        return 1
    fi
    
    echo -e "${GREEN}[✓] Configuration valid:${NC}"
    echo "    Public IP: $PUBLIC_IP"
    echo "    Server Port: $SERVER_PORT"
    return 0
}

################################################################################
# HELPER FUNCTION: Build the webserver
# 
# Compiles the C webserver if needed. This is done before starting the server
# to ensure we have the latest binary.
# 
# Returns: 0 on successful build, 1 on failure
################################################################################
build_webserver() {
    echo ""
    echo -e "${BLUE}[*] Checking if webserver needs building...${NC}"
    
    cd "$SCRIPT_DIR"
    
    # Check if Makefile exists
    if [ ! -f "Makefile" ]; then
        echo -e "${YELLOW}[WARN] Makefile not found, skipping build${NC}"
        return 0
    fi
    
    # Check if the webserver binary exists
    if [ -f "./webserver" ]; then
        echo -e "${GREEN}[✓] Webserver binary found, skipping build${NC}"
        return 0
    fi
    
    echo -e "${YELLOW}Building webserver...${NC}"
    
    # Clean any old build artifacts and rebuild
    # The -j flag enables parallel compilation for faster builds
    if make clean > /dev/null 2>&1; then
        if make -j build; then
            echo -e "${GREEN}[✓] Webserver build successful${NC}"
            return 0
        fi
    fi
    
    echo -e "${RED}[ERROR] Failed to build webserver${NC}"
    return 1
}

################################################################################
# HELPER FUNCTION: Start the webserver
# 
# Starts the webserver as a background process and captures output to a log file.
# The log file is monitored by the health check test to detect when server is ready.
# 
# Returns: 0 on successful start, sets global WEBSERVER_PID to the process ID
################################################################################
start_webserver() {
    echo ""
    echo -e "${BLUE}[*] Starting CADBay webserver...${NC}"
    
    cd "$SCRIPT_DIR"
    
    # Check that webserver binary exists
    if [ ! -x "./webserver" ]; then
        echo -e "${RED}[ERROR] Webserver binary not found or not executable${NC}"
        return 1
    fi
    
    # Clear the old log file to start fresh
    > "$SERVER_LOG_FILE"
    
    # Start webserver in background with output redirected to log file
    # This allows us to capture the "Server listening" message for the health check
    # stdout is sent to log file, stderr is also sent to log file for troubleshooting
    # The & sends the process to background
    ./webserver >> "$SERVER_LOG_FILE" 2>&1 &
    
    # Capture the process ID of the webserver
    WEBSERVER_PID=$!
    
    echo -e "${GREEN}[✓] Webserver started (PID: $WEBSERVER_PID)${NC}"
    echo "    Output log: $SERVER_LOG_FILE"
    echo ""
    
    # Give the server a moment to start
    sleep 1
    
    # Verify the server process is still running (didn't crash immediately)
    if ! kill -0 $WEBSERVER_PID 2>/dev/null; then
        echo -e "${RED}[ERROR] Webserver process exited unexpectedly${NC}"
        echo "    Check log file: $SERVER_LOG_FILE"
        return 1
    fi
    
    return 0
}

################################################################################
# HELPER FUNCTION: Run the on-boot health check tests
# 
# Executes the comprehensive health check test suite. This test:
# - Waits for server to signal ready
# - Tests root endpoint with multiple clients
# - Tests all page endpoints
# - Generates summary report
# 
# Parameters:
#   $1 = Process ID of webserver (for monitoring)
#   $2 = Log file to monitor for "listening" message
# 
# Returns: 0 if all tests pass, 1 if any fail
################################################################################
run_health_checks() {
    local webserver_pid=$1
    local log_file=$2
    
    echo -e "${BLUE}[*] Running on-boot health checks...${NC}"
    echo ""
    
    # Execute the test script, passing the webserver PID and log file for monitoring
    # The test script will use these to detect when server is ready
    if "$TEST_SCRIPT" "$webserver_pid" "$log_file"; then
        echo -e "${GREEN}[✓] All health checks passed!${NC}"
        return 0
    else
        echo -e "${RED}[✗] Health checks failed. Check output above for details.${NC}"
        return 1
    fi
}

################################################################################
# HELPER FUNCTION: Cleanup on script exit
# 
# Ensures that the webserver is properly terminated if this launcher script
# is interrupted (Ctrl+C) or exits abnormally.
# 
# This is registered as a trap handler in the main function.
################################################################################
cleanup() {
    echo ""
    echo -e "${YELLOW}[*] Cleaning up...${NC}"
    
    # Kill the webserver if it's still running
    if [ -n "${WEBSERVER_PID:-}" ] && kill -0 $WEBSERVER_PID 2>/dev/null; then
        echo -e "${YELLOW}[*] Stopping webserver (PID: $WEBSERVER_PID)...${NC}"
        # Send SIGTERM signal to gracefully terminate
        kill -TERM $WEBSERVER_PID 2>/dev/null || true
        
        # Wait up to 5 seconds for graceful shutdown
        local wait_count=0
        while [ $wait_count -lt 50 ]; do
            if ! kill -0 $WEBSERVER_PID 2>/dev/null; then
                echo -e "${GREEN}[✓] Webserver stopped gracefully${NC}"
                return
            fi
            sleep 0.1
            wait_count=$((wait_count + 1))
        done
        
        # If still running, force kill
        echo -e "${YELLOW}[*] Forcing webserver shutdown...${NC}"
        kill -9 $WEBSERVER_PID 2>/dev/null || true
    fi
}

################################################################################
# MAIN SCRIPT EXECUTION
# 
# Execution flow:
# 1. Setup logging infrastructure
# 2. Verify configuration and test script
# 3. Build webserver if needed
# 4. Start webserver in background
# 5. Run health checks once server signals ready
# 6. Display results and exit
#
# A trap handler is registered to ensure cleanup on exit or interrupt.
################################################################################

main() {
    echo ""
    echo "================================================================================"
    echo "    CADBay Server Launcher with On-Boot Health Checks"
    echo "================================================================================"
    echo ""
    
    # Register cleanup function to run on script exit (including Ctrl+C)
    # The trap handler ensures the webserver is properly terminated
    trap cleanup EXIT SIGINT SIGTERM
    
    # Step 1: Setup logging
    if ! setup_logging; then
        exit 1
    fi
    echo ""
    
    # Step 2: Verify configuration and test script
    if ! verify_config; then
        exit 1
    fi
    echo ""
    
    if ! verify_test_script; then
        exit 1
    fi
    echo ""
    
    # Step 3: Build webserver if needed
    if ! build_webserver; then
        exit 1
    fi
    echo ""
    
    # Step 4: Start the webserver
    if ! start_webserver; then
        exit 1
    fi
    
    # Step 5: Run health checks
    # Pass the webserver PID and log file path so the test script can monitor them
    if run_health_checks "$WEBSERVER_PID" "$SERVER_LOG_FILE"; then
        echo ""
        echo -e "${GREEN}[SUCCESS] Server is ready for use!${NC}"
        echo ""
        echo "Next steps:"
        echo "  - Open browser to: http://$PUBLIC_IP:$SERVER_PORT"
        echo "  - Server logs: $SERVER_LOG_FILE"
        echo ""
        echo "Press Ctrl+C to stop the server"
        echo ""
        
        # Keep the server running until user interrupts (Ctrl+C)
        # The trap handler will ensure clean shutdown
        wait $WEBSERVER_PID
    else
        echo ""
        echo -e "${RED}[FAILURE] Server failed health checks${NC}"
        exit 1
    fi
}

# Execute main function with error handling
main "$@" || exit $?
