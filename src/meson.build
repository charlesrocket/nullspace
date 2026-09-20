exe = executable(
  'nullspace',
  'nullspace.c',
  'trimming/trimming.c',
  'wallpaper.c',
  generated,
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
