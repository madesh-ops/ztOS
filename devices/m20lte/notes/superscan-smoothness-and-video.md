# Superscan on the M20: smoothness, crashes and video

Findings from the session of 2026-11-07. Nothing here is committed or pushed yet.

## Where things stand

| Item | State |
|---|---|
| Black flashes while scrolling | Gone in the user's testing (picture scroller off) |
| Superscan window redrawing during a scroll | Fixed: no 9 ms runs any more |
| Whole-system lag while browsing | Fixed in the user's testing (shell on the A73s at nice -10) |
| Vertical scrolling | Better; hitches of 34-100 ms were 0.8% of frames in the last clean run |
| Sideways scrolling | Native scrollers now use the fast path; JavaScript carousels unchanged |
| Tap on a YouTube video pausing it | Fixed: confirmed by screenshot, the mobile controls show |
| YouTube comments not scrolling | Fix installed; comments visible, scrolling not confirmed |
| Clock locked to the shell's frames | Installed; not measured on a page without video |
| Hardware video in Superscan | Off: plays, but freezes after a seek (see below) |
| The freeze of 2026-11-05 | Not reproduced; cause unknown; `ztos-hangwatch` armed |
| The boot loop of 2026-11-07 | One bad boot; cause unknown |

## The freeze (not solved)

- It happened on 2026-11-05 at about 20:13, 53 minutes into that boot and 13 minutes after a new shell went in.
- `journalctl -b -6` was the boot with no "Journal stopped". There was no panic: pstore held only a console record.
- The phone answered ping while SSH timed out, so the kernel was alive and user programs were not running. That suggests a memory or storage stall, not a dead SoC. This is unproven.
- Panfrost was purging GPU buffers (`Purging ... bytes`) in the minutes before, with Android running beside the browser.
- This kernel has no soft lockup detector, no hung task detector and no hardware watchdog, so a hang leaves no record. The last ~30 s of the journal are lost at a forced restart.

### ztos-hangwatch

`devices/m20lte/hangwatch/ztos-hangwatch.c`, installed at `/usr/local/bin/ztos-hangwatch`.

- Real-time, memory locked, with three canaries: scheduling (20 s), memory (25 s) and storage (60 s).
- Logs each second to `/var/lib/ztos-hangwatch/log`; `log.1` is the previous start.
- On a stall it dumps sysrq m, w and l to the kernel log, then crashes the kernel on purpose. The record lands in `/var/lib/ztos-pstore/<time>-<bootid>/dmesg-ramoops-0`.
- Test it with `kill -STOP` on the second `pgrep -x ztos-hangwatch`. Tested twice: the record survived and the phone came back by itself in 45 s.
- A false trip reboots the phone: stop the service before anything that stalls on purpose.

A panic restart obeys a stale INFORM3 (PMU `0x11c80000 + 0x80c`), which holds `0x12345674` (recovery) while ztOS runs. The first test landed in OrangeFox. hangwatch now writes `0x12345670` (normal) when it starts.

## Whole-system lag

- All six A53s ran at about 38% while the A73s sat near 20% at their lowest clock, with `ztmetro` on a slow core beside WebKit.
- `zrnperfd` was never run on the M20. It is written for the S9 (hotplug, uclamp, cluster settings) and should stay disabled.
- `cores_setup()` in `ztmetro.c` keeps the shell on the fastest cores, found from `cpu_capacity`, so other devices are unaffected. `child_priority()` puts each app back on all cores.
- `greetd.service.d/20-m20-priority.conf` sets `Nice=-10`.

## Scrolling

### What the log counter means

`app frames` in `ztmetro.log` counts commits on the app's main surface only, not WebKit's direct subsurface. The 9.4 ms runs were Superscan's own window being redrawn, not page frames. This misled two rounds of work.

To see the shell's output, use `frametrace MS` on `/run/user/1000/ztmetro.ctl`, then read `/tmp/zt-frametrace.log`. It holds a steady 60 Hz (median 16.6 ms). For WebKit's frames, set `ZTB_TRACE=1` and read the `ztb-direct` lines. Measure on a page with no video: 30 fps video looks like skipped frames.

### Causes and fixes

