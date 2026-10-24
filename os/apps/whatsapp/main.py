"""WhatsApp in the phone's own design: chats and status side by side in a
pivot, a chat as the Messaging app's bubbles. WhatsApp itself is
whatsapp-native (native/, whatsmeow in Go) behind a private stdio pipe: no
browser.

Rows draw themselves once into a picture (ztos._Pressable) and the chat list
is made again only when what it shows changed, and only while it is seen:
channels and status updates arrive every few seconds and must not cost a
redraw each."""
import base64
import hashlib
import json
import os
import shutil
import sys
import time
from pathlib import Path
from typing import Optional

import ztos
from ztos import FONT_REGULAR, FONT_SEMIBOLD, FONT_SEMILIGHT, GREY
from PySide6.QtCore import QPointF, QProcess, QProcessEnvironment, QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QFontMetricsF, QGuiApplication, QImage, QPainter, QPainterPath, QPixmap
from PySide6.QtWidgets import QLabel, QSizePolicy, QVBoxLayout, QWidget

APP = ztos.App()
BINARY = 'native/whatsapp-native'


def install_binary(root):
    """The client carried in the app, copied out where it can run (again only
    when the app brought a different one)."""
    data = ztos.resource(BINARY)
    digest = hashlib.sha256(data).hexdigest()
    target, stamp = root/'whatsapp-native', root/'whatsapp-native.sha256'
    if not target.is_file() or not stamp.is_file() or stamp.read_text().strip() != digest:
        partial = root/'whatsapp-native.part'
        partial.write_bytes(data)
        partial.chmod(0o700)
        partial.replace(target)
        stamp.write_text(digest)
    return target


def forget_browser_bridge(root):
    """What the Chromium bridge (before October 2026) left: ~90 MB of Node
    packages and a browser profile. Its link can't be carried over."""
    for name in ('node_modules', 'session', '.wwebjs_cache'):
        shutil.rmtree(root/name, ignore_errors=True)
    for name in ('bridge.js', 'bridge_test.js', 'package.json', 'package-lock.json'):
        (root/name).unlink(missing_ok=True)


class Bridge:
    def __init__(self):
        self.process = QProcess(APP.window)
        self.process.setProcessChannelMode(QProcess.SeparateChannels)
        self.buffer = bytearray()
        self.pending = {}
        self.timeouts = {}
        self.sequence = 0
        self.listeners = []
        self.process.readyReadStandardOutput.connect(self.read)
        self.process.readyReadStandardError.connect(self.log_errors)
        self.process.errorOccurred.connect(lambda _: self.tell({'event':'error','error':"Could not start WhatsApp's client."}))
        self.process.finished.connect(self.finished)
        APP.qt.aboutToQuit.connect(self.stop)

    def tell(self, event):
        for listener in self.listeners:
            listener(event)

    def log_errors(self):
        # (the app's log: the client's warnings, never messages)
        sys.stderr.write(bytes(self.process.readAllStandardError()).decode(errors='replace'))

    def start(self):
        root = Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))/'ztos/whatsapp'
        root.mkdir(parents=True, exist_ok=True, mode=0o700)
        forget_browser_bridge(root)
        try:
            binary = install_binary(root)
        except OSError as e:
            self.tell({'event':'error','error':f'Could not set up WhatsApp: {e}'})
            return
        env = QProcessEnvironment.systemEnvironment()
        env.insert('ZTOS_WHATSAPP_DATA', str(root))
        self.process.setProcessEnvironment(env)
        self.process.setWorkingDirectory(str(root))
        self.process.start(str(binary), [])

    def request(self, method, callback, **fields):
        if self.process.state() != QProcess.Running:
            callback(None, 'WhatsApp is not connected')
            return
        self.sequence += 1
        ident = self.sequence
        self.pending[ident] = callback
        timer = QTimer(APP.window, singleShot=True, interval=60000)
        timer.timeout.connect(lambda: self.expired(ident))
        self.timeouts[ident] = timer
        timer.start()
        self.process.write((json.dumps(dict(id=ident, method=method, **fields))+'\n').encode())

    def expired(self, ident):
        timer = self.timeouts.pop(ident, None)
        if timer:
            timer.deleteLater()
        callback = self.pending.pop(ident, None)
        if callback:
            callback(None, 'Request timed out. Check the conversation before retrying a send.')

    def finished(self, *_):
        for ident in list(self.pending):
            self.expired(ident)
        self.tell({'event':'error','error':'WhatsApp stopped. Reopen the app to reconnect.'})

    def read(self):
        self.buffer.extend(bytes(self.process.readAllStandardOutput()))
        while b'\n' in self.buffer:
            line, _, self.buffer = self.buffer.partition(b'\n')
            try:
                data = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            callback = self.pending.pop(data.get('id'), None)
            if callback:
                timer = self.timeouts.pop(data['id'], None)
                if timer:
                    timer.stop()
                    timer.deleteLater()
                try:
                    callback(data.get('result'), data.get('error'))
                except RuntimeError:
                    pass        # its page was closed meanwhile
            else:
                self.tell(data)

    def stop(self):
        self.process.closeWriteChannel()
        if not self.process.waitForFinished(1500):
            self.process.terminate()
            if not self.process.waitForFinished(1000):
                self.process.kill()
                self.process.waitForFinished(1000)


