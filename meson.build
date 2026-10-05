project(
  'nullspace',
  'c',
  meson_version: '>= 1.8.0',
  version: '0.2.0',
  default_options: [
    'c_std=c99',
    'warning_level=3',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-D_POSIX_C_SOURCE=200809L',
  '-Wshadow',
  '-Wconversion',
  '-Wnull-dereference',
  '-Wdouble-promotion',
  '-Wfloat-equal',
  '-Wsign-conversion',
  '-Wredundant-decls',
  '-Wswitch',
  '-Wunreachable-code',
  '-Wextra-semi',
  '-Wunused',
]), language: 'c')

if get_option('asan')
  add_project_arguments([
    '-fno-omit-frame-pointer',
    '-fno-optimize-sibling-calls',
  ], language: 'c')
endif

if get_option('wallpaper')
  add_project_arguments('-DWALLPAPER', language: 'c')
endif

wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')

wl_mod = import('wayland')
generated = wl_mod.scan_xml(
  'protocol/river-window-management-v1.xml',
  'protocol/river-xkb-bindings-v1.xml',
  'protocol/river-xkb-config-v1.xml',
  'protocol/river-libinput-config-v1.xml',
  'protocol/river-input-management-v1.xml',
  'protocol/river-layer-shell-v1.xml',
)

inc_public = include_directories('include')

subdir('contrib')
subdir('src')

exe = executable(
  'nullspace',
  src_files,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  include_directories: inc_public,
  install: true,
)
