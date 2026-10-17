# Adreno 506 Vulkan driver — Wayland WSI port and GPU-fault investigation

Status date: 2026-10-17. Branch: `a5xx-vulkan`. Last commit before this work: `62954fe`.

This is a handover document. It records what was measured, what was concluded, and
what is still wrong. Claims are labelled by how they were established; conjecture is
called out as such.

---

## 1. Where things stand

| Area | State |
|---|---|
| Headless vkmark | **Works.** All 13 cases, exit 0. |
| Wayland presentation | **Broken.** phoc segfaults on our buffer. |
| GPU fault storm | **Open.** Reproduced twice, cause unknown, not reproduced since. |
| Port to shared `wsi_common` | **Environment unblocked**, driver not yet ported. |

---

## 2. Confirmed working

Measured on the device (`172.16.42.1`, Adreno 506, gpu_id 506, chip 0x5000600):

```
VK_ICD_FILENAMES=/home/ishu/a5xx-test-tmp/a5xx_icd.json \
MESA_SHADER_CACHE_DISABLE=true vkmark --winsys headless --size 300x300
→ 13/13 scenes, exit 0, score 1371
```

Isolated scenes each pass, including `[clear]` (which needed `ta5_CmdClearColorImage`,
added in `81cf6a4` and corrected in `62954fe`).

---

## 3. The GPU fault storm — open

### 3.1 What was observed

The first full headless run logged **160 printed GPU faults** plus thousands of
suppressed callbacks:

```
*** gpu fault: iova=00000000030a6100 flags=0 (0,0,0,0)
qcom-iommu-ctx: Unhandled context fault: fsr=0x40000202, iova=0x030a6100, fsynr=0x13, cb=0
adreno_fault_handler: 22526 callbacks suppressed
```

Fault IOVAs cluster in three pairs (`…a6100/…a6180`, `…a6200/…a6280`, `…a6400/…a6480`),
striding 0x80.

### 3.2 What was established

* **Attribution.** Using `TA5_TRACE_NULL` (which queries real IOVAs via
  `MSM_INFO_GET_IOVA`) against `TA5_TRACE_BO`, the faulting addresses sit
  **0x100–0x480 bytes past the end** of a 368640-byte allocation based at
  `0x0304c000`. So: a read just past the end of a buffer.
* **Which scenes.** Per-scene isolation (one process per scene, fresh kernel log each):

  | Faults | Scenes |
  |---|---|
  | yes | `vertex` (both), `texture` (both), `shading` (all four) |
  | no | `effect2d` (both), `desktop`, `cube`, `clear` |

  Every faulting scene has a **depth attachment**; no clean scene does.
* **Not textures.** `TA5_TRACE_VIEW` shows `tex=0` — no sampled texture is bound in
  these scenes at all. `fmt=126` is D24S8 depth, `fmt=50` is swapchain colour.
* **Depth-clear path implicated.** Existing bisect results recorded in
  `ta5_cmd_buffer.cc`: sizes whose height is not a whole number of 64-pixel tiles
  fault (64/256/512/1024 clean; 1023/1025/1080/1080x2280 fault).

### 3.3 What is NOT established

* **No fix.** The cause remains unknown.
* **Not currently reproducible.** After the first full run, **0 faults across 10+
  consecutive runs**, including a deliberate phoc-stopped test
  (`/home/ishu/a5xx-test-tmp/interference.sh` → 0/0/0 with phoc up, 0/0/0/0 without).
  The storm is therefore *state-dependent*, and the trigger was never isolated.
* **`TA5_TRACE_BO` prints the wrong address.** It uses `MSM_INFO_GET_OFFSET`
  (an mmap offset, e.g. `0x102bf8000`), not `MSM_INFO_GET_IOVA` (the real IOVA,
  e.g. `0x0304c000`). Its comment claims "the mmap offset is the IOVA on this
  driver" — **that is false on this kernel.** Attribution required
  `TA5_TRACE_NULL`. Worth fixing so the next person is not misled.

### 3.4 Dead ends already ruled out

