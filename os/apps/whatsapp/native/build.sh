#!/bin/sh
# Builds whatsapp-native for the phone (linux/arm64, no C: SQLite is modernc's
# pure Go). Needs Go 1.25 or newer. The app bundle carries the result
# (ztos.toml includes native/whatsapp-native).
set -e
cd "$(dirname "$0")"
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags "-s -w" -o whatsapp-native .
ls -l whatsapp-native
