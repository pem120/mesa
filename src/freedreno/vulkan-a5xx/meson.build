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
)

# Generated adreno register/pm4 headers (adreno_pm4.xml.h etc.) are pulled in
# via common/fd5_hw.h.  List them as sources so ninja generates them before
# compiling -- otherwise the include fails on a clean cross build.
libta5_files += freedreno_xml_header_files

libta5_includes = [
    inc_src,
    inc_freedreno,   # covers '.' , './registers', './registers/adreno', './common'
]

ta5_link_with = []
ta5_deps = []
ta5_flags = [sse2_args]
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
  link_args : [
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