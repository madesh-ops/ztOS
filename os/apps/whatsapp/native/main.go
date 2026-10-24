// whatsapp-native: WhatsApp's linked-device protocol (whatsmeow) behind the
// same private stdin/stdout JSON lines as bridge.js was, without a browser:
// one process of a few tens of MB where Chromium running WhatsApp Web took
// some 900 MB.
//
// Requests (one JSON object a line):  {"id":1,"method":"chats"}
//
//	{"id":2,"method":"messages","chat":"..."}
//	{"id":3,"method":"send","chat":"...","text":"..."}
//
// Answers: {"id":1,"result":...} or {"id":1,"error":"..."}
// Events:  {"event":"qr","image":"data:image/png;base64,..."}, "status",
//
//	"ready", "error", "history" (chats changed), "message".
//
// whatsmeow keeps no chats or messages, only keys and contacts: the chats
// and their messages (from the phone's history sync at linking, then as
// they arrive) are kept in two tables of the same SQLite file.
package main

import (
	"bufio"
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"runtime/debug"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/skip2/go-qrcode"
	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/store/sqlstore"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	waLog "go.mau.fi/whatsmeow/util/log"
	"google.golang.org/protobuf/proto"
	_ "modernc.org/sqlite"
)

var (
	ctx      = context.Background()
	cli      *whatsmeow.Client
	db       *sql.DB
	outMu    sync.Mutex
	readyMu  sync.Mutex
	ready    bool
	dataRoot string // ZTOS_WHATSAPP_DATA
)

// ---- talking to the app ---------------------------------------------------------------

func emit(v any) {
	b, _ := json.Marshal(v)
	outMu.Lock()
	os.Stdout.Write(append(b, '\n'))
	outMu.Unlock()
}

func status(text string) { emit(map[string]any{"event": "status", "text": text}) }
func fail(text string)   { emit(map[string]any{"event": "error", "error": text}) }

func setReady(on bool) {
	readyMu.Lock()
	ready = on
	readyMu.Unlock()
}

func isReady() bool {
	readyMu.Lock()
	defer readyMu.Unlock()
	return ready
}

// whatsmeow's own log: warnings and errors to stderr (the app's log), never stdout
type stderrLog struct{ module string }

func (l stderrLog) Warnf(msg string, args ...any)  { l.out("WARN", msg, args...) }
func (l stderrLog) Errorf(msg string, args ...any) { l.out("ERROR", msg, args...) }
func (l stderrLog) Infof(string, ...any)           {}
func (l stderrLog) Debugf(string, ...any)          {}
func (l stderrLog) Sub(module string) waLog.Logger { return stderrLog{l.module + "/" + module} }
func (l stderrLog) out(level, msg string, args ...any) {
	fmt.Fprintf(os.Stderr, "whatsapp %s [%s] %s\n", level, l.module, fmt.Sprintf(msg, args...))
}

// ---- what a message says --------------------------------------------------------------

func join(label, caption string) string {
	if caption == "" {
		return label
	}
	return label + " · " + caption
}

// text: what a message shows in the list; "" for what is not shown (reactions,
// receipts and other protocol messages)
func text(m *waE2E.Message) string {
	if m == nil {
		return ""
	}
	switch {
	case m.GetConversation() != "":
		return m.GetConversation()
	case m.GetExtendedTextMessage() != nil:
		return m.GetExtendedTextMessage().GetText()
	case m.GetImageMessage() != nil:
		return join("📷 Photo", m.GetImageMessage().GetCaption())
	case m.GetVideoMessage() != nil:
		return join("🎥 Video", m.GetVideoMessage().GetCaption())
	case m.GetAudioMessage() != nil:
		if m.GetAudioMessage().GetPTT() {
			return "🎤 Voice message"
		}
		return "🎵 Audio"
	case m.GetDocumentMessage() != nil:
		d := m.GetDocumentMessage()
		return join("📄 "+firstOf(d.GetFileName(), "Document"), d.GetCaption())
	case m.GetDocumentWithCaptionMessage() != nil:
		return text(m.GetDocumentWithCaptionMessage().GetMessage())
	case m.GetStickerMessage() != nil:
		return "Sticker"
	case m.GetLocationMessage() != nil, m.GetLiveLocationMessage() != nil:
		return "📍 Location"
	case m.GetContactMessage() != nil:
		return "👤 " + firstOf(m.GetContactMessage().GetDisplayName(), "Contact")
	case m.GetContactsArrayMessage() != nil:
		return "👤 Contacts"
	case m.GetPollCreationMessage() != nil:
		return "📊 " + m.GetPollCreationMessage().GetName()
	case m.GetPollCreationMessageV3() != nil:
		return "📊 " + m.GetPollCreationMessageV3().GetName()
	case m.GetCallLogMesssage() != nil:
		return callText(m.GetCallLogMesssage().GetIsVideo(), m.GetCallLogMesssage().GetCallOutcome() == waE2E.CallLogMessage_CONNECTED)
	}
	return ""
}

