# CADBay mail server groundwork

This directory is the starting point for CADBay's mail service. The first
implementation should be an authenticated, outbound-only notification service;
it must not accept unauthenticated SMTP relay traffic.

## Initial boundaries

- Keep SMTP credentials and private keys outside the repository.
- Bind local development services to `127.0.0.1` by default.
- Require authentication and an explicit allow-list for outbound recipients.
- Queue messages before delivery so web requests are not blocked by SMTP.
- Record delivery status without storing message passwords or authentication
  tokens in logs.

## Planned components

1. A small internal API used by CADBay account and notification flows.
2. A queue backed by a local spool directory or database.
3. An SMTP delivery worker configured through environment variables.
4. Retry, timeout, and dead-letter handling.
5. Health and queue-status checks exposed only to the local monitor.

Before adding SMTP listeners or mailbox storage, decide whether CADBay needs
outbound notifications only, inbound mailboxes, or both. Inbound mailboxes
require separate spam, abuse, retention, and domain-verification controls.

Configuration is intentionally documented here before implementation so no
mail credentials are accidentally included in the public, designer, or
engineer repository projections.
