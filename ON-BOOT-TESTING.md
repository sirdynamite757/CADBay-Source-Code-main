################################################################################
#
#  CADBay Server On-Boot Health Check System
#
#  A comprehensive automated testing system that validates server availability
#  and page accessibility immediately after the CADBay server starts.
#
################################################################################

## Overview

This system provides two main components:

1. **test-server-onboot.sh** - Comprehensive health check test suite
2. **start-server-with-tests.sh** - Integrated server launcher with automatic testing

The system is designed to:
- Automatically detect when the server is ready to serve requests
- Test server availability using multiple HTTP clients (curl and wget)
- Verify that all HTML pages in the Body/ directory are accessible
- Provide detailed pass/fail reporting
- Integrate seamlessly with the server startup workflow

## Configuration

Before using these scripts, update the CADBay configuration file:

### File: monitor.conf

Edit `/home/sirdynamite757/CADBay-Source-Code-main/monitor.conf` and set:

```
# Server on-boot test configuration
PUBLIC_IP=203.0.113.42          # Replace with your actual public IP
SERVER_PORT=4000                # Replace with your server port (if different)
```

Examples:
```
# For local testing:
PUBLIC_IP=127.0.0.1
SERVER_PORT=4000

# For production with public IP:
PUBLIC_IP=203.0.113.42
SERVER_PORT=4000
```

## Usage

### Option 1: Use the Integrated Launcher (Recommended)

This is the easiest method - starts the server and automatically runs tests:

```bash
cd /home/sirdynamite757/CADBay-Source-Code-main
./start-server-with-tests.sh
```

This script will:
1. Setup logging infrastructure
2. Verify configuration
3. Build the webserver (if needed)
4. Start the webserver in background
5. Run comprehensive health checks
6. Display results and keep server running

Press Ctrl+C to stop the server.

### Option 2: Run Tests Standalone

If the server is already running, test it separately:

```bash
cd /home/sirdynamite757/CADBay-Source-Code-main
./test-server-onboot.sh [optional_pid] [optional_logfile]
```

Examples:
```bash
# Basic usage - will wait for server to be ready:
./test-server-onboot.sh

# With process monitoring:
./test-server-onboot.sh 12345

# With log file monitoring:
./test-server-onboot.sh 12345 /path/to/server.log
```

### Option 3: Manual Integration

Add the test to your own startup scripts:

```bash
# Start server in background
./webserver >> logs/server.log 2>&1 &
SERVER_PID=$!

# Run health checks
./test-server-onboot.sh $SERVER_PID logs/server.log
TEST_RESULT=$?

# Take action based on test results
if [ $TEST_RESULT -eq 0 ]; then
    echo "Server is healthy and ready"
else
    echo "Server health checks failed"
    kill $SERVER_PID
    exit 1
fi
```

## What Gets Tested

### 1. Server Root Endpoint
- Tests HTTP GET on `http://PUBLIC_IP:SERVER_PORT/`
- Verifies HTTP 200 OK response
- Uses both curl and wget for cross-client validation

### 2. All Page Endpoints
The test scans the Body/ directory and tests each HTML page:

Current pages detected:
- Start.html
- Main.html
- LoginRegister.html
- AccountSuspended.html
- Header.html
- Footer.html
- Sidebar.html
- ColorDefault.css

For each page, the test tries multiple URL patterns:
- `/pagename`
- `/pagename.html`
- `/pages/pagename`
- `/Body/pagename.html`

This ensures compatibility with different routing configurations.

### 3. Multi-Client Validation
Each endpoint is tested with:
- **curl** - Common HTTP client, used in scripts and automation
- **wget** - Alternative HTTP client, provides redundancy

If one client fails but another succeeds, the page is still marked as accessible.

## Understanding the Output

### Success Output Example

```
================================================================================
         CADBay Server On-Boot Health Check Test
================================================================================

[*] Loading configuration from /path/to/monitor.conf
[✓] Configuration loaded:
    Public IP: 127.0.0.1
    Server Port: 4000

[*] Waiting for server to signal 'listening' status...
    Target: http://127.0.0.1:4000
[✓] Server is listening!

[*] Testing server root endpoint...
  Testing: http://127.0.0.1:4000/

    Testing with curl... OK (HTTP 200)
    Verifying with wget... OK (HTTP 200)
[✓] Root endpoint verified with multiple clients

[*] Testing server page endpoints...
[✓] Found 8 pages to test

[BLUE]Testing page: Start[NC]
    Testing with curl... OK (HTTP 200)

[SUCCESS] All on-boot tests passed!
```

### Failure Output Example

```
[*] Testing page: LoginRegister
    Testing with curl... FAILED (HTTP 404)
    Testing with wget... FAILED (HTTP 000)

[FAILURE] Some tests failed. Review the output above for details.
Server may not be fully operational.

Failed tests:
  - Page: LoginRegister
```

