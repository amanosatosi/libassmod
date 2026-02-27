test_src = files('test.c')

libass_test = executable(
    'libass_test',
    test_src,
    install: false,
    include_directories: incs,
    dependencies: deps + png_deps,
    link_with: libass_for_tools,
)

distort_unit = executable(
    'distort_unit',
    files('distort_test.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

clip_unit = executable(
    'clip_unit',
    files('clip_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

test('distortion-warp', distort_unit)
test('clip-regression', clip_unit)
