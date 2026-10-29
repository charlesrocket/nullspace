exe = executable(
  'test_util',
  'test_util.c',
  test_support,
  src_files,
  proto_sources,
  include_directories: [test_includes, includes],
  dependencies: deps,
  build_by_default: false,
  install: false,
)

test('util', exe, suite: 'unit')
