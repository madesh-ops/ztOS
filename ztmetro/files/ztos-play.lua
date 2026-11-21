-- ztos-play.lua: a touch player UI for mpv as ztOS's video player
-- (files/ztos-play runs mpv with --osc=no and this script).
--
-- Liquid glass, laid out for a phone, in proportion to the screen's short side:
--   a tap shows the controls (and hides them again); while playing they go
--   after 3.5 s by themselves, while paused they stay;
--   in the middle a big play/pause button, with -10 s and +10 s beside it;
--   along the bottom a thick seek bar to tap or drag, the time played and
--   the time left above it; at the top a back button and the file's name;
--   a double tap on the left or right third seeks 10 s back or forward, in
--   the middle it pauses or plays.
-- The shell's back button arrives as Escape and quits (mpv-input.conf).

local SEEK = 10
local HIDE_AFTER = 3.5
local ACCENT = "EF5000"		-- cobalt 0x0050ef, as ASS BBGGRR
local K = 0.5523		-- a circle from four beziers

local overlay = mp.create_osd_overlay("ass-events")
local visible = false
local dragging = false
local hide_timer = nil
local tap_timer = nil
local last_up = 0
local last_drawn = ""

local function geometry()
	local w, h = mp.get_osd_size()
	if not w or w <= 0 or not h or h <= 0 then return nil end
	local u = math.min(w, h) / 12		-- 90 px on the M20's 1080-wide screen
	local g = { w = w, h = h, u = u, cx = w / 2, cy = h / 2 }
	-- the top pill: back, and the name
	g.top = { 0.45 * u, 0.45 * u, w - 0.45 * u, 1.75 * u }
	g.back_x, g.back_y = 1.15 * u, 1.1 * u
	-- the middle capsule: -10, play/pause, +10
	-- the middle: three glass buttons of their own, -10, play/pause, +10
	g.seek_dx = 2.75 * u
	g.play_r = 1.1 * u
	g.seek_r = 0.8 * u
	-- the bottom pill: the times over the seek bar
	g.bot = { 0.45 * u, h - 2.75 * u, w - 0.45 * u, h - 1.05 * u }
	g.bar_x1, g.bar_x2, g.bar_y = 1.2 * u, w - 1.2 * u, h - 1.65 * u
	g.bar_t, g.knob_r = 0.13 * u, 0.3 * u
	g.time_y = h - 2.25 * u
	return g
end

local function circle(x, y, r)
	local k = K * r
	return string.format("m %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d",
		x, y - r,
		x + k, y - r, x + r, y - k, x + r, y,
		x + r, y + k, x + k, y + r, x, y + r,
		x - k, y + r, x - r, y + k, x - r, y,
		x - r, y - k, x - k, y - r, x, y - r)
end

local function rect(x1, y1, x2, y2)
	return string.format("m %d %d l %d %d l %d %d l %d %d", x1, y1, x2, y1, x2, y2, x1, y2)
end

-- a rectangle with round corners of radius r (a pill when r is half its height)
local function rrect(x1, y1, x2, y2, r)
	r = math.min(r, (x2 - x1) / 2, (y2 - y1) / 2)
	local k = K * r
	return string.format("m %d %d l %d %d b %d %d %d %d %d %d l %d %d b %d %d %d %d %d %d " ..
		"l %d %d b %d %d %d %d %d %d l %d %d b %d %d %d %d %d %d",
		x1 + r, y1, x2 - r, y1,
		x2 - r + k, y1, x2, y1 + r - k, x2, y1 + r,
		x2, y2 - r,
		x2, y2 - r + k, x2 - r + k, y2, x2 - r, y2,
		x1 + r, y2,
		x1 + r - k, y2, x1, y2 - r + k, x1, y2 - r,
		x1, y1 + r,
		x1, y1 + r - k, x1 + r - k, y1, x1 + r, y1)
end

-- style: fill colour and alpha, optionally an outline (rim) and a blur
local function shape(path, color, alpha, extra)
	return string.format("{\\an7\\pos(0,0)\\bord0\\shad0%s\\1c&H%s&\\1a&H%s&\\p1}%s{\\p0}",
		extra or "", color, alpha or "00", path)
end

local function text(x, y, size, s, align, bold)
	return string.format("{\\an%d\\pos(%d,%d)\\fs%d\\b%d\\bord%d\\shad0\\3c&H000000&\\3a&HA0&\\1c&HFFFFFF&}%s",
		align or 5, x, y, size, bold and 1 or 0, math.max(1, size / 16), s)
end

