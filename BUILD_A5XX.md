# Mesa build recipe — a5xx Vulkan work

## Environment

```bash
# uv-managed python (do NOT use plain venv)
cd /home/ishu/Projects/Android/msm8953
uv venv mesa-py
uv pip install --python mesa-py/bin/python mako setuptools packaging pyyaml
```

`mako` + `setuptools` (distutils) + `packaging` + `pyyaml` are all hard
requirements. Mesa's `meson.build` errors out on any missing one.

## Configure (host compile-check)

```bash
cd /home/ishu/Projects/Android/msm8953/mesa
PATH="/home/ishu/Projects/Android/msm8953/mesa-py/bin:$PATH" \
meson setup build-host \
  -Dgallium-drivers=freedreno \
  -Dvulkan-drivers=freedreno \
  -Dallow-fallback-for=libdrm \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dplatforms=""
```

## Build

```bash
cd /home/ishu/Projects/Android/msm8953/mesa/build-host
PATH="/home/ishu/Projects/Android/msm8953/mesa-py/bin:$PATH" ninja
```

`PATH` must be set for **both** configure and build — codegen scripts use
`#!/usr/bin/env python3`, so the shebang resolves via PATH.

## Why each flag

| Flag | Reason |
|---|---|
| `-Dallow-fallback-for=libdrm` | Host has libdrm **2.4.124**; Mesa wants ≥2.4.133. Mesa's bundled `drm-shim` satisfies it. Without this, configure fails. |
| `-Dplatforms=""` | Empty platform list = pure driver build. Host X11 stack is incomplete (`xcb-glx`, `x11-xcb`, `xcb-keysyms` missing). `platforms=android` needs Bionic `cutils`. `platforms=x11` needs the above. |
| `-Dglx=disabled -Degl=disabled -Dgbm=disabled` | Drop display stacks not needed to validate compilation. |

Full driver build produces both:
- `src/gallium/drivers/freedreno/libfreedreno.a` (GL)
- `src/freedreno/vulkan/libvulkan_freedreno.so` (Vulkan / Turnip)

## aarch64 cross-build (target verification)

Host toolchain: `aarch64-linux-gnu-gcc/g++` (Debian 14.2.0). Cross file:
`aarch64.cross` in the Mesa root.

```bash
cd /home/ishu/Projects/Android/msm8953/mesa
PATH="/home/ishu/Projects/Android/msm8953/mesa-py/bin:$PATH" \
PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig \
meson setup build-aarch64 --cross-file aarch64.cross \
  -Dgallium-drivers=freedreno -Dvulkan-drivers=freedreno \
  -Dallow-fallback-for=libdrm -Dglx=disabled -Degl=disabled \
  -Dgbm=disabled -Dplatforms=""

cd build-aarch64
PATH="/home/ishu/Projects/Android/msm8953/mesa-py/bin:$PATH" ninja \
  src/freedreno/vulkan-a5xx/libvulkan_freedreno_a5xx.so \
  src/freedreno/vulkan-a5xx/a5xx_freedreno_icd.aarch64.json \
  src/freedreno/vulkan/libvulkan_freedreno.so
```

Notes:
- Do **not** pass `--sysroot` in the cross file; the `aarch64-linux-gnu-*`
  drivers already know their sysroot. Passing it additionally breaks meson's
  library probes (e.g. 'rt' not found), because the linker then ignores its
  builtin search paths.
- Mesa falls back to building **libdrm 2.4.133** from source for the cross
  target (host has 2.4.124). This also resolves the device-side libdrm
  requirement — the shipped driver links a recent libdrm.
- A full `ninja` in the cross build fails only in the `libarchive` subproject
  (needs aarch64 `libbz2`), which is a **tools-only** dependency for the
  `rnn`/`decode` developer utilities. It does not affect the drivers. Build the
  two ICD targets explicitly to avoid it.

Verified aarch64 output:
- `libvulkan_freedreno_a5xx.so` — ELF aarch64, exports the two ICD loader
  symbols, API 1.1 manifest
- `libvulkan_freedreno.so` (Turnip) — still builds, no regression

## Verified baseline

Clean build, no errors — this is the reference to diff against for the
a5xx work. Branch `a5xx-vulkan` (branched from `main` @ `1f49528`).

## Note on device testing

This host config is **x86_64, compile-only**. It proves the new driver
compiles and links. Actual a5xx Vulkan rendering requires either:
- an aarch64 cross-build, or
- building on the device (postmarketOS + mainline msm8953 kernel)

The on-device libdrm version is a separate concern — must be ≥2.4.133 or
`drm-shim` fallback used there too.