| Hypothesis | Result |
|---|---|
| phoc (GL client) interference | **Not it.** Faults persist with phoc killed; faults absent with phoc running. |
| `TA5_RESOLVE_BLITDST` | Only 6→4 faults, and its own comment's "0 faults" claim **did not reproduce**. |
| `TA5_DEPTH_NOTS` | 0 faults but FPS 764→138, and depth is not written. Rejected. |
| `TA5_RESOLVE_MRT` | 0 faults but repoints the blit so depth lands on the colour target — wrong pixels. Rejected. |
| `TA5_SKIP_CLEAR_DEPTH` | 0 faults because the required clear is skipped. Rejected. |

---

## 4. Wayland: root cause, from the compositor's own log

This is the best-established finding in the whole investigation.

### 4.1 The evidence

phoc's journal, with the display awake:

```
[backend/drm/drm.c:980] connector DSI-1: Modesetting with 1080x2280 @ 60.000 Hz
[EGL] command: eglCreateImageKHR, error: EGL_BAD_ALLOC (0x3003),
      message: "createImageFromDmaBufs failed"
[render/egl.c:818] eglCreateImageKHR failed
[types/buffer/client.c:109] Failed to create texture
[types/wlr_compositor.c:437] Failed to upload buffer
Process 25384 (phoc) terminated abnormally with signal 11/SEGV, dumped core
```

After the restart the session stays broken:

```
[backend/drm/atomic.c:82] connector DSI-1: Atomic commit failed: Resource busy
phoc_layout_transaction_notify_layer_configured:
      assertion 'self->pending_layer_configures > 0' failed
```

### 4.2 The chain

1. Swapchain images are **system memory** — this driver has one heap, and
   `ta5_CreateImage` lays every image out linearly.
2. Present exports that memory as a dma-buf via `drmPrimeHandleToFD` +
   `wl_drm_create_prime_buffer` (`ta5_wayland.cc:800-810`), passing **no modifier**.
3. Mesa's EGL on the compositor side cannot import it →
   `createImageFromDmaBufs` → `EGL_BAD_ALLOC`.
4. wlroots retries the failed upload; **phoc segfaults**.
5. With no compositor, no `wl_surface.frame` callbacks arrive, so swapchain images
   stay `busy` and `ta5_AcquireNextImageKHR` blocks.

### 4.3 Proof the stall is ours, not vkmark's

vkcube, not vkmark, reproduces it cleanly:

| Run | Result |
|---|---|
| `vkcube --wsi wayland --c 3` | completes in 0s |
| `vkcube --wsi wayland --c 10` | hangs, 45s timeout |
| `vkcube --wsi wayland --c 30` | phoc dies |

**3 is exactly our `image_count`.** Present works for `image_count` frames, then
acquire blocks. Headless never stalls: no compositor, no frame callbacks.

### 4.4 What is NOT ours

