# Apache + Monitor Integration Guide

> **Status: OPTIONAL / LEGACY — NOT REQUIRED.**
>
> CADBay is **self-contained**. The C `webserver` serves static files
> (including extensionless URLs like `/loginRegister`) and every `/api/*`
> endpoint directly on port 4000. It does **not** need Apache, systemd, or
> any elevated/`polkit` path — just run `./start-server.sh`.
>
> This document is kept for reference only. The system-wide install it
> describes is opt-in: `apache-monitor-setup.sh` now performs no changes
> unless invoked as `sudo ./apache-monitor-setup.sh --install`. To undo that
> install, run `sudo ./enable-apache.sh`.

This guide explains how to connect your `monitor-mod` executable to Apache for system monitoring and web server integration.

## Overview

Your setup includes:
- **Apache 2.4.58** - Web server
- **monitor-mod** - Monitoring executable
- **monitor.conf** - Monitor configuration

This guide integrates them to provide system monitoring for your Apache web server.

## Architecture

```
┌─────────────────────┐
│   Apache 2.4.58     │
│   (Web Server)      │
└──────────┬──────────┘
           │
           │ (logs & requests)
           ▼
┌─────────────────────┐
│ cadbay-monitor      │
│ (systemd service)   │
│                     │
│ - Resource Monitor  │
│ - Port Monitor      │
│ - Disk Monitor      │
│ - Port 9000 (API)   │
└─────────────────────┘
```

## Prerequisites

- Apache 2.4+ installed
- `sudo` access (for system installation)
- `systemd` (for service management)

## Installation Steps

> **You almost certainly do not need these steps.** Run `./start-server.sh`
> and use `http://127.0.0.1:4000` directly. The steps below describe the
> legacy, opt-in system integration.

### Step 1: Install Monitor as System Service (legacy, opt-in)

```bash
cd /path/to/CADBay-Source-Code-main
chmod +x apache-monitor-setup.sh
sudo ./apache-monitor-setup.sh --install
```

Without `--install` the script prints a notice and changes nothing.

This will:
- Copy `monitor-mod` to `/usr/local/bin/cadbay-monitor`
- Copy `monitor.conf` to `/etc/cadbay-monitor.conf`
- Create a systemd service file
- Enable the service for auto-startup

To undo all of the above later:
```bash
sudo ./enable-apache.sh
```

### Step 2: Start the Monitor Service

```bash
sudo systemctl start cadbay-monitor
```

Verify it's running:
```bash
sudo systemctl status cadbay-monitor
```

View logs:
```bash
sudo journalctl -u cadbay-monitor -f
```

### Step 3: Configure Apache

```bash
cd /path/to/CADBay-Source-Code-main
chmod +x enable-apache.sh
sudo ./enable-apache.sh
```

This will:
- Enable required Apache modules
- Install Apache site configuration
- Enable the CADBay virtual host
- Restart Apache

### Step 4: Configure /etc/hosts (for local testing)

```bash
sudo tee -a /etc/hosts > /dev/null << 'EOF'
127.0.0.1 cadbay.local
127.0.0.1 www.cadbay.local
EOF
```

## Verification

### Check Monitor Status
```bash
sudo systemctl status cadbay-monitor
```

### Check Apache Status
```bash
sudo systemctl status apache2
```

### Test Apache Configuration
```bash
sudo apache2ctl configtest
```

### View Logs
```bash
# Monitor logs
sudo journalctl -u cadbay-monitor -n 50

# Apache access logs
sudo tail -f /var/log/apache2/cadbay_access.log

# Apache error logs
sudo tail -f /var/log/apache2/cadbay_error.log
```

### Test Web Server
```bash
curl http://cadbay.local/
curl http://localhost/
```

## Configuration Details

### Monitor Configuration (/etc/cadbay-monitor.conf)

Key settings:
- **MAX_CPU**: 80% - Alert when CPU exceeds this
- **MAX_MEMORY_GB**: 24 - Alert when memory exceeds this
- **MAX_DISK**: 90% - Alert when disk exceeds this
- **PERMITTED_PORTS**: Ports to monitor (3000,3001,3002,etc.)
- **DISK_PATHS**: Paths to monitor disk usage
- **COLOR_SCHEME**: Visual theme (default, dark, light, blue, green, red)

