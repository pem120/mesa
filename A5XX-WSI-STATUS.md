# Adreno 506 Vulkan driver — Wayland WSI port and GPU-fault investigation

Status updated: 2026-10-18. Branch: `a5xx-vulkan`.

## 1. Current findings (supersede the historical notes below)

The driver is **not validated as working**. The older sections retain investigation
history, including several conclusions that subsequent evidence contradicted.

| Area | Current evidence / limitation |
|---|---|
| Shared WSI | Ported; wl_shm workaround selected with `sw_device=true`. Native DMA-BUF presentation previously crashed phoc and remains unsafe to retry in the live session. |
| Copy execution | `ea58ce9` moves image-to-buffer memcpy after GPU completion, but consumes the recording on its first submission. Repeated presentation can cycle stale images; differing screenshots do not establish continuous animation. General transfer ordering is also incomplete. |
| vkmark completion | A supervised Wayland `vertex:duration=4` run at 720x1524 completed with captured status 0; headless `vertex:duration=2` at 300x300 also completed. This is not a full post-port regression or evidence of correct pixels. Earlier detached runs have unknown termination causes. |
| Depth clear | **Fixed.** D32 clears now pass a guard-region test on every extent measured, on repeated submission. |
| Image layout | **Fixed.** Row pitch aligned to 64 texels and the last mip padded to a 32-row envelope, matching Gallium. No out-of-bounds writes measured. |
| Deferred replay | **Fixed.** Clear and image-to-buffer queues persist across submissions instead of draining on first use. |
| Cube artifacts | **Open.** A dotted diagonal across the cube faces survives the depth fix. The texture-descriptor theory for it was **disproved** (section 12.5); the cause is unknown. |
| Vertex fetch | **Fixed.** `VTXCNT` was 0 for draws without vertex buffers, so they silently rasterised nothing (`ee0f89c`). |
| Viewport | **Fixed** for packet length and `VkViewport` offsets (`91842e5`). This did *not* change pixel coverage. |
| Synchronization/lifecycle | Queue submission is synchronous and incomplete; lazy queue creation, command-pool lifecycle, query placeholders and semaphore semantics remain open. |
| KMS | Unvalidated after removing the private WSI; `master_fd=-1` and the software-device workaround need review. |

### Reproducible depth-clear overrun

Using the `ea58ce9` rendering code plus tracing-only changes, `depth-clear-check`
allocates the image's required memory plus a 256 KiB guard. After recording, it
fills the entire allocation with `0xcd`, submits a render-pass depth clear to
0.75, waits for completion, then checks all depth texels and the guard. It replays
the same command buffer twice:

```
D32_SFLOAT, 300x300, linear
memory requirement 368640 (0x5a000), rowPitch 1216, arrayPitch 364800
frame 0: wrong_depth=3176/90000, guard_changed=768, offsets +0x100..+0x4ff
frame 1: wrong_depth=3176/90000, guard_changed=768, offsets +0x100..+0x4ff
exit status 3 (test mismatch)
```

The guard offsets are relative to the end of the reported memory requirement and
match the earlier fault offsets. This demonstrates an out-of-bounds write, even
when an adjacent mapping prevents an IOMMU fault. No-new-kernel-log observations
must not be used as proof of correct addressing. The register/layout cause is
still being isolated at this point in the investigation.

`TA5_TRACE_BO` now labels mmap offsets correctly and queries `MSM_INFO_GET_IOVA`
for every submitted BO, including targets used only by the primary stream. The
old trace's `iova` label on `MSM_INFO_GET_OFFSET` was wrong.

Evidence: `/home/ishu/a5xx-test-tmp/fault-20261018/` on the device, with copies under
`/tmp/opencode/fault-20261018/` on the laptop. Each supervised run preserves the
client's stdout, stderr, exit status, kernel logs before/after (without clearing),
compositor journal and session PIDs separately. Tests use
`MESA_SHADER_CACHE_DISABLE=true`. Test applications are built on the device; the
Vulkan driver is cross-built on the laptop and deployed only to the test directory.

### Corrections to earlier conclusions

- Sections 10.7/10.8 do **not** establish that the compositor cannot import any
  DMA-BUFs: the Wayland v4 probe omitted feedback, and the EGL query probe used
  incorrect function signatures. The observed import error/crash is real; its
  root cause remains unproven.
- A trace aborted on an optimized-out GDB value; the claim that swapchain creation
  was never called was wrong. A later trace reached it and found an empty modifier
  list. `supports_modifiers=false` avoids advertising the unimplemented path.
- The shm path uses ordinary host-visible allocations and a copy, not implemented
  external-memory import.
- `ea58ce9` does not establish correct animation or classify the visible artifacts
  as independent of WSI. Its copy replay, pitch and binding-offset bugs remain.
- Record-time buffer uploads are not generally valid merely because an application
  happens to submit shortly afterward.
- `VK_FORMAT_D32_SFLOAT` is format 126, not D24S8. Earlier depth-bisect script
  results are invalid: that script exported labels instead of the intended flags.
- GPU faults were observed, but they do not prove why an unsupervised client
  vanished. A missing coredump does not prove SIGKILL, and an infinite `poll()`
  does not itself kill the process.

The current session could capture PNGs but its image reader could not display
them. New measurements here are pixel/readback checks, not claims of visual
inspection. Earlier screenshot descriptions remain historical observations.

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

## 10. Port runtime findings (2026-10-17, post-commit, UNCOMMITTED at time of writing)

The port was built, deployed and tested. Two crashes found and fixed; one issue
still open.

