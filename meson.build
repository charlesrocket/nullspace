project(
  'nullspace',
  'c',
  meson_version: '>= 1.8.0',
  version: '0.1',
  default_options: [
    'c_std=c99',
    'warning_level=3',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
	'-D_POSIX_C_SOURCE=200809L',
	'-Wshadow',
	'-Wconversion',
	'-Wnull-dereference',
	'-Wdouble-promotion',
	'-Wfloat-equal',
	'-Woverloaded-virtual',
	'-Wsign-conversion',
	'-Wredundant-decls',
	'-Wswitch',
	'-Wunreachable-code',
	'-Wno-extra-semi',
	'-Wno-unused-parameter',
]), language: 'c')

wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')

wl_mod = import('wayland')
generated = wl_mod.scan_xml(
  'protocol/river-window-management-v1.xml',
  'protocol/river-xkb-bindings-v1.xml',
)

exe = executable(
  'nullspace',
  'src/nullspace.c',
  'src/wallpaper.c',
  generated,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  install: true,
)
