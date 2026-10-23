# Raw-IP tunnel

The optional `cadbay-tunnel` program is a small TCP forwarder. It does not
provide a public IP by itself; the host, router, firewall, or cloud VM must
already be reachable on the bind port.

Build it with:

```sh
make tunnel
```

Run it directly:

```sh
./cadbay-tunnel --listen 0.0.0.0:8080 127.0.0.1:4000
```

Or enable it through the normal launcher:

```sh
TUNNEL_ENABLED=1 TUNNEL_BIND=0.0.0.0:8080 TUNNEL_TARGET=127.0.0.1:4000 ./start-server.sh
```

Clients then use `http://SERVER_RAW_IP:8080`. Open or forward the selected
port in the host firewall and router as needed. Use a TLS reverse proxy for
HTTPS; this program forwards TCP and does not terminate TLS.