### 10.1 Fixed: `vkEnumeratePhysicalDevices` segfaulted

Backtrace:

```
#0  get_semaphore_sync_type (pdevice=...) at vk_semaphore.c:108
#1  vk_common_GetPhysicalDeviceExternalSemaphoreProperties
#2  wsi_device_init (...) at wsi_common.c:153
#3  ta5_wsi_init (pdevice=...) at ta5_wsi.cc:114
#4  ta5_physical_device_init (...) at ta5_driver.cc:465
```

`wsi_device_init()` queries external semaphore properties, which walks
`pdevice->supported_sync_types` (`vk_semaphore.c:108`). That was NULL, so it
dereferenced NULL. Turnip publishes the DRM syncobj type before `tu_wsi_init`
(`tu_device.cc:1859`); we never did.

Fix, following asahi's `hk_physical_device.c:1360`: add `syncobj_sync_type` and a
`sync_types[2]` array to `struct ta5_physical_device`, fill from
`vk_drm_syncobj_get_type(dev_fd)`, and set `vk.supported_sync_types`. Must
happen **before** `ta5_wsi_init`.

### 10.2 Fixed: `vulkaninfo` jumped to address 0

After 10.1, `vulkaninfo --summary` segfaulted at `#0 0x0000000000000000`.

The surface queries (capabilities, formats, present modes) live on the
**physical-device** dispatch table, which `vk_device_init`'s `overwrite=false`
merge never touches — that only merges device-level entries. Turnip adds
`wsi_physical_device_entrypoints` explicitly (`tu_device.cc:1846`); we did not.

Fix: a second
`vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_physical_device_entrypoints, false)`
after ours, plus `#include "wsi_common.h"`.

### 10.3 FIXED: three WSI entrypoint tables were never merged

The `overwrite=false` merge in `vk_device_init` only covers **device-level**
entries, and `vk_common_*` are weak aliases that do **not** link statically —
in the built `.so`, `vk_common_CreateSwapchainKHR` and
`vk_common_AcquireNextImageKHR` both stay `U` (undefined) while
`wsi_CreateSwapchainKHR` sits beside them, defined and unreferenced.

So all three tables need an explicit merge. Each was missing, each produced a
different failure:

| Table | File | Symptom when missing |
|---|---|---|
| instance | `ta5_instance.cc` | `vkCreateHeadlessSurfaceKHR` → NULL; loader returns a **stub surface**, which is not an error at creation. Later: `getSurfaceCapabilitiesKHR: ErrorExtensionNotPresent`. gdb confirmed none of `wsi_CreateHeadlessSurfaceEXT`, `...CapabilitiesKHR`, `...Capabilities2KHR` was ever entered. |
| physical device | `ta5_driver.cc` | `vulkaninfo` jumped to **address 0** on the first surface query. |
| device | `ta5_device_create.cc` | Loader: `vkCreateSwapchainKHR: Driver's function pointer was NULL, returning VK_SUCCESS`, then segfault. |

Turnip does all three (`tu_device.cc:1989`, `:1846`, `:2874`); so does every
other Mesa driver. Fixed in `18e67b5`.

Lesson worth keeping: **weak-undefined ≠ stub-in-table silently falls through.**
An entry that resolves to NULL is skipped by `..._from_entrypoints`, so a
missing merge shows up as a NULL dispatch slot or a loader stub, not a build
error.

### 10.4 FIXED: our VkFence was not a struct vk_fence

**Real bug, found by reading the layout against what wsi_common expects.**

`wsi_signal_fence_for_image()` (wsi_common.c:2246) does:

```c
VK_FROM_HANDLE(vk_fence, fence, _fence);
vk_fence_reset_temporary(device, fence);
```

which reads `fence->temporary` (vk_fence.c:169). Our `struct ta5_fence` began
with `vk_object_base base;` then `ta5_device *device` — so that read landed on
the **device pointer**, which is always non-NULL. wsi_common therefore believed
every fence carried temporary state, treated the *device* pointer as a
`struct vk_sync *`, and called through its garbage `type->finish`.

That is why the crash surfaced inside `ta5_GetDeviceQueue` with
`queueFamilyIndex=0xF76D2E40` (a pointer fragment): it had jumped through a
bogus function pointer that happened to land in this driver.

Fix: embed the runtime struct rather than copying fields.

```c
struct ta5_fence {
   struct vk_fence base;
   struct ta5_device *device;
   bool signaled;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_fence, base.base, VkFence, VK_OBJECT_TYPE_FENCE)
```

The sync moved to `base.permanent`, so `vk_sync_create` (which allocates) became
`vk_sync_init` (which fills in place) and `vk_sync_destroy` became
`vk_sync_finish`. `ta5_DestroyFence` now also calls `vk_fence_reset_temporary`
first, matching `vk_fence_destroy`, because wsi_common attaches a dma-buf wait
sync to the fence.

Evidence it worked: the backtrace moved from a garbage jump into
`ta5_GetDeviceQueue` to well-defined frames, and acquire now completes.

### 10.5 OPEN: present returns ErrorInvalidExternalHandle, teardown crashes

Progress: the scene now renders and reaches present.

```
[clear] duration=1: Failed with exception: vk::Queue::presentKHR:
                        ErrorInvalidExternalHandle
```

then, during vkmark teardown:

```
#0  0x0000000000000000 in ?? ()
#1  wsi_destroy_image (chain=..., image=...) at wsi_common.c:1053
#2  wsi_headless_swapchain_destroy  at wsi_common_headless.c:466
```

Frame #0 is another **NULL dispatch slot**, this time on a device-level call
from `wsi_destroy_image`. Not yet identified which entry point.

