src_files = files(
  'nullspace.c',
  'layouts/layout.c',
  'layouts/trimming.c',
  'layouts/vertical.c',
  'layouts/horizontal.c',
) + generated

if get_option('wallpaper')
  src_files += files('wallpaper.c')
endif

exe = executable(
  'nullspace',
  src_files,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  install: true,
)

trimming_test = executable(
  'test_trimming',
  'layouts/test_trimming.c',
  'layouts/trimming.c',
)

test('trimming', trimming_test)