BRIDGE = Bridge()


# ---- words for times ----------------------------------------------------------------------

def when(ts) -> str:
    """09:41 today, Tue this week, 12 Oct before."""
    if not ts:
        return ''
    t, now = time.localtime(ts), time.localtime()
    if t.tm_year == now.tm_year and t.tm_yday == now.tm_yday:
        return time.strftime('%H:%M', t)
    if time.time() - ts < 6 * 86400:
        return time.strftime('%a', t)
    return time.strftime('%d %b', t).lstrip('0')


def ago(ts) -> str:
    d = max(0, time.time() - ts)
    if d < 60:
        return 'just now'
    if d < 3600:
        return f'{int(d // 60)} min ago'
    return f'{int(d // 3600)} h ago'


def thumb_image(b64: str) -> Optional[QImage]:
    if not b64:
        return None
    image = QImage.fromData(base64.b64decode(b64))
    return None if image.isNull() else image


def cover(p: QPainter, image: QImage, rect: QRectF, round_: bool = False):
    """A picture filling rect, cut to it (a circle when round_)."""
    p.save()
    if round_:
        clip = QPainterPath()
        clip.addEllipse(rect)
        p.setClipPath(clip)
    ratio, want = image.width() / max(1, image.height()), rect.width() / max(1.0, rect.height())
    if ratio > want:
        w = image.height() * want
        src = QRectF((image.width() - w) / 2, 0, w, image.height())
    else:
        h = image.width() / want
        src = QRectF(0, (image.height() - h) / 2, image.width(), h)
    p.drawImage(rect, image, src)
    p.restore()


def argb(value: int, fallback: QColor) -> QColor:
    if not value:
        return fallback
    c = QColor.fromRgba(value & 0xFFFFFFFF)
    c.setAlpha(255)
    return c


# ---- rows -----------------------------------------------------------------------------------

class ChatRow(ztos._Pressable):
    """A chat: its name, the last message in grey under it, the time and the
    unread count at the right."""
    H = 66

    def __init__(self, chat, on_tap):
        super().__init__()
        self.chat = chat
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setFixedHeight(self.H)
        self.tapped.connect(on_tap)

    def draw(self, p: QPainter):
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing)
        w = self.width()
        unread = self.chat.get('unread') or 0
        right = w
        p.setFont(ztos._font(FONT_REGULAR, 11.5))
        fm = QFontMetricsF(p.font())
        stamp = when(self.chat.get('t'))
        if stamp:
            p.setPen(ztos.accent() if unread else GREY)
            p.drawText(QPointF(w - fm.horizontalAdvance(stamp), 10 + fm.ascent()), stamp)
            right = w - fm.horizontalAdvance(stamp) - 10
        p.setPen(QColor('white'))
        p.setFont(ztos._font(FONT_SEMILIGHT, 20))
        fm = QFontMetricsF(p.font())
        p.drawText(QPointF(0, 4 + fm.ascent()), fm.elidedText(self.chat['name'], Qt.ElideRight, right))
        p.setFont(ztos._font(FONT_REGULAR, 13))
        fm2 = QFontMetricsF(p.font())
        last_right = w
        if unread:
            badge = str(unread)
            bw = max(20.0, fm2.horizontalAdvance(badge) + 10)
            r = QRectF(w - bw, 36, bw, 20)
            p.fillRect(r, ztos.accent())
            p.setPen(QColor('white'))
            p.drawText(r, Qt.AlignCenter, badge)
            last_right = w - bw - 10
        p.setPen(ztos.accent() if unread else GREY)
        last = (self.chat.get('last') or '').replace('\n', ' ')
        p.drawText(QPointF(0, 38 + fm2.ascent()), fm2.elidedText(last, Qt.ElideRight, last_right))


