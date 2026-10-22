# Draft Mesa bug report: Panfrost, screen-space derivatives wrong on Mali-G71 r0p0

A draft for https://gitlab.freedesktop.org/mesa/mesa/-/issues, not posted yet.
`derivtest.c` beside it is the test program.

---

**Title:** panfrost: dFdx/dFdy/fwidth wrong on Mali-G71 r0p0 (Bifrost v6, CLPER_OLD)

### System

- Samsung Galaxy M20 (Exynos 7904), Mali-G71 MP2, `GPU_ID 0x60a00002` (r0p0), 2 shader cores
- Linux 7.1, Panfrost: `mali-g71 id 0x60a0 major 0x0 minor 0x0 status 0x2`,
  `features 00000000,00000577 issues 00000000,28000400`
- Mesa 26.2.5 (Arch Linux ARM), GL_RENDERER `Mali-G71 MC2 (Panfrost)`, OpenGL ES 3.1

### What happens

Screen-space derivatives are wrong. `derivtest.c` draws a full-screen quad into an
RGBA8 FBO with a varying that grows by exactly 1 per pixel in x and 2 in y, and
writes `dFdx`, `dFdy` and `fwidth` of it (divided by 8). llvmpipe reads back 1 and 2
everywhere; Panfrost on the G71 does not:

```
Mali-G71 MC2 (Panfrost)
dFdx(x), pixels x 0..7 by rows 0..3 (want 1.00; 8.00 = 8 or more):
  0.00  1.51  0.00  3.51  0.00  5.49  0.00  7.50
  ...
highp   dFdx(x)   want 1, got 0.00 .. 8.00 WRONG
highp   fwidth(x) want 1, got 1.00 .. 8.00 WRONG
mediump ...       (the same)
```

The left pixel of each pair reads 0 and the right one its own value (x + 0.5), as if
`right - left` used 0 for the neighbour and the sign fix-up then flipped it.

Visible effects: Firefox's WebRender sizes every anti-aliased edge with `fwidth()`, so
rounded corners, borders and box shadows render as large blurred squares; GTK 4's GL
renderer draws text and button labels nearly invisible. Both are fine with software
rendering.

### What I tried (Mesa 26.2.5 built on the device, `MESA_SHADER_CACHE_DISABLE=true`)

`bi_emit_derivative()` on `BIFROST_LIMITED_CLPER` hardware lowers to `CLPER_OLD.i32`:

1. As shipped (fine derivatives with the XOR lane op emulated as `lane_id ^ axis`):
   the result above.
2. Fine derivatives through absolute lanes, as before 59c307a3 ("Fix and improve the
   !abs && !coarse case in bi_emit_derivative()"): `lane1 = lane_id & ~axis`,
   `lane2 = lane1 + axis`: every derivative reads 0.
3. Fixed lanes (`#0` and `#axis`, no `lane_id`): every derivative reads 0.
4. Fixed lanes with `CLPER_OLD`'s operands swapped (lane first): 0.
5. Fixed lanes as full words (`bi_imm_u32`; `bi_imm_u8` gives `#0x1010101`): the
   disassembly shows `CLPER_OLD.i32 %36, #0x1`, and still 0.

So on this G71 `CLPER_OLD.i32` seems to return the invocation's own value whatever lane
it is given. The `LIMITED_CLPER` lowering was fixed for the Mali-G31 (75353622,
"Fix clper_xor on Mali-G31"); the G71 (v6) may need something else, or a hardware
erratum of r0p0 may be involved. The AFBC issue on the same GPU is separate
(`PAN_MESA_DEBUG=noafbc` is set throughout; it fixed black 16x16 tiles in Firefox).

### Reproduce

```
cc -O2 -o derivtest derivtest.c -lEGL -lGLESv2
./derivtest
LIBGL_ALWAYS_SOFTWARE=1 ./derivtest   # reference
```