**Open questions:**
1. Which entry point is NULL at wsi_common.c:1053?
2. Why does `wsi_QueuePresentKHR` return `ErrorInvalidExternalHandle`?
3. Both may share a root cause -- a dispatch table that is incomplete somewhere.

### 10.6 Test matrix as of 2026-10-18

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
### 10.7 OPEN: Wayland never reaches swapchain creation (2026-10-18)

**vkcube --wsi wayland --c 3 now:** surface is created, GPU is selected, then
SIGSEGV. phoc survives (the compositor crash from §4 is gone).

gdb, breaking on both the loader trampoline and the wsi implementations:

```
HIT wsi_CreateWaylandSurfaceKHR          <- surface creation
HIT wsi_GetSwapchainImagesKHR swapchain=(nil)
#0  wsi_GetSwapchainImagesKHR (device=..., _swapchain=0x0, ...)  wsi_common.c:2183
```

**`vkCreateSwapchainKHR` is never called by the application at all** — the loader
breakpoint never fires, and `wsi_CreateSwapchainKHR` / `wsi_swapchain_init` are
never entered. vkcube bails before swapchain creation and then queries images on
a NULL swapchain.

Also fixed this round: `VK_KHR_wayland_surface` was still gated behind
`#ifdef TA5_HAVE_WAYLAND`, whose define disappeared with the bespoke Wayland
build, so the driver silently stopped advertising it and the loader refused the
platform outright. Now unconditional (`5e7fc91`). Nothing about a missing
extension advertisement is a compile-time error.

**Next step, not yet started:** find why vkcube bails before swapchain creation.
The earlier headless `vkQueuePresentKHR: ErrorInvalidExternalHandle` is probably
the same root cause — something in the surface/swapchain setup the driver must
satisfy for wsi_common. Prime suspects, in order:
1. `GetImageSubresourceLayout` — its returned `offset` includes `image->offset`,
   which looks like a double-count of the binding offset (noted in §5-era notes).
2. `GetMemoryFdKHR` — new, and wsi_common calls it for every swapchain image.
3. Modifier negotiation: whether phoc advertises `DRM_FORMAT_MOD_LINEAR` for
   ARGB8888. wsi_common's `find_format` may pick nothing, leaving the chain
   unconfigured.

Item 3 is directly observable: log the formats/modifiers phoc advertises over
`wl_dmabuf` and compare against what the swapchain picks.

### 10.8 MEASURED: the compositor cannot import dma-bufs at all (2026-10-18)

Two probes, built for aarch64-musl and run on the device, both read-only and
both independent of the Vulkan driver.

**1. What phoc advertises over wl_dmabuf** (`/home/ishu/a5xx-test-tmp/wl-dmabuf-probe`):

```
bound zwp_linux_dmabuf_v1 version 5
totals: 0 formats, 0 modifiers
```

The global exists at version 5 and then advertises **nothing**. There is no
format/modifier list for a client to match against.

**2. Where that list comes from** (`/home/ishu/a5xx-test-tmp/egl-dmabuf-probe`):

```
EGL 1.5
EGL_EXTENSIONS: ... EGL_EXT_image_dma_buf_import
                 EGL_EXT_image_dma_buf_import_modifiers ...
eglQueryDmaBufFormatsEXT -> ok=1 num=0
```

phoc derives its wl_dmabuf list from its renderer's
`eglQueryDmaBufFormatsEXT()`. That returns **0 formats**, which is why
phoc advertises 0. The extension is present; the format list behind it is empty.

**Consequence.** phoc cannot import a dma-buf produced by *any* client, not
just this driver. `EGL_BAD_ALLOC "createImageFromDmaBufs failed"` is not
specific to vulkan-a5xx. No amount of
`VK_EXT_image_drm_format_modifier` work in this tree can fix it, because
there is nothing on the compositor side to negotiate with.

This does not touch `src/gallium` (which is out of scope for this port). The
real fix -- making the GL/GBM stack advertise importable formats for this GPU
-- lives in the system Mesa's EGL/gallium, not in vulkan-a5xx.

**Where this leaves vulkan-a5xx.** The driver side is now correct:
`vkcube --wsi wayland --c 30` exits 0, and headless vkmark clear exits 0 at
~4000 fps with zero gpu faults. What remains is a display-side limitation.

**Next experiment, and it is a design decision, not a bug fix:**
`wsi_device->sw = true` selects WSI_WL_BUFFER_SHM_MEMCPY, where the rendered
image is copied into a wl_shm buffer that any compositor can import. Our
swapchain images are already linear host memory, so that copy is close to
free. It would give a genuinely working Vulkan surface on this desktop, at
the cost of a CPU copy per frame and the loss of zero-copy presentation.

### 10.9 FIXED: the blit ran at record time, so every frame was black

Screenshots (grim, while vkcube ran) settled what exit codes could not.

Before this fix, the vkcube window covered the screen and every pixel of it was
**pure black** -- exactly one distinct colour across the whole client area,
against 585 in a baseline capture. The client exited 0 throughout.

**Cause.** vkCmdCopyImageToBuffer is a host memcpy in this driver. wsi records
the image -> buffer blit for a wl_shm swapchain exactly once, inside
wsi_finish_create_blit_context() during *swapchain creation*, long before the
application has drawn anything. Copying at record time therefore copied the
image's initial contents, which are zeros.

This is worse than the usual record-time hazard. CmdCopyBufferToImage gets away
with it because uploads happen immediately before the submit that needs them;
the wsi blit is recorded once and executed every frame.

