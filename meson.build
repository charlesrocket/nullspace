project(
  'nullspace',
  'c',
  meson_version: '>= 1.8.0',
  version: '0.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-U_FORTIFY_SOURCE',
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
  '-DWLR_USE_UNSTABLE',
]), language: 'c')

if get_option('asan')
  add_project_arguments([
    '-fno-omit-frame-pointer',
    '-fno-optimize-sibling-calls',
  ], language: 'c')
endif

wayland_server = dependency('wayland-server')
wayland_proto = dependency('wayland-protocols')
wlroots = dependency('wlroots-0.20')
scenefx = dependency('scenefx-0.5')
xkbcommon = dependency('xkbcommon')
xcb = dependency('xcb', required: get_option('xwayland'))
xcb_randr = dependency('xcb-randr', required: get_option('xwayland'))
libinput = dependency('libinput')
pixman = dependency('pixman-1')
spng = dependency('spng', required: get_option('wallpaper'))

build_xwayland = xcb.found() and xcb_randr.found()

if build_xwayland
  add_project_arguments('-DXWAYLAND', language: 'c')
endif

build_wallpaper = spng.found()

if build_wallpaper
  add_project_arguments('-DWALLPAPER', language: 'c')
endif

inc_public = include_directories('include')

subdir('protocol')
subdir('contrib')
subdir('src')

executable(
  'nullspace',
  src_files,
  proto_sources,
  dependencies: [
    wayland_server,
    scenefx,
    wlroots,
    xkbcommon,
    xcb_randr,
    xcb,
    libinput,
    pixman,
    spng,
  ],
  include_directories: [
    inc_public,
    proto_inc,
  ],
  install: true,
)