| Cause | Fix |
|---|---|
| The picture scroller redrew the window each frame, took full-document snapshots (about 30 MB) and faded back onto a dropped buffer, which showed black | `ZTB_NO_PICTURE=1` in `session.env` |
| simpledrm has no vblank, so `webview_on_vblank` failed and panning fell back to the window's frames | It now always has a clock |
| WebKit's clock free-ran against the panel | `panel_beat()` and `beat_wait()` lock a timer to the time stamps in the shell's frame callbacks |
| WebKit was told a direct frame was done on hand-over | Acked on the frame callback, with a 100 ms fallback in `webview_pump` |
| Sideways drags went through the main thread, and 8 points of vertical drift turned them into vertical scrolls | The axis locks early; native scrollers pan on the scrolling thread |
| Page tiles used MSAA | `WEBKIT_SKIA_MSAA_SAMPLE_COUNT=0`: hitches 1.1% to 0.8%, GPU time at top clock 80% to 72% |

The session.env values reach the shell only at its next start.

The page is asked what scrolls under the finger (`webview_scrollers_at`), so JavaScript carousels keep their touch events and a panel in a page that cannot scroll is not taken for a pull past the end.

Moving the web compositor thread to the A73s showed no difference in one A/B run, but that run filtered out most of the scrolling, so it is inconclusive.

### What still limits it

- The GPU spends most of a scroll at its top clock.
- WebKit's main thread was at 41-55% of a core while scrolling.
- Spikes of 50-90 ms remain and their cause is not proven.
- `ZTB_MAX_SCALE` takes whole numbers only, so 1.5x needs code changes in `webview_place` and `direct_frame`.

## Touch

`ZtDisplay` never declared any input devices, so pages took the phone for a computer with a mouse and YouTube's player paused at a tap, as at a click. `dpy_connect` now calls `wpe_display_set_available_input_devices()` with `WPE_AVAILABLE_INPUT_DEVICE_TOUCHSCREEN`. Other sites may change behaviour because of this.

## Video

Superscan decodes in software, at about 190% of a core at an unmeasured resolution.

### Hardware decoding test

- `v4l2h264dec` decoded a 720p test clip at 183 fps, against 144 fps for `avdec_h264`. That clip was simple and the CPU figures were system-wide, so treat the comparison as rough.
- It played a YouTube stream for about 65 s with no trouble.
- A seek stalled it. A new pipeline came up, the decoder's output thread was gone, every pipeline thread sat in a futex wait and playback stayed at 0:58 on a black picture.
- CMA free fell from 107 MB to about 20 MB in that state. Whether that matters is not known.

### Driver bug (fixed)

`devices/m20lte/kernel/patches/s5p-mfc-stop-mid-frame.patch`

`try_run()` takes `hw_lock` while it looks for work. `s5p_mfc_stop_streaming()` read that as a frame in flight and waited for `FRAME_DONE`, which never came. Signs: `Interrupt (ctx->int_type:0, command:13) timed out`, `hw_lock 0` afterwards, no watchdog. It then skipped the DPB flush. It now waits on the lock, and the flush always runs. The timeout was gone in the next test, but the freeze after a seek remained, so that one lies above the driver.

### Options left

1. Tune software decoding: measure 480p and 720p, then cap the resolution to fit, for example 480p in the small player and 720p in full screen.
2. Fix hardware seeking: restart the decoder after a flush in the GStreamer path, or rebuild the video pipeline on each seek. Not known to be possible.

## Other changes worth knowing

Chromium was considered. CEF 156 exists for ARM64 (431 MB minimal build) and `webview.c` is the only engine code, but the user chose to stay on WebKit.

## Traps

- A plain `sudo` in a `$(...)` subshell of a script has no cached ticket. Three empty tries lock sudo for 10 minutes through `pam_faillock`. `journalctl` and the pstore files are readable without sudo.
- `GST_DEBUG_FILE` in `/var/tmp` crashes WebKit's bwrap sandbox at start: `Can't mount on symlink destination /var/tmp`.
- `GST_PLUGIN_FEATURE_RANK` is set with overwrite off in `superscan.c`, so an environment variable overrides it for tests.
- `rmmod s5p_mfc` works when nothing holds it. Afterwards run `chmod 777 /dev/video0 /dev/video1`, or Android's video service loses access.
- The clock is stale until NTP after each boot. `up 0 min` with a later date is not a reboot; read `/proc/uptime`.
- pstore records can be archived again under new names. Read the time stamps inside them before trusting them.
- A boot that dies early leaves about 3 s of journal.
- `webview.c` and `superscan.c` use CRLF in this checkout; the phone's build tree uses LF.

## Backups on the phone

| File | What it is |
|---|---|
| `/usr/local/bin/ztmetro.before-cores` | Shell before the core placement |
| `/usr/local/bin/ztmetro.before-framedone` | Shell before frame-done pacing |
| `/usr/local/bin/superscan.before-ack` | Superscan before the direct-frame ack change |
| `/root/s5p-mfc.ko.before-seek` | Decoder module before the stop fix |
