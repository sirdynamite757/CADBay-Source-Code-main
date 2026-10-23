# CADBay-Source-Code

The Central repo for the cad bay source code. Security functions not available (for obvious reasons).

## Running standalone (no Apache required)

CADBay is self-contained. The C webserver serves static files (including
extensionless URLs such as `/loginRegister`) and all `/api/*` endpoints on
port 4000 by itself — no Apache, systemd service, or elevated/`polkit` setup:

```bash
./start-server.sh          # builds and runs on http://127.0.0.1:4000
```

Apache integration exists only as an optional legacy extra. If it was ever
installed on your machine, remove it with:

```bash
sudo ./enable-apache.sh
```

## Packaging, encryption & restricted files

See **[PACKAGING_AND_ENCRYPTION.md](PACKAGING_AND_ENCRYPTION.md)** for:

- the **restricted-file prefixes** (which filename prefix marks a file as
  `virus`, `malware`, `banned`, `copyright`, etc.) and what each means;
- the **page-access markers** — capitalised names are premium-only, `+` is
  developer/moderator-only, `.` is hidden;
- the **20-character encryption key** format used by the packager;
- how to **recover the server without the decryptor** using the generated
  standalone `cadbay-recover.c`;
- how the server reads `server.conf` / `monitor.conf` (including per-drive paths).

Build the packaging tools with:

```bash
make tools     # builds cadbay-packager and cadbay-deployer (GTK3 GUIs)
```

## Managing the separated repositories

Use `manage-cadbay-repos.sh` to preview or generate local projections for the
public, designer, and engineer repositories:

```bash
./manage-cadbay-repos.sh              # dry run
./manage-cadbay-repos.sh --sync       # write local projections
./manage-cadbay-repos.sh --sync --commit
```

The script never pushes to GitHub. Review each generated repository and push it
manually from GitHub or with your preferred Git workflow. Credentials,
databases, logs, private keys, and generated binaries are excluded.

## Temporary website sharing

The local `monitor-mod` program includes a **Temporary Share** tab for
development demonstrations. Install and authenticate the Microsoft Dev
Tunnels CLI, boot CADBay locally, then start the share from that tab:

```bash
devtunnel host --port 4000 --allow-anonymous
```

The monitor starts this command itself, displays the tunnel URL printed by the
CLI, and terminates the tunnel when you stop sharing or close the monitor.
Only the configured CADBay web port is forwarded; monitor controls are not
forwarded. Anonymous mode is intended only for short-lived testing with
non-sensitive development data.

## Mail service groundwork

The initial mail-service boundaries and planned components are documented in
[`mail-server/README.md`](mail-server/README.md). The service is intentionally
not exposed or started yet; SMTP credentials, keys, queues, and mailbox data
must remain local runtime data.
