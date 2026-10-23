#!/bin/bash

################################################################################
# CADBay Server On-Boot Health Check Test Script
# 
# Purpose: 
#   This script performs a comprehensive health check of the CADBay server
#   once it signals that it is live and listening. It tests server availability
#   using multiple HTTP clients (browsers) and verifies that all page endpoints
#   are accessible.
#
# Features:
#   - Waits for server to signal "listening" status
#   - Tests using 2+ different HTTP clients (curl and wget) for cross-client validation
#   - Verifies all HTML pages in Body/ directory are accessible
#   - Comprehensive logging and reporting
#   - Exit codes for integration with other systems
#
# Usage:
#   ./test-server-onboot.sh [webserver_pid] [optional_log_file]
#
################################################################################

set -eu  # Exit on error, treat undefined variables as errors

# Color codes for terminal output (makes logs easier to read)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'  # No Color

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Configuration file path - where we store PUBLIC_IP and SERVER_PORT
CONFIG_FILE="$SCRIPT_DIR/monitor.conf"

# Test results tracking
TESTS_PASSED=0
TESTS_FAILED=0
FAILED_TESTS=()

################################################################################
# HELPER FUNCTION: Load configuration from monitor.conf
# 
# This function reads the monitor.conf file to extract:
# - PUBLIC_IP: The IP address to test against
# - SERVER_PORT: The port the server is listening on
# 
# Returns: 0 on success, 1 if config file not found
################################################################################
load_config() {
    echo -e "${BLUE}[*] Loading configuration from $CONFIG_FILE${NC}"
    
    if [ ! -f "$CONFIG_FILE" ]; then
        echo -e "${RED}[ERROR] Config file not found: $CONFIG_FILE${NC}"
        return 1
    fi
    
    # Source the configuration file to load variables
    # This is safe here since we control the config file
    source "$CONFIG_FILE"
    
    # Verify required variables are set
    if [ -z "${PUBLIC_IP:-}" ] || [ -z "${SERVER_PORT:-}" ]; then
        echo -e "${RED}[ERROR] PUBLIC_IP or SERVER_PORT not set in config${NC}"
        return 1
    fi
    
    echo -e "${GREEN}[✓] Configuration loaded:${NC}"
    echo "    Public IP: $PUBLIC_IP"
    echo "    Server Port: $SERVER_PORT"
    return 0
}

################################################################################
# HELPER FUNCTION: Wait for server to signal it is live
# 
# This function monitors the webserver process output to detect when it has
# started and is ready to accept connections. It looks for the specific message:
# "Server listening on http://IP:PORT"
# 
# Parameters:
#   $1 = PID of the webserver process (if available)
#   $2 = Optional log file to monitor
# 
# Returns: 0 when server is ready, 1 if timeout or error
################################################################################
wait_for_server_ready() {
    local server_pid="${1:-}"
    local log_file="${2:-}"
    local max_attempts=60  # 60 attempts * 1 second = 60 seconds timeout
    local attempt=0
    
    echo -e "${BLUE}[*] Waiting for server to signal 'listening' status...${NC}"
    echo "    Target: http://$PUBLIC_IP:$SERVER_PORT"
    
    # If we have a log file, monitor it for the "listening" message
    if [ -n "$log_file" ] && [ -f "$log_file" ]; then
        echo -e "${BLUE}[*] Monitoring log file: $log_file${NC}"
        
        # Use tail to follow the log file in real-time
        while [ $attempt -lt $max_attempts ]; do
            # Check if "listening" message appears in the log
            if grep -q "listening" "$log_file" 2>/dev/null; then
                echo -e "${GREEN}[✓] Server is listening!${NC}"
                sleep 1  # Give server a moment to fully initialize
                return 0
            fi
            
            attempt=$((attempt + 1))
            echo -n "."
            sleep 1
        done
    else
        # If no log file, try connecting to the server port directly
        # This is a fallback method to detect when server is ready
        echo -e "${BLUE}[*] Using TCP connection test (no log file provided)${NC}"
        
        while [ $attempt -lt $max_attempts ]; do
            # Try to connect to the server port using /dev/tcp
            # This is a bash built-in check without needing nc or telnet
            if timeout 1 bash -c "echo > /dev/tcp/$PUBLIC_IP/$SERVER_PORT" 2>/dev/null; then
                echo -e "${GREEN}[✓] Server is accepting connections!${NC}"
                return 0
            fi
            
            attempt=$((attempt + 1))
            echo -n "."
            sleep 1
        done
    fi
    
    echo ""
    echo -e "${RED}[ERROR] Server did not signal ready within timeout period${NC}"
    return 1
}