func callText(video, answered bool) string {
	switch {
	case video && answered:
		return "📹 Video call"
	case video:
		return "📹 Missed video call"
	case answered:
		return "📞 Voice call"
	}
	return "📞 Missed voice call"
}

// stubText: what history sync gives as a bare notice rather than a message
// (calls, mostly)
func stubText(w *waWeb.WebMessageInfo) string {
	switch w.GetMessageStubType() {
	case waWeb.WebMessageInfo_CALL_MISSED_VOICE, waWeb.WebMessageInfo_CALL_MISSED_GROUP_VOICE:
		return callText(false, false)
	case waWeb.WebMessageInfo_CALL_MISSED_VIDEO, waWeb.WebMessageInfo_CALL_MISSED_GROUP_VIDEO:
		return callText(true, false)
	}
	return ""
}

func firstOf(s ...string) string {
	for _, x := range s {
		if x != "" {
			return x
		}
	}
	return ""
}

// ---- chats: one key for a chat whatever address it comes under -----------------------

// chatKey: people are addressed by phone number or by LID (WhatsApp is moving
// to LIDs); a chat is kept under its LID when one is known, so the two meet.
func chatKey(j types.JID) types.JID {
	j = j.ToNonAD()
	if j.Server == types.DefaultUserServer {
		if lid, err := cli.Store.LIDs.GetLIDForPN(ctx, j); err == nil && !lid.IsEmpty() {
			return lid
		}
	}
	return j
}

func nameOf(j types.JID, stored string) string {
	if stored != "" && !strings.Contains(stored, "∙") {
		return stored
	}
	look := []types.JID{j}
	if j.Server == types.HiddenUserServer {
		if pn, err := cli.Store.LIDs.GetPNForLID(ctx, j); err == nil && !pn.IsEmpty() {
			look = append(look, pn)
		}
	}
	for _, k := range look {
		if c, err := cli.Store.Contacts.GetContact(ctx, k); err == nil && c.Found {
			if n := firstOf(c.FullName, c.FirstName, c.BusinessName, c.PushName); n != "" {
				return n
			}
		}
	}
	for _, k := range look {
		if k.Server == types.DefaultUserServer {
			return "+" + k.User
		}
	}
	return firstOf(stored, j.User)
}

// mergeKeys: chats kept under a phone number before WhatsApp told us their
// LID move under the LID, with their messages (one chat, not two)
func mergeKeys() {
	rows, err := db.Query(`SELECT id FROM zt_chat WHERE id LIKE '%@s.whatsapp.net'`)
	if err != nil {
		return
	}
	var ids []string
	for rows.Next() {
		var id string
		rows.Scan(&id)
		ids = append(ids, id)
	}
	rows.Close()
	for _, id := range ids {
		pn, err := types.ParseJID(id)
		if err != nil {
			continue
		}
		lid := chatKey(pn)
		if lid.Server != types.HiddenUserServer {
			continue
		}
		to := lid.String()
		db.Exec(`UPDATE OR IGNORE zt_msg SET chat = ? WHERE chat = ?`, to, id)
		db.Exec(`DELETE FROM zt_msg WHERE chat = ?`, id)
		db.Exec(`INSERT INTO zt_chat (id, name, t, unread, archived, readonly, last)
			SELECT ?, name, t, unread, archived, readonly, last FROM zt_chat WHERE id = ?
			ON CONFLICT (id) DO UPDATE SET name = CASE WHEN name = '' THEN excluded.name ELSE name END,
				last = CASE WHEN excluded.t > t THEN excluded.last ELSE last END,
				t = max(t, excluded.t), unread = unread + excluded.unread`, to, id)
		db.Exec(`DELETE FROM zt_chat WHERE id = ?`, id)
	}
}