class Downloads:
    """Photos and stickers fetched one at a time (a chat opening with twenty
    of them must not start twenty downloads), newest asked first."""

    def __init__(self):
        self.queue = []
        self.busy = False

    def get(self, chat, msg_id, on_path):
        self.queue.append((chat, msg_id, on_path))
        self._next()

    def _next(self):
        if self.busy or not self.queue:
            return
        chat, msg_id, on_path = self.queue.pop()
        self.busy = True

        def done(path, error):
            self.busy = False
            if path and not error:
                try:
                    on_path(path)
                except RuntimeError:
                    pass        # its bubble went with its page
            self._next()
        BRIDGE.request('media', done, chat=chat, msg=msg_id)


DOWNLOADS = Downloads()


class Bubble(ztos._Pressable):
    """A message as the Messaging app draws it: a block of the accent colour
    (yours, at the right) or grey (theirs), with its corner tail, the time
    small under the text. A photo fills its bubble (its preview until it is
    downloaded; tap it to see it whole); a sticker stands on its own. Hold
    for copy and delete."""
    PAD, GAP, TAIL = 10, 8, 8
    STICKER = 140

    def __init__(self, msg, chat_id, on_delete):
        super().__init__()
        self.msg, self.chat_id, self.on_delete = msg, chat_id, on_delete
        self.kind = msg.get('kind') or ''
        self.image: Optional[QImage] = thumb_image(msg.get('thumb')) if self.kind else None
        self.path = ''
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self._laid = -1
        self.setFixedHeight(40)
        self.held.connect(self._menu)
        self.tapped.connect(self._open)
        if self.kind in ('image', 'sticker'):
            DOWNLOADS.get(chat_id, msg['id'], self._downloaded)

    # -- what it holds ------------------------------------------------------------------------
    def _text(self) -> str:
        if self.kind == 'image':
            return self.msg.get('caption') or ''
        if self.kind == 'sticker':
            return ''
        return self.msg['body']

    def _picture_size(self):
        """The photo's place in the bubble (points): as wide as fits up to
        240, as tall as its shape asks within 100..300."""
        if self.kind == 'sticker':
            return float(self.STICKER), float(self.STICKER)
        w = min(self._box_width() - 8, 240.0)
        pw, ph = self.msg.get('w') or 0, self.msg.get('h') or 0
        if not (pw and ph) and self.image is not None:
            pw, ph = self.image.width(), self.image.height()
        ratio = ph / pw if pw and ph else 0.75
        return w, max(100.0, min(300.0, w * ratio))

    def _downloaded(self, path):
        image = QImage(path)
        if image.isNull():
            return
        self.path = path
        w, h = self._picture_size()
        ratio = self.devicePixelRatioF()
        # kept at the size it is shown (a phone photo is ~12 MB of pixels)
        mode = Qt.KeepAspectRatio if self.kind == 'sticker' else Qt.KeepAspectRatioByExpanding
        self.image = image.scaled(int(w * ratio), int(h * ratio), mode, Qt.SmoothTransformation)
        self.redraw()
        self.content_changed()

    def _open(self):
        if self.kind == 'image' and self.path:
            ztos.haptic('tap')
            APP.push(PhotoViewer(self.path, self.msg.get('caption') or ''))

    def _menu(self):
        items = []
        if self._text():
            items.append(('copy', lambda: QGuiApplication.clipboard().setText(self._text())))
        items.append(('delete for me', lambda: self._delete(False)))
        if self.msg['fromMe'] and time.time() - (self.msg.get('timestamp') or 0) < 2 * 86400 \
                and not self.msg['body'].startswith('You deleted'):
            items.append(('delete for everyone', lambda: self._delete(True)))
        ztos.menu(self, items)

    def _delete(self, everyone):
        title = 'Delete for everyone?' if everyone else 'Delete for me?'
        text = ('It goes from everyone\'s phones in this chat; they see that you deleted a message.'
                if everyone else 'It goes from this phone only; your other phones and the chat keep it.')
        ztos.message(title, text, ('delete', 'cancel'),
                     lambda i: self.on_delete(self, everyone) if i == 0 else None)

    def deleted_for_everyone(self):
        self.msg.update(body='You deleted this message', kind='', caption='')
        self.kind, self.image = '', None
        self._laid = -1
        self._layout()
        self.redraw()
        self.content_changed()

    # -- its shape ------------------------------------------------------------------------------
    def _fonts(self):
        return ztos._font(FONT_REGULAR, 15), ztos._font(FONT_SEMIBOLD, 11.5), ztos._font(FONT_REGULAR, 10.5)

    def _box_width(self) -> float:
        return self.width() * 0.8

    def _author(self) -> str:
        return '' if self.msg['fromMe'] else (self.msg.get('author') or '')

    def _layout(self):
        """The height it needs at this width."""
        if self._laid == self.width():
            return
        self._laid = self.width()
        body, small, stamp = self._fonts()
        h = 0.0
        if self._author():
            h += QFontMetricsF(small).height()
        if self.kind:
            h += self._picture_size()[1] + 4
        text = self._text()
        if text:
            inner = self._box_width() - 2 * self.PAD
            h += QFontMetricsF(body).boundingRect(QRectF(0, 0, inner, 1e5), Qt.TextWordWrap, text).height() + 4
        h += QFontMetricsF(stamp).height() + 2 * self.PAD + self.TAIL + self.GAP
        self.setFixedHeight(int(h + 0.5))

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._layout()

    def draw(self, p: QPainter):
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing | QPainter.SmoothPixmapTransform)
        body, small, stamp = self._fonts()
        mine = self.msg['fromMe']
        text, author = self._text(), self._author()
        stamp_text = when(self.msg.get('timestamp'))
        inner = self._box_width() - 2 * self.PAD

        if self.kind == 'sticker':      # no bubble: the sticker, the time under it
            w, h = self._picture_size()
            x = self.width() - w if mine else 0
            y = 0.0
            if author:
                p.setFont(small)
                p.setPen(GREY)
                p.drawText(QPointF(x, QFontMetricsF(small).ascent()), author)
                y += QFontMetricsF(small).height()
            if self.image is not None:
                s = min(w / self.image.width(), h / self.image.height())
                iw, ih = self.image.width() * s, self.image.height() * s
                p.drawImage(QRectF(x + (w - iw) / 2, y + (h - ih) / 2, iw, ih), self.image)
            p.setFont(stamp)
            p.setPen(GREY)
            p.drawText(QPointF(x, y + h + 4 + QFontMetricsF(stamp).ascent()), stamp_text)
            return

        # as narrow as what it holds, up to 80 % of the row
        tw = QFontMetricsF(body).boundingRect(QRectF(0, 0, inner, 1e5), Qt.TextWordWrap, text).width() if text else 0
        if author:
            tw = max(tw, QFontMetricsF(small).horizontalAdvance(author))
        tw = max(tw, QFontMetricsF(stamp).horizontalAdvance(stamp_text))
        box_w = tw + 2 * self.PAD
        if self.kind:
            box_w = max(box_w, self._picture_size()[0] + 8)
        box_w = min(self._box_width(), box_w)
        x = self.width() - box_w if mine else 0
        box = QRectF(x, 0 if mine else self.TAIL, box_w, self.height() - self.GAP - self.TAIL)
        colour = ztos.accent() if mine else QColor(0x3a, 0x3a, 0x3a)
        p.fillRect(box, colour)
        path = QPainterPath()
        if mine:        # the tail under the box, at its right
            path.moveTo(box.right() - 20, box.bottom())
            path.lineTo(box.right() - 4, box.bottom())
            path.lineTo(box.right() - 4, box.bottom() + self.TAIL)
        else:           # over the box, at its left
            path.moveTo(box.left() + 4, box.top())
            path.lineTo(box.left() + 4, box.top() - self.TAIL)
            path.lineTo(box.left() + 20, box.top())
        path.closeSubpath()
        p.fillPath(path, colour)
        y = box.top() + (4 if self.kind and not author else self.PAD)
        if author:
            p.setFont(small)
            p.setPen(QColor(255, 255, 255, 200))
            fm = QFontMetricsF(small)
            p.drawText(QPointF(x + self.PAD, y + fm.ascent()), author)
            y += fm.height() + (4 if self.kind else 0)
        if self.kind:
            w, h = self._picture_size()
            pic = QRectF(x + 4, y, box_w - 8, h)
            if self.image is not None:
                cover(p, self.image, pic)
            else:
                p.fillRect(pic, QColor(0, 0, 0, 90))
            y += h + 4
        if text:
            p.setFont(body)
            p.setPen(QColor('white'))
            rect = QRectF(x + self.PAD, y, inner, box.bottom() - y)
            used = p.boundingRect(rect, Qt.TextWordWrap, text)
            p.drawText(rect, Qt.TextWordWrap, text)
            y = used.bottom() + 4
        p.setFont(stamp)
        p.setPen(QColor(255, 255, 255, 170))
        p.drawText(QPointF(x + self.PAD, y + QFontMetricsF(stamp).ascent()), stamp_text)


