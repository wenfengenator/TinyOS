# Cycle 7 Plan — Graphics stack (TinyGL): app → UMD → KMD → display

> Vision (user): a full **zero-copy, tearing-free, hardware-accelerated** graphics
> stack — `app → user-mode driver (UMD) → kernel-mode driver (KMD) → display` —
> with a **software rasterizer** fallback and a clean, bespoke command API —
> **TinyGL** — for both **rendering** and **composition**.

## The one constraint that shapes everything

**Hardware 3D acceleration is realistic only on QEMU, not on Hyper-V.**

| Target | Display transport | 2D flip / tearing-free | 3D hardware acceleration |
|---|---|---|---|
| **QEMU** (dev) | **virtio-gpu** (+ GOP fallback) | ✅ double-buffered scanout + `SET_SCANOUT` swap | ✅ **virgl** (OpenGL) / **venus** (Vulkan) proxied to the host GPU |
| **Hyper-V** (daily) | synthetic video (GOP framebuffer, DIRT rects) | ⚠️ only mitigated (scanline-diff; no real page-flip/vsync) | ❌ none (GPU-P needs the vendor driver stack) |

Implication: **the daily-driver (Hyper-V) path is software-rasterized only.** The
hardware-accelerated, tearing-free, zero-copy path lives on **QEMU + virtio-gpu +
KVM + virgl/venus**. A reasonable decision is to make **QEMU the graphics dev
target** for this cycle and keep Hyper-V on the software path. The SW rasterizer
is therefore not optional — it is the portable renderer *and* the reference
implementation of the API.

## What "zero-copy" and "tearing-free" concretely mean here

- **Zero-copy**: the app renders into a **buffer object (BO)** that is *itself*
  the scanout/compositor source — no CPU `memcpy` to present. On virtio-gpu the
  BO is a host-backed resource; on the SW path the compositor composes directly
  into the scanout buffer.
- **Tearing-free**: present by **flipping whole buffers** on a frame boundary
  (double/triple buffer + `SET_SCANOUT` swap on virtio-gpu). On the pure GOP
  framebuffer there is no flip, so Hyper-V can only *reduce* tearing (the existing
  `disp64` scanline-diff), not eliminate it.

## Where TinyOS is today

`fb64` (raw GOP framebuffer) + `disp64` (one software double buffer + scanline-diff
present) + `wm64` (CPU-blit compositor). That is a rudimentary **software
display + compositor** — the seed of the "display" and "compositor" layers, with
no GPU driver, no BOs, no command submission, and no page-flip.

## Target architecture (mapped onto TinyOS)

```
 app  ─────────────────────────────────────────────┐  (ring 3)
   │ minimal gfx API (render + composition)         │
   ▼                                                │
 UMD (user-mode lib, in the app's address space) ───┘
   │  emits: SW-raster calls  ── or ──  virgl/venus command streams
   ▼  submit / BO / fence ioctls
 KMD (kernel) ── virtio-gpu driver  |  GOP fallback
   │  BO/memory manager (GEM-lite), fences, command queues
   ▼
 display (KMS-lite): outputs, modes, scanout buffers, present/flip
```

Two API layers, mirroring Wayland + DRM/KMS:
- a **rendering API** (draw triangles/textures into a surface), and
- a **composition API** (place surfaces/layers with z/alpha, flip to screen).

## Adopted approach: software-first (aligned with the review)

A second review recommended a **software-first** path — build the whole stack
(clean API → software 2D → software 3D → compositor) on the existing framebuffer
*before* any GPU driver, and treat every display transport as a pluggable
**backend**. That is the right call here, and it sharpens this plan:

- **Steps 1–2 already exist.** The "framebuffer driver" is `fb64` (the GOP linear
  framebuffer — the same surface Hyper-V's synthetic video exposes after
  `ExitBootServices`), and "double buffering" is `disp64` (one back buffer +
  scanline-diff present). So the concrete next step is the **clean API** and a
  **software renderer**, not a GPU driver.
- **The critical path is entirely software** (G1–G6) and therefore runs on the
  **Hyper-V daily driver**. GPU transports — virtio-gpu, a dedicated Hyper-V
  **synthvid** VMBus path, GPU-PV, and virgl/venus acceleration — become **later,
  optional backends** behind the display seam (G7).
- **On "tear-free":** presenting a full back buffer to a *single-scanout*
  framebuffer (GOP / Hyper-V) is **visually tear-free in practice** (the reviewer
  is right — fast full present + host-side compositing avoids visible tearing),
  but it is not hardware **vsync page-flip** tear-free, because there is no flip.
  True vsync-tear-free needs a **flip-capable backend** (virtio-gpu on QEMU) —
  which is exactly why that sits in the optional-backend bucket, not the critical
  path.

## Phased plan (software-first)

### G1 — Display backend abstraction  *(partly done)*
**Why:** one seam so the same renderer/compositor runs on any transport (GOP /
Hyper-V synthvid / virtio-gpu / GPU-PV) and backends are swappable later.
**Tasks:** generalize `disp64` into a `display` interface — enumerate
outputs/modes, own N scanout buffers, `present(buffer)` / `flip()`. The GOP /
Hyper-V-framebuffer backend already exists (`fb64` + scanline-diff `disp64`);
give it the interface shape and a registration point for future backends.
**Gate:** the WM and a test app present through the `display` interface unchanged.