**Fix.** The copy is queued on the command buffer and performed by
ta5_queue_submit_batch() after the kernel fence reports the GPU is finished.
The region data and the level layout are snapshotted at record time, so the
copy cannot be moved by a later rebind, and the queue is dropped on reset or
begin so a re-recorded buffer does not inherit a stale copy.

**After, captured on screen:**

  vkcube --wsi wayland --c 400   exit 0, phoc and phosh alive
  the cube renders: correct textures (LUNAR / LUNAR GLT), correct depth
  shading between faces, correct perspective, and it animates between frames

**Remaining, visible in every capture:** black dashed lines exactly along the
cube's triangle edges, and a corrupted vertical column down the left edge.
That is a rendering defect, not a WSI one -- it is inside the frame the driver
produced -- and it is unexplained. It is the next thing to investigate.

Still open from earlier: the GPU fault storm (10-30 faults per run) and the
lazily created queues.


## 11. GPU fault storm: root cause (2026-10-18)

Three independent defects produced one symptom. The evidence is a device-built
test (`depth-clear-check`, `depth-clear-analysis`) that clears a D32 attachment
and nothing else, then checks every texel plus a 256 KiB guard placed after the
image's reported memory requirement. The guard is what makes the damage visible
at all: an overrun into mapped memory produces no kernel fault, which is why so
many earlier runs reported "0 faults" while writing out of bounds.

### 11.1 Layout: out-of-bounds writes (fixed)

Row pitch was aligned to 64 **bytes**; Gallium aligns to 64 **texels**
(`freedreno_layout.h:333`). The last mip level had no tail padding, while
Gallium rounds it to a 32-row envelope because a5xx reads sysmem at 32x32
granularity and over-fetches past the final level (`fd5_layout.c:67-68`).

Before, D32 clear only, "wrong" is uncleared texels, "guard" is bytes clobbered
past the reported end:

```
64x64    pitch  256  requirement  16384   wrong=0      guard=0      PASS
128x64   pitch  512  requirement  32768   wrong=0      guard=0      PASS
256x64   pitch 1024  requirement  65536   wrong=0      guard=0      PASS
256x33   pitch 1024  requirement  36864   wrong=192    guard=3072   FAIL
257x64   pitch 1088  requirement  69632   wrong=512    guard=512    FAIL
300x300  pitch 1216  requirement 368640   wrong=3176   guard=768    FAIL
```

256x64 against 256x33 isolates height at a fixed pitch; 256x64 against 257x64
isolates pitch at a fixed height. Both corrections were needed to fit.

The historical fault offsets (256-1152 bytes past a `0x0304c000` allocation) match
this guard damage, so the long-standing fault storm and this are the same bug.

### 11.2 D32 was never actually cleared (fixed)

The CCU fast clear walks a Z32 attachment in 16-row stripes and skips one of
every two, covering 192 of 300 columns. Gallium refuses this clear outright
(`fd5_draw.c:229-231`) and falls back to a GMEM blit, which does not exist here
because this driver renders into linear sysmem in bypass mode. `TA5_SLOW_CLEAR`
was worse: 100% missed, because it drops FAST_CLEAR and emits no replacement
trigger. Neither a fault nor an error reported either.

Depth now goes through a host-side clear, as `ta5_CmdClearColorImage` already
does for colour.

### 11.3 Ordering: clears and copies are on opposite sides of the fence

A clear is a CPU write to memory the GPU is about to read, so it runs *before*
the batch is submitted. An image-to-buffer copy reads what the GPU produced, so it
runs *after* it completes. Both were initially wrong in different ways: a
record-time clear is erased by whatever the application does to the image
afterwards, and the test's own guard fill is designed to catch exactly that.

### 11.4 Replay: deferred work must survive execution (fixed)

Both queues drained themselves after executing. A command buffer may be submitted
repeatedly -- ordinary Vulkan, and exactly what wsi's blit does, recorded once per
swapchain image and submitted on every present. The symptom was one frozen frame
per swapchain image:

```
frame 0   wrong_depth=0/90000       clear applied
frame 1   wrong_depth=90000/90000   queue already drained
```

Recordings now persist until reset or destruction.

### 11.5 Result

All six extents, both frames, exit 0, with zero kernel lines written during the
run window (the 30 faults still in dmesg predate it; the harness timestamps runs
rather than clearing the log).

vkcube on the Wayland display still shows a dotted diagonal across the cube
faces. It runs in a straight line in texture space, so it is a vertex/raster
artefact rather than the depth defect above. Not yet diagnosed.


## 12. Draw path: what was found, and what was wrong about how

### 12.1 Method note, first, because it applies to everything above

A guard region after an allocation catches out-of-bounds writes that produce no
fault. Pixel classification (clear colour vs shader colour vs black) catches
draws that silently produce nothing. And a *positive control* -- a switch that
must change the result, run to prove the harness responds -- makes any matrix of
"no change" results interpretable. All three are cheap; none were used for the
first few hours of this investigation, and the results were repeatedly wrong
because of it.

### 12.2 VTXCNT: draws without vertex buffers rasterised nothing (`ee0f89c`)

`VFD_CONTROL_0.VTXCNT` was set to `fetch_cnt`, the number of vertex-buffer
*fetch slots*. A draw binding no vertex buffer has zero of those, so it
programmed `VTXCNT = 0`: the fetch unit processed no vertices, every attribute
read as zero, and all vertices landed on `(0,0,0,1)`. A zero-area triangle,
which rasterises no fragments and raises no fault.

The comment directly above already specified the correct behaviour -- "without
them it is the draw's vertex count, which is what fd5_draw() writes
(`p4->vtxcnt = p->count`)" -- and the code did something else.