### Apache Configuration (apache-cadbay.conf)

- **Port 80**: HTTP (unencrypted)
- **Port 443**: HTTPS (requires SSL certificates)
- **Document Root**: `/home/sirdynamite757/CADBay-Source-Code-main`
- **Logging**: 
  - Access: `/var/log/apache2/cadbay_access.log`
  - Errors: `/var/log/apache2/cadbay_error.log`

## Managing the Services

### Start/Stop Monitor
```bash
sudo systemctl start cadbay-monitor
sudo systemctl stop cadbay-monitor
sudo systemctl restart cadbay-monitor
```

### Start/Stop Apache
```bash
sudo systemctl start apache2
sudo systemctl stop apache2
sudo systemctl restart apache2
```

### Enable Auto-startup
```bash
sudo systemctl enable cadbay-monitor
sudo systemctl enable apache2
```

### Disable Auto-startup
```bash
sudo systemctl disable cadbay-monitor
sudo systemctl disable apache2
```

## SSL/HTTPS Setup (Optional)

To enable HTTPS, generate or install certificates:

### Self-signed Certificate (for testing)
```bash
sudo openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout /etc/ssl/private/cadbay.key \
  -out /etc/ssl/certs/cadbay.crt
```

### Let's Encrypt Certificate (production)
```bash
sudo apt-get install certbot python3-certbot-apache
sudo certbot --apache -d cadbay.local
```

Then restart Apache:
```bash
sudo systemctl restart apache2
```

## Troubleshooting

### Monitor Service Won't Start
```bash
# Check system logs
sudo journalctl -u cadbay-monitor -n 100

# Check permissions on monitor executable
ls -la /usr/local/bin/cadbay-monitor

# Check configuration file
sudo cat /etc/cadbay-monitor.conf
```

### Apache Won't Start
```bash
# Test configuration
sudo apache2ctl configtest

# Check Apache error log
sudo tail -f /var/log/apache2/error.log

# Check if ports are in use
sudo lsof -i :80 -i :443
```

### Can't Access http://cadbay.local
- Add entry to `/etc/hosts` (see Step 4 above)
- Check firewall: `sudo ufw allow 80` and `sudo ufw allow 443`
- Check Apache is running: `sudo systemctl status apache2`
- Check DNS resolution: `ping cadbay.local`

### Monitor Not Monitoring
```bash
# Check monitor configuration
sudo cat /etc/cadbay-monitor.conf

# Check if monitor process is running
ps aux | grep cadbay-monitor

# Check monitor logs
sudo journalctl -u cadbay-monitor -f
```

## Performance Monitoring

Monitor the integration in real-time:

```bash
# Watch Apache requests
sudo tail -f /var/log/apache2/cadbay_access.log

# Monitor system resources
top  # or htop if installed

# Monitor network connections
sudo netstat -tlnp | grep apache

# Monitor open files
lsof -p $(pgrep cadbay-monitor)
```

## Port Configuration

By default:
- **Apache**: Port 80 (HTTP) and 443 (HTTPS)
- **Monitor**: Uses configured ports from `monitor.conf`

To change Apache port:
1. Edit `/etc/apache2/sites-available/cadbay.conf`
2. Change `<VirtualHost *:80>` to `<VirtualHost *:8080>`
3. Run `sudo systemctl restart apache2`
4. Update firewall if needed: `sudo ufw allow 8080`

## Uninstall

To remove the integration:

```bash
# Disable Apache site
sudo a2dissite cadbay.conf

# Remove Apache configuration
sudo rm /etc/apache2/sites-available/cadbay.conf

# Stop and disable monitor service
sudo systemctl stop cadbay-monitor
sudo systemctl disable cadbay-monitor

# Remove monitor installation
sudo rm /usr/local/bin/cadbay-monitor
sudo rm /etc/cadbay-monitor.conf
sudo rm /etc/systemd/system/cadbay-monitor.service

# Reload systemd
sudo systemctl daemon-reload

# Restart Apache
sudo systemctl restart apache2
```

## Support

For issues with:
- **Monitor functionality**: Check `monitor.conf` settings
- **Apache configuration**: Review `apache-cadbay.conf`
- **System logs**: Use `sudo journalctl -xe`
- **Connectivity**: Check firewall rules with `sudo ufw status`

---

Last Updated: 2026-04-28
