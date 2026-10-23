#!/bin/bash

# CADBay Apache De-Integration
#
# The CADBay C webserver is self-contained: it serves static files
# (including extensionless URLs) and all /api endpoints directly, with no
# Apache, systemd service, or elevated/polkit path required.
#
# This script REPLACES the old Apache-install behaviour. Running it removes
# the Apache front-end and the cadbay-monitor system service that were
# previously installed, so CADBay runs entirely on its own.
#
# The legacy Apache reverse-proxy config is kept in apache-cadbay.conf for
# reference only; it is no longer installed by anything.

set -e

echo "========================================="
echo "  CADBay Apache De-Integration"
echo "========================================="
echo ""
echo "CADBay is self-contained and does not need Apache."
echo "This will remove any previously-installed Apache + monitor integration."
echo ""

if [ "$EUID" -ne 0 ]; then
    echo "This script must be run as root to remove system files. Use: sudo $0"
    exit 1
fi

# 1. Stop and remove the cadbay-monitor systemd service (installed by
#    apache-monitor-setup.sh). This is the piece that pulled in the
#    elevated/polkit install path.
if [ -f /etc/systemd/system/cadbay-monitor.service ]; then
    echo "Removing cadbay-monitor.service..."
    systemctl disable --now cadbay-monitor.service 2>/dev/null || true
    rm -f /etc/systemd/system/cadbay-monitor.service
    systemctl daemon-reload 2>/dev/null || true
    echo "  removed"
else
    echo "  (cadbay-monitor.service not installed - nothing to remove)"
fi

# 2. Remove the system-wide monitor binary/config that the setup copied out.
[ -f /usr/local/bin/cadbay-monitor ] && rm -f /usr/local/bin/cadbay-monitor && echo "  removed /usr/local/bin/cadbay-monitor"
[ -f /etc/cadbay-monitor.conf ] && rm -f /etc/cadbay-monitor.conf && echo "  removed /etc/cadbay-monitor.conf"

# 3. Disable the CADBay Apache virtual hosts, if Apache is present.
if command -v a2dissite >/dev/null 2>&1; then
    for site in cadbay.conf CADBay.conf; do
        if [ -e "/etc/apache2/sites-enabled/$site" ]; then
            echo "Disabling Apache site $site..."
            a2dissite "$site" 2>/dev/null || true
            echo "  disabled"
        fi
    done
fi

echo ""
echo "========================================="
echo "  Done"
echo "========================================="
echo ""
echo "CADBay now runs standalone. Start it with:"
echo "  ./start-server.sh"
echo ""
echo "Then open: http://127.0.0.1:4000"
echo ""
