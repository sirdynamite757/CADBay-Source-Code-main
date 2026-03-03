#!/usr/bin/env bash
# Start a TLS -> HTTP proxy using socat (preferred) or openssl s_server (fallback)
# Usage: ./start-tls-proxy.sh <listen-port> <backend-host> <backend-port>

LISTEN_PORT=${1:-443}
BACKEND_HOST=${2:-127.0.0.1}
BACKEND_PORT=${3:-3000}
CERT_DIR=${4:-certs}
CERT_FILE="$CERT_DIR/server.cert.pem"
KEY_FILE="$CERT_DIR/server.key.pem"

if command -v socat >/dev/null 2>&1; then
  echo "Using socat to proxy TLS:$LISTEN_PORT -> $BACKEND_HOST:$BACKEND_PORT"
  socat OPENSSL-LISTEN:$LISTEN_PORT,reuseaddr,cert=$CERT_FILE,key=$KEY_FILE,verify=0,fork TCP:$BACKEND_HOST:$BACKEND_PORT
else
  echo "socat not found; falling back to openssl s_server with simple proxy via /dev/tcp is not supported"
  echo "Install socat or use a proper TLS reverse proxy (nginx/caddy/stunnel)."
  exit 1
fi
