"""Tube's pages past the home pivot: a video, a channel, your channels, settings."""
import os
import re
import webbrowser
from typing import Optional

from PySide6.QtCore import QPointF, QRectF, QSizeF, Qt, QTimer
from PySide6.QtGui import QColor, QFontMetricsF, QGuiApplication, QImage, QPainter, QPainterPath
from PySide6.QtWidgets import QSizePolicy, QVBoxLayout, QWidget

import library
import player as playback
import state
import ztos
from widgets import ChannelRow, VideoRow, Words, count, details, draw_cover, note, poster_of, thumbs

# Segoe MDL2 Assets
G_PLAY, G_PAUSE = "", ""
G_DOWNLOAD = ""
G_AUDIO = ""          # headphones: sound only
G_VIDEO = ""
G_SUBSCRIBE, G_SUBSCRIBED = "", ""
G_MORE = "more"


def freeze_video():
    """Before a page turns, or the app is left. The playing picture is a native
    surface of its own: the turn, drawn from pictures of the pages, leaves it
    black in them, and it stays put on top of the turning pages. Its last
    frame goes into the page's poster, an ordinary widget that turns with the
    page, and the surface is hidden; VideoPage._attach shows it again when
    the page is still."""
    box, surface = playback.container, playback.surface
    if box is None or surface is None or not box.isVisible():
        return
    try:
        frame = surface.grabFramebuffer()
        if not frame.isNull():
            playback.get().last_frame = frame
    except Exception:
        pass
    page = box.parentWidget()
    while page is not None and not isinstance(page, VideoPage):
        page = page.parentWidget()
    # the surface out of the slot's layout first: both shown for a moment, they
    # split its height, and the turn pictured the poster at half of it
    box.hide()
    if page is not None:
        page.poster.show_frame(playback.get().last_frame)
        page.poster.show()
        page.slot_layout.activate()


def open_video(video: dict, local: Optional[dict] = None):
    ztos.haptic("tap")
    freeze_video()
    state.app.push(VideoPage(video, local))


def open_channel(channel_id: str, name: str = ""):
    if not channel_id:
        return
    ztos.haptic("tap")
    freeze_video()
    state.app.push(ChannelPage(channel_id, name))


def video_menu(row: VideoRow, extra: list = ()):
    """The hold menu of a video row."""
    v = row.video
    items = [("play sound only", lambda: open_video(dict(v, _audio_only=True)))]
    if not v.get("live"):
        items.append(("download video", lambda: state.downloads.start(v, "video", int(library.setting("quality")))))
        items.append(("download sound", lambda: state.downloads.start(v, "audio")))
    if v.get("channel_id"):
        items.append((f"open {v.get('channel') or 'channel'}", lambda: open_channel(v["channel_id"], v.get("channel", ""))))
    items.append(("copy link", lambda: QGuiApplication.clipboard().setText(f"https://youtu.be/{v['id']}")))
    items.extend(extra)
    ztos.haptic("tick")
    ztos.menu(row, items)