class PhotoViewer(ztos.Page):
    """A photo, whole, on black; its caption under it."""

    def __init__(self, path, caption=''):
        super().__init__('photo', scroll=False, header=False)
        self.source = QImage(path)
        self.view = QWidget()
        self.view.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.view.paintEvent = self._paint
        self.layout().addWidget(self.view, 1)
        if caption:
            label = QLabel(caption)
            label.setWordWrap(True)
            label.setFont(ztos._font(FONT_REGULAR, 14))
            label.setStyleSheet('color: white;')
            label.setContentsMargins(ztos.MARGIN, 6, ztos.MARGIN, 10)
            self.layout().addWidget(label)
        self.shown_image: Optional[QImage] = None

    def _paint(self, e):
        p = QPainter(self.view)
        p.setRenderHints(QPainter.SmoothPixmapTransform)
        r = QRectF(self.view.rect())
        p.fillRect(r, QColor('black'))
        if self.source.isNull():
            return
        ratio = self.devicePixelRatioF()
        size = (int(r.width() * ratio), int(r.height() * ratio))
        if self.shown_image is None or self._for != size:
            # scaled once to the screen; the photo itself let go
            self.shown_image = self.source.scaled(size[0], size[1], Qt.KeepAspectRatio, Qt.SmoothTransformation)
            self._for = size
        img = self.shown_image
        w, h = img.width() / ratio, img.height() / ratio
        p.drawImage(QRectF((r.width() - w) / 2, (r.height() - h) / 2, w, h), img)