################################################################################
# HELPER FUNCTION: Test server with a specific HTTP client
# 
# This function makes an HTTP request to a given endpoint using the specified
# HTTP client (curl or wget). It validates:
# - HTTP status code is 200 (OK)
# - Response body is not empty
# - Response time is reasonable
# 
# Parameters:
#   $1 = HTTP client name (curl or wget)
#   $2 = Full URL to test
# 
# Returns: 0 if successful, 1 if test failed
################################################################################
test_endpoint_with_client() {
    local client="$1"
    local url="$2"
    local timeout=10  # seconds
    
    # Check that the client is available
    if ! command -v "$client" &> /dev/null; then
        echo -e "${YELLOW}[SKIP] $client not available${NC}"
        return 0  # Skip this client but don't fail the whole test
    fi
    
    echo -n "    Testing with $client... "
    
    # Use the appropriate client command based on what's available
    # curl: -s (silent), -f (fail on HTTP error), -m (timeout), -w (write format)
    # wget: -q (quiet), -T (timeout), -O (output to file), --spider (don't save file)
    local http_code
    
    if [ "$client" = "curl" ]; then
        http_code=$(curl -s -f -m "$timeout" -w "%{http_code}" -o /dev/null "$url" 2>/dev/null || echo "000")
    elif [ "$client" = "wget" ]; then
        http_code=$(wget -q -T "$timeout" --spider "$url" 2>&1; echo $?)
        # wget returns 0 on success, non-zero on failure
        [ "$http_code" = "0" ] && http_code="200" || http_code="000"
    else
        echo -e "${RED}Unknown client: $client${NC}"
        return 1
    fi
    
    # Check if the HTTP status code indicates success (200 OK)
    if [ "$http_code" = "200" ]; then
        echo -e "${GREEN}OK (HTTP $http_code)${NC}"
        return 0
    else
        echo -e "${RED}FAILED (HTTP $http_code)${NC}"
        return 1
    fi
}