const schema = `
CREATE TABLE IF NOT EXISTS zt_chat (id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', t INTEGER NOT NULL DEFAULT 0,
	unread INTEGER NOT NULL DEFAULT 0, archived INTEGER NOT NULL DEFAULT 0, readonly INTEGER NOT NULL DEFAULT 0,
	last TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS zt_msg (chat TEXT NOT NULL, id TEXT NOT NULL, from_me INTEGER NOT NULL, t INTEGER NOT NULL,
	body TEXT NOT NULL, author TEXT NOT NULL DEFAULT '', PRIMARY KEY (chat, id));
CREATE INDEX IF NOT EXISTS zt_msg_t ON zt_msg (chat, t);
CREATE TABLE IF NOT EXISTS zt_status (id TEXT PRIMARY KEY, sender TEXT NOT NULL, name TEXT NOT NULL DEFAULT '',
	from_me INTEGER NOT NULL DEFAULT 0, t INTEGER NOT NULL, kind TEXT NOT NULL, text TEXT NOT NULL DEFAULT '',
	bg INTEGER NOT NULL DEFAULT 0, thumb BLOB, media BLOB);
CREATE INDEX IF NOT EXISTS zt_status_sender ON zt_status (sender, t);`

// ---- status: what contacts posted, for a day ------------------------------------------

const statusLife = 24 * 60 * 60