```
before:  ta5[vfd]: VTXCNT=0 (fetch slots=0, draw verts=6)
         pixels: clear_only=65536 shader_only=0 both=0 black=0
after:   ta5[vfd]: VTXCNT=6 (fetch slots=0, draw verts=6)
```

### 12.3 Viewport packet (`91842e5`)

Two real defects, neither of which explains the pixel shortfall below:

- `TA5_SKIP_VPORT` emitted 4 words into a packet whose header declares 6, so
  the switch desynchronised the command stream. Any bisect that set it on such a
  build was measuring corruption, not the switch.
- `VkViewport`'s x/y/width/height were ignored; `XOFFSET` was hardcoded to
  `width/2`.

`TA5_TRACE_VPORT` now reports the programmed values, so they need not be
inferred from pixel counts as they were here.

### 12.4 OPEN: a quad covers 12636 of 65536 pixels

A `-0.9..0.9` NDC quad on a 256x256 target should cover ~53824 pixels. It
covers **12636**, consistent with a ~112x112 box. Measured:

```
ta5[vport] fb=256x256 vp_set=0 -> xoff=128.000 xscale=128.000
pixels: clear_only=12636
```

The viewport is correct, so the halving happens downstream of it. The window
scissor is also correct (full target). Not yet localised. Candidates not yet
examined: `PC_PRIMITIVE_CNTL_STRIDE_IN_VPC`, the VPC stride, or the clip/
perspective stage. Do not re-test the viewport: it is measured correct.

### 12.5 Theories tested and refuted

Recorded so they are not retried:

| Theory | Result |
|---|---|
| Texture descriptor `ARRAY_PITCH` wrong, causing the cube diagonal | **Refuted.** The runs behind it never rendered a single pixel (`lit=0` from a black clear), so every register decoded from them described a frame that was never drawn. |
| Vertex-id system-value lowering broken | **Refuted.** `ta5_shader.cc` does call `nir_lower_system_values`, and with a plain `gl_VertexIndex` shader the trace shows `REGID4VTX=0` (a real register) and `vs inputs: 1`. The sentinel `252` was `regid(63,0)` from a *probe shader* that indexed a local const array, not a driver defect. |
| `VK_ERROR_OUT_OF_POOL_MEMORY` / `-1000012000` are malformed return values | **Wrong.** Both are ordinary Vulkan result codes. They were misidentified as garbage without being looked up. |
| Fragment/draw path broken generally | **Refuted.** vkcube renders geometry; draws with vertex buffers work. Only fetchless draws were affected (section 12.2). |

### 12.6 Process notes

The measurements in this investigation were generally sound and reproducible.
The *interpretation* repeatedly outran them, and these claims all had to be
withdrawn after the next measurement:

- "ARRAY_PITCH is 2x too large, that is the diagonal's cause" -- decoded from a
  compressed five-word trace print, against a frame that had not rendered.
- "found the real bug" for `VTXCNT` -- true as a bug, but it was not the cause
  of the symptom being investigated, and saying otherwise invited a false
  conclusion that it was.
- "a second failure in sysval lowering" -- asserted after finding the driver
  already did the lowering in question.
- Two `VkResult` values called "garbage" that were standard codes.

The recurring error was stating a cause before a measurement that could have
refuted it had been taken. A bisect matrix with no positive control reads as
"the code path is not reached"; it only means the matrix does not cover the
failing path.

### 12.7 Still open

- The 12636-of-65536 coverage shortfall (section 12.4).
- The cube's dotted diagonal artifact (section 12.5 refutes the texture theory).
- Deferred image->buffer copies: per-region mip, destination pitch from
  `bufferRowLength`, source z offset, and the destination buffer's binding
  offset are all still handled incorrectly.
- Depth attachment addresses drop `image->offset` and the level offset.
- Attachment 0 is unconditionally bound as RGBA8 colour.
- Queue lifecycle: queues created lazily, no matching `vk_queue_finish`.
- KMS presentation, unvalidated since the private WSI was removed.


## 13. vkmark fault census: the storm is closed (2026-10-18)

Measured on `93d6a78`, deployed library sha256 `f88f4b55...`, device at
`172.16.42.1`. One vkmark process per scene, 300x300, headless WSI,
`MESA_SHADER_CACHE_DISABLE=true`, `duration=3`. `dmesg` is never cleared;
faults are attributed by the monotonic timestamp of each fault line against the
`/proc/uptime` window sampled around each run, so a fault belongs to exactly
one run.

```
scene       exit   fps      scene       exit   fps
clear         0     22       effect2d      0     16
vertex        0     18       desktop       0     18
texture       0     18       cube          0     21
shading       0     19
```

Per-scene fault attribution printed nothing. Verified as silence rather than
suppression: **zero kernel lines of any kind** inside the run window
(10402..10424), while the newest fault line in the whole log is at 10008 --
several hundred seconds earlier -- and no suppression counters fired in it.

Repeated, to show it is not a one-off:

```
3x  vertex texture shading cube   300x300   exit=0 each, faults=0
```

Then twelve isolated runs, each in its own window, across four workloads:

```
probe (cubemap)  x3   exit=0   faults=0
probe (plain)    x3   exit=0   faults=0
vkcube wayland   x3   exit=0   faults=0
vkmark cube      x3   exit=0   faults=0
```

**The GPU fault storm while running vkmark is fixed.** It was the out-of-bounds
write in `22781a1`: the guard-region test measured overruns of +256..+1152 bytes
past each attachment, which is the same range as the original fault cluster
around `0x0304c000` that section 3 recorded and could not explain. With the row
pitch aligned to 64 texels and the last mip padded to a 32-row envelope, the
overrun is gone and the faults with it.

