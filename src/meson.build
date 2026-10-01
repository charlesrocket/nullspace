src_files = files(
  'main.c',
  'nullspace.c',
  'layouts/layout.c',
  'layouts/vertical.c',
  'layouts/horizontal.c',
  'ipc.c',
  'seat.c',
  'keymap.c',
  'input.c',
  'output.c',
  'animation.c',
)

if get_option('wallpaper')
  src_files += files('wallpaper.c')
endif

src_files += generated
