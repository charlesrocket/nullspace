src_files = files(
  'nullspace.c',
  'layouts/layout.c',
  'layouts/trimming.c',
  'layouts/vertical.c',
  'layouts/horizontal.c',
  'keymap.c',
  'input.c',
  'output.c',
  'animation.c',
  'wallpaper.c',
) + generated

trimming_test = executable(
  'test_trimming',
  'layouts/test_trimming.c',
  'layouts/trimming.c',
)

test('trimming', trimming_test)
