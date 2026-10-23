src_files = files(
  'main.c',
  'manager.c',
  'log.c',
  'layers.c',
  'layouts/layout.c',
  'layouts/vertical.c',
  'layouts/horizontal.c',
  'activation.c',
  'ipc.c',
  'popup.c',
  'window.c',
  'session.c',
  'seat.c',
  'spawn.c',
  'server.c',
  'rules.c',
  'input.c',
  'output.c',
  'cursor.c',
  'keymap.c',
  'animation.c',
  'background.c',
)

if build_wallpaper
  src_files += files('wallpaper.c')
endif

if build_xwayland
  src_files += files('xwayland.c')
endif
