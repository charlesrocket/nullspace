src_files = files(
  'nullspace.c',
  'trimming/trimming.c',
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
  'trimming/test_trimming.c',
  'trimming/trimming.c',
)

test('trimming', trimming_test)
