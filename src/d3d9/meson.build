# MADEIRA (WOW64_DESIGN.md section 8): in dxmt_madeira_native this directory
# is not a DLL at all -- it is the native ARM64 half, archived into
# libdxmt_unix.a and reached from the i386 shim through a unix-call table.
# There is no version resource and no import library, and the shim, not this,
# is what installs as d3d9.dll.
if not dxmt_madeira
d3d9_res = wrc_generator.process('version.rc')
endif

d3d9_src = [
  'd3d9.cpp',
  'd3d9_buffer.cpp',
  # MADEIRA (WOW64_DESIGN.md section 8.4): [d3d9-census] counters/report.
  # Its call sites and d3d9_census_names.h are both emitted by
  # gen_d3d9_census.py; run `gen_d3d9_census.py --check` after touching any
  # STDMETHODCALLTYPE definition in this directory.
  'd3d9_census.cpp',
  'd3d9_clear_quad.cpp',
  'd3d9_cube_texture.cpp',
  'd3d9_device.cpp',
  'd3d9_format.cpp',
  'd3d9_fvf.cpp',
  'd3d9_interface.cpp',
  'd3d9_mem.cpp',
  'd3d9_query.cpp',
  'd3d9_shader.cpp',
  'd3d9_shader_scan.cpp',
  'd3d9_state_block.cpp',
  'd3d9_state_defaults.cpp',
  'd3d9_surface.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_texture.cpp',
  'd3d9_validation.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_volume.cpp',
  'd3d9_volume_texture.cpp',
]

if dxmt_madeira
  # The generated boundary (src/d3d9shim/gen_d3d9_thunks.py owns the first
  # two -- regenerate, do not edit) plus the hand-written glue.
  d3d9_src += [
    'unix/d3d9_unix.c',
    'unix/d3d9_unix_table.c',
    'unix/d3d9_native_glue.cpp',
  ]

  d3d9_lib = static_library('d3d9_native', d3d9_src, dxmt_version,
    dependencies        : [ util_dep, winemetal_dep, airconv_forward_dep, dxmt_dep ],
    include_directories : [
      dxmt_include_path,
      include_directories('.'),
      include_directories('unix'),
      include_directories('../dxmt'),
      include_directories('../d3d9shim'),
    ],
    cpp_args            : [ '-fexceptions', '-frtti' ],
  )

  d3d9_dep = declare_dependency(
    link_with           : [ d3d9_lib ],
    include_directories : [ dxmt_include_path ],
  )

  subdir_done()
endif

d3d9_ld_args      = ['-L' + windows_native_install_dir]
d3d9_link_depends = []

d3d9_dll = shared_library('d3d9', d3d9_src, d3d9_res, dxmt_version,
  name_prefix         : '',
  dependencies        : [ util_dep, winemetal_dep, airconv_forward_dep, dxmt_dep ],
  include_directories : [ dxmt_include_path, include_directories('../dxmt') ],
  install             : true,
  install_dir         : windows_native_install_dir,
  install_tag         : 'runtime-metal3',
  vs_module_defs      : 'd3d9.def',
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxmt_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d9',
  input : d3d9_dll,
  output: 'd3d9.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d9_dll,
  build_by_default : true
)
endif
