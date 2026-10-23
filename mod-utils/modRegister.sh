# open the db file and append the given username and password to it, then create a mod id and add it as well, require sudo for createion of high level mod accounts
# read from the file named "prevID.db" and read the last line, then extract the mod id from it and increment it by 1 to create a new mod id, the mod id should be 6 ints long

#!/bin/sh

username=$1
password=$2
account_level=$3

if [ "$account_level" -gt 1 ] && [ "$(id -u)" -ne 0 ]; then
    echo "You need to run this script as root to create a high-level mod account."
    exit 1
fi

PrevID=$(tail -n 1 prevID.db)

if [ -z "$PrevID" ]; then
    PrevID=0
fi

NewID=$((PrevID + 1))
NewID=$(printf "%06d" "$NewID")

echo "$NewID" >> prevID.db
echo "$username $password $NewID $account_level" >> modAuth.db
