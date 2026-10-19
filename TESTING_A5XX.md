# Testing the Adreno 5xx Vulkan driver

The driver is loaded from the user's home directory; nothing is installed into
`/lib`. That makes testing non-invasive and the revert is just deleting a
directory.

## One-time setup (device)

```bash
sudo apk add vulkan-tools      # provides vulkaninfo
```

Firmware (`a506_zap.*`, `a530_pm4.fw`, `a530_pfp.fw`) must already be in
`/lib/firmware/qcom/`; it is extracted from the stock `sakura` vendor image.

## Build (host)

```bash
cd mesa
meson setup build-musl --cross-file aarch64-musl.cross \
    -Dgallium-drivers=freedreno -Dvulkan-drivers=freedreno \
    -Dallow-fallback-for=libdrm -Dglx=disabled -Degl=disabled \
    -Dgbm=disabled -Dplatforms=""
ninja -C build-musl src/freedreno/vulkan-a5xx/libvulkan_freedreno_a5xx.so
```

Cross-compiling needs this one-time host fix, because zig's bundled copy of
`drm/msm_drm.h` predates the `__cplusplus` guard around the field named `or`
(which is a C++ keyword):

```bash
cp /usr/include/drm/msm_drm.h ~/.zig/lib/libc/include/any-linux-any/drm/msm_drm.h
```

## Deploy and run

```bash
scp build-musl/src/freedreno/vulkan-a5xx/libvulkan_freedreno_a5xx.so \
    ishu@<device>:~/a5xx-test-tmp/
```

On the device, write the manifest (library_path must be absolute):

```json
{
    "ICD": {
        "api_version": "1.1.363",
        "library_arch": "64",
        "library_path": "/home/ishu/a5xx-test-tmp/libvulkan_freedreno_a5xx.so"
    },
    "file_format_version": "1.0.1"
}
```

Then:

```bash
VK_DRIVER_FILES=$HOME/a5xx-test-tmp/a5xx_icd.json vulkaninfo --summary
```

Expected output includes:

```
ta5: found a5xx device '/dev/dri/renderD128' gpu_id=506 chip_id=0x5000600 (Adreno (TM) 506)
GPU0:
        apiVersion         = 1.1.0
        vendorID           = 0x5143
        deviceID           = 0x01fa
        deviceName         = Adreno (TM) 506
```

## Diagnosing a jump to address 0

The loader calls whatever a proc-addr lookup returns, so any entry point the
driver leaves undefined becomes a crash with no message. Set `TA5_TRACE_NULL=1`
to log every name that resolves to NULL:

```bash
TA5_TRACE_NULL=1 VK_DRIVER_FILES=$HOME/a5xx-test-tmp/a5xx_icd.json vulkaninfo --summary
```

`VK_LOADER_DEBUG=all` additionally shows how far the loader itself got.

Two traps that cost real time here:

* **Do not define the `*KHR` aliases** of core functions. The runtime's
  compaction table maps a promoted alias onto the same dispatch slot as the
  core function; defining both trips
  `assert(disp[disp_index] == NULL)` in
  `vk_instance_dispatch_table_from_entrypoints`.
* **`VkFormatProperties2` and `VkImageFormatProperties2` nest their payload**
  (`formatProperties`, `imageFormatProperties`), but
  `VkPhysicalDeviceImageFormatInfo2` is **flat** (`format`, `type`, `tiling`,
  `usage`, `flags`). Mixed access produces confusing "no member named" errors.

## Testing without a Qualcomm GPU

`deploy/fake-msm.c` is an `LD_PRELOAD` shim that intercepts `drmGetVersion`
(reports `"msm"`) and `drmCommandWriteRead` (answers `MSM_PARAM_GPU_ID` = 506,
`MSM_PARAM_CHIP_ID` = 0x5000600). Combined with the host build of the driver
and an x86_64 Vulkan loader, the full enumeration path can be run anywhere:

```bash
gcc -shared -fPIC -o libfake-msm.so deploy/fake-msm.c -I/usr/include/libdrm -ldrm
LD_PRELOAD=./libfake-msm.so vulkaninfo --summary
```

The DRM values are overridable via `FAKE_GPU_ID` and `FAKE_CHIP_ID`.

