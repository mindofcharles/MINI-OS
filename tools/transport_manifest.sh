#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 ROOT EXCLUDED_PATH" >&2
    exit 2
fi

root=$1
excluded_path=$2

if [ ! -d "$root" ]; then
    echo "error: transport root '$root' is not a directory" >&2
    exit 1
fi

find "$root" \
    \( -path "$excluded_path" -o -name '.git' -o -name '.DS_Store' \
       -o -name 'Thumbs.db' -o -name '._*' \) -prune -o -print |
    LC_ALL=C sort |
    while IFS= read -r path; do
        if [ -L "$path" ]; then
            printf 'L %s\n' "$path"
        elif [ -d "$path" ]; then
            printf 'D %s\n' "$path"
        elif [ -f "$path" ]; then
            printf 'F '
            cksum "$path"
        else
            printf 'O %s\n' "$path"
        fi
    done