class Poster(QWidget):
    """The video's picture, until it plays: dims while the streams are found."""

    def __init__(self, video: dict, on_tap):
        super().__init__()
        self.image: Optional[QImage] = None
        self.waiting = True
        self.message = ""
        self.on_tap = on_tap
        self.sharp = False
        self.frame = False      # showing the playing video's last frame (freeze_video)
        thumbs.get(video.get("thumb") or "", self._image)          # the row's, already here
        thumbs.get(poster_of(video.get("thumb") or ""), self._sharp)  # then the big one

    def _image(self, image):
        if not self.sharp and not self.frame:
            self.image = image
            self.update()

    def _sharp(self, image):
        self.sharp = True
        if not self.frame:
            self.image = image
            self.update()

    def set_waiting(self, on: bool, message: str = ""):
        self.waiting, self.message = on, message
        self.frame = False
        self.update()

    def show_frame(self, image: Optional[QImage]):
        """The video as it last was, plain, standing in for it while a page turns"""
        if image is None or image.isNull():
            return
        self.image, self.frame = image, True
        self.waiting, self.message = False, ""
        self.update()

    def mouseReleaseEvent(self, e):
        self.on_tap()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.Antialiasing | QPainter.SmoothPixmapTransform | QPainter.TextAntialiasing)
        r = QRectF(self.rect())
        p.fillRect(r, QColor("black"))
        draw_cover(p, self.image, r)
        if self.frame:
            return
        p.fillRect(r, QColor(0, 0, 0, 140 if self.waiting else 70))
        c = r.center()
        if self.message:
            p.setPen(QColor("white"))
            p.setFont(ztos._font(ztos.FONT_REGULAR, 13))
            p.drawText(r.adjusted(24, 0, -24, 0), Qt.AlignCenter | Qt.TextWordWrap, self.message)
        elif not self.waiting:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(0, 0, 0, 150))
            p.drawEllipse(c, 30, 30)
            path = QPainterPath()
            path.moveTo(c.x() - 9, c.y() - 14)
            path.lineTo(c.x() + 15, c.y())
            path.lineTo(c.x() - 9, c.y() + 14)
            path.closeSubpath()
            p.setBrush(QColor("white"))
            p.drawPath(path)