* **vkmark's abort is a vkmark bug.** GDB showed `SwapchainWindowSystem::
  next_vulkan_image` aborting on `std::vector<vk::Image>::operator[](65535)`.
  Our `GetSwapchainImagesKHR` returns 3 valid images (verified: vector size 3,
  indices 0/1/2 read correctly) and `AcquireNextImageKHR` returns 0,1,2. vkmark
  ignores the `VK_TIMEOUT` from acquire and indexes an uninitialised value.
  *Not our bug; not fixed on our side.*
* **The stall itself is correct behaviour.** `wsi_common_wayland.c` blocks in
  acquire waiting for frame callbacks too. Blocking is not the bug; the
  *import failure* is.

### 4.5 Why the device sat at the login page

`DSI-1 enabled = disabled`, backlight `bl_power = 4`. The compositor had no output,
so it composited nothing and sent no frame callbacks. Waking the display
(`enabled`, `bl_power 0`) is required before Wayland can be tested at all.

---

## 5. Turnip comparison — the reason for the port

Turnip defines **no WSI entry points at all**. `tu_wsi.cc` is 67 lines:

```c
const struct wsi_device_options options = { .sw_device = false };
wsi_device_init(&physical_device->wsi_device, ..., physical_device->master_fd, ...);
physical_device->wsi_device.supports_modifiers = true;
physical_device->wsi_device.can_present_on_device = tu_wsi_can_present_on_device;
physical_device->vk.wsi_device = &physical_device->wsi_device;
```

Everything else comes from `vk_common_entrypoints.c`, which
`vk_device_init` merges **without overwriting driver-provided entries**
(`vk_device.c:179`, `..._from_entrypoints(..., false)`). So a driver that simply
does not provide them gets the wsi_common implementations.

The critical difference is at `wsi_common_wayland.c:3910`:

```c
if (wsi_wl_surface->display->wl_dmabuf && wsi_device->supports_modifiers) {
   f = find_format(&chain->wsi_wl_surface->display->formats, imageFormat);
   num_drm_modifiers = u_vector_length(&f->modifiers);
   drm_image_params.modifiers = &drm_modifiers;
}
buffer_type = WSI_WL_BUFFER_NATIVE;
```

**The contract:** the compositor advertises `(fourcc, modifier)` pairs over
`wl_dmabuf`; the driver allocates the swapchain image with a matching modifier
(`wsi_drm_image_params`, `wsi_common_private.h:64`); present exports that memory
via `zwp_linux_dmabuf_v1_create_params` carrying
`image->base.drm_modifier` (`wsi_common_wayland.c:3629`).

**We do none of this.** We bind `wl_dmabuf` (`ta5_wayland.cc:96`) and never read
its advertised formats.

Also note: `WSI_WL_BUFFER_SHM_MEMCPY` exists but upstream uses it only when
`wsi_device->sw` is set — never as a hardware fallback. So "just use shm" is a
deviation from upstream, not the reference behaviour.

### 5.1 What the driver must add

| Requirement | Source |
|---|---|
| `GetMemoryFdKHR` — export dmabuf, set modifier metadata | `tu_device.cc:4439` |
| `GetMemoryFdPropertiesKHR` — report memoryTypeBits | `tu_device.cc:4483` |
| `GetImageDrmFormatModifierPropertiesEXT` | required by `wsi_common.c:217` |
| Honour `DRM_MEMORY_MODIFIER_EXTENSIONS` in `AllocateMemory` | modifier contract |

The device CBs wsi_common requires — `QueueSubmit2`, `GetFenceStatus`,
`WaitForFences`, `ResetFences`, `GetSemaphoreFdKHR` — all already exist in
`ta5_queue.cc` / `ta5_sync.cc`, so `wsi_device_init` should bind.

---

## 6. Separate bug found: advertised but unimplemented extension

**`VK_KHR_external_memory_fd` is advertised at `ta5_driver.cc:117` while neither
`GetMemoryFdKHR` nor `GetMemoryFdPropertiesKHR` is implemented.** Those dispatch
slots are NULL, so any application calling them jumps to address 0 — the same
class of bug that killed vkmark repeatedly. This predates the port and is
independent of it. Fixed as part of the port.

---

## 7. Port progress

### 7.1 Done

* `src/freedreno/vulkan-a5xx/meson.build` — link `idep_vulkan_wsi`; remove
  `ta5_wayland.cc` and the whole bespoke `ta5-wayland` build block (158 lines).
* `meson.build` (top level, shared) — `dep_wayland_server` made optional. Justified
  in the comment there; see §7.2.
* `aarch64-musl.cross` — `pkg_config_libdir` pointed at the cross prefix.
* `/tmp/opencode/wayland-cross-pc/{wayland-client,wayland-protocols}.pc`.
* `libwayland-client.so` → `libwayland-client.so.0` symlink so `-lwayland-client`
  resolves.

**Verified:** `meson setup --reconfigure -Dplatforms=wayland` succeeds, and
`ninja src/vulkan/wsi/libvulkan_wsi.a` builds clean (29/29) for aarch64-musl,
including `wsi_common_wayland.c`, `wsi_common_headless.c`, `wsi_common_drm.c` and
`wsi_common_display.c`.

### 7.2 Why `wayland-server` was made optional

`wsi_common_wayland.c` includes only `<wayland-client.h>` and
`<loader/loader_wayland_helper.h>`, and calls **no** server-side symbols (its only
`wl_display_create*` match is `wl_display_create_queue_with_name`, a client
function). `dep_wayland_server` is used only by EGL's wayland platform, gbm's dri
backend and `egl/wayland/wayland-drm` — all dead when `egl` and `gbm` are disabled,
which they are in this build. Requiring it forced cross builds to build a library
they never link.

**This was my error originally:** I reported "we need wayland-server" from reading
the meson gate without checking what the WSI actually uses. The user's question
("why do we require wayland now?") is what caught it. In fairness, wayland-**client**
*is* genuinely required — but it already was, before this port.

### 7.3 Remaining

**The driver port is written and compiles. It is NOT deployed and NOT tested.**

Done in the working tree (uncommitted at time of writing):
- `struct wsi_device wsi_device` + `int master_fd` in `struct ta5_physical_device`
- `ta5_wsi_init()` / `ta5_wsi_finish()` mirroring `tu_wsi_init`;
  `supports_modifiers = true`, `master_fd = -1` (no KMS master node)
- `ta5_GetMemoryFdKHR` / `ta5_GetMemoryFdPropertiesKHR` — also closes the
  §6 bug
- `ta5_wsi.cc` cut from 1478 to ~215 lines; `ta5_wayland.cc` deleted (1021 lines)
- proc-addr hack removed from `ta5_instance.cc`

Verified by inspection of the built `.so`:
- `ta5_GetMemoryFdKHR` / `ta5_GetMemoryFdPropertiesKHR` are **defined** (`t`)
- `ta5_CreateSwapchainKHR` is **undefined** (`U`) — our implementation is gone
- `CreateWaylandSurfaceKHR`, `DestroySurfaceKHR`,
  `GetPhysicalDeviceSurfaceCapabilitiesKHR`, `...FormatsKHR`,
  `...PresentModesKHR`, `CreateDisplayPlaneSurfaceKHR`,
  `GetPhysicalDeviceDisplayPropertiesKHR`,
  `GetPhysicalDeviceWaylandPresentationSupportKHR` — **none defined by us**

The mechanism is sound: the generated entrypoints declare unimplemented
functions `extern template` weak, which alias to address 0 on ELF;
`vk_device_dispatch_table_from_entrypoints(..., true)` skips `NULL` entries
(`vk_dispatch_table.c:9881`), and `vk_device_init` then fills those slots
from `vk_common_device_entrypoints` because it merges with `overwrite=false`
(`vk_device.c:179`). Turnip relies on exactly this.

Still to do:
1. **Deploy and test** — blocked on the Wayland session being healthy.
2. **Log phoc's advertised `wl_dmabuf` format/modifier table** (see §7.4).
3. Confirm headless vkmark still passes 13/13 — `wsi_common_headless.c` now
   serves that path instead of our own code, so this is a real regression risk,
   not a formality.
4. Verify `vkcube --wsi kms` still works: the old KMS path is gone, replaced by
   `wsi_common_drm.c`, and we pass `master_fd = -1`. **Expect this to have
   regressed** — it is untested and may well not work without a master node.

### 7.4 Known risk

Our images are linear, so the only honest modifier is `DRM_FORMAT_MOD_LINEAR`.
Adreno gbm typically advertises tiled/compressed modifiers. **If phoc does not
advertise LINEAR for ARGB8888, the port will build and run but still fail to
import** — with a much clearer error. Step 6 exists to find this out before going
deep. This is conjecture, not measured; do not record it as such.

---

## 8. Environment notes

* Device: `ishu@172.16.42.1`, uid 10000, root via `echo 1234 | sudo -S -p ""`.
  postmarketOS / musl, aarch64.
* Deploy only to `/home/ishu/a5xx-test-tmp`. **Never write to `/lib`.**
* Always `MESA_SHADER_CACHE_DISABLE=true`.
* Build: `ninja src/freedreno/vulkan-a5xx/libvulkan_freedreno_a5xx.so` in
  `build-musl`. Host cross-build (approved by user), not on-device — no ninja or
  meson on the device.
* **Always propagate exit status.** Earlier pipelines of the form
  `ninja ... | grep ...; echo "build ok"` masked failures and deployed stale
  binaries. Use `bash -o pipefail -c '...'` and check `$?`.
* Wayland session: owned by **uid 10000**, socket `/run/user/10000/wayland-0`,
  `XDG_RUNTIME_DIR=/run/user/10000`, `WAYLAND_DISPLAY=wayland-0`.
  * Do not `pkill phoc`* — it drops the device to greetd's login page and the
    socket becomes stale; this was done several times during this work.
  * A `phoc` process running as uid 108 belongs to greetd, not the session.
* The display must be awake (`DSI-1 enabled`, `bl_power 0`) or the compositor
  sends no frame callbacks and nothing Wayland can be tested.
* Cross-compile aarch64-musl with `/home/ishu/.zig/zig`; cross file
  `aarch64-musl.cross`.

---

## 9. Useful traces

Presence-based; `=0` still enables. From `ta5_cmd_buffer.cc` / `ta5_submit.cc`.

| Flag | Shows |
|---|---|
| `TA5_TRACE_NULL` | **Real** IOVAs (`MSM_INFO_GET_IOVA`) for IB/ring relocations. Use this for fault attribution. Intrusive: adds 4 BO scans + 4×300ms sleeps in `MapMemory`. |
| `TA5_TRACE_CB`, `TA5_TRACE_DRAW` | Command buffers and per-draw IB dwords. |
| `TA5_TRACE_PKT=all` | Packet-level emission. |
| `TA5_CS_DUMP=<path>` | Raw pre-relocation stream. **Overwrites** each call (`"wb"`). |
| `TA5_TRACE_DEPTH` | Depth pitch/array-pitch vs allocation size. |
| `TA5_TRACE_TEX`, `TA5_TRACE_VIEW`, `TA5_TRACE_VFD` | Descriptors, views, vertex input. |

Depth-clear bisect knobs: `TA5_NO_DEPTH_CLEAR`, `TA5_NO_DEPTH_STATE`,
`TA5_NO_DEPTH_RESOLVE`, `TA5_NO_DEPTH_BUFINFO`, `TA5_NO_FASTCLEAR`,
`TA5_SLOW_CLEAR`, `TA5_RESOLVE_MRT`, `TA5_RESOLVE_BLITDST`, `TA5_DEPTH_EVENT`,
`TA5_DEPTH_NOTS`.

**Caveat:** `TA5_TRACE_CB` labels post-submit values "patched" but reads the
original CPU `stream`; submission copies into `queue->ring_map`, which is what
actually gets patched. Prefer `TA5_TRACE_NULL`.

---

## 10. Logs kept

Local: `/tmp/opencode/`. Device: `/home/ishu/a5xx-test-tmp/`.

| File | What |
|---|---|
| `fault-null-20261017.log` | Fault attribution run — real IOVAs. |
| `dmesg-full-20261017.log` | The 160-fault run. |
| `depth-bisect.sh` + `depth-bisect-20261017/` | Per-knob depth bisect. |
| `interference.sh` + `interference-20261017/` | phoc up vs down — faults absent either way. |
| `wl-vec.sh`, `wl-vec/` | Vector size 3, index 65535 — the vkmark abort. |
| `isolate-vkmark-faults.sh` + `fault-isolation-20261017/` | Per-scene fault table. |

---

## 11. Discipline reminders

* `main` must stay pristine; `git diff main -- src/gallium/` must be empty.
* One phase per commit, `git -c commit.gpgsign=false commit`.
* Zero new warnings per phase (pre-existing `else_block` excepted).
* Never patch shared code without asking — `src/asahi` is off-limits; shared IR3
  needs explicit approval. The top-level `meson.build` change in §7.1 was made
  after asking and was approved.
* Inspect rendered images; do not infer correctness from FPS or pixel counts.
  **No visual-correctness claim has been made for anything in this document.**
* Report measurements, not conjecture.