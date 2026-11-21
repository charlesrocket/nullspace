nctl = executable(
  'nctl',
  'main.c',
  controller_c,
  wlr_output_power_c,
  controller_client_h,
  wlr_output_power_client_h,
  include_directories: [inc_public, proto_inc],
  dependencies: [wayland_client, xkbcommon],
  install: true,
)