class VideoPage(ztos.Page):
    """A video: the picture at the top (whole screen when the phone turns),
    its title, channel and description under it, and the app bar."""

    def __init__(self, video: dict, local: Optional[dict] = None):
        super().__init__("", header=False)
        self.summary = video
        self.audio_only = bool(video.get("_audio_only"))
        self.local = local or library.downloaded(video["id"])
        if self.local and self.local.get("kind") == "audio":
            self.audio_only = True
        self.info: Optional[dict] = None
        self.request: Optional[int] = None
        self.play_when_ready = True
        self.p = playback.get()
        self.live = bool(video.get("live"))
        self._landscape = False
        self._left_full = False     # turned, but the full screen was left (its button)
        self.picker: Optional["PlayerPicker"] = None     # quality and speed, while it is up
        playback.video_widget()     # made now: its GL context is ready before play
        self.controls = playback.surface.controls

        # the top: the picture, outside the scrolling list (its controls,
        # the progress bar among them, are drawn on it: controls.py)
        self.top = QWidget()
        top = QVBoxLayout(self.top)
        top.setContentsMargins(0, 0, 0, 0)
        top.setSpacing(0)
        self.slot = QWidget()
        self.slot_layout = QVBoxLayout(self.slot)
        self.slot_layout.setContentsMargins(0, 0, 0, 0)
        self.poster = Poster(video, self._play_tapped)
        self.slot_layout.addWidget(self.poster)
        top.addWidget(self.slot)
        self.layout().insertWidget(1, self.top)

        # the list
        self.space(6)
        self.title_label = self.text(video.get("title") or "", size=20)
        self.title_label.setWordWrap(True)
        self.detail_label = self.text(details(video, channel=False), size=12, color=ztos.GREY)
        self.channel_row = self.add(ChannelRow(video.get("channel") or "", "", self._avatar(video.get("channel_id")),
                                               on_tap=lambda: open_channel(self.summary.get("channel_id"),
                                                                           self.summary.get("channel", ""))))
        self.space(12)
        self.description = self.text("", size=13)
        self.description.setWordWrap(True)
        self.description.setTextInteractionFlags(Qt.NoTextInteraction)

        self.b_play = ztos.RoundButton(G_PLAY, "play", self._play_tapped)
        self.b_audio = ztos.RoundButton(G_AUDIO, "sound only", self._toggle_audio)
        self.b_download = ztos.RoundButton(G_DOWNLOAD, "download", self._download)
        self.b_sub = ztos.RoundButton(G_SUBSCRIBE, "subscribe", self._toggle_subscribe)
        self.b_more = ztos.RoundButton(G_MORE, "more", self._more)
        self.bar(self.b_play, self.b_audio, self.b_download, self.b_sub, self.b_more)
        self._show_subscribed()

        self.p.paused.connect(self._on_paused)
        self.p.changed.connect(self._on_changed)
        self.p.failed.connect(self._on_failed)
        self.p.mode.connect(self._on_mode)
        self.controls.title = video.get("title") or ""
        self.controls.full_screen.connect(self._full_tapped)
        self.controls.settings.connect(self._settings)

        self._attach_pending = False
        if self._mine():
            self.info = self.p.video
            self.poster.set_waiting(False)
            if self.p.with_video:
                self.poster.show_frame(self.p.last_frame)    # (until the turn in is over)
            self._fill(self.p.video)
        elif self.local:
            self.info = dict(video, **self.local)
            self.poster.set_waiting(False)
            self._fill(self.info)
            QTimer.singleShot(0, self._start)
        else:
            self.poster.set_waiting(True, "")
            self.request = state.worker.call("video", self._resolved, self._resolve_failed, video_id=video["id"],
                                             live=bool(video.get("live")))

    # -- what the page shows ------------------------------------------------------------------
    def _avatar(self, channel_id) -> str:
        for s in library.subscriptions():
            if s["id"] == channel_id:
                return s.get("avatar") or ""
        return ""

    def _mine(self) -> bool:
        return bool(self.p.video) and self.p.video.get("id") == self.summary.get("id")

    def _fill(self, info: dict):
        title = info.get("title") or self.summary.get("title") or ""
        if info.get("live"):
            title = re.sub(r" \d{4}-\d{2}-\d{2} \d{2}:\d{2}$", "", title)     # (yt-dlp adds the hour)
        self.title_label.setText(title)
        if self._mine() or not self.p.video:
            self.controls.title = title
        parts = [count(info.get("views"), "views"), details(dict(info, views=None), channel=False),
                 count(info.get("likes"), "likes")]
        self.detail_label.setText(" • ".join(x for x in parts if x))
        if info.get("channel"):
            self.summary["channel"] = info["channel"]
            self.summary["channel_id"] = info.get("channel_id") or self.summary.get("channel_id")
            self.channel_row.name = info["channel"]
            self.channel_row.set_detail(count(info.get("subscribers"), "subscribers"))
        self.description.setText((info.get("description") or "").strip())
        self.live = bool(info.get("live"))
        self._show_subscribed()
        self._changed(explicit=True)

    def _resolved(self, info: dict):
        self.request = None
        self.busy(False)
        info["thumb"] = self.summary.get("thumb") or info.get("thumb")
        self.info = info
        self._fill(info)
        self.poster.set_waiting(False)
        if self.play_when_ready:
            self._start()

    def _resolve_failed(self, err: str):
        self.request = None
        self.busy(False)
        self.poster.set_waiting(True, f"Can't play this video.\n{err}")

    def _start(self):
        if not self.info:
            return
        self.p.play(self.info, with_video=not self.audio_only)
        self._attach()
        self._on_paused(self.p.is_paused)

    # -- the picture in its place ------------------------------------------------------------
    def _attach_later(self):
        self._attach_pending = False
        if self.isVisible():
            self._attach()

    def _attach(self):
        if not self._mine():
            return
        if self.p.with_video:
            if not self.isVisible():
                # not in place yet (its stream was known before the page was
                # pushed): a native surface shown now would stand on the screen
                # before the page does. showEvent attaches it.
                return
            if state.app.turning():
                # the native picture cannot turn with the page (freeze_video):
                # the poster stands in until the page is still
                if not self._attach_pending:
                    self._attach_pending = True
                    QTimer.singleShot(40, self._attach_later)
                return
            box = playback.video_widget()
            if box.parentWidget() is not self.slot:
                box.setParent(self.slot)
                self.slot_layout.addWidget(box)
            self.poster.hide()
            box.show()
        else:
            self.poster.show()
            self.poster.set_waiting(False, "playing the sound only")
            if playback.container and playback.container.parentWidget() is self.slot:
                playback.container.hide()

    def showEvent(self, e):
        super().showEvent(e)
        self._place_top()
        if self.request is not None:
            self.busy(False)
            self.busy(True)         # the dots across the top while YouTube is asked (made at today's width)
        if self._mine():
            if not self.audio_only and QGuiApplication.applicationState() == Qt.ApplicationActive:
                self.p.want_video(True)
            # (the page turning in is pictured after this: the poster first,
            # the picture once it is still)
            box = playback.container
            if self.p.with_video and box is not None and box.parentWidget() is self.slot:
                box.hide()
                self.poster.show_frame(self.p.last_frame)
                self.poster.show()
                self.slot_layout.activate()
            if not self._attach_pending:
                self._attach_pending = True
                QTimer.singleShot(0, self._attach_later)
            self._on_paused(self.p.is_paused)

    def hideEvent(self, e):
        super().hideEvent(e)
        if QGuiApplication.applicationState() == Qt.ApplicationActive:
            self._window_full(False)    # (another page: not on the whole screen)
        # left for another page (a channel), or the app went away: sound only
        if self._mine() and self.p.with_video:
            if library.setting("background"):
                self.p.want_video(False)
            else:
                self.p.pause()

    def back(self) -> bool:
        if self.picker is not None:
            self.picker.back()          # the picker's list first
            return True
        if self._is_full():
            self._full_tapped(False)    # then the full screen
            return True
        freeze_video()      # (before the turn back pictures this page)
        if self.request is not None:
            state.worker.forget(self.request)
            self.request = None
        if self._mine() and self.p.playing() and not library.setting("background"):
            self.p.stop()
        for sig, slot in ((self.p.paused, self._on_paused),
                          (self.p.changed, self._on_changed), (self.p.failed, self._on_failed),
                          (self.p.mode, self._on_mode), (self.controls.full_screen, self._full_tapped),
                          (self.controls.settings, self._settings)):
            try:
                sig.disconnect(slot)
            except (RuntimeError, TypeError):
                pass
        QTimer.singleShot(1500, self._dispose)      # (after the turn away from it)
        return False

    def _dispose(self):
        """The app keeps a page it went back from, hidden: let this one go, but
        not the video surface (there is one for the app)."""
        box = playback.container
        if box is not None and box.parentWidget() is self.slot:
            box.hide()
            self.slot_layout.removeWidget(box)
            box.setParent(state.app.window)
        if self not in state.app.stack:
            self.deleteLater()

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._place_top()

    def _place_top(self):
        """Upright: the picture is 16:9 across the top. On its side: the whole
        window is the picture, and the window the whole screen (the shell
        takes its status and nav bars away). Upright again, it gives it back."""
        landscape = self.width() > self.height()
        self._landscape = landscape
        self.controls.set_full(landscape)
        self.viewport.setVisible(not landscape)
        if self.app_bar:
            self.app_bar.setVisible(not landscape)
        if not landscape:
            self._left_full = False
        if self.isVisible() and landscape != self._is_full() and not (landscape and self._left_full):
            QTimer.singleShot(0, lambda: self.isVisible() and self._window_full(self._landscape and not self._left_full))
        outer = self.layout()
        if landscape:
            self.slot.setMinimumHeight(0)
            self.slot.setMaximumHeight(16777215)
            outer.setStretch(outer.indexOf(self.top), 1)
        else:
            h = int(self.width() * 9 / 16)
            self.slot.setFixedHeight(h)
            outer.setStretch(outer.indexOf(self.top), 0)

    # -- buttons --------------------------------------------------------------------------------
    def _play_tapped(self):
        ztos.haptic("tap")
        if self._mine():
            self.p.toggle()
        elif self.info:
            self._start()
        else:
            self.play_when_ready = True

    # -- the whole screen ------------------------------------------------------------------------------
    @staticmethod
    def _is_full() -> bool:
        return bool(state.app.window.windowState() & Qt.WindowFullScreen)

    @staticmethod
    def _window_full(on: bool):
        w = state.app.window
        st = w.windowState()
        if bool(st & Qt.WindowFullScreen) != on:
            w.setWindowState(st | Qt.WindowFullScreen if on else st & ~Qt.WindowFullScreen)

    def _full_tapped(self, on: bool):
        """The full-screen button, or a swipe. Asked for upright, the shell
        turns the app on its side; left on its side, it turns it upright,
        each until the phone is turned."""
        self._left_full = not on
        self._window_full(on)

    def _settings(self):
        if self._mine() and self.picker is None:
            self.picker = PlayerPicker(self)

    def _toggle_audio(self):
        ztos.haptic("tap")
        self.audio_only = not self.audio_only
        if self._mine():
            self.p.want_video(not self.audio_only)
        self._on_mode(not self.audio_only if self._mine() else True)

    def _download(self):
        if self.live:
            ztos.notify("Tube", "a live stream can't be downloaded")
            return
        v = dict(self.summary)
        if self.info:
            v.update({k: self.info.get(k) for k in ("title", "channel", "channel_id", "duration", "views")})
        h = int(library.setting("quality"))
        ztos.menu(self.b_download, [
            (f"video ({h}p)", lambda: state.downloads.start(v, "video", h)),
            ("sound only", lambda: state.downloads.start(v, "audio")),
        ])

    def _toggle_subscribe(self):
        cid = self.summary.get("channel_id")
        if not cid:
            return
        ztos.haptic("tap")
        if library.is_subscribed(cid):
            library.unsubscribe(cid)
        else:
            library.subscribe(cid, self.summary.get("channel") or cid)
            ztos.notify("Subscribed", self.summary.get("channel") or "")
        self._show_subscribed()

    def _show_subscribed(self):
        on = library.is_subscribed(self.summary.get("channel_id") or "")
        self.b_sub.set_glyph(G_SUBSCRIBED if on else G_SUBSCRIBE)
        self.b_sub.set_label("subscribed" if on else "subscribe")

    def _more(self):
        items = []
        if self._mine():
            for h in reversed(self.p.heights()):
                mark = "  ✓" if h == self.p.height else ""
                items.append((f"{h}p{mark}", lambda h=h: self.p.set_height(h)))
        url = f"https://youtu.be/{self.summary['id']}"
        items.append(("copy link", lambda: QGuiApplication.clipboard().setText(url)))
        items.append(("open in the browser", lambda: webbrowser.open(url)))
        if self._mine():
            items.append(("stop", self._stop))
        ztos.menu(self.b_more, items)

    def _stop(self):
        self.p.stop()
        self.poster.show()
        self.poster.set_waiting(False)
        if playback.container:
            playback.container.hide()

    # -- the player's news -------------------------------------------------------------------------
    def _on_paused(self, paused: bool):
        if self._mine():
            self.b_play.set_glyph(G_PLAY if paused else G_PAUSE)
            self.b_play.set_label("play" if paused else "pause")

    def _on_changed(self, video: dict):
        if not self._mine():
            self.b_play.set_glyph(G_PLAY)
            self.b_play.set_label("play")
            self.poster.show()
            self.poster.set_waiting(False)

    def _on_failed(self, err: str):
        if self._mine():
            self.poster.show()
            self.poster.set_waiting(True, f"Can't play this video.\n{err}")

    def _on_mode(self, with_video: bool):
        self.b_audio.set_glyph(G_VIDEO if self.audio_only else G_AUDIO)
        self.b_audio.set_label("picture" if self.audio_only else "sound only")
        if self._mine() and self.isVisible():
            self._attach()


