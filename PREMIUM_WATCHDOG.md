# Premium expiry watchdog

The optional `premium_watchdog` scans all `accdta/*.txt` account files. On the
calendar day five days before an expiry it calls an email/text notification
stub and records the warning in `.premium_watchdog_state/`. On or after 8:00 AM
on the expiry date, it changes the account's `premium` field to JSON `false`.

Build and run it directly:

```sh
make watchdog
./premium_watchdog 60
```

Start it with the server launcher:

```sh
PREMIUM_WATCHDOG_ENABLED=1 ./start-server.sh
```

The notification function currently logs a message. Replace
`send_expiry_warning()` in `premium_watchdog.c` with the email/SMS provider
integration later. Times use the server's local timezone.