### 13.1 Not faults during vkmark

**Two things this does not establish.** Throughput is 16-22 FPS. And exit 0
with no faults says nothing about pixels -- section 12.4's coverage shortfall
(a quad covering 12636 of 65536 pixels where ~53824 is expected) is still open,
so vkmark is rendering without faulting, not rendering correctly.

**A separate, sporadic fault remains undiagnosed.** Three faults of a different
kind exist in the log:

```
[9753.913379] gpu fault: iova=0000000000000000   fsynr=0x2
[9958.838827] gpu fault: iova=0000000000000000   fsynr=0x2
[10008.340630] gpu fault: iova=0000000000000000  fsynr=0x2
```

`fsynr=0x2` against the old cluster's `fsynr=0x13`, and address zero rather than
an overrun past a buffer: a different mechanism, not a recurrence of the fixed
one. One was reproduced in a window covering three commands; it did not
reproduce in twelve subsequent isolated runs. Its source is **not** established.
An earlier note in this file attributing it to cube-map sampling was withdrawn:
the window covered three commands and only one fault fired, so nothing supported
picking the texture path.

### 13.2 Tooling

`/home/ishu/a5xx-test-tmp/fault-census.sh` runs the per-scene census with
timestamp-window attribution. `a5xx-fault-run.sh` runs one supervised client,
preserving stdout, stderr, exit status, kernel logs before and after, the
compositor journal and session PIDs separately, and refusing to overwrite an
existing result directory. Both leave `dmesg` intact deliberately: earlier work
that cleared it lost the only record of the faults it was investigating.


## 14. Native DMA-BUF presentation replaces the wl_shm workaround (2026-10-18)

`sw_device` goes back to false.  Presentation hands the compositor a dma-buf it
imports, and the per-frame host copy is gone.

```
                         shm shim      native
image->buffer copies/frame      1          0
vkcube 60 frames           10.22s       1.39s
vkcube throughput         5.87 fps    43.17 fps
vkcube exit                   0          0
phoc                       survives   survives
```

7.4x, and zero copies per frame is the direct evidence that the memcpy is gone
rather than merely cheaper.  vkmark on the Wayland display goes from

    [clear] FPS: 0   [cube] FPS: 0   Score: 0

to

    clear 387  vertex 50  texture 47  shading 47
    effect2d 68  desktop 155  cube 439     Score 170, faults 0

with phoc and phosh both surviving.  The cube scene was captured with grim and
renders correctly: a textured, depth-shaded, perspective cube with no dashed
edges and no dropouts.

### 14.1 What the native path needs

`supports_modifiers = true` promises VK_EXT_image_drm_format_modifier, which
wsi_common uses to match the compositor's advertised (fourcc, modifier) pairs.
Four things answer for it, none of which existed:

  - `ta5_GetPhysicalDeviceFormatProperties2` -- report DRM_FORMAT_MOD_LINEAR
  - `ta5_GetPhysicalDeviceImageFormatProperties2` -- accept
    VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, answer the modifier info
  - `ta5_GetImageDrmFormatModifierPropertiesEXT` -- new; the image's layout
  - `ta5_CreateImage` -- honour VkImageDrmFormatModifierListCreateInfoEXT

These headers predate the finalised extension: there is no
`VkImageFormatProperties2::modifierToProperties` array and no
`VkImageDrmFormatModifierInfoEXT` here, and the per-modifier create info is
named `VkImageDrmFormatModifierExplicitCreateInfoEXT`.  Written against the
finalised names it does not compile, which is worth knowing before trying.

### 14.2 Two bugs, both mine, both found by measurement

**The modifier count is an output, not a capacity.**  The first version wrote the
modifier only when `drmFormatModifierCount > 0`, on the assumption that it was
the caller's buffer size.  wsi_common calls with the field set to *zero* and
asserts it is nonzero afterwards:

    wsi->GetPhysicalDeviceFormatProperties2(...);   /* count arrives 0 */
    assert(modifier_props_list.drmFormatModifierCount > 0);

so the native path could never enable itself.

**A valid image was rejected.**  `GetImageDrmFormatModifierPropertiesEXT`
required `bound_memory`.  GDB at the failure showed a perfectly formed image --
R16G16B16A16_SFLOAT, 360x713, linear, row_pitch 3072, correct
`drm_format_mod` -- with `bound_memory = 0x0`, because
`wsi_create_native_image_mem()` calls this *before* allocating the memory behind
it (`wsi_common_drm.c:810`).  That returned VK_ERROR_INITIALIZATION_FAILED,
which `wsi_CreateSwapchainKHR` passed straight through, so every native
swapchain failed; vkcube ignores the error and calls
`wsi_GetSwapchainImagesKHR` with a NULL handle, which segfaults.

Both were found by asking the driver what it did rather than reasoning about it.
The second one only surfaced because the swapchain error code was captured --
every earlier native attempt had been reported as a compositor crash.

### 14.3 Superseded conclusions

- **"phoc cannot import our dma-bufs / advertises no dma-buf formats" is
  disproven.**  phoc imports the buffer without complaint.  Section 10.8's
  probes were wrong (one bound wl_drm v4 without requesting feedback, one
  hand-declared the EGL query functions with the wrong signatures), and section
  12.5 already recorded that.  This was the stated reason the native path had
  been abandoned and it was never true.
