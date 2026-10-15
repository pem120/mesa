# Copyright © 2026 Adreno 5xx Vulkan driver authors
# SPDX-License-Identifier: MIT

ta5_entrypoints = custom_target(
  'ta5_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['ta5_entrypoints.h', 'ta5_entrypoints.cc'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'ta5',
    '--include', 'adreno_common.xml.h',
    '--tmpl-prefix', 'ta5', '--tmpl-param', 'chip CHIP',
    '--tmpl-variants', '<A5XX>',
    '--beta', with_vulkan_beta.to_string(),
    '--device-prefix', 'ta5_rmv',
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

libta5_files = files(
  'ta5_device.cc',
  'ta5_device_create.cc',
  'ta5_driver.cc',
  'ta5_formats.cc',
  'ta5_memory.cc',
  'ta5_sync.cc',
  'ta5_cmd_buffer.cc',
  'ta5_descriptor.cc',
  'ta5_shader.cc',
  'ta5_render.cc',
  'ta5_sampler.cc',
  'ta5_texel.cc',
  'ta5_pipeline.cc',
  'ta5_queue.cc',
  'ta5_submit.cc',
  'ta5_instance.cc',
  'ta5_wsi.cc',
  'ta5_wayland.cc',
)

# ---------------------------------------------------------------------------
# VK_KHR_wayland_surface
#
# Four things have to line up: a wayland-scanner, the xdg-shell and
# linux-dmabuf protocol descriptions, the wayland-client headers, and a
# libwayland-client to link.  Any one of them missing and the extension is not
# built and not advertised -- an application handed a
# vkCreateWaylandSurfaceKHR that is not there is worse than one told the
# extension does not exist.
#
# The headers and the protocol XMLs are architecture-independent, so a cross
# build uses the host's.  The library is not: the a5xx cross build is -static
# and the device ships only libwayland-client.so.0, so ta5-wayland-lib names
# that file and it becomes the driver's one dynamic dependency.
# ---------------------------------------------------------------------------
ta5_wayland_opt = get_option('ta5-wayland')
ta5_wayland = false
ta5_wayland_inc = []
ta5_wayland_libs = []
ta5_wayland_files = []
ta5_wayland_flags = []
ta5_link_args = []

if ta5_wayland_opt.enabled() or ta5_wayland_opt.auto()
  wl_scanner = find_program('wayland-scanner', required : ta5_wayland_opt.enabled())

  # The a5xx cross build sets pkg_config_libdir to '' so that host pkg-config
  # is never consulted for target libraries, which also means wayland-client.pc
  # and wayland-protocols.pc are invisible here.  Neither is a library though:
  # the headers and the protocol XMLs are architecture-independent data, so
  # pkg-config is asked first and the standard locations are the fallback.
  wl_dep = dependency('wayland-client', required : false)
  wl_protos_dir = ''

  if wl_dep.found()
    wl_pkgdatadir = run_command(
      pkg-config.get_variable(pkgconfig : 'wayland-protocols', variable : 'pkgdatadir',
                              required : false),
      check : false)
    if wl_pkgdatadir.returncode() == 0
      wl_protos_dir = wl_pkgdatadir.stdout().strip()
    endif
  endif

  if wl_protos_dir == ''
    wl_protos_dir = '/usr/share/wayland-protocols'
  endif

  # ta5-wayland-include wins over pkg-config, for a build whose headers are not
  # where pkg-config would find them, which is every cross build.
  wl_inc_opt = get_option('ta5-wayland-include')
  if wl_inc_opt != ''
    ta5_wayland_inc = ['-I' + wl_inc_opt]
  elif wl_dep.found()
    ta5_wayland_inc = wl_dep.get_include_arguments()
  endif

  wl_lib_opt = get_option('ta5-wayland-lib')
  if wl_lib_opt != ''
    # An explicit path, not -l: the device's libwayland-client.so.0 is not in
    # any directory the cross linker searches, and naming it directly is what
    # makes lld record a DT_NEEDED for it while the rest of the driver stays
    # -static.
    ta5_wayland_libs = [wl_lib_opt]
    ta5_link_args += [wl_lib_opt]
  else
    wl_found = cc.find_library('wayland-client', required : false)
    if wl_found.found()
      ta5_wayland_libs = wl_found
    endif
  endif

  xdg_shell_xml = join_paths(wl_protos_dir, 'stable', 'xdg-shell', 'xdg-shell.xml')
  dmabuf_xml = join_paths(wl_protos_dir, 'stable', 'linux-dmabuf',
                          'linux-dmabuf-v1.xml')

  # The protocol XMLs are custom_target inputs, so a wrong directory fails at
  # generate time with the path in the message; there is nothing to test here
  # that the build will not test better.
  # A boolean per piece rather than a length: include_directories() is not a
  # list, so there is nothing to take a length of.
  wl_have_inc = ta5_wayland_inc.length() > 0
  wl_have_lib = ta5_wayland_libs.length() > 0
  wl_ready = wl_scanner.found() and wl_have_inc and wl_have_lib

  if ta5_wayland_opt.enabled() and not wl_ready
    error('ta5-wayland=enabled, but missing: ' +
          (wl_scanner.found() ? '' : 'wayland-scanner ') +
          (wl_have_lib ? '' : 'a libwayland-client (ta5-wayland-lib) ') +
          (wl_have_inc ? '' : 'the wayland-client headers (ta5-wayland-include) ') +
          '')
  endif

  if wl_ready
    ta5_wayland = true

    # wayland-scanner takes one mode per invocation, so each protocol is two
    # targets: client-header for the inline request functions, private-code for
    # the wl_interface tables the stubs marshal through.  The core wl_*
    # interfaces come from libwayland-client itself.
    wl_protos = [
      ['xdg_shell', join_paths(wl_protos_dir, 'stable', 'xdg-shell', 'xdg-shell.xml')],
      ['linux_dmabuf', join_paths(wl_protos_dir, 'stable', 'linux-dmabuf', 'linux-dmabuf-v1.xml')],
    ]

    foreach proto : wl_protos
      ta5_wayland_files += custom_target(
        'wl_' + proto[0] + '_header',
        input : proto[1],
        output : proto[0].underscorify() + '-client-protocol.h',
        command : [wl_scanner, 'client-header', '@INPUT0@', '@OUTPUT0@'],
      )
      ta5_wayland_files += custom_target(
        'wl_' + proto[0] + '_code',
        input : proto[1],
        output : proto[0].underscorify() + '-protocol.c',
        command : [wl_scanner, 'private-code', '@INPUT0@', '@OUTPUT0@'],
      )
    endforeach
  endif
endif

if ta5_wayland
  ta5_wayland_files += files('ta5_wayland.cc')
  # VK_USE_PLATFORM_WAYLAND_KHR has to be a build flag, not a define inside
  # ta5_wayland.cc.  The generated ta5_entrypoints.cc builds its entrypoint
  # struct from the same macro: without it the wayland fields are absent from
  # the designated initialiser and stay NULL, so the loader's
  # vk_icdGetInstanceProcAddr query for vkCreateWaylandSurfaceKHR comes back
  # empty and the loader answers vkCreateWaylandSurfaceKHR itself with a stub
  # surface that no ICD backs.  vkcube then reads physical_devices[-1] because
  # surface support came back false for every device.
  #
  # The include paths ride along as flags too: the Wayland headers come from
  # outside the tree, and a directory holding only them keeps them from
  # shadowing the tree's own headers.
  ta5_wayland_flags = ['-DTA5_HAVE_WAYLAND=1',
                       '-DVK_USE_PLATFORM_WAYLAND_KHR=1'] + ta5_wayland_inc

  # The generated protocol headers land in this subdirectory's build dir, which
  # inc_freedreno does not cover: it is declared one level up, so its '.' is
  # src/freedreno rather than src/freedreno/vulkan-a5xx.
  ta5_wayland_inc += '-I' + meson.current_build_dir()
  message('a5xx Vulkan: VK_KHR_wayland_surface enabled (linux-dmabuf present)')
else
  message('a5xx Vulkan: VK_KHR_wayland_surface disabled')
endif

# Generated adreno register/pm4 headers (adreno_pm4.xml.h etc.) are pulled in
# via common/fd5_hw.h.  List them as sources so ninja generates them before
# compiling -- otherwise the include fails on a clean cross build.
libta5_files += freedreno_xml_header_files

# Generated Wayland protocol code, when VK_KHR_wayland_surface is being built.
libta5_files += ta5_wayland_files

libta5_includes = [
    inc_src,
    inc_freedreno,   # covers '.' , './registers', './registers/adreno', './common'
]


ta5_link_with = []
ta5_deps = []
ta5_flags = [sse2_args] + ta5_wayland_flags
ta5_cpp_args = cpp.get_supported_arguments([
  '-fno-exceptions',
  '-fno-rtti',
])

if freedreno_kmds.contains('msm')
  ta5_flags += '-DTA5_HAS_MSM'
  ta5_deps += dep_libdrm
endif

ta5_build_id = get_option('ta5-build-id')
if ta5_build_id != ''
  ta5_flags += '-DTA5_BUILD_ID_OVERRIDE="' + ta5_build_id + '"'
endif

ta5_icd_sym = meson.project_source_root() / 'src/vulkan/vulkan.sym'

libvulkan_freedreno_a5xx = shared_library(
  'vulkan_freedreno_a5xx',
  libta5_files,
  ta5_entrypoints,
  include_directories : libta5_includes,
  link_with : ta5_link_with + [
    # IR3 shader compiler.  a5xx is generation 5, so IR3 selects its a4xx
    # back end -- the same path the GL backend uses on this part, and it
    # emits CP_LOAD_STATE4, the only state-load family this hardware has.
    libfreedreno_ir3,
  ],
  dependencies : [
    dep_m,
    dep_thread,
    idep_nir,
    ta5_deps,
    idep_vulkan_util,
    idep_vulkan_runtime,
    idep_mesautil,
    # provides fd_dev_info_raw() and the shared per-generation device table
    idep_libfreedreno_common,
  ],
  c_args : [no_override_init_args, ta5_flags, soong_compat_c_args],
  cpp_args : [ta5_cpp_args, ta5_flags, soong_compat_cpp_args],
  gnu_symbol_visibility : 'hidden',
  # NOTE on link flags:
  #  - ld_args_bsymbolic and ld_args_gc_sections are omitted; the musl cross
  #    build links through zig/lld, which mis-parses them.
  #  - the version script uses the "--option=file" form.  zig's -Wl, splitting
  #    does not preserve a separate argument, so lld absorbed every following
  #    flag as another version-script filename.
  # The exported ICD surface is unchanged: it is pinned by the version script,
  # and the vk_icd* entrypoints carry default visibility regardless.
  link_args : ta5_link_args + [
    '-Wl,--version-script=' + ta5_icd_sym,
    ld_args_build_id,
  ],
  link_depends : files(ta5_icd_sym),
  install : true,
)

ta5_icd_file_name = libname_prefix + 'vulkan_freedreno_a5xx.' + libname_suffix

ta5_icd = custom_target(
  'ta5_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'a5xx_freedreno_icd.' + vulkan_manifest_suffix,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.1', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', ta5_icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'a5xx_freedreno_devenv_icd.@0@.json'.format(host_machine.cpu())
ta5_dev_icd = custom_target(
  'ta5_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.1', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', ta5_icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', ta5_dev_icd.full_path())