-- Liquid glass, as near as libass comes (it cannot blur what is under it):
-- a soft shadow, a clear body lightly frosted, a bright rim, and a sheen
-- along the top, the light caught in the glass.
local function glass(ev, box, u)
	local x1, y1, x2, y2 = box[1], box[2], box[3], box[4]
	local r = (y2 - y1) / 2
	ev[#ev + 1] = shape(rrect(x1, y1 + 0.08 * u, x2, y2 + 0.08 * u, r), "000000", "DC",
		string.format("\\blur%d", math.floor(0.22 * u)))
	-- the body: a light grey frost, three quarters clear
	ev[#ev + 1] = shape(rrect(x1, y1, x2, y2, r), "8A8480", "C4")
	ev[#ev + 1] = shape(rrect(x1 + 0.35 * r, y1 + 0.07 * (y2 - y1), x2 - 0.35 * r, y1 + 0.42 * (y2 - y1), r * 0.6),
		"FFFFFF", "C8", string.format("\\blur%d", math.floor(0.12 * u)))
	ev[#ev + 1] = string.format("{\\an7\\pos(0,0)\\bord%d\\shad0\\1a&HFF&\\3c&HFFFFFF&\\3a&H70&\\p1}%s{\\p0}",
		math.max(1, math.floor(0.035 * u)), rrect(x1, y1, x2, y2, r))
end

-- a round glass button of radius r at (x, y)
local function glass_button(ev, x, y, r, u)
	glass(ev, { x - r, y - r, x + r, y + r }, u)
end

local function clock(t)
	if not t or t < 0 then t = 0 end
	t = math.floor(t)
	local h, m, s = math.floor(t / 3600), math.floor(t % 3600 / 60), t % 60
	if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
	return string.format("%d:%02d", m, s)
end

local function render()
	local g = geometry()
	if not visible or not g then
		if last_drawn ~= "" then
			overlay:remove()
			last_drawn = ""
		end
		return
	end
	local paused = mp.get_property_bool("pause")
	local pos = mp.get_property_number("time-pos") or 0
	local dur = mp.get_property_number("duration") or 0
	local frac = dur > 0 and math.max(0, math.min(1, pos / dur)) or 0
	local u = g.u
	local ev = {}
	-- (No shade over the picture: on the M20 any overlay costs mpv some
	-- frames while shown, a full-screen one 20 of 60 a second; the glass
	-- pieces are kept to the controls themselves.)
	-- top: back, and the name, bigger
	glass(ev, g.top, u)
	ev[#ev + 1] = text(g.back_x, g.back_y, 0.95 * u, "‹", 5, true)
	local name = mp.get_property("media-title") or ""
	name = name:gsub("%.[%w]+$", "")
	if #name > 24 then name = name:sub(1, 22) .. "…" end
	ev[#ev + 1] = text(1.85 * u, g.back_y, 0.56 * u, name, 4, true)
	-- middle: -10, play/pause, +10, each a glass button of its own
	glass_button(ev, g.cx - g.seek_dx, g.cy, g.seek_r, u)
	glass_button(ev, g.cx, g.cy, g.play_r, u)
	glass_button(ev, g.cx + g.seek_dx, g.cy, g.seek_r, u)
	local r = 0.72 * g.play_r	-- the icon's size
	if paused then
		ev[#ev + 1] = shape(string.format("m %d %d l %d %d l %d %d",
			g.cx - 0.3 * r, g.cy - 0.5 * r, g.cx + 0.52 * r, g.cy, g.cx - 0.3 * r, g.cy + 0.5 * r), "FFFFFF")
	else
		ev[#ev + 1] = shape(rrect(g.cx - 0.4 * r, g.cy - 0.48 * r, g.cx - 0.1 * r, g.cy + 0.48 * r, 0.08 * r) .. " " ..
			rrect(g.cx + 0.1 * r, g.cy - 0.48 * r, g.cx + 0.4 * r, g.cy + 0.48 * r, 0.08 * r), "FFFFFF")
	end
	ev[#ev + 1] = text(g.cx - g.seek_dx, g.cy, 0.55 * u, "−" .. SEEK, 5, true)
	ev[#ev + 1] = text(g.cx + g.seek_dx, g.cy, 0.55 * u, "+" .. SEEK, 5, true)
	-- bottom: the times over the seek bar
	glass(ev, g.bot, u)
	local x1, x2, y, t = g.bar_x1, g.bar_x2, g.bar_y, g.bar_t
	local kx = x1 + (x2 - x1) * frac
	ev[#ev + 1] = shape(rrect(x1, y - t / 2, x2, y + t / 2, t / 2), "FFFFFF", "A8")
	if kx > x1 + t then
		ev[#ev + 1] = shape(rrect(x1, y - t / 2, kx, y + t / 2, t / 2), ACCENT)
	end
	local kr = dragging and 1.35 * g.knob_r or g.knob_r
	ev[#ev + 1] = shape(circle(kx, y, kr), "FFFFFF", "00",
		dragging and string.format("\\bord%d\\3c&H%s&\\3a&H40&", math.floor(0.08 * u), ACCENT) or nil)
	ev[#ev + 1] = text(x1, g.time_y, 0.42 * u, clock(pos), 4, true)
	ev[#ev + 1] = text(x2, g.time_y, 0.42 * u, "-" .. clock(dur - pos), 6, true)
	local data = table.concat(ev, "\n")
	if data ~= last_drawn then
		overlay.res_x, overlay.res_y = g.w, g.h
		overlay.data = data
		overlay:update()
		last_drawn = data
	end
end

local function hide()
	visible = false
	render()
end

local function keep_shown()
	if hide_timer then hide_timer:kill() end
	hide_timer = nil
	if not mp.get_property_bool("pause") and not dragging then
		hide_timer = mp.add_timeout(HIDE_AFTER, hide)
	end
end

local function show()
	visible = true
	render()
	keep_shown()
end

local function seek_by(seconds)
	mp.commandv("seek", seconds, "relative", "exact")
	mp.osd_message(seconds < 0 and ("-" .. SEEK .. " s") or ("+" .. SEEK .. " s"), 0.7)
end

local function seek_to_x(x, exact)
	local g = geometry()
	if not g then return end
	local f = math.max(0, math.min(1, (x - g.bar_x1) / (g.bar_x2 - g.bar_x1)))
	mp.commandv("seek", f * 100, exact and "absolute-percent+exact" or "absolute-percent+keyframes")
end

local function on_bar(g, y)
	return math.abs(y - g.bar_y) < 0.7 * g.u
end

local function near(x, y, cx, cy, r)
	return (x - cx) ^ 2 + (y - cy) ^ 2 <= r * r
end

local function inside(box, x, y)
	return x >= box[1] and x <= box[3] and y >= box[2] and y <= box[4]
end

-- a tap with the controls shown: a button, the bar, a pill, or nowhere (hides)
local function tap_shown(g, x, y)
	if inside(g.top, x, y) and x < 2.2 * g.u then
		mp.command("quit")
		return true
	elseif near(x, y, g.cx, g.cy, g.play_r * 1.15) then
		mp.command("cycle pause")
	elseif near(x, y, g.cx - g.seek_dx, g.cy, g.seek_r * 1.25) then
		seek_by(-SEEK)
	elseif near(x, y, g.cx + g.seek_dx, g.cy, g.seek_r * 1.25) then
		seek_by(SEEK)
	elseif not (inside(g.top, x, y) or inside(g.bot, x, y)) then
		return false
	end
	keep_shown()
	render()
	return true
end

local function double_tap(g, x)
	if x < g.w / 3 then
		seek_by(-SEEK)
	elseif x > g.w * 2 / 3 then
		seek_by(SEEK)
	else
		mp.command("cycle pause")
	end
	if visible then keep_shown() end
end

local function touch(t)
	local g = geometry()
	if not g then return end
	local x, y = mp.get_mouse_pos()
	if t.event == "down" then
		if visible and on_bar(g, y) then
			dragging = true
			if hide_timer then hide_timer:kill() end
			seek_to_x(x, false)
			render()
		end
		return
	end
	if t.event ~= "up" then return end
	if dragging then
		dragging = false
		seek_to_x(x, true)
		keep_shown()
		render()
		return
	end
	if visible and tap_shown(g, x, y) then
		last_up = 0
		return
	end
	-- a second tap soon after the first: a double tap, not two single ones
	local now = mp.get_time()
	if tap_timer and now - last_up < 0.3 then
		tap_timer:kill()
		tap_timer = nil
		last_up = 0
		double_tap(g, x)
		return
	end
	last_up = now
	tap_timer = mp.add_timeout(0.28, function()
		tap_timer = nil
		if visible then hide() else show() end
	end)
end

mp.add_forced_key_binding("MBTN_LEFT", "ztos-touch", touch, { complex = true })
-- mpv's own double click is full screen: here double taps are ours
mp.add_forced_key_binding("MBTN_LEFT_DBL", "ztos-no-dbl", function() end)

mp.observe_property("mouse-pos", "native", function(_, pos)
	if dragging and pos then seek_to_x(pos.x, false) end
end)
mp.observe_property("pause", "bool", function(_, paused)
	if paused then
		show()	-- paused: the controls stay
	elseif visible then
		keep_shown()
		render()
	end
end)
local last_second = -1
mp.observe_property("time-pos", "number", function(_, t)
	if visible and t and math.floor(t) ~= last_second then
		last_second = math.floor(t)
		render()
	end
end)
mp.observe_property("osd-dimensions", "native", function() render() end)
-- the controls show for a moment as it starts, so the gestures are found
mp.register_event("file-loaded", function() show() end)