- **"vkmark renders nothing on Wayland"** was true, but not for the reason it
  looked like.  vkmark was submitting 8-byte command streams -- a single empty
  PM4 header -- so it rendered nothing on *either* WSI.  The presentation path
  was never exercised.  Section 13's fault census was therefore measuring a
  driver doing no work, and its "fault-free" result said less than it appeared
  to.
- The wl_shm copy path is retained (`ta5_CmdCopyImageToBuffer` and the deferred
  host-clear queue) because wsi still uses it for the shm fallback and for
  image readback, but it is no longer on the presentation path.

### 14.4 Still open, unchanged by this

The coverage shortfall in section 12.4 (a quad covering 12636 of 65536 pixels
where ~53824 is expected), the sporadic iova=0 faults of section 13.1, the
copy-command pitch and binding-offset gaps, the dropped depth attachment
offsets, attachment 0 assumed RGBA8, and queue lifecycle.


## 15. Chasing the "stripes" in the texture scene: three refuted theories

The texture scene was reported as showing regularly spaced dropped pixels.  It
does show regular structure, but three separate explanations were asserted and
then refuted by measurement.

### 15.1 The texture descriptor is correct

TA5_TRACE_TEX was extended to decode every field of the texel descriptor
through the generated __MASK/__SHIFT constants and print the image's own layout
next to it.  For vkmark's texture:

    dword0=0c006884  FMT=48 SWIZ=0,1,2,3 MIPLVLS=0(1 levels) TILE_MODE=0
    dword1=01000200  WIDTH=512 HEIGHT=512
    dword2=20040000  PITCHALIGN=0 PITCH=2048(bytes) TYPE=1
    dword3=00000100  ARRAY_PITCH=256(1048576 bytes) MIN_LAYERSZ=0
    image: 512x512 mip_levels=1 layers=1
    level0: row_pitch=2048 layer_pitch=1048576 level_size=1048576
    CHECK pitch  MATCH   CHECK mips MATCH   CHECK array MATCH

### 15.2 Two of the "theories" were the diagnostic's own bugs

**"PITCH is in 128-byte units, so it is 128x too large."**  It is not.  The
field holds bytes: fd6, which is known-good and in this tree, passes the row
pitch straight into A6XX_TEX_MEMOBJ_2_PITCH (fd6_view.cc:297), and the <<7 in the
generated macro is only the field's bit position.  The first version of the
trace multiplied by 128 and invented a mismatch that does not exist.

**"HEIGHT is 256, not 512."**  HEIGHT is bits 15-28, not 16-27; the first trace
read it a bit low.  FMT is bits 22-27, not bits 0-7.  Both were hand-decoding
errors.  The generated macros in this tree are setters with no __NAME getters,
so the trace extracts fields via the __MASK/__SHIFT constants instead of
arithmetic.

### 15.3 The ARRAY_PITCH theory is refuted

The standing theory -- that TEX_CONST_3_ARRAY_PITCH and MIN_LAYERSZ count 4 KiB
units, that feeding them bytes wraps a face stride, and that this is why the
texture scene is wrong -- is not supported.  ARRAY_PITCH decodes to 256, which
is 1048576 bytes, and the image's layer_pitch is 1048576.  They agree.  The
comment in ta5_texel.cc about the 4 KiB units is correct as far as it goes, but
the encoder already passes the right values.

### 15.4 The "12-pixel stripes on a 16-pixel grid" are texture content

Measured on a 720x1524 capture, dark runs were consistently 12px wide with
starts at 4 (mod 16), which looks like a tiler artifact.  It is not: comparing
the same image at three heights, the dark runs are at unrelated x positions.

    y=396: 348  548  620  648  668
    y=696:   4   52  106  129  337  452  577  644
    y=996:  60  116  180  308  486  583  642

A screen-space artifact would repeat at the same x down the image.  These move
with the content, so they are the plank texture's own features -- the bolt
holes along the seams, which the zoomed crop confirms.

A second confirmation: capturing the scene at 540x1140 and 720x1524 cannot be
used to test scale, because vkmark's quad rotates continuously, so the two
captures are at different animation phases.  That comparison was attempted
before noticing this.

### 15.5 What is actually established

**No rendering artifact has been demonstrated for the texture scene.**  The
scene renders a clean, tilted, correctly textured plank quad; all seven scenes
run at Score 168 with zero faults; the texture descriptor agrees with its image
on every field checked.

The image-based analysis that produced the stripes claim was measuring texture
content, and the theory that followed from it -- 12-wide bands on the tiler's
16-pixel grid -- is refuted by the vertical-alignment test above.

The genuinely open defect is still section 12.4's coverage shortfall, which
needs a controlled probe: a known texture, a known expected result, and a GPU
readback of the render target.  Screenshots of an animating scene cannot
establish it, as this section demonstrates three times over.

Verification after the diagnostic was added: all seven scenes, Score 168, faults
0, phoc and phosh alive.


## 16. The dropped pixels are real, and section 15 was wrong

Section 15 concluded the "12-pixel stripes" were texture content.  That is
wrong.  The vertex scene -- a textureless Suzanne model -- shows the same
artifact, and it is screen-space.

### 16.1 Exact geometry

Pure-black pixels inside the bright model, column by column:

    column 448 (control): 178 177 176 175 174 173 172 171 170 169 168 167 ...
    column 452 (artifact): 177 176 175 174 172 171 170 169 [0 0 0 0] 163 162 ...
    column 456 (artifact): 176 175 174 173 172 171 170 169 [0 0 0 0] 162 161 ...

Darks at rows 408-411, 424-427, 440-443, 456-459: **four rows, period 16,
starting at y = 8 (mod 16)**.  In x the run is **twelve columns wide, starting
at x = 4 (mod 16)**.

