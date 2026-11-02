"""The player's controls, drawn over the picture in the phone's own design.

The picture is a native surface of its own (player.VideoSurface): nothing of
Qt's widgets can be over it, so the controls are painted into that surface,
after mpv's frame, and its touches come here.

They work as YouTube's do. A tap shows them (the picture dimmed, play/pause
in a ring in the middle, the slider with the time under it, the full-screen
glyph, "..." for quality and speed) and another tap hides them; playing, they
go by themselves after 3 s. Two taps on either side jump ten seconds, more
taps further. Holding plays at twice the speed. The slider is dragged to
seek. Up in a swipe goes full screen, down leaves it. Full screen, two
fingers spread fill the screen with the picture (its edges cut off), and
pinched together fit it again.

They look as Windows Phone 8.1's do: flat, square, Segoe, the accent colour
from Settings, the SDK's round buttons, its five-dot progress ring.
"""
import math
import time

from PySide6.QtCore import QObject, QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFontMetricsF, QPainter, QPen

import ztos

HIDE_MS = 3000
FADE_MS = 150
DOUBLE_MS = 300                     # two taps closer than this are a double tap
HOLD_MS = 500                       # held this long: twice the speed
SEEK_STEP = 10

# Segoe MDL2 Assets
G_FULL, G_WINDOW = "", ""
G_MORE = ""
G_BACK = ""
G_FAST = ""
G_PLAY, G_PAUSE, G_AGAIN = "", "", ""     # as the app bar's


def clock(seconds: float) -> str:
    s = int(max(0, seconds))
    h, m, s = s // 3600, s // 60 % 60, s % 60
    return f"{h}:{m:02}:{s:02}" if h else f"{m}:{s:02}"


