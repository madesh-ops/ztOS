# WhatsApp for ztOS

A Metro frontend over `whatsapp-native` (`native/`), a small Go program that
speaks WhatsApp's linked-device protocol itself through
[whatsmeow](https://github.com/tulir/whatsmeow). No browser: about 24 MB
where the earlier Chromium + whatsapp-web.js bridge took some 900 MB.
Runs in `eco` mode. No TCP server, no background service: closing the app
closes the client; the link stays on disk.

The app and the client talk over the client's stdin/stdout, one JSON object a
line (requests `chats`, `messages`, `send`; events `qr`, `status`, `ready`,
`history`, `message`, `error`; see `native/main.go`).

whatsmeow keeps keys and contacts; the chats and messages it does not keep go
into two tables of the same SQLite file
(`~/.local/share/ztos/whatsapp/whatsapp.db`): the history your phone sends
when linking, then messages as they arrive while the app is open.

## Building

```
os/apps/whatsapp/native/build.sh     # Go 1.25+; linux/arm64, no C compiler needed
sdk/bin/ztos-sdk install -C os/apps/whatsapp --device ztos@172.16.42.1
```

The app carries the client (`native/whatsapp-native`, not in git) and copies
it into its data folder on first start.

## Using it

Link by scanning the code shown from WhatsApp → Linked devices → Link a
device (a second phone, which stays the account's main device).

- **chats**: the 100 most recent; a chat shows its latest 50 messages as
  Messaging-style bubbles (hold one to copy it), and sends text. Photos,
  voice messages and documents are shown by name; calls as call notices.
- **status**: your contacts' updates from the last 24 hours, one row a
  person; open one to page through them (tap the right of the picture or
  "next"). Photos download when shown and are kept for the day
  (`status/` in the data folder); text updates show on their colour;
  videos show their preview only. Updates collect while the app is open
  (and those WhatsApp queued while it was closed).

No read receipts are sent, for messages or statuses. The list is made again
only when it changed and only while it is on screen (channel posts and
status updates arrive every few seconds).

This is an unofficial WhatsApp client.