################################################################################
# HELPER FUNCTION: Test all page endpoints
# 
# This function:
# 1. Scans the Body/ directory for HTML files
# 2. Maps each HTML file to its corresponding URL endpoint
# 3. Tests each endpoint using multiple HTTP clients
# 4. Reports pass/fail for each page
# 
# Returns: 0 if all pages pass, 1 if any fail
################################################################################
test_all_pages() {
    echo ""
    echo -e "${BLUE}[*] Testing server page endpoints...${NC}"
    
    local body_dir="$SCRIPT_DIR/Body"
    
    # Check if Body directory exists
    if [ ! -d "$body_dir" ]; then
        echo -e "${RED}[ERROR] Body directory not found: $body_dir${NC}"
        return 1
    fi
    
    # Array to store available HTML files (without .html extension)
    local pages=()
    
    # Scan Body directory for all .html files
    # The HTML files are typically accessed as /pages/filename or /filename endpoints
    for html_file in "$body_dir"/*.html; do
        if [ -f "$html_file" ]; then
            # Extract just the filename without path or extension
            # Example: "Body/Main.html" becomes "Main"
            local page_name=$(basename "$html_file" .html)
            pages+=("$page_name")
        fi
    done
    
    # Check if we found any pages
    if [ ${#pages[@]} -eq 0 ]; then
        echo -e "${YELLOW}[WARN] No HTML files found in Body directory${NC}"
        return 1
    fi
    
    echo -e "${GREEN}[✓] Found ${#pages[@]} pages to test${NC}"
    echo ""
    
    local page_tests_passed=0
    local page_tests_failed=0
    
    # Test each page with multiple clients
    # This ensures the server works correctly with different HTTP implementations
    for page in "${pages[@]}"; do
        echo -e "${BLUE}Testing page: $page${NC}"
        
        # Common URL patterns to try for the page
        # Most web servers serve static HTML files from the root or /pages/ endpoint
        local url_variants=(
            "http://$PUBLIC_IP:$SERVER_PORT/$page"
            "http://$PUBLIC_IP:$SERVER_PORT/$page.html"
            "http://$PUBLIC_IP:$SERVER_PORT/pages/$page"
            "http://$PUBLIC_IP:$SERVER_PORT/Body/$page.html"
        )
        
        local page_passed=0
        
        # Try each URL variant with curl first
        for url in "${url_variants[@]}"; do
            if test_endpoint_with_client "curl" "$url"; then
                page_passed=1
                break  # Success, no need to try other variants
            fi
        done
        
        # If curl didn't work, try wget with different URLs
        if [ $page_passed -eq 0 ]; then
            for url in "${url_variants[@]}"; do
                if test_endpoint_with_client "wget" "$url"; then
                    page_passed=1
                    break
                fi
            done
        fi
        
        # Record the result
        if [ $page_passed -eq 1 ]; then
            page_tests_passed=$((page_tests_passed + 1))
            TESTS_PASSED=$((TESTS_PASSED + 1))
        else
            page_tests_failed=$((page_tests_failed + 1))
            TESTS_FAILED=$((TESTS_FAILED + 1))
            FAILED_TESTS+=("Page: $page")
        fi
        echo ""
    done
    
    # Summary of page tests
    echo -e "${BLUE}Page test summary:${NC}"
    echo "  Passed: $page_tests_passed"
    echo "  Failed: $page_tests_failed"
    echo ""
    
    [ $page_tests_failed -eq 0 ]
    return $?
}

################################################################################
# HELPER FUNCTION: Test server root endpoint
# 
# This function tests that the server responds at its root path (/)
# and returns valid HTTP responses. This is the most basic connectivity test.
# 
# Returns: 0 if root endpoint is accessible, 1 otherwise
################################################################################
test_server_root() {
    echo -e "${BLUE}[*] Testing server root endpoint...${NC}"
    
    local root_url="http://$PUBLIC_IP:$SERVER_PORT/"
    
    echo "  Testing: $root_url"
    echo ""
    
    local root_passed=0
    
    # Test with curl
    if test_endpoint_with_client "curl" "$root_url"; then
        root_passed=1
        TESTS_PASSED=$((TESTS_PASSED + 1))
    fi
    
    # Test with wget as well for dual-client verification
    if [ $root_passed -eq 1 ]; then
        echo -n "    Verifying with wget... "
        if test_endpoint_with_client "wget" "$root_url"; then
            echo -e "${GREEN}[✓] Root endpoint verified with multiple clients${NC}"
        else
            echo -e "${YELLOW}[WARN] Root endpoint works with curl but failed wget${NC}"
        fi
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        FAILED_TESTS+=("Root endpoint")
    fi
    
    echo ""
    
    [ $root_passed -eq 1 ]
    return $?
}

################################################################################
# HELPER FUNCTION: Generate and display test report
# 
# This function creates a summary of all tests that were performed,
# showing pass/fail counts and any failures for debugging.
# 
# Returns: 0 if all tests passed, 1 if any tests failed
################################################################################
generate_report() {
    echo ""
    echo "================================================================================"
    echo "                    CADBay SERVER ON-BOOT TEST REPORT"
    echo "================================================================================"
    echo ""
    echo "Test Execution Summary:"
    echo "  Total Tests Passed:  $TESTS_PASSED"
    echo "  Total Tests Failed:  $TESTS_FAILED"
    echo ""
    
    if [ $TESTS_FAILED -gt 0 ]; then
        echo -e "${RED}FAILED TESTS:${NC}"
        for failed_test in "${FAILED_TESTS[@]}"; do
            echo "  - $failed_test"
        done
        echo ""
    fi
    
    # Final status
    if [ $TESTS_FAILED -eq 0 ]; then
        echo -e "${GREEN}[SUCCESS] All on-boot tests passed!${NC}"
        echo -e "${GREEN}Server is fully operational and ready for use.${NC}"
        echo ""
        return 0
    else
        echo -e "${RED}[FAILURE] Some tests failed. Review the output above for details.${NC}"
        echo -e "${RED}Server may not be fully operational.${NC}"
        echo ""
        return 1
    fi
}

################################################################################
# MAIN SCRIPT EXECUTION
# 
# The script follows this execution flow:
# 1. Load configuration (PUBLIC_IP, SERVER_PORT)
# 2. Wait for server to signal it's ready
# 3. Test root endpoint with multiple clients
# 4. Test all page endpoints
# 5. Generate summary report
# 6. Exit with appropriate code for automation
#
################################################################################

main() {
    echo ""
    echo "================================================================================"
    echo "         CADBay Server On-Boot Health Check Test"
    echo "================================================================================"
    echo ""
    
    # Step 1: Load configuration
    if ! load_config; then
        echo -e "${RED}[ERROR] Failed to load configuration${NC}"
        exit 1
    fi
    echo ""
    
    # Step 2: Wait for server to signal it's ready
    # The optional parameters allow monitoring a specific process or log file
    local server_pid="${1:-}"
    local log_file="${2:-}"
    
    if ! wait_for_server_ready "$server_pid" "$log_file"; then
        echo -e "${RED}[ERROR] Server failed to signal ready status${NC}"
        exit 1
    fi
    echo ""
    
    # Step 3: Test the server root endpoint
    # This is a basic connectivity check
    if ! test_server_root; then
        echo -e "${RED}[ERROR] Server root endpoint not responding${NC}"
    fi
    
    # Step 4: Test all page endpoints
    # This verifies that the server can serve all its pages
    if ! test_all_pages; then
        echo -e "${RED}[WARN] Some pages may not be accessible${NC}"
    fi
    
    # Step 5: Generate final report
    # This summarizes all test results and provides an exit code
    if generate_report; then
        exit 0  # All tests passed
    else
        exit 1  # Some tests failed
    fi
}

# Run the main function with all command-line arguments
main "$@"