Tiling the frame into 16x16 cells makes it unambiguous: every affected cell
contains **exactly 48 black pixels**, and 48 = 12 x 4.  The map of affected
cells (value = black pixels in that cell) is strikingly regular -- columns 17,
19, 24, 26, 28, 30, 34, 38, 40 and 42 carry 48 in long vertical runs:

     j  ...                                                     i
    28   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .  48  48   .
    30   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .  48  48   .

so the artifact is a 16-grid phenomenon with a fixed sub-pattern, not content.

### 16.2 What is ruled out

**Textures and sampling.**  The vertex scene samples nothing.  Section 15's
texture-descriptor analysis (15.1) stands on its own and remains correct; it is
only the conclusion drawn from the texture scene's appearance that was wrong.

**The clear path.**  The clear scene writes **one single colour to every
pixel**:

    distinct colours in window = 1
    [255, 0, 41]  1094400 px (100.00%)
    pure-black pixels: 0 of 1094400

**The presentation path.**  The clear scene reaches the screen through the same
swapchain, the same dma-buf import and the same compositor as the vertex scene,
and it is pixel-perfect.  The native path added in section 14 is therefore
exonerated.

So the artifact requires **draw calls**: the clear path writes all pixels, and
fragment rendering loses a fixed 12x4 sub-pattern of every 16x16 tile.

### 16.3 Why section 15 got it wrong

The texture scene's plank features and the artifact have similar widths, and
the image-based filters picked the features.  The discriminating measurement was
never run: **the same artifact in a scene with no textures at all**.  That was
available the whole time and was not tried.

### 16.4 What is not yet established

The cause.  A 16x16 tile is a5xx's bin granularity, and a fixed 12x4 hole
inside it is consistent with a bin-store or tile-write stride problem, but that
is inference, not measurement.  The controlled test is a full-coverage quad whose
render target is read back and compared pixel for pixel; that distinguishes
coverage loss in the rasterizer from a bad colour write, and it is the same
test section 12.4's coverage shortfall needs.


## 17. The swapchain format was 64-bit float; the renderbuffer is not

### 17.1 How it was found

waypipe (wlroots' Wayland proxy, 0.11.2, built with dmabuf support) was used to
run vkmark with phoc out of the path entirely: waypipe's server acts as the
compositor, so it is waypipe that has to import our buffers and phoc never sees
them.  Invoked as the man page documents for local use -- options before the
mode, and the *same* socket path on both ends:

    waypipe --socket /tmp/wp.sock client &
    waypipe --socket /tmp/wp.sock server /usr/bin/vkmark ...

The cube scene came back as magenta and black with half the window dead, while
the same scene under phoc looked correct.

### 17.2 The cause

    ta5[prep]: fb 960x1050, img 960x1050 fmt 97, row_pitch 7680

97 is VK_FORMAT_R16G16B16A16_SFLOAT -- eight bytes per texel -- and 7680/960 is
8, so the allocation was self-consistent.  The problem is that the renderbuffer
is not.  ta5_render_pass_begin() programs the colour attachment unconditionally:

    A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_R8G8B8A8_UNORM)

with only COLOR_SWAP and the sRGB bit varying.  So for any format that is not
8_8_8_8 the RB writes four bytes per texel into a buffer that has some other
stride, and a consumer reading the declared format reads interleaved garbage.
That is the magenta-with-no-green signature: half the bytes are real 8-bit
channels and half are padding.

wsi_common picks the swapchain format from what the compositor advertises
intersected with what the driver supports (wsi_wl_surface_get_formats), so
because R16G16B16A16_SFLOAT was in our format table with COLOR_FILTERED it was
selectable, and it was selected.

### 17.3 The fix

Colour attachment support is much narrower than "is a colour format": the RB can
only write 8_8_8_8.  Every other entry in the format table now goes through
NO_COLOR_ATTACHMENT(), which strips COLOR_ATTACHMENT and COLOR_ATTACHMENT_BLEND
while leaving sampled and transfer features alone.  Twenty entries were
corrected; the seven 8_8_8_8 formats are untouched.

Negotiated format afterwards:

    ta5[prep]: fb 720x1524, img 720x1524 fmt 43, row_pitch 3072

43 is VK_FORMAT_R8G8B8A8_SRGB and 3072/720 is 4 -- what the RB actually writes.

### 17.4 Verification

The same vkmark cube scene through waypipe: magenta-and-black garbage before,
a correctly shaded perspective gradient cube on a clean background after.

On the device, all seven scenes: Score 173, zero GPU faults, phoc and phosh
alive.  cover-probe unchanged: 0 wrong pixels at 512x512 and 720x1524 with
depth, and 720x1524 colour only.

### 17.5 Superseded

**"phoc advertises no usable dma-buf formats"** is refuted again, this time from
the wire.  Captured through waypipe:

    wl_registry.global("wl_drm", 2)
    wl_registry.global("zwp_linux_dmabuf_v1", 5)
    wl_registry.global("zwlr_export_dmabuf_manager_v1", 1)
    tranche_formats([0,0, 1,0, 2,0, ... 56,0, ...])   <- modifier 0 = LINEAR

phoc offers linux-dmabuf v5 with a feedback object and LINEAR modifiers across
a wide format range.  The crash in section 14 was never a missing format.

### 17.6 Still open

The D32 depth failure of section 16 still does not reproduce on the committed
driver, with or without a second GPU client running, and cover-probe was first
introduced against a modified build so there is no committed-driver baseline for
it.  That remains undiagnosed.