SPEEDS = (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0)


def _speed_name(speed: float) -> str:
    return "Normal" if abs(speed - 1.0) < 0.01 else f"{speed:g}x"


class PlayerPicker(QWidget):
    """"..." on the player: quality and playback speed, as Windows Phone's
    list pickers are. The whole window: the app's name in capitals, the
    setting in large lowercase, its value under it in the accent; tapped, the
    choices in a list of their own, the chosen one in the accent. The picture
    stands still under it (freeze_video); back closes it a level at a time."""
    ROW_MAIN = 72
    ROW = 56

    def __init__(self, page: "VideoPage"):
        host = state.app.window
        super().__init__(host)
        self.page = page
        self.p = page.p
        self.level = "main"
        freeze_video()
        self.setGeometry(host.rect())
        self._t = 0.0
        self._closing = False
        self._timer = QTimer(self, interval=16)
        self._timer.timeout.connect(self._step)
        self._timer.start()
        ztos.haptic("tap")
        self.show()
        self.raise_()

    def _speed(self) -> float:
        try:
            return float(self.p.mpv.speed or 1.0)
        except Exception:
            return 1.0

    def _rows(self) -> list:
        """(key, text, value or chosen)"""
        if self.level == "main":
            h = self.p.height
            return [("quality", "quality", f"{h}p" if h else "automatic"),
                    ("speed", "playback speed", _speed_name(self._speed()).lower())]
        if self.level == "quality":
            return [(h, f"{h}p", h == self.p.height) for h in reversed(self.p.heights())]
        return [(v, _speed_name(v).lower(), abs(v - self._speed()) < 0.01) for v in SPEEDS]

    def _header(self) -> str:
        return {"main": "TUBE", "quality": "CHOOSE QUALITY", "speed": "CHOOSE PLAYBACK SPEED"}[self.level]

    def _top(self) -> float:
        return 20.0 if self.width() > self.height() else 28.0

    def _row_h(self) -> float:
        return self.ROW_MAIN if self.level == "main" else self.ROW

    def _per_column(self) -> int:
        """Rows a column holds: on its side the list goes on in a second
        column (and a third) instead of off the screen"""
        room = self.height() - self._top() - 44 - 8
        return max(1, int(room // self._row_h()))

    def _place(self, i: int) -> QPointF:
        n = self._per_column()
        cols = -(-len(self._rows()) // n)
        col_w = (self.width() - ztos.MARGIN - 8) / max(1, cols) if cols > 1 else self.width()
        return QPointF(ztos.MARGIN + 8 + (i // n) * col_w, self._top() + 44 + (i % n) * self._row_h())

    def _step(self):
        # in as Windows Phone's pages come: up a little, from nothing
        self._t = max(0.0, self._t - 0.14) if self._closing else min(1.0, self._t + 0.14)
        self.update()
        if self._t in (0.0, 1.0):
            self._timer.stop()
            if self._closing:
                self.page.picker = None
                self.deleteLater()
                if self.page.isVisible():
                    self.page._attach()     # the picture back in its place

    def close_picker(self):
        if not self._closing:
            self._closing = True
            self._timer.start()

    def back(self):
        if self.level != "main":
            self.level = "main"
            self.update()
        else:
            self.close_picker()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing)
        ease = 1 - (1 - self._t) ** 3
        p.fillRect(self.rect(), QColor(0, 0, 0, int(255 * min(1.0, ease * 1.2))))
        p.setOpacity(ease)
        p.translate(0, 40 * (1 - ease))
        x = float(ztos.MARGIN + 8)
        y = self._top()
        p.setPen(QColor("white"))
        p.setFont(ztos._font(ztos.FONT_SEMIBOLD, 13))
        fm = QFontMetricsF(p.font())
        p.drawText(QPointF(x, y + fm.ascent()), self._header())
        for i, (_key, text, value) in enumerate(self._rows()):
            at = self._place(i)
            x, y = at.x(), at.y()
            if self.level == "main":
                p.setPen(QColor("white"))
                p.setFont(ztos._font(ztos.FONT_SEMILIGHT, 26))
                fm = QFontMetricsF(p.font())
                p.drawText(QPointF(x, y + fm.ascent()), text)
                p.setPen(ztos.accent())
                p.setFont(ztos._font(ztos.FONT_REGULAR, 14))
                fm2 = QFontMetricsF(p.font())
                p.drawText(QPointF(x, y + fm.height() + fm2.ascent()), value)
            else:
                p.setPen(ztos.accent() if value else QColor("white"))
                p.setFont(ztos._font(ztos.FONT_LIGHT, 30))
                fm = QFontMetricsF(p.font())
                p.drawText(QPointF(x, y + fm.ascent()), text)

    def mousePressEvent(self, e):
        pass

    def mouseReleaseEvent(self, e):
        rows = self._rows()
        pos = e.position()
        # (the last first: a row's box runs on under the columns to its right)
        i = next((i for i in reversed(range(len(rows))) if QRectF(self._place(i), QSizeF(
            self.width(), self._row_h())).contains(pos)), -1)
        if i < 0:
            return
        key = rows[i][0]
        ztos.haptic("tap")
        if self.level == "main":
            self.level = key
            self.update()
            return
        if self.level == "quality":
            self.p.set_height(key)
        else:
            self.p.mpv.speed = key
        self.close_picker()


class ChannelPage(ztos.Page):
    """A channel's newest videos, and the subscribe button."""

    def __init__(self, channel_id: str, name: str = ""):
        super().__init__((name or "channel").lower())
        self.channel_id = channel_id
        self.name = name
        self.avatar = ""
        self.loaded = False
        self.request = None
        self.head = self.add(ChannelRow(name or channel_id, "", ""))
        self.space(8)
        self.section("LATEST VIDEOS")
        self.rows = QWidget()
        self.rows_layout = QVBoxLayout(self.rows)
        self.rows_layout.setContentsMargins(0, 0, 0, 0)
        self.rows_layout.setSpacing(0)
        self.add(self.rows)
        self.b_sub = ztos.RoundButton(G_SUBSCRIBE, "subscribe", self._toggle)
        self.bar(self.b_sub, ztos.RoundButton("refresh", "refresh", self._load))
        self._show_subscribed()

    def shown(self):
        if not self.loaded:
            self._load()

    def back(self) -> bool:
        if self.request:
            state.worker.forget(self.request)
        return False

    def _load(self):
        self.loaded = True
        self.busy(True)
        self.request = state.worker.call("channel", self._got, self._failed, channel_id=self.channel_id)

    def _got(self, data: dict):
        self.request = None
        self.busy(False)
        self.channel_id = data.get("channel_id") or self.channel_id
        self.name = data.get("channel") or self.name
        self.title = (self.name or "channel").lower()
        self.avatar = data.get("avatar") or ""
        head = ChannelRow(self.name, count(data.get("subscribers"), "subscribers"), self.avatar)
        self.list.replaceWidget(self.head, head)
        self.head.deleteLater()
        self.head = head
        if library.is_subscribed(self.channel_id) and self.avatar:
            # keep the picture for the subscription list
            for s in library._db()["subscriptions"]:
                if s["id"] == self.channel_id:
                    s["avatar"], s["name"] = self.avatar, self.name or s["name"]
            library.save()
        _clear(self.rows_layout)
        for v in data.get("items") or []:
            self.rows_layout.addWidget(VideoRow(v, on_tap=lambda v=v: open_video(v), show_channel=False,
                                                on_hold=None))
        if not data.get("items"):
            self.rows_layout.addWidget(note("No videos."))
        for i in range(self.rows_layout.count()):
            w = self.rows_layout.itemAt(i).widget()
            if isinstance(w, VideoRow):
                w.held.connect(lambda w=w: video_menu(w))
        self._show_subscribed()
        self._changed(explicit=True)

    def _failed(self, err: str):
        self.request = None
        self.busy(False)
        _clear(self.rows_layout)
        self.rows_layout.addWidget(note(f"Couldn't load the channel: {err}"))

    def _toggle(self):
        ztos.haptic("tap")
        if library.is_subscribed(self.channel_id):
            library.unsubscribe(self.channel_id)
        else:
            library.subscribe(self.channel_id, self.name or self.channel_id, self.avatar)
        self._show_subscribed()

    def _show_subscribed(self):
        on = library.is_subscribed(self.channel_id)
        self.b_sub.set_glyph(G_SUBSCRIBED if on else G_SUBSCRIBE)
        self.b_sub.set_label("subscribed" if on else "subscribe")


class ChannelsPage(ztos.Page):
    """Your subscriptions; hold one to unsubscribe."""

    def __init__(self, on_change):
        super().__init__("channels")
        self.on_change = on_change
        self.fill()

    def fill(self):
        self.clear()
        exports = library.newpipe_exports()
        for path in exports:
            self.entry("import subscriptions", os.path.basename(path), lambda path=path: self._import(path))
        if exports:
            self.space(8)
        subs = library.subscriptions()
        if not subs:
            self.add(note("No channels yet. Open a channel and tap subscribe, or export your "
                          "subscriptions from NewPipe (Settings > Content) or Google Takeout into Downloads."))
        for s in subs:
            row = ChannelRow(s["name"], "", s.get("avatar") or "",
                             on_tap=lambda s=s: open_channel(s["id"], s["name"]))
            row.held.connect(lambda row=row, s=s: ztos.menu(row, [("unsubscribe", lambda: self._drop(s))]))
            self.add(row)

    def _import(self, path: str):
        try:
            n = library.import_newpipe(path)
            ztos.notify("Subscriptions", f"{n} channels added from {os.path.basename(path)}")
        except (OSError, ValueError) as e:
            ztos.notify("Subscriptions", f"couldn't read {os.path.basename(path)}: {e}")
        self.fill()
        self.on_change()

    def _drop(self, s: dict):
        library.unsubscribe(s["id"])
        self.fill()
        self.on_change()


class SettingsPage(ztos.Page):
    def __init__(self):
        super().__init__("settings")
        self.section("HIGHEST QUALITY")
        self.add(Words([("360", "360p"), ("480", "480p"), ("720", "720p"), ("1080", "1080p")],
                       library.setting("quality"), lambda k: library.set_setting("quality", k)))
        self.toggle("60 frames a second", library.setting("fps60"),
                    lambda on: library.set_setting("fps60", on))
        self.text("Smoother where a video has it, for twice the data.", size=12, color=ztos.GREY)
        self.toggle("Play in the background", library.setting("background"),
                    lambda on: library.set_setting("background", on))
        self.toggle("Start where I stopped", library.setting("resume"),
                    lambda on: library.set_setting("resume", on))
        self.space(8)
        self.section("ABOUT")
        self.version = self.entry("YouTube extractor", f"yt-dlp {state.worker.version or '...'}",
                                  self._update)
        self.text("Tube is a NewPipe-style YouTube client for ztOS: no account, no ads, "
                  "subscriptions kept on this phone.", size=12, color=ztos.GREY)

    def _update(self):
        self.version.set_detail("checking for a newer yt-dlp...")
        state.worker.call("update", lambda d: self.version.set_detail(
            f"yt-dlp {d.get('version')}" + (", restarting" if d.get("restarting") else ", up to date")),
            lambda e: self.version.set_detail(f"couldn't check: {e}"))


def _clear(layout):
    while layout.count():
        item = layout.takeAt(0)
        if item.widget():
            item.widget().deleteLater()