// keepStatus: one status update (status@broadcast). A photo's message is
// kept for its download; its small preview for the list.
func keepStatus(evt *events.Message) {
	info, m := evt.Info, evt.Message
	if pm := m.GetProtocolMessage(); pm != nil {
		if pm.GetType() == waE2E.ProtocolMessage_REVOKE {
			db.Exec(`DELETE FROM zt_status WHERE id = ?`, pm.GetKey().GetID())
		}
		return
	}
	t := info.Timestamp.Unix()
	if time.Now().Unix()-t > statusLife {
		return
	}
	var kind, body string
	var bg uint32
	var thumb []byte
	switch {
	case m.GetImageMessage() != nil:
		kind, body, thumb = "image", m.GetImageMessage().GetCaption(), m.GetImageMessage().GetJPEGThumbnail()
	case m.GetVideoMessage() != nil:
		kind, body, thumb = "video", m.GetVideoMessage().GetCaption(), m.GetVideoMessage().GetJPEGThumbnail()
	case m.GetExtendedTextMessage() != nil:
		kind, body, bg = "text", m.GetExtendedTextMessage().GetText(), m.GetExtendedTextMessage().GetBackgroundArgb()
	default:
		if body = text(m); body == "" {
			return
		}
		kind = "text"
	}
	sender := chatKey(info.Sender)
	name := "My status"
	if !info.IsFromMe {
		name = nameOf(sender, "")
		if strings.Contains(name, "∙") || strings.HasPrefix(name, "+") {
			name = firstOf(info.PushName, name)
		}
	}
	var media []byte
	if kind == "image" {
		media, _ = proto.Marshal(m)
	}
	_, err := db.Exec(`INSERT OR IGNORE INTO zt_status (id, sender, name, from_me, t, kind, text, bg, thumb, media)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, info.ID, sender.String(), name, info.IsFromMe, t, kind, body, bg, thumb, media)
	if err != nil {
		fmt.Fprintln(os.Stderr, "whatsapp: status:", err)
	}
}

// pruneStatus: a day old, gone (and the photos kept for them)
func pruneStatus(root string) {
	cut := time.Now().Unix() - statusLife
	db.Exec(`DELETE FROM zt_status WHERE t < ?`, cut)
	dir := filepath.Join(root, "status")
	entries, _ := os.ReadDir(dir)
	for _, e := range entries {
		if info, err := e.Info(); err == nil && info.ModTime().Unix() < cut {
			os.Remove(filepath.Join(dir, e.Name()))
		}
	}
}

// statusPhoto: a status photo downloaded (once) into root/status/ID.jpg
func statusPhoto(root, id string) (string, error) {
	safe := strings.Map(func(r rune) rune {
		if r == '/' || r == '.' {
			return '_'
		}
		return r
	}, id)
	path := filepath.Join(root, "status", safe+".jpg")
	if _, err := os.Stat(path); err == nil {
		return path, nil
	}
	var raw []byte
	if err := db.QueryRow(`SELECT media FROM zt_status WHERE id = ?`, id).Scan(&raw); err != nil || len(raw) == 0 {
		return "", errors.New("This status has no photo to show")
	}
	var m waE2E.Message
	if err := proto.Unmarshal(raw, &m); err != nil || m.GetImageMessage() == nil {
		return "", errors.New("This status has no photo to show")
	}
	dctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	data, err := cli.Download(dctx, m.GetImageMessage())
	if err != nil {
		return "", fmt.Errorf("Could not download the photo: %w", err)
	}
	os.MkdirAll(filepath.Dir(path), 0o700)
	if err := os.WriteFile(path+".part", data, 0o600); err != nil {
		return "", err
	}
	return path, os.Rename(path+".part", path)
}

func saveChat(id, name string, t int64, unread int, archived, readonly bool) {
	_, err := db.Exec(`INSERT INTO zt_chat (id, name, t, unread, archived, readonly) VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT (id) DO UPDATE SET name = CASE WHEN excluded.name != '' THEN excluded.name ELSE name END,
			t = max(t, excluded.t), unread = excluded.unread, archived = excluded.archived, readonly = excluded.readonly`,
		id, name, t, unread, archived, readonly)
	if err != nil {
		fmt.Fprintln(os.Stderr, "whatsapp: chat:", err)
	}
}

type msgOut struct {
	ID        string  `json:"id"`
	Body      string  `json:"body"`
	FromMe    bool    `json:"fromMe"`
	Timestamp int64   `json:"timestamp"`
	Author    *string `json:"author"`
	Kind      string  `json:"kind,omitempty"` // "image", "sticker"; text otherwise
	Caption   string  `json:"caption,omitempty"`
	W         int     `json:"w,omitempty"` // the picture's size, for its place before it's here
	H         int     `json:"h,omitempty"`
	Thumb     string  `json:"thumb,omitempty"` // its small preview (base64 JPEG or PNG)
}

// pictured: what a photo or sticker message brings for showing it: its kind,
// caption, size, preview, and the message itself (for the download)
func pictured(m *waE2E.Message) (kind, caption string, w, h int, thumb, media []byte) {
	switch {
	case m.GetImageMessage() != nil:
		i := m.GetImageMessage()
		kind, caption, w, h, thumb = "image", i.GetCaption(), int(i.GetWidth()), int(i.GetHeight()), i.GetJPEGThumbnail()
	case m.GetStickerMessage() != nil:
		s := m.GetStickerMessage()
		kind, w, h, thumb = "sticker", int(s.GetWidth()), int(s.GetHeight()), s.GetPngThumbnail()
	default:
		return
	}
	media, _ = proto.Marshal(m)
	return
}

// mediaFile: a chat's photo or sticker, downloaded once into root/media/
func mediaFile(root, chat, id string) (string, error) {
	var kind string
	var raw []byte
	if err := db.QueryRow(`SELECT kind, media FROM zt_msg WHERE chat = ? AND id = ?`, chat, id).Scan(&kind, &raw); err != nil || len(raw) == 0 {
		return "", errors.New("This picture can't be downloaded any more")
	}
	ext := ".jpg"
	if kind == "sticker" {
		ext = ".webp"
	}
	safe := strings.NewReplacer("/", "_", ".", "_", "@", "_").Replace(chat + "_" + id)
	path := filepath.Join(root, "media", safe+ext)
	if _, err := os.Stat(path); err == nil {
		return path, nil
	}
	var m waE2E.Message
	if err := proto.Unmarshal(raw, &m); err != nil {
		return "", err
	}
	var what whatsmeow.DownloadableMessage
	if m.GetImageMessage() != nil {
		what = m.GetImageMessage()
	} else if m.GetStickerMessage() != nil {
		what = m.GetStickerMessage()
	} else {
		return "", errors.New("This message has no picture")
	}
	dctx, cancel := context.WithTimeout(ctx, 90*time.Second)
	defer cancel()
	data, err := cli.Download(dctx, what)
	if err != nil {
		return "", fmt.Errorf("Could not download it: %w", err)
	}
	os.MkdirAll(filepath.Dir(path), 0o700)
	if err := os.WriteFile(path+".part", data, 0o600); err != nil {
		return "", err
	}
	return path, os.Rename(path+".part", path)
}

// pruneMedia: downloads not opened for a month go (they can be fetched again)
func pruneMedia(root string) {
	dir := filepath.Join(root, "media")
	entries, _ := os.ReadDir(dir)
	cut := time.Now().Add(-30 * 24 * time.Hour)
	for _, e := range entries {
		if info, err := e.Info(); err == nil && info.ModTime().Before(cut) {
			os.Remove(filepath.Join(dir, e.Name()))
		}
	}
}

// remove: a message deleted for everyone (yours: WhatsApp takes it back from
// every phone in the chat) or just from this phone
func remove(chat, id string, everyone bool) error {
	if everyone {
		var fromMe bool
		var t int64
		if err := db.QueryRow(`SELECT from_me, t FROM zt_msg WHERE chat = ? AND id = ?`, chat, id).Scan(&fromMe, &t); err != nil {
			return errors.New("That message is not on this phone")
		}
		if !fromMe {
			return errors.New("Only your own messages can be deleted for everyone")
		}
		if time.Now().Unix()-t > 2*24*3600 {
			return errors.New("WhatsApp only takes back messages from the last two days")
		}
		to, err := types.ParseJID(chat)
		if err != nil {
			return err
		}
		sctx, cancel := context.WithTimeout(ctx, 30*time.Second)
		defer cancel()
		if _, err := cli.SendMessage(sctx, to, cli.BuildRevoke(to, types.EmptyJID, id)); err != nil {
			return err
		}
		_, err = db.Exec(`UPDATE zt_msg SET body = 'You deleted this message', kind = '', media = NULL, thumb = NULL
			WHERE chat = ? AND id = ?`, chat, id)
		return err
	}
	_, err := db.Exec(`DELETE FROM zt_msg WHERE chat = ? AND id = ?`, chat, id)
	return err
}

// keep: a message into its chat; incoming ones count as unread
func keep(evt *events.Message, live bool) *msgOut {
	info := evt.Info
	m := evt.Message
	if info.Chat == types.StatusBroadcastJID {
		keepStatus(evt)
		return nil
	}
	chat := chatKey(info.Chat).String()
	if pm := m.GetProtocolMessage(); pm != nil {
		switch pm.GetType() {
		case waE2E.ProtocolMessage_REVOKE:
			db.Exec(`UPDATE zt_msg SET body = 'This message was deleted', kind = '', media = NULL, thumb = NULL
				WHERE chat = ? AND id = ?`, chat, pm.GetKey().GetID())
		case waE2E.ProtocolMessage_MESSAGE_EDIT:
			if body := text(pm.GetEditedMessage()); body != "" {
				db.Exec(`UPDATE zt_msg SET body = ? WHERE chat = ? AND id = ?`, body, chat, pm.GetKey().GetID())
			}
		}
		return nil
	}
	if evt.IsEdit {
		return nil
	}
	body := text(m)
	if body == "" && evt.SourceWebMsg != nil {
		body = stubText(evt.SourceWebMsg)
	}
	if body == "" {
		return nil
	}
	author := ""
	if info.IsGroup && !info.IsFromMe {
		author = firstOf(info.PushName, nameOf(chatKey(info.Sender), ""))
	}
	t := info.Timestamp.Unix()
	kind, caption, w, h, thumb, media := pictured(m)
	res, err := db.Exec(`INSERT OR IGNORE INTO zt_msg (chat, id, from_me, t, body, author, kind, caption, w, h, thumb, media)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		chat, info.ID, info.IsFromMe, t, body, author, kind, caption, w, h, thumb, media)
	if err != nil {
		fmt.Fprintln(os.Stderr, "whatsapp: message:", err)
		return nil
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return nil // (seen already)
	}
	unread := 0
	if live && !info.IsFromMe {
		unread = 1
	}
	db.Exec(`INSERT INTO zt_chat (id, t, unread, last) VALUES (?, ?, ?, ?)
		ON CONFLICT (id) DO UPDATE SET unread = unread + excluded.unread,
			last = CASE WHEN excluded.t >= t THEN excluded.last ELSE last END, t = max(t, excluded.t)`,
		chat, t, unread, body)
	out := &msgOut{ID: info.ID, Body: body, FromMe: info.IsFromMe, Timestamp: t,
		Kind: kind, Caption: caption, W: w, H: h, Thumb: base64.StdEncoding.EncodeToString(thumb)}
	if author != "" {
		out.Author = &author
	}
	return out
}

