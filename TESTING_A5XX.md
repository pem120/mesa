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