### G2 — TinyGL: clean bespoke command API + software present  *(step 3)*
**Why:** the app/UMD-facing surface — **TinyGL**, a small bespoke command API
(not a GL/GL-ES subset).
**Tasks:** define TinyGL resources (buffer, texture, sampler, `surface`/render
target), state (blend/depth/cull), and commands (`bind`, `clear`, `draw`,
`present`); present via the existing double buffer. Split into a **rendering API**
and a **composition API** (Wayland/KMS-like).
**Gate:** an app clears a surface and presents it through TinyGL.

### G3 — Software 2D renderer  *(step 4)*
**Why:** the bulk of UI/desktop rendering; usable immediately on Hyper-V.
**Tasks:** filled/stroked rects and lines, blits with source-over **alpha**,
scaled/clipped blits, gradients, and text (glyph atlas); a clip stack.
**Gate:** a demo draws alpha-blended, clipped 2D content at a steady frame rate.

### G4 — Software 3D rasterizer  *(step 5)*
**Why:** the 3D reference renderer; matures the pipeline before any GPU.
**Tasks:** vertex transform, triangle setup + rasterization, perspective-correct
attribute interpolation, texture sampling, a depth buffer, and a small
**programmable** fragment stage (callbacks); back-face cull, near-plane clip.
Correctness first; SIMD later.
**Gate:** a spinning, textured, depth-tested mesh renders into a surface.

### G5 — Compositor / window manager v2  *(step 6)*
**Why:** multiple surfaces on screen, tear-free, minimal redraw.
**Tasks:** surfaces/layers with position, z-order, alpha, and **damage tracking**;
compose directly into the scanout buffer and present; evolve `wm64` onto the new
API. Multi-app composition needs Tier 1 (concurrent processes).
**Gate:** two apps' surfaces composited with damage-limited, visually tear-free
present.

### G6 — Buffer objects + zero-copy user mapping  *(GEM-lite)*
**Why:** apps render into a buffer the compositor uses directly — no CPU copy.
**Tasks:** BO alloc/free, handles, refcount, and **map a BO into a process's
address space** (`mmap` of shared memory — leans on the Tier 1/2 VM work); fences
for producer/consumer sync; the compositor samples client BOs in place.
**Gate:** a client renders into a mapped BO and the compositor presents it with no
intermediate copy.

### G7 — Optional backends: virtio-gpu, synthvid, GPU-PV, HW accel  *(step 7, later)*
**Why:** true vsync page-flip (tear-free) and eventually GPU acceleration —
mostly QEMU; a synthvid path can also improve the Hyper-V present.
**Tasks (each an independent backend behind G1):**
- **virtio-gpu 2D** (QEMU): control/cursor virtqueues (reuse `vblk64`),
  `RESOURCE_CREATE_2D`/`ATTACH_BACKING`/`TRANSFER_TO_HOST_2D`/`RESOURCE_FLUSH`/
  `SET_SCANOUT`, double-buffered scanout **flip** = true vsync tear-free.
- **Hyper-V synthvid** (VMBus, DIRT rectangles): a dedicated present path that
  ships only dirty regions and updates the frame atomically — less bandwidth and
  better atomicity than the raw GOP framebuffer on the daily driver.
- **virgl / venus** (QEMU): the UMD emits OpenGL / Vulkan command streams over
  virtio-gpu 3D; the host GPU renders; import as a scanout/composited BO.
- **GPU-PV / real GPU**: only if a real-hardware or GPU-partitioned target
  appears (needs the vendor stack — large, out of scope for now).
**Gate:** the same app/compositor runs unchanged on a flip-capable backend with
true vsync tear-free present; (stretch) a host-GPU-rendered 3D frame via virgl.

## TinyGL API sketch (bespoke command API)

**TinyGL** is a small, clean **command API** designed for this stack — *not* a
GL/GL-ES subset. Shading is done with **callbacks** (the vertex and fragment
stages are function pointers on the software backend; the same slots map to
virgl/venus shaders on the hardware backend), so there is no shader language to
parse.

- **Resources:** `tg_buffer` (vertex/index/uniform), `tg_texture`, `tg_sampler`,
  `tg_surface` (render target / scanout-capable BO).
- **Pipeline (shader callbacks):** `tg_pipeline{ vertex_fn, fragment_fn,
  vertex_layout, blend, depth, cull }` — `vertex_fn` transforms a vertex and
  emits varyings; `fragment_fn` shades an interpolated fragment → colour. On the
  HW backend these slots bind to compiled virgl/venus shaders instead.
- **Commands:** `tg_bind(pipeline/resources)`, `tg_clear`, `tg_draw(prim, count)`,
  `tg_present(surface)`.
- **Composition:** `tg_layer{ surface, x, y, z, alpha, damage }`, `tg_compose()`
  + `tg_flip()`.

> Note: "TinyGL" is also the name of Fabrice Bellard's small software-OpenGL
> library. This TinyGL is a **bespoke command API**, unrelated to it — worth
> knowing to avoid confusion when searching.

## Dependencies & out of scope

- **Depends on:** per-process VM + `mmap` of device memory (Tier 1/2 memory work)
  for BO mapping; multiple concurrent processes (Tier 1 Phase B) for the
  compositor + clients.
- **Out of scope this cycle:** a full OpenGL/Vulkan API surface (only a minimal
  subset), GPU-P on Hyper-V, multi-GPU, video decode.
- **Decisions (resolved 2026-07-30):** **QEMU + virtio-gpu is the
  hardware-acceleration proof-of-concept target** (Hyper-V stays on the software
  path). The API is **TinyGL** — a **clean bespoke command API**, not a GL/GL-ES
  subset. The software rasterizer is **programmable via shader callbacks**
  (vertex/fragment function pointers), not fixed-function.