// ---- WhatsApp's news ------------------------------------------------------------------

// listed: a chat the app's list shows (channels' posts and broadcasts would
// only make it redraw for nothing)
func listed(j types.JID) bool {
	return j.Server != types.NewsletterServer && j.Server != types.BroadcastServer
}

func onEvent(raw any) {
	switch evt := raw.(type) {
	case *events.Message:
		if msg := keep(evt, true); msg != nil && listed(evt.Info.Chat) {
			emit(map[string]any{"event": "message", "chat": chatKey(evt.Info.Chat).String(), "message": msg})
		}
	case *events.HistorySync:
		history(evt)
	case *events.PairSuccess:
		status("Linked. Loading chats…")
	case *events.Connected:
		mergeKeys()
		pruneStatus(dataRoot)
		pruneMedia(dataRoot)
		setReady(true)
		emit(map[string]any{"event": "ready"})
	case *events.Disconnected:
		setReady(false)
	case *events.LoggedOut:
		setReady(false)
		cli.Store.Delete(ctx) // (the next start shows a new code)
		db.Exec(`DELETE FROM zt_msg; DELETE FROM zt_chat`)
		fail("Logged out from your phone. Reopen WhatsApp to link it again.")
	case *events.StreamReplaced:
		setReady(false)
		fail("WhatsApp was opened on this account elsewhere. Reopen to reconnect.")
	case *events.TemporaryBan:
		fail("WhatsApp has temporarily banned this account: " + evt.String())
	}
}

