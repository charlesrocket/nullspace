wayland_scanner = find_program('wayland-scanner')
wl_mod          = import('wayland')

wlr_layer_shell_xml = files('wlr-layer-shell-unstable-v1.xml')

wlr_layer_shell_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input:  wlr_layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

wlr_layer_shell_c = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.c',
  input:  wlr_layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_xml = files('xdg-shell.xml')

xdg_shell_h = custom_target(
  'xdg-shell-protocol.h',
  input:  xdg_shell_xml,
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_c = custom_target(
  'xdg-shell-protocol.c',
  input:  xdg_shell_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_bg_effect_xml = files('ext-background-effect-v1.xml')

ext_bg_effect_h = custom_target(
  'ext-background-effect-v1-protocol.h',
  input:  ext_bg_effect_xml,
  output: 'ext-background-effect-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

ext_bg_effect_c = custom_target(
  'ext-background-effect-v1-protocol.c',
  input:  ext_bg_effect_xml,
  output: 'ext-background-effect-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

proto_inc = include_directories('.')

proto_sources = [
  wlr_layer_shell_h,
  wlr_layer_shell_c,
  xdg_shell_h,
  xdg_shell_c,
  ext_bg_effect_h,
  ext_bg_effect_c,
]