class Controls(QObject):
    full_screen = Signal(bool)      # the glyph, or a swipe: True to go full screen
    settings = Signal()             # "...": quality and speed
    changed = Signal()              # something to draw

    def __init__(self, player):
        super().__init__()
        self.p = player
        self.full = False           # the page has the whole screen (set_full)
        self.zoomed = False         # full screen, the picture fills it (cut to its shape)
        self.note = ""              # a word on a band of the accent for a moment ("ZOOMED TO FILL")
        self.note_at = 0.0
        self.title = ""
        self.shown = False
        self.alpha = 0.0            # how far the controls have faded in
        self.buffering = False
        self.scrub: float | None = None     # the time under the finger, dragging the slider
        self.pressed = ""                   # the button under the finger (filled with the accent)
        self.ripple_side = 0                # -1 left, 1 right: the double-tap jump on show
        self.ripple_total = 0
        self.ripple_at = 0.0
        self.fast = False                   # held: twice the speed
        self._speed_before = 1.0
        self._down: QPointF | None = None
        self._moved = False
        self._on_bar = False
        self._last_tap = 0.0
        self._last_tap_x = 0.0
        self._w = self._h = 1.0
        self._hide = QTimer(self, singleShot=True, interval=HIDE_MS)
        self._hide.timeout.connect(self._auto_hide)
        self._tap = QTimer(self, singleShot=True, interval=DOUBLE_MS)
        self._tap.timeout.connect(self._single_tap)
        self._hold = QTimer(self, singleShot=True, interval=HOLD_MS)
        self._hold.timeout.connect(self._held)
        self._fade = QTimer(self, interval=16)
        self._fade.timeout.connect(self._fade_step)
        self._fade_from = self._fade_to = 0.0
        self._fade_at = 0.0
        self._ripple = QTimer(self, interval=33)
        self._ripple.timeout.connect(self._ripple_step)
        self._note = QTimer(self, singleShot=True, interval=1200)
        self._note.timeout.connect(self._note_gone)
        player.paused.connect(self._on_paused)
        player.buffering.connect(self._on_buffering)
        player.ended.connect(self._on_ended)
        player.position.connect(lambda *_: self.shown and self.changed.emit())

    # -- the whole screen, and the picture filling it -------------------------------------------
    def set_full(self, full: bool):
        if full != self.full:
            self.full = full
            if not full and self.zoomed:
                self.zoom(False, say=False)     # (upright, the picture fits its slot again)

    def zoom(self, on: bool, say: bool = True):
        """Fill the screen (mpv's panscan: the picture as large as it takes
        to leave no bars, its edges cut off) or fit it"""
        if on and not self.full:
            return
        if on != self.zoomed:
            self.zoomed = on
            try:
                self.p.mpv["panscan"] = 1.0 if on else 0.0
            except Exception:
                pass
            if say:
                ztos.haptic("tick")
                self.note, self.note_at = "ZOOMED TO FILL" if on else "ORIGINAL", time.monotonic()
                self._note.start()
            self.changed.emit()

    def _note_gone(self):
        self.note = ""
        self.changed.emit()

    def cancel(self):
        """A second finger: what the first one started is not a tap, a hold
        or a drag (a pinch, see player.VideoSurface)"""
        self._down = None
        self._on_bar = False
        self.scrub = None
        self.pressed = ""
        self._hold.stop()
        self._tap.stop()
        if self.fast:
            self.fast = False
            self.p.mpv.speed = self._speed_before
        self.changed.emit()

    # -- showing and hiding ---------------------------------------------------------------------
    def show(self, keep: bool = False):
        if not self.shown:
            self.shown = True
            self._fade_to_(1.0)
        self._hide.stop()
        if not keep and self.p.playing():
            self._hide.start()
        self.changed.emit()

    def hide(self):
        self._hide.stop()
        if self.shown:
            self.shown = False
            self._fade_to_(0.0)

    def _auto_hide(self):
        if self.p.playing() and self.scrub is None:
            self.hide()

    def _fade_to_(self, to: float):
        self._fade_from, self._fade_to, self._fade_at = self.alpha, to, time.monotonic()
        self._fade.start()

    def _fade_step(self):
        t = min(1.0, (time.monotonic() - self._fade_at) * 1000 / FADE_MS)
        self.alpha = self._fade_from + (self._fade_to - self._fade_from) * t
        if t >= 1.0:
            self._fade.stop()
        self.changed.emit()

    def _on_paused(self, paused: bool):
        if self.shown:
            self.show(keep=paused)      # paused, they stay; playing again, the clock starts
        self.changed.emit()

    def _on_buffering(self, on: bool):
        self.buffering = on
        self.changed.emit()

    def _on_ended(self):
        # (mpv says so for a moment as a file loads, too)
        if self.p.dur > 0 and self.p.pos >= self.p.dur - 1.5:
            self.show(keep=True)        # the end: again, in the middle

    def _ended(self) -> bool:
        return bool(self.p.video) and self.p.is_paused and self.p.dur > 0 and self.p.pos >= self.p.dur - 1.0

    def _live(self) -> bool:
        return bool((self.p.video or {}).get("live"))

    # -- where things are ------------------------------------------------------------------------
    def _bar(self) -> tuple:
        """The slider: left, right, y"""
        w, h = self._w, self._h
        if self.full:
            return 24.0, w - 64.0, h - 44.0
        return 12.0, w - 48.0, h - 30.0

    def _buttons(self) -> dict:
        """name: (centre, radius) of what a tap on the controls can press"""
        w, h = self._w, self._h
        _, right, y = self._bar()
        d = {"play": (QPointF(w / 2, h / 2 - (6 if not self.full else 0)), 26 if not self.full else 30),
             "more": (QPointF(w - 22, 20 if not self.full else 26), 16),
             "full": (QPointF((right + w) / 2 + 2, y), 16)}
        if self.full:
            d["back"] = (QPointF(26, 26), 16)
        return d

    def _hit(self, pos: QPointF):
        for name, (c, r) in self._buttons().items():
            if math.hypot(pos.x() - c.x(), pos.y() - c.y()) <= r + 10:
                return name
        return None

    def _time_at(self, x: float) -> float:
        left, right, _ = self._bar()
        return max(0.0, min(1.0, (x - left) / max(1.0, right - left))) * (self.p.dur or 0)

    # -- touch (as mouse events: Wayland's touch emulation) --------------------------------------
    def press(self, pos: QPointF, w: float, h: float):
        self._w, self._h = w, h
        self._down, self._moved = pos, False
        left, right, y = self._bar()
        # the slider is held where it is drawn, give or take a finger
        self._on_bar = self.shown and not self._live() and self.p.dur > 0 and abs(pos.y() - y) < 22 \
            and left - 12 <= pos.x() <= right + 12
        if self._on_bar:
            self.scrub = self._time_at(pos.x())
            self._hide.stop()
            self.changed.emit()
            return
        if self.shown and self.alpha > 0.5:
            self.pressed = self._hit(pos) or ""
            if self.pressed:
                self.changed.emit()
                return
        self._hold.start()

    def move(self, pos: QPointF):
        if self._down is None:
            return
        dx, dy = pos.x() - self._down.x(), pos.y() - self._down.y()
        if self._on_bar:
            self.scrub = self._time_at(pos.x())
            self.changed.emit()
            return
        if abs(dx) + abs(dy) > 16:
            self._moved = True
            self._hold.stop()
            if self.pressed:
                self.pressed = ""
                self.changed.emit()

    def release(self, pos: QPointF):
        if self._down is None:
            return
        down, self._down = self._down, None
        self._hold.stop()
        if self._on_bar:
            self._on_bar = False
            if self.scrub is not None:
                self.p.seek(self.scrub)
                self.p.pos = self.scrub         # (the slider stays where it was let go)
            self.scrub = None
            self.show()
            return
        if self.pressed:
            name, self.pressed = self.pressed, ""
            self._tap.stop()
            self._button(name)
            return
        if self.fast:
            self.fast = False
            self.p.mpv.speed = self._speed_before
            self.changed.emit()
            return
        dy = pos.y() - down.y()
        if self._moved:
            if dy < -60 and not self.full:
                self.full_screen.emit(True)
            elif dy > 60 and self.full:
                self.full_screen.emit(False)
            return
        now = time.monotonic()
        side = -1 if pos.x() < self._w * 0.4 else 1 if pos.x() > self._w * 0.6 else 0
        # a run of taps on one side keeps jumping
        if side and (now - self._last_tap) * 1000 < DOUBLE_MS * 2 and self.ripple_side == side \
                and now - self.ripple_at < 0.9:
            self._jump(side)
            self._last_tap = now
            return
        if side and (now - self._last_tap) * 1000 < DOUBLE_MS and (self._last_tap_x < self._w / 2) == (side < 0):
            self._tap.stop()
            self._jump(side)
            self._last_tap = now
            return
        self._last_tap, self._last_tap_x = now, pos.x()
        self._tap.start()

    def _single_tap(self):
        if self.shown:
            self.hide()
        else:
            self.show()

    def _held(self):
        if self._down is None or self._moved or not self.p.playing():
            return
        ztos.haptic("tap")
        self.fast = True
        self._speed_before = float(self.p.mpv.speed or 1.0)
        self.p.mpv.speed = 2.0
        self.hide()
        self.changed.emit()

    def _button(self, name: str):
        ztos.haptic("tap")
        if name == "play":
            if self._ended():
                self.p.seek(0)
                self.p.mpv.pause = False
            else:
                self.p.toggle()
            self.show()
        elif name == "full":
            self.full_screen.emit(not self.full)
        elif name == "back":
            self.full_screen.emit(False)
        elif name == "more":
            self._hide.stop()
            self.settings.emit()
        self.changed.emit()

    def _jump(self, side: int):
        if not self.p.video or self._live():
            return
        ztos.haptic("tick")
        if self.ripple_side != side or time.monotonic() - self.ripple_at > 0.9:
            self.ripple_total = 0
        self.ripple_side = side
        self.ripple_total += SEEK_STEP
        self.ripple_at = time.monotonic()
        self.p.seek(side * SEEK_STEP, relative=True)
        self.hide()
        self._ripple.start()
        self.changed.emit()

    def _ripple_step(self):
        if time.monotonic() - self.ripple_at > 0.9:
            self._ripple.stop()
            self.ripple_side = 0
        self.changed.emit()

    # -- drawing ----------------------------------------------------------------------------------
    def paint(self, p: QPainter, w: float, h: float):
        self._w, self._h = w, h
        p.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing)
        a = 1.0 if self.scrub is not None else self.alpha
        if a > 0.01:
            p.fillRect(QRectF(0, 0, w, h), QColor(0, 0, 0, int(140 * a)))
            p.setOpacity(a)
            self._paint_controls(p, w, h)
            p.setOpacity(1.0)
        elif not self.full and self.p.dur > 0 and not self._live():
            # upright, a thin line of the accent along the bottom
            done = w * max(0.0, min(1.0, self.p.pos / self.p.dur))
            p.fillRect(QRectF(0, h - 2, done, 2), ztos.accent())
        if self.ripple_side:
            self._paint_jump(p, w, h)
        if self.fast:
            self._paint_band(p, w, "2x SPEED", G_FAST)
        elif self.note:
            self._paint_band(p, w, self.note, "")
        if self.buffering and not self.p.is_paused:
            self._paint_ring(p, QPointF(w / 2, h / 2), 20 if not self.full else 26)

    def _glyph(self, p: QPainter, c: QPointF, glyph: str, size: float):
        p.setPen(QColor("white"))
        p.setFont(ztos._font(ztos.FONT_SYMBOLIC, size))
        p.drawText(QRectF(c.x() - 24, c.y() - 24, 48, 48), Qt.AlignCenter, glyph)

    def _ring(self, p: QPainter, c: QPointF, r: float, name: str):
        """The SDK's round button: a white ring, the accent inside while pressed"""
        if self.pressed == name:
            p.setPen(Qt.NoPen)
            p.setBrush(ztos.accent())
            p.drawEllipse(c, r, r)
        p.setPen(QPen(QColor("white"), 2.2))
        p.setBrush(Qt.NoBrush)
        p.drawEllipse(c, r - 1.1, r - 1.1)

    def _paint_controls(self, p: QPainter, w: float, h: float):
        b = self._buttons()
        white = QColor("white")
        # the middle: play, pause, or again at the end
        c, r = b["play"]
        if not (self.buffering and not self.p.is_paused):
            self._ring(p, c, r, "play")
            # (the font's glyphs, as the app bar under the picture has them: the
            # SDK's drawn ones are outlines, and its pause read as two boxes)
            glyph = G_AGAIN if self._ended() else G_PLAY if self.p.is_paused else G_PAUSE
            self._glyph(p, c, glyph, r * 0.7)
        # the top: "..." (and full screen, back and the title)
        c, _ = b["more"]
        if self.pressed == "more":
            p.fillRect(QRectF(c.x() - 16, c.y() - 16, 32, 32), ztos.accent())
        self._glyph(p, c, G_MORE, 16)
        if self.full:
            c, _ = b["back"]
            if self.pressed == "back":
                p.fillRect(QRectF(c.x() - 16, c.y() - 16, 32, 32), ztos.accent())
            self._glyph(p, c, G_BACK, 16)
            if self.title:
                p.setPen(white)
                p.setFont(ztos._font(ztos.FONT_SEMILIGHT, 17))
                fm = QFontMetricsF(p.font())
                text = fm.elidedText(self.title, Qt.ElideRight, w - 120)
                p.drawText(QPointF(c.x() + 24, c.y() + fm.ascent() / 2 - 3), text)
        # the bottom: the slider, the time under its ends, the full-screen glyph
        c, _ = b["full"]
        if self.pressed == "full":
            p.fillRect(QRectF(c.x() - 16, c.y() - 16, 32, 32), ztos.accent())
        self._glyph(p, c, G_WINDOW if self.full else G_FULL, 16)
        left, right, y = self._bar()
        p.setFont(ztos._font(ztos.FONT_REGULAR, 11.5 if not self.full else 12.5))
        fm = QFontMetricsF(p.font())
        ty = y + 10 + fm.ascent()
        if self._live():
            p.fillRect(QRectF(left, y - 1, right - left, 2), ztos.accent())
            p.setPen(white)
            p.drawText(QPointF(left, ty), "LIVE")
            return
        if self.p.dur <= 0:
            return
        pos = self.scrub if self.scrub is not None else self.p.pos
        self._paint_slider(p, left, right, y, pos)
        p.setPen(white)
        p.drawText(QPointF(left, ty), clock(pos))
        total = clock(self.p.dur)
        p.setPen(ztos.GREY)
        p.drawText(QPointF(right - fm.horizontalAdvance(total), ty), total)

    def _paint_slider(self, p: QPainter, left: float, right: float, y: float, pos: float):
        """Windows Phone's slider: a flat track, the accent up to the thumb, the
        stream read so far a shade lighter, and a white bar for the thumb"""
        dur = self.p.dur or 1.0
        done = left + (right - left) * max(0.0, min(1.0, pos / dur))
        cached = left + (right - left) * max(0.0, min(1.0, (self.p.cached or 0.0) / dur))
        th = 4.0
        p.fillRect(QRectF(left, y - th / 2, right - left, th), QColor(255, 255, 255, 60))
        if cached > done:
            p.fillRect(QRectF(done, y - th / 2, cached - done, th), QColor(255, 255, 255, 110))
        p.fillRect(QRectF(left, y - th / 2, done - left, th), ztos.accent())
        tw, tall = (8.0, 22.0) if self.scrub is not None else (6.0, 18.0)
        p.fillRect(QRectF(done - tw / 2, y - tall / 2, tw, tall), QColor("white"))
        if self.scrub is not None:
            # the time where the finger is, above it, large
            p.setFont(ztos._font(ztos.FONT_LIGHT, 28 if self.full else 22))
            fm = QFontMetricsF(p.font())
            text = clock(self.scrub)
            tw = fm.horizontalAdvance(text)
            x = max(8.0, min(self._w - tw - 8, done - tw / 2))
            p.setPen(QColor("white"))
            p.drawText(QPointF(x, y - 20), text)

    def _paint_jump(self, p: QPainter, w: float, h: float):
        """Two taps on a side: that side goes a shade darker and says how far,
        flat, in Segoe Light"""
        age = time.monotonic() - self.ripple_at
        a = 1.0 if age < 0.6 else max(0.0, 1 - (age - 0.6) / 0.3)
        left = self.ripple_side < 0
        band = QRectF(0, 0, w * 0.4, h) if left else QRectF(w * 0.6, 0, w * 0.4, h)
        p.fillRect(band, QColor(0, 0, 0, int(110 * a)))
        p.setOpacity(a)
        p.setPen(QColor("white"))
        p.setFont(ztos._font(ztos.FONT_LIGHT, 34 if self.full else 28))
        fm = QFontMetricsF(p.font())
        text = f"{'-' if left else '+'}{self.ripple_total}"
        cy = h / 2
        p.drawText(QPointF(band.center().x() - fm.horizontalAdvance(text) / 2, cy + fm.ascent() / 2 - 8), text)
        p.setFont(ztos._font(ztos.FONT_SEMIBOLD, 11))
        fm2 = QFontMetricsF(p.font())
        p.drawText(QPointF(band.center().x() - fm2.horizontalAdvance("SECONDS") / 2, cy + 26), "SECONDS")
        p.setOpacity(1.0)

    def _paint_band(self, p: QPainter, w: float, text: str, glyph: str):
        """A word at the top on a flat band of the accent: "2x SPEED" while
        held, "ZOOMED TO FILL" for a moment"""
        p.setFont(ztos._font(ztos.FONT_SEMIBOLD, 12))
        fm = QFontMetricsF(p.font())
        extra = 44 if glyph else 24
        r = QRectF((w - fm.horizontalAdvance(text) - extra) / 2, 12, fm.horizontalAdvance(text) + extra, 26)
        p.fillRect(r, ztos.accent())
        p.setPen(QColor("white"))
        p.drawText(r.adjusted(12, 0, 0, 0), Qt.AlignLeft | Qt.AlignVCenter, text)
        if glyph:
            p.setFont(ztos._font(ztos.FONT_SYMBOLIC, 11))
            p.drawText(r.adjusted(0, 0, -12, 0), Qt.AlignRight | Qt.AlignVCenter, glyph)

    def _paint_ring(self, p: QPainter, c: QPointF, r: float):
        """Windows Phone's progress ring: five dots chasing round, each
        slowing at the top and hurrying through the bottom"""
        t = time.monotonic() % 2.0
        p.setPen(Qt.NoPen)
        p.setBrush(QColor("white"))
        for i in range(5):
            u = (t - i * 0.12) / 1.6
            if not 0.0 <= u <= 1.0:
                continue
            # an ease that lingers as the dots pass the top, so they bunch there
            a = -math.pi / 2 + 2 * math.pi * (u + 0.25 * math.sin(2 * math.pi * u) / (2 * math.pi) * 2.2)
            p.drawEllipse(QPointF(c.x() + r * math.cos(a), c.y() + r * math.sin(a)), 2.4, 2.4)
        self.changed.emit()         # (it turns while nothing else draws)