// history: the phone's past chats, sent in blobs after linking
func history(evt *events.HistorySync) {
	data := evt.Data
	var mappings []store.LIDMapping
	for _, c := range data.GetConversations() {
		if c.GetLidJID() != "" && c.GetPnJID() != "" {
			lid, e1 := types.ParseJID(c.GetLidJID())
			pn, e2 := types.ParseJID(c.GetPnJID())
			if e1 == nil && e2 == nil {
				mappings = append(mappings, store.LIDMapping{LID: lid, PN: pn})
			}
		}
	}
	if len(mappings) > 0 {
		cli.Store.LIDs.PutManyLIDMappings(ctx, mappings)
	}
	for _, c := range data.GetConversations() {
		jid, err := types.ParseJID(c.GetID())
		if err != nil {
			continue
		}
		if jid == types.StatusBroadcastJID {
			for _, hm := range c.GetMessages() {
				if parsed, err := cli.ParseWebMessage(jid, hm.GetMessage()); err == nil {
					keepStatus(parsed)
				}
			}
			continue
		}
		if jid.Server == types.BroadcastServer {
			continue
		}
		key := chatKey(jid).String()
		saveChat(key, firstOf(c.GetName(), c.GetDisplayName()), int64(c.GetConversationTimestamp()),
			int(c.GetUnreadCount()), c.GetArchived(), c.GetReadOnly())
		for _, hm := range c.GetMessages() {
			if parsed, err := cli.ParseWebMessage(jid, hm.GetMessage()); err == nil {
				keep(parsed, false)
			}
		}
	}
	mergeKeys()
	emit(map[string]any{"event": "history"})
}

// ---- what the app asks -----------------------------------------------------------------

type request struct {
	ID       int64  `json:"id"`
	Method   string `json:"method"`
	Chat     string `json:"chat"`
	Text     string `json:"text"`
	Msg      string `json:"msg"`      // a message's id (media, delete)
	Everyone bool   `json:"everyone"` // delete: for everyone, not just here
}

