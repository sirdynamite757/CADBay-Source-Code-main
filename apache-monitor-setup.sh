#!/bin/bash

# Apache Monitor Integration Setup Script
# This script connects the monitor-mod to Apache.
#
# NOTE: CADBay no longer requires this. The C webserver is self-contained
# and serves static files + /api directly. This script performs a SYSTEM-WIDE
# install (root-owned files under /etc, /usr/local/bin and a systemd unit),
# which is the path that pulls in the elevated/polkit machinery. It is
# therefore OPT-IN: it does nothing unless you explicitly pass --install.
#
#   ./apache-monitor-setup.sh            -> prints usage, makes no changes
#   sudo ./apache-monitor-setup.sh --install  -> perform the legacy install
#
# To undo it, run: sudo ./enable-apache.sh

set -e

if [ "${1:-}" != "--install" ]; then
    echo "========================================="
    echo "  Apache Monitor Setup (disabled)"
    echo "========================================="
    echo ""
    echo "CADBay is self-contained and does not need this Apache/systemd setup."
    echo "No changes were made."
    echo ""
    echo "Just start the server directly:"
    echo "  ./start-server.sh"
    echo ""
    echo "If you genuinely want the legacy system-wide install, run:"
    echo "  sudo $0 --install"
    echo ""
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MONITOR_MOD="$SCRIPT_DIR/monitor-mod"
MONITOR_CONF="$SCRIPT_DIR/monitor.conf"

echo "========================================="
echo "  Apache Monitor Setup (legacy)"
echo "========================================="
echo ""

# Check if monitor-mod exists
if [ ! -x "$MONITOR_MOD" ]; then
    echo "Error: monitor-mod not found at $MONITOR_MOD"
    exit 1
fi

# Check if monitor.conf exists
if [ ! -f "$MONITOR_CONF" ]; then
    echo "Error: monitor.conf not found at $MONITOR_CONF"
    exit 1
fi

echo "✓ Found monitor-mod executable"
echo "✓ Found monitor.conf configuration"
echo ""

# Step 1: Copy monitor-mod to /usr/local/bin for system-wide access
echo "Installing monitor-mod to system..."
sudo cp "$MONITOR_MOD" /usr/local/bin/cadbay-monitor
sudo chmod +x /usr/local/bin/cadbay-monitor
echo "✓ Installed to /usr/local/bin/cadbay-monitor"

# Step 2: Copy monitor.conf to /etc for system-wide access
echo ""
echo "Installing monitor.conf to system..."
sudo cp "$MONITOR_CONF" /etc/cadbay-monitor.conf
sudo chmod 644 /etc/cadbay-monitor.conf
echo "✓ Installed to /etc/cadbay-monitor.conf"

# Step 3: Create systemd service for monitor-mod
echo ""
echo "Creating systemd service..."
sudo tee /etc/systemd/system/cadbay-monitor.service > /dev/null << 'EOF'
[Unit]
Description=CADBay Monitor Service
After=network.target apache2.service
Wants=apache2.service

[Service]
Type=simple
User=www-data
Group=www-data
ExecStart=/usr/local/bin/cadbay-monitor
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

echo "✓ Created systemd service"

# Step 4: Enable and start the service
echo ""
echo "Enabling and starting the monitor service..."
sudo systemctl daemon-reload
sudo systemctl enable cadbay-monitor.service
echo "✓ Monitor service enabled (will start on system reboot)"

echo ""
echo "========================================="
echo "  Setup Complete!"
echo "========================================="
echo ""
echo "Monitor service is ready. To start it now, run:"
echo "  sudo systemctl start cadbay-monitor"
echo ""
echo "To check status:"
echo "  sudo systemctl status cadbay-monitor"
echo ""
echo "To view logs:"
echo "  sudo journalctl -u cadbay-monitor -f"
echo ""
