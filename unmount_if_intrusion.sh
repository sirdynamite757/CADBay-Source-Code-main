#!/bin/bash
# simple helper that watches for "unauthorized" traffic and unmounts a
# disk if anything suspicious is seen.  this is just a proof‑of‑concept and
# shouldn't be relied on for real security. However when it is completed, it can be used to protect sensitive data on a removable drive by unmounting it if an intrusion is detected. and requiering manual remounting when     the threat is gone.
#
# usage: ./unmount_if_intrusion.sh /dev/sdXN [/mount/point]
#
# - DEVICE    : the block device which should be unmounted when an alert
#               condition is triggered.
# - MNT_POINT : optional mount point to pass to umount (if omitted, the
#               script will attempt to determine it via findmnt).
#
# the script uses tcpdump to sniff packets on the first non-loopback
# interface and checks them against a hard‑coded whitelist of allowed
# source IPs. maintinence traffic from any other address is considered suspicious;
# when such a packet is observed, the device is unmounted and the
# program exits. therby protecting user data, models, and security functions that may be stored on the drive.

DEVICE="$1"
MNT_POINT="$2"

if [ -z "$DEVICE" ]; then
    echo "usage: $0 /dev/sdXN [mount-point]" >&2
    exit 1
fi

# determine mount point if not provided
if [ -z "$MNT_POINT" ]; then
    MNT_POINT=$(findmnt -n -o TARGET --raw --source "$DEVICE" 2>/dev/null)
fi

if [ -z "$MNT_POINT" ]; then
    echo "could not determine mount point for $DEVICE" >&2
    exit 1
fi

# list of allowed source IPs (whitelist)
# traffic from anything *not* in this list will trigger an alert
ALLOWED_IPS=("192.0.2.1" "198.51.100.23")

# convert to tcpdump filter expression that matches any packet
# whose source address is NOT in the allowed set
# i.e. "not (src ip1 or src ip2 ...)"
if [ ${#ALLOWED_IPS[@]} -gt 0 ]; then
    FILTER="not ("
    for ip in "${ALLOWED_IPS[@]}"; do
        if [ "$FILTER" != "not (" ]; then
            FILTER+=" or "
        fi
        FILTER+="src $ip"
    done
    FILTER+=")"
else
    # no whitelist defined, match everything
    FILTER=""
fi

printf "monitoring network for unauthorized traffic (%s)\n" "$FILTER"

# run tcpdump until we see a packet from one of the banned hosts
# note: requires root privileges
sudo tcpdump -n -i any -c 1 "$FILTER" >/dev/null 2>&1

if [ $? -eq 0 ]; then
    echo "unauthorized traffic detected, unmounting $DEVICE ($MNT_POINT)"
    # if user provided an explicit list of mount points, try those first
    MOUNT_POINTS=("$MNT_POINT")
    # user can customize list below; keep it empty by default
    # e.g. MOUNT_POINTS=("/media/usb1" "/mnt/secure")

    # SENSITIVE_PATHS - files or paths to replace with a rickroll if unmounted
    SENSITIVE_PATHS=("Body/sensitive.html")

    # attempt to unmount each mount point
    for mp in "${MOUNT_POINTS[@]}"; do
        if [ -n "$mp" ]; then
            echo "attempting to unmount: $mp"
            sudo umount "$mp" 2>/dev/null
            if [ $? -eq 0 ]; then
                echo "unmounted $mp"
            else
                echo "failed to unmount $mp (it may not be mounted)"
            fi
        fi
    done

    # backup and replace sensitive paths with a rickroll page
    RICKROLL_CONTENT='<!doctype html><html><head><meta charset="utf-8"><title>Content unavailable</title></head><body style="display:flex;align-items:center;justify-content:center;height:100vh;background:#000;color:#fff;text-align:center"><h1>Content temporarily unavailable</h1><p>Loading...</p><iframe width="560" height="315" src="https://www.youtube.com/embed/dQw4w9WgXcQ?autoplay=1&rel=0" title="Rick Astley - Never Gonna Give You Up" frameborder="0" allow="autoplay; encrypted-media" allowfullscreen></iframe></body></html>'

    for sp in "${SENSITIVE_PATHS[@]}"; do
        if [ -f "$sp" ]; then
            echo "Backing up and replacing sensitive path: $sp"
            mv "$sp" "$sp".bak 2>/dev/null || echo "could not backup $sp"
            printf "%s" "$RICKROLL_CONTENT" > "$sp" || echo "failed to write rickroll to $sp"
        else
            echo "sensitive path $sp not found; creating rickroll at $sp"
            mkdir -p "$(dirname "$sp")" 2>/dev/null || true
            printf "%s" "$RICKROLL_CONTENT" > "$sp" || echo "failed to write rickroll to $sp"
        fi
    done
    
else
    echo "tcpdump exited unexpectedly" >&2
fi