func handle(req request) (any, error) {
	if !isReady() {
		return nil, errors.New("WhatsApp is still connecting")
	}
	switch req.Method {
	case "chats":
		rows, err := db.Query(`SELECT id, name, unread, last, t FROM zt_chat
			WHERE archived = 0 AND readonly = 0 AND id NOT LIKE '%@newsletter' AND id NOT LIKE '%@broadcast'
			ORDER BY t DESC LIMIT 100`)
		if err != nil {
			return nil, err
		}
		defer rows.Close()
		type chatOut struct {
			ID     string `json:"id"`
			Name   string `json:"name"`
			Unread int    `json:"unread"`
			Last   string `json:"last"`
			T      int64  `json:"t"`
		}
		out := []chatOut{}
		var stored []string
		for rows.Next() {
			var c chatOut
			var name string
			rows.Scan(&c.ID, &name, &c.Unread, &c.Last, &c.T)
			out = append(out, c)
			stored = append(stored, name)
		}
		rows.Close() // (before the names: they read the database too)
		var unnamed []types.JID
		for i := range out {
			if j, err := types.ParseJID(out[i].ID); err == nil {
				out[i].Name = nameOf(j, stored[i])
				if stored[i] == "" && j.Server == types.GroupServer {
					out[i].Name = "Group"
					unnamed = append(unnamed, j)
				}
			}
		}
		if len(unnamed) > 0 {
			go nameGroups(unnamed)
		}
		return out, nil
	case "messages":
		db.Exec(`UPDATE zt_chat SET unread = 0 WHERE id = ?`, req.Chat) // (here; no read receipt is sent)
		rows, err := db.Query(`SELECT id, body, from_me, t, author, kind, caption, w, h, thumb FROM
			(SELECT * FROM zt_msg WHERE chat = ? ORDER BY t DESC LIMIT 50) ORDER BY t`, req.Chat)
		if err != nil {
			return nil, err
		}
		defer rows.Close()
		out := []msgOut{}
		for rows.Next() {
			var m msgOut
			var author string
			var thumb []byte
			rows.Scan(&m.ID, &m.Body, &m.FromMe, &m.Timestamp, &author, &m.Kind, &m.Caption, &m.W, &m.H, &thumb)
			m.Thumb = base64.StdEncoding.EncodeToString(thumb)
			if author != "" {
				a := author
				m.Author = &a
			}
			out = append(out, m)
		}
		rows.Close()
		return out, nil
	case "statuses":
		// one row a person, the newest update first (yours on top)
		rows, err := db.Query(`SELECT sender, name, from_me, count(*), max(t),
				(SELECT thumb FROM zt_status s2 WHERE s2.sender = s.sender ORDER BY t DESC LIMIT 1)
			FROM zt_status s WHERE t >= ? GROUP BY sender ORDER BY from_me DESC, max(t) DESC`,
			time.Now().Unix()-statusLife)
		if err != nil {
			return nil, err
		}
		defer rows.Close()
		type person struct {
			Sender string `json:"sender"`
			Name   string `json:"name"`
			Mine   bool   `json:"mine"`
			Count  int    `json:"count"`
			T      int64  `json:"t"`
			Thumb  string `json:"thumb"`
		}
		out := []person{}
		for rows.Next() {
			var p person
			var thumb []byte
			rows.Scan(&p.Sender, &p.Name, &p.Mine, &p.Count, &p.T, &thumb)
			p.Thumb = base64.StdEncoding.EncodeToString(thumb)
			out = append(out, p)
		}
		return out, nil
	case "status_list":
		rows, err := db.Query(`SELECT id, t, kind, text, bg, thumb FROM zt_status WHERE sender = ? AND t >= ? ORDER BY t`,
			req.Chat, time.Now().Unix()-statusLife)
		if err != nil {
			return nil, err
		}
		defer rows.Close()
		type update struct {
			ID    string `json:"id"`
			T     int64  `json:"t"`
			Kind  string `json:"kind"`
			Text  string `json:"text"`
			Bg    uint32 `json:"bg"`
			Thumb string `json:"thumb"`
		}
		out := []update{}
		for rows.Next() {
			var u update
			var thumb []byte
			rows.Scan(&u.ID, &u.T, &u.Kind, &u.Text, &u.Bg, &thumb)
			u.Thumb = base64.StdEncoding.EncodeToString(thumb)
			out = append(out, u)
		}
		return out, nil
	case "media":
		return mediaFile(dataRoot, req.Chat, req.Msg) // (a path: the app reads the file)
	case "delete":
		if err := remove(req.Chat, req.Msg, req.Everyone); err != nil {
			return nil, err
		}
		return true, nil
	case "status_photo":
		return statusPhoto(dataRoot, req.Chat) // (a path: the app reads the file)
	case "send":
		body := strings.TrimSpace(req.Text)
		if body == "" || len(req.Text) > 10000 {
			return nil, errors.New("Enter a message of up to 10,000 characters")
		}
		to, err := types.ParseJID(req.Chat)
		if err != nil {
			return nil, err
		}
		m := &waE2E.Message{Conversation: proto.String(req.Text)}
		sctx, cancel := context.WithTimeout(ctx, 45*time.Second)
		defer cancel()
		resp, err := cli.SendMessage(sctx, to, m)
		if err != nil {
			return nil, err
		}
		evt := &events.Message{Info: types.MessageInfo{
			MessageSource: types.MessageSource{Chat: to, Sender: *cli.Store.ID, IsFromMe: true, IsGroup: to.Server == types.GroupServer},
			ID:            resp.ID, Timestamp: resp.Timestamp}, Message: m}
		if out := keep(evt, false); out != nil {
			return out, nil
		}
		return msgOut{ID: resp.ID, Body: req.Text, FromMe: true, Timestamp: resp.Timestamp.Unix()}, nil
	}
	return nil, errors.New("Unknown operation")
}

var naming sync.Mutex

// nameGroups: their names asked of WhatsApp in the background (once: then
// kept), the app told when they are in
func nameGroups(groups []types.JID) {
	if !naming.TryLock() {
		return // (already at it)
	}
	defer naming.Unlock()
	named := 0
	for _, j := range groups {
		gctx, cancel := context.WithTimeout(ctx, 10*time.Second)
		info, err := cli.GetGroupInfo(gctx, j)
		cancel()
		if err == nil && info.Name != "" {
			db.Exec(`UPDATE zt_chat SET name = ? WHERE id = ?`, info.Name, j.String())
			named++
		}
	}
	if named > 0 {
		emit(map[string]any{"event": "history"})
	}
}

// ---- start ------------------------------------------------------------------------------

