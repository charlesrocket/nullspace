src_files = files(
  'manager.c',
  'config.c',
  'log.c',
  'layers.c',
  'layout/layout.c',
  'layout/vertical.c',
  'layout/horizontal.c',
  'node/text.c',
  'activation.c',
  'ipc.c',
  'popup.c',
  'window.c',
  'overview.c',
  'icon.c',
  'decoration.c',
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
  'wallpaper.c',
  'help.c',
)

if build_xwayland
  src_files += files('xwayland.c')
endif

subdir('test')
