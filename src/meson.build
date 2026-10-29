src_files = files(
  'manager.c',
  'config.c',
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
  'space.c',
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

subdir('test')