`deploy/a5xx-icd-test.c` drives the ICD entry points directly, bypassing the
loader. It is useful when the loader is not the thing under test, but it
cannot be cross-compiled for musl by zig (zig emits static binaries without
`dlopen` support), so it is a host-only tool.
## Diagnostic environment variables

Every `TA5_*` knob is a no-op unless set. With one exception noted below,
they change no rendering behaviour; they exist to obtain evidence.

Start from the shipped combination and vary one at a time:

    VK_ICD_FILENAMES=<path to a5xx-<name>.json>
    MESA_SHADER_CACHE_DISABLE=true
    XDG_RUNTIME_DIR=/run/user/10000
    WAYLAND_DISPLAY=wayland-0

Most useful for the kgx investigation:

| Knob | Answers |
|---|---|
| `TA5_CAPTURE_FB=1` | writes `fb-*.raw` captures of the render-pass attachment |
| `TA5_DUMP_IMAGES=1` | non-zero coverage of every live image's level 0 |
| `TA5_DUMP_ATLAS=1` | writes a sampled image out so its bytes can be looked at, not counted |
| `TA5_TRACE_FB=1` | `vkCreateFramebuffer` request versus what its attachment provides |
| `TA5_TRACE_IMG=1` | `vkCreateImage` extent and the device's own `maxImageDimension2D` |
| `TA5_TRACE_PASS=1` | render pass begin/end with framebuffer size and render area |
| `TA5_TRACE_DRAW=1` | every draw call, and which of the three ways it is discarded |
| `TA5_TRACE_BLEND=1` | component mask, blend control, MRT control per draw |
| `TA5_NO_BLEND=1` | forces blending off; see the premultiplied-alpha trap below |
| `TA5_SKIP_DRAW=1` | skips every draw; anything still visible came from a clear |
| `TA5_CLEAR_IN_DRAW=1` | magenta clear at the top of the draw path |
| `TA5_DISASM=1` (`TA5_TRACE_DISASM`) | IR3 disassembly of each shader variant |

### The `getenv` truthiness trap

A knob is enabled by `getenv` returning non-NULL. **The string `"0"` is
non-NULL, so a variable set to `0` is enabled.** Shells that pass
`VAR=0` through to the process rather than unsetting it will silently
enable that probe. Anything that forwards knobs into a harness should
unset them when unset, not pass `"0"`:

    for knob in $(env | sed -n 's/^\(TA5_[A-Z_][A-Z0-9_]*\)=.*/\1/p'); do
        eval "value=\${$knob:-0}"
        [ "$value" = 0 ] && unset "$knob" || export "$knob=$value"
    done

This produced a false reading once: a run that appeared to show a fully
magenta frame was really `TA5_CLEAR_IN_DRAW=0` being honoured as *on*.

### Premultiplied blending hides a zero sample

GTK blends text with `dst = src + dst * (1 - src.a)`. A fragment shader
that outputs `(0,0,0,0)` leaves the destination **unchanged**, so a
broken texture sample is invisible and indistinguishable from an empty
scissor in a framebuffer capture.

`TA5_NO_BLEND=1` forces blending off, so the same zero is written and the
pixels turn black. `black=0` after that probe is what establishes that no
fragment was produced at all, rather than one that was discarded.

## Reading the captures

`fb-*.raw` are `width x height` BGRA for formats 44/50 and BGRA/UNORM
otherwise, tightly packed with no row padding. Convert with:

    python3 -c "from PIL import Image; Image.frombytes('RGB',(931,1006),open('fb.raw','rb').read(),'raw','BGRX').save('out.png')"

Check `rows_with_variation`, not just non-background pixel count. A frame
can be 7% non-background and still be a handful of flat bands with no
rasterised content, which is exactly what kgx produces.

## Probes

`tex-probe`, `atlas-probe` and `blend-probe` live in the deployment
directory. `atlas-probe` is the only one that currently rasterises a
textured quad correctly. `tex-probe` fails exactly the way kgx does --
`clear_only=65536` of 65536 means the entire target is the clear colour,
so its `pattern_mismatch` percentage is not a partial success -- and being
small and compositor-free it is the better place to iterate.