func eco() {
	// ztOS's power manager runs this process in its frugal class; elsewhere it isn't there
	if f, err := os.OpenFile("/run/zrnperfd.ctl", os.O_WRONLY|syscall.O_NONBLOCK, 0); err == nil {
		fmt.Fprintf(f, "policy %d eco\n", os.Getpid())
		f.Close()
	}
}

func qr(code string) {
	png, err := qrcode.Encode(code, qrcode.Medium, 280)
	if err != nil {
		fail(err.Error())
		return
	}
	emit(map[string]any{"event": "qr", "image": "data:image/png;base64," + base64.StdEncoding.EncodeToString(png)})
}

func main() {
	// a small heap, collected often: this waits on the network almost always
	debug.SetGCPercent(50)
	debug.SetMemoryLimit(40 << 20)
	eco()
	root := os.Getenv("ZTOS_WHATSAPP_DATA")
	if root == "" {
		fail("ZTOS_WHATSAPP_DATA is not set")
		os.Exit(2)
	}
	os.MkdirAll(root, 0o700)
	dataRoot = root
	syscall.Umask(0o077)

	address := "file:" + filepath.Join(root, "whatsapp.db") +
		"?_pragma=foreign_keys(1)&_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=cache_size(-1024)"
	log := stderrLog{"wa"}
	var err error
	db, err = sql.Open("sqlite", address)
	if err != nil {
		fail("Could not open WhatsApp's data: " + err.Error())
		os.Exit(1)
	}
	db.SetMaxOpenConns(2) // (WAL: a reader beside the writer; each costs its page cache)
	container := sqlstore.NewWithDB(db, "sqlite", log.Sub("db"))
	if err := container.Upgrade(ctx); err != nil {
		fail("Could not open WhatsApp's data: " + err.Error())
		os.Exit(1)
	}
	if _, err := db.Exec(schema); err != nil {
		fail("Could not open WhatsApp's data: " + err.Error())
		os.Exit(1)
	}
	// statuses an earlier build kept as messages of a chat "status@broadcast"
	db.Exec(`INSERT OR IGNORE INTO zt_status (id, sender, name, t, kind, text)
		SELECT id, 'name:' || author, author, t, 'text', body FROM zt_msg WHERE chat = 'status@broadcast'`)
	db.Exec(`DELETE FROM zt_msg WHERE chat = 'status@broadcast'`)
	db.Exec(`DELETE FROM zt_chat WHERE id = 'status@broadcast'`)
	// columns added since the table was first made (an existing one keeps them)
	for _, column := range []string{"kind TEXT NOT NULL DEFAULT ''", "caption TEXT NOT NULL DEFAULT ''",
		"w INTEGER NOT NULL DEFAULT 0", "h INTEGER NOT NULL DEFAULT 0", "thumb BLOB", "media BLOB"} {
		db.Exec(`ALTER TABLE zt_msg ADD COLUMN ` + column) // (fails harmlessly when it is there)
	}
	store.SetOSInfo("ztOS", [3]uint32{0, 1, 0})
	device, err := container.GetFirstDevice(ctx)
	if err != nil {
		fail(err.Error())
		os.Exit(1)
	}
	cli = whatsmeow.NewClient(device, log.Sub("client"))
	cli.AddEventHandler(onEvent)

	if cli.Store.ID == nil {
		qrs, _ := cli.GetQRChannel(ctx)
		if err := cli.Connect(); err != nil {
			fail("Could not reach WhatsApp: " + err.Error())
		}
		go func() {
			for item := range qrs {
				switch item.Event {
				case "code":
					qr(item.Code)
				case "timeout":
					fail("The code expired. Reopen WhatsApp to get a new one.")
				case "success":
				default:
					if item.Error != nil {
						fail("Linking failed: " + item.Error.Error())
					}
				}
			}
		}()
	} else {
		status("Connecting to WhatsApp…")
		if err := cli.Connect(); err != nil {
			fail("Could not reach WhatsApp: " + err.Error())
		}
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGTERM, syscall.SIGINT)
	done := make(chan struct{})
	go func() {
		in := bufio.NewScanner(os.Stdin)
		in.Buffer(make([]byte, 64<<10), 1<<20)
		for in.Scan() {
			var req request
			if err := json.Unmarshal(in.Bytes(), &req); err != nil {
				continue
			}
			go func(req request) {
				result, err := handle(req)
				if err != nil {
					emit(map[string]any{"id": req.ID, "error": err.Error()})
				} else {
					emit(map[string]any{"id": req.ID, "result": result})
				}
			}(req)
		}
		close(done) // the app closed: so do we
	}()
	select {
	case <-stop:
	case <-done:
	}
	cli.Disconnect()
	container.Close()
}
