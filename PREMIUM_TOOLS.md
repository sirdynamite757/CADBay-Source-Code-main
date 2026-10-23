# Premium account tools

These tools update the premium field in an existing `accdta/<account-id>.txt`
file. They do not process payments.

Build both tools:

```sh
make premium
```

Activate an account through a supplied expiry date:

```sh
./add_premium_member ACCOUNT_ID_OR_USERNAME MM-DD-YYYY
```

Renew an account by a number of days. If the account is still active, days are
added to its existing expiry; otherwise they are added from today:

```sh
./renew_premium ACCOUNT_ID_OR_USERNAME DAYS
```