class StatusRow(ztos._Pressable):
    """Someone's status: their newest picture in a ring, the name, how many
    updates and how long ago."""
    H = 72

    def __init__(self, person, on_tap):
        super().__init__()
        self.person = person
        self.image = thumb_image(person.get('thumb'))
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setFixedHeight(self.H)
        self.tapped.connect(on_tap)

    def draw(self, p: QPainter):
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing | QPainter.SmoothPixmapTransform)
        ring = QRectF(2, 8, 56, 56)
        p.setPen(ztos.accent())
        p.setBrush(Qt.NoBrush)
        p.drawEllipse(ring)
        inner = ring.adjusted(4, 4, -4, -4)
        if self.image is not None:
            cover(p, self.image, inner, round_=True)
        else:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(0x3a, 0x3a, 0x3a))
            p.drawEllipse(inner)
            p.setPen(QColor('white'))
            p.setFont(ztos._font(FONT_SEMILIGHT, 20))
            p.drawText(inner, Qt.AlignCenter, (self.person['name'] or '?')[:1].upper())
        x, w = 74, self.width() - 74
        p.setPen(QColor('white'))
        p.setFont(ztos._font(FONT_SEMILIGHT, 20))
        fm = QFontMetricsF(p.font())
        p.drawText(QPointF(x, 12 + fm.ascent()), fm.elidedText(self.person['name'], Qt.ElideRight, w))
        n = self.person['count']
        p.setPen(GREY)
        p.setFont(ztos._font(FONT_REGULAR, 13))
        fm2 = QFontMetricsF(p.font())
        p.drawText(QPointF(x, 42 + fm2.ascent()), f"{n} update{'s' if n != 1 else ''} · {ago(self.person['t'])}")


class Rows(QWidget):
    def __init__(self):
        super().__init__()
        self.rows = QVBoxLayout(self)
        self.rows.setContentsMargins(0, 0, 0, 0)
        self.rows.setSpacing(0)

    def clear(self):
        while self.rows.count():
            item = self.rows.takeAt(0)
            if item.widget():
                item.widget().deleteLater()

    def add(self, w):
        self.rows.addWidget(w)
        return w


def page_changed(widget):
    page = widget.parentWidget()
    while page is not None and not isinstance(page, ztos.Page):
        page = page.parentWidget()
    if page is not None:
        page._changed(explicit=True)


# ---- a conversation -----------------------------------------------------------------------

class Conversation(ztos.Page):
    def __init__(self, chat):
        super().__init__(chat['name'].lower() if len(chat['name']) < 24 else chat['name'])
        self.chat = chat
        self.ids = set()
        self.status = self.text('', size=13, color=GREY)
        self.status.setWordWrap(True)
        self.status.hide()
        self.messages = self.add(Rows())
        self.composer = ztos.TextBox('', placeholder='Message', on_submit=lambda _: self.send())
        self.layout().addWidget(self.composer)     # (outside the scrolling history)
        self.send_button = ztos.RoundButton('', 'send', self.send)
        self.bar(self.send_button)
        self.busy(True)
        BRIDGE.request('messages', self.loaded, chat=chat['id'])

    def say(self, text):
        self.status.setText(text)
        self.status.setVisible(bool(text))

    def loaded(self, messages, error):
        self.busy(False)
        if error:
            self.say(error)
            return
        self.say('' if messages else 'No messages from this chat on this phone yet. New ones show up here.')
        self.messages.clear()
        self.ids.clear()
        for msg in messages:
            self.append(msg, scroll=False)
        page_changed(self.messages)
        QTimer.singleShot(0, lambda: self.scroll_to(self._max()))

    def append(self, msg, scroll=True):
        if msg['id'] in self.ids:
            return
        self.ids.add(msg['id'])
        self.messages.add(Bubble(msg, self.chat['id'], self.delete))
        if scroll:
            self.say('')
            page_changed(self.messages)
            QTimer.singleShot(0, lambda: self.scroll_to(self._max()))

    def delete(self, bubble, everyone):
        def done(ok, error):
            if error:
                self.say('Not deleted · ' + error)
                return
            if everyone:
                bubble.deleted_for_everyone()
            else:
                self.ids.discard(bubble.msg['id'])
                self.messages.rows.removeWidget(bubble)
                bubble.deleteLater()
                page_changed(self.messages)
            ztos.haptic('tick')
        BRIDGE.request('delete', done, chat=self.chat['id'], msg=bubble.msg['id'], everyone=everyone)

    def send(self):
        text = self.composer.text().strip()
        if not text or not self.send_button.isEnabled():
            return
        self.send_button.setEnabled(False)
        self.say('sending…')

        def done(msg, error):
            self.send_button.setEnabled(True)
            if error:
                self.say('Not sent · ' + error)
                return
            if self.composer.text().strip() == text:
                self.composer.field.clear()
            self.say('')
            self.append(msg)
        BRIDGE.request('send', done, chat=self.chat['id'], text=text)


# ---- status --------------------------------------------------------------------------------

class StatusPicture(QWidget):
    """One status, filling the page: the photo (its preview until it is
    downloaded), or the text on its colour. Taps at the left go back one,
    elsewhere on."""

    def __init__(self, on_back, on_next):
        super().__init__()
        self.on_back, self.on_next = on_back, on_next
        self.update_ = None
        self.image: Optional[QImage] = None
        self.note = ''

    def show_update(self, update, image=None, note=''):
        self.update_, self.image, self.note = update, image, note
        self.update()

    def mouseReleaseEvent(self, e):
        (self.on_back if e.position().x() < self.width() / 3 else self.on_next)()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing | QPainter.SmoothPixmapTransform)
        r = QRectF(self.rect())
        u = self.update_
        if u is None:
            return
        if u['kind'] == 'text':
            p.fillRect(r, argb(u.get('bg'), ztos.accent()))
            p.setPen(QColor('white'))
            p.setFont(ztos._font(FONT_SEMILIGHT, 24 if len(u['text']) < 120 else 17))
            p.drawText(r.adjusted(24, 24, -24, -24), Qt.AlignCenter | Qt.TextWordWrap, u['text'])
            return
        p.fillRect(r, QColor('black'))
        if self.image is not None:
            # the whole picture, as large as fits
            s = min(r.width() / self.image.width(), r.height() / self.image.height())
            w, h = self.image.width() * s, self.image.height() * s
            p.drawImage(QRectF((r.width() - w) / 2, (r.height() - h) / 2, w, h), self.image)
        if self.note:
            p.fillRect(QRectF(0, r.height() / 2 - 22, r.width(), 44), QColor(0, 0, 0, 160))
            p.setPen(QColor('white'))
            p.setFont(ztos._font(FONT_REGULAR, 13))
            p.drawText(QRectF(0, r.height() / 2 - 22, r.width(), 44), Qt.AlignCenter, self.note)


class StatusViewer(ztos.Page):
    """Someone's updates, one at a time, oldest first."""

    def __init__(self, person):
        super().__init__(person['name'].lower(), scroll=False)
        self.person = person
        self.updates = []
        self.index = 0
        self.picture = StatusPicture(self.previous, self.next)
        self.picture.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.layout().addWidget(self.picture, 1)
        self.caption = QLabel('')
        self.caption.setWordWrap(True)
        self.caption.setFont(ztos._font(FONT_REGULAR, 14))
        self.caption.setStyleSheet('color: white;')
        self.caption.setContentsMargins(ztos.MARGIN, 6, ztos.MARGIN, 0)
        self.layout().addWidget(self.caption)
        self.when = QLabel('')
        self.when.setFont(ztos._font(FONT_REGULAR, 12))
        self.when.setStyleSheet('color: #999999;')
        self.when.setContentsMargins(ztos.MARGIN, 2, ztos.MARGIN, 8)
        self.layout().addWidget(self.when)
        self.bar(ztos.RoundButton('', 'previous', self.previous), ztos.RoundButton('', 'next', self.next))
        self.busy(True)
        BRIDGE.request('status_list', self.loaded, chat=person['sender'])

    def loaded(self, updates, error):
        self.busy(False)
        if error or not updates:
            self.picture.show_update({'kind': 'text', 'text': error or 'No updates now.', 'bg': 0})
            return
        self.updates = updates
        self.show_at(0)

    def show_at(self, i):
        self.index = i
        u = self.updates[i]
        self.when.setText(f'{i + 1} of {len(self.updates)} · {ago(u["t"])}')
        self.caption.setText(u['text'] if u['kind'] != 'text' else '')
        self.caption.setVisible(u['kind'] != 'text' and bool(u['text']))
        preview = thumb_image(u.get('thumb'))
        if u['kind'] == 'image':
            self.picture.show_update(u, preview, 'loading the photo…')
            BRIDGE.request('status_photo', lambda path, err, i=i: self.photo(i, path, err), chat=u['id'])
        elif u['kind'] == 'video':
            self.picture.show_update(u, preview, 'a video: watch it on your phone')
        else:
            self.picture.show_update(u)

    def photo(self, i, path, error):
        if i != self.index:
            return              # moved on meanwhile
        u = self.updates[i]
        if error:
            self.picture.show_update(u, self.picture.image, error)
            return
        image = QImage(path)
        if image.isNull():
            self.picture.show_update(u, self.picture.image, "couldn't open the photo")
            return
        # kept at the screen's size: a phone photo is ~12 MB of pixels
        ratio = self.devicePixelRatioF()
        image = image.scaled(int(self.picture.width() * ratio), int(self.picture.height() * ratio),
                             Qt.KeepAspectRatio, Qt.SmoothTransformation)
        self.picture.show_update(u, image)

    def previous(self):
        if self.updates and self.index > 0:
            ztos.haptic('tick')
            self.show_at(self.index - 1)

    def next(self):
        if self.updates and self.index < len(self.updates) - 1:
            ztos.haptic('tick')
            self.show_at(self.index + 1)
        elif self.updates:
            APP.back()          # the last one: back to the list


class Status(ztos.Page):
    def __init__(self):
        super().__init__('status')
        self.note = self.text('', size=14, color=GREY)
        self.note.setWordWrap(True)
        self.rows = self.add(Rows())
        self.seen = None
        self.loading = False

    def shown(self):
        self.refresh()

    def refresh(self):
        if self.loading:
            return
        self.loading = True
        self.busy(True)
        BRIDGE.request('statuses', self.loaded)

    def loaded(self, people, error):
        self.loading = False
        self.busy(False)
        if error:
            self.note.setText(error)
            return
        key = [(x['sender'], x['count'], x['t']) for x in people]
        if key == self.seen:
            return
        self.seen = key
        self.note.setText('' if people else
                          'No status updates from the last day yet. New ones show up here while WhatsApp is open.')
        self.note.setVisible(not people)
        self.rows.clear()
        for person in people:
            self.rows.add(StatusRow(person, lambda person=person: open_status(person)))
        page_changed(self.rows)


def open_status(person):
    ztos.haptic('tap')
    APP.push(StatusViewer(person))


# ---- chats ---------------------------------------------------------------------------------

class Chats(ztos.Page):
    def __init__(self):
        super().__init__('chats')
        self.status = self.text('Connecting to WhatsApp…', size=15)
        self.status.setWordWrap(True)
        self.qr = self.add(QLabel())
        self.qr.setAlignment(Qt.AlignCenter)
        self.qr.hide()
        self.help = self.text('On your phone: WhatsApp → Linked devices → Link a device. Scan the code here.',
                              size=14, color=GREY)
        self.help.hide()
        self.rows = self.add(Rows())
        self.seen = None            # what the list shows: made again only when this changes
        self.dirty = False
        self.loading = False
        self.later = QTimer(self, singleShot=True, interval=1500)
        self.later.timeout.connect(self.refresh)

    def say(self, text):
        self.status.setText(text)
        self.status.setVisible(bool(text))

    def event_(self, event):
        kind = event.get('event')
        if kind == 'qr':
            image = QPixmap()
            image.loadFromData(base64.b64decode(event['image'].split(',', 1)[1]))
            self.qr.setPixmap(image)
            self.qr.show()
            self.help.show()
            self.say('Link WhatsApp')
        elif kind == 'ready':
            self.qr.clear()
            self.qr.hide()
            self.help.hide()
            self.say('')
            self.refresh()
        elif kind in ('error', 'status'):
            self.say(event.get('error') or event.get('text'))
        elif kind in ('history', 'message'):
            if kind == 'message':
                page = APP.page
                if isinstance(page, Conversation) and page.chat['id'] == event['chat']:
                    page.append(event['message'])
            self.dirty = True
            if self.on_screen():
                self.later.start()      # (a burst of messages: one refresh after it)

    def on_screen(self) -> bool:
        return self.isVisible() and QGuiApplication.applicationState() == Qt.ApplicationActive

    def shown(self):
        if self.dirty or self.seen is None:
            self.refresh()

    def refresh(self):
        if self.loading:
            self.dirty = True
            return
        self.loading = True
        self.dirty = False
        BRIDGE.request('chats', self.loaded)

    def loaded(self, chats, error):
        self.loading = False
        if error:
            if 'still connecting' not in error:
                self.say(error)
            return
        key = [(c['id'], c['name'], c['unread'], c['last'], c.get('t')) for c in chats]
        if key != self.seen:
            live_tile(chats)
            self.seen = key
            self.rows.clear()
            for chat in chats:
                self.rows.add(ChatRow(chat, lambda chat=chat: open_chat(chat)))
            page_changed(self.rows)
        if self.dirty and self.on_screen():
            self.later.start()


def live_tile(chats):
    """Start's tile, as Messaging's: the unread count on its front, and on its
    back the chats that wrote last (the unread ones first)."""
    unread = sum(c['unread'] or 0 for c in chats)
    first = sorted(chats, key=lambda c: (not c['unread'], -(c.get('t') or 0)))
    lines = [f"{c['name']}: {c['last']}" if c['last'] else c['name'] for c in first[:3]]
    ztos.tile_update(f'{unread} unread' if unread else 'WhatsApp', lines, count=unread)


def open_chat(chat):
    ztos.haptic('tap')
    APP.push(Conversation(chat))


class Home(ztos.Pivot):
    def __init__(self):
        self.chats, self.status = Chats(), Status()
        super().__init__([self.chats, self.status])
        self.bar(ztos.RoundButton('', 'refresh', self.refresh))
        BRIDGE.listeners.append(self.chats.event_)

    def refresh(self):
        ztos.haptic('tap')
        page = self.current()
        if page is self.status:
            self.status.seen = None
        else:
            self.chats.seen = None
        page.refresh()


def app_state(state):
    # back in front: the list as it is now (it was left alone meanwhile)
    if state == Qt.ApplicationActive and HOME.chats.dirty and HOME.chats.isVisible():
        HOME.chats.refresh()


HOME = Home()
QGuiApplication.instance().applicationStateChanged.connect(app_state)
QTimer.singleShot(0, BRIDGE.start)
APP.run(HOME)