### Exit Codes

```
0   - All tests passed, server is healthy
1   - One or more tests failed, server may have issues
```

Use exit codes for automation:
```bash
./test-server-onboot.sh
if [ $? -eq 0 ]; then
    # Proceed with operations
fi
```

## Logs and Troubleshooting

### Server Log File
```
Location: /home/sirdynamite757/CADBay-Source-Code-main/logs/server.log
```

Contains complete webserver output and error messages. Monitor this file if tests fail.

### Viewing Logs

```bash
# View current server log
cat logs/server.log

# Follow server log in real-time
tail -f logs/server.log

# Search logs for errors
grep -i error logs/server.log
```

### Common Issues

**Issue: "Server did not signal ready within timeout period"**
- Solution: Server may be taking longer to start, check server.log for errors
- Ensure PUBLIC_IP and SERVER_PORT are correct in monitor.conf

**Issue: "HTTP 404" errors for pages**
- Solution: Check that Body/ directory has the HTML files
- Verify server is configured to serve static files correctly
- Check server log for routing errors

**Issue: "Connection refused"**
- Solution: Server may not be running or port is wrong
- Verify PUBLIC_IP and SERVER_PORT in monitor.conf
- Check if port is already in use: `lsof -i :PORT`

**Issue: "curl/wget: command not found"**
- Solution: Install curl and/or wget:
  ```bash
  # Ubuntu/Debian
  sudo apt-get install curl wget
  
  # CentOS/RHEL
  sudo yum install curl wget
  ```

## Architecture and Concepts

### How the Test Detects Server Ready

The test uses two mechanisms to detect when the server is ready:

1. **Log File Monitoring** (Primary)
   - Watches the server log file for "listening" message
   - Indicates server has bound to port and is accepting connections
   - More reliable than connection testing

2. **TCP Connection Test** (Fallback)
   - Attempts to connect to the server port
   - Works if no log file is available
   - Uses bash built-in /dev/tcp for portability

### Multi-Client Testing Philosophy

Testing with multiple HTTP clients ensures:
- **Compatibility**: Server works with different client implementations
- **Robustness**: Doesn't rely on quirks of a single client
- **Redundancy**: If one client fails, the test has a backup

### URL Pattern Discovery

The test tries multiple URL patterns for each page:
- Direct filename: `/Start`
- With extension: `/Start.html`
- Pages subdirectory: `/pages/Start`
- Full path: `/Body/Start.html`

This handles different server configurations and routing rules.

## Performance Considerations

### Timeouts
- Server readiness check: 60 seconds (configurable in test script)
- Individual endpoint tests: 10 seconds each
- Total test time: ~30-60 seconds depending on page count

### Resource Usage
- Minimal memory footprint (shell scripts)
- Lightweight HTTP requests (one per page)
- Does not stress the server

## Integration with CI/CD

These scripts are designed for automation. Example Jenkins integration:

```groovy
pipeline {
    agent any
    
    stages {
        stage('Start Server') {
            steps {
                sh 'cd /path/to/cadbay && ./start-server-with-tests.sh &'
                sh 'sleep 30'  // Wait for server startup
            }
        }
        
        stage('Run Tests') {
            steps {
                sh 'cd /path/to/cadbay && ./test-server-onboot.sh'
            }
        }
    }
    
    post {
        always {
            // Cleanup
            sh 'pkill -f "./webserver"'
        }
    }
}
```

## Security Considerations

### What These Scripts Do NOT Do

- Do not attempt unauthorized access
- Do not perform security testing (XSS, SQL injection, etc.)
- Do not modify server state
- Do not store sensitive data

### Safe to Run

- Scripts use read-only HTTP GET requests
- No authentication credentials are used
- Safe for production environments (just checks availability)

## Future Enhancements

Possible improvements:

1. **Performance Metrics**
   - Measure response times
   - Track server latency trends
   - Alert on degraded performance

2. **Extended Testing**
   - Test POST endpoints (if applicable)
   - Verify SSL/TLS certificates
   - Test database connectivity

3. **Notification System**
   - Email alerts on test failure
   - Slack/Discord integration
   - Syslog reporting

4. **Automated Recovery**
   - Automatically restart server on failure
   - Escalation procedures
   - Fallback server activation

## Support and Debugging

### Enable Debug Mode

Add this to the top of the script to see more details:
```bash
set -x  # Print each command before executing
```

### Manual Testing

Test a single page manually:
```bash
# Using curl
curl -v http://127.0.0.1:4000/Start.html

# Using wget
wget -v http://127.0.0.1:4000/Start.html

# Using telnet (check port is open)
telnet 127.0.0.1 4000
```

### Contact Information

For issues or questions about these test scripts, refer to the CADBay documentation.

################################################################################
#  End of CADBay Server On-Boot Health Check System Documentation
################################################################################
