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

furi_unit = executable(
    'furi_unit',
    files('furi_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

multiborder_unit = executable(
    'multiborder_unit',
    files('multiborder_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

colorcoding_sources = files('colorcoding_selftest.c')
# The positioned-gradient math checks use internal gradient helpers. They are
# not part of the shared-library export surface, so provide the implementation
# directly when tests are linked against a shared libass.
if get_option('default_library') == 'shared'
    colorcoding_sources += gradient_test_src
endif

colorcoding_unit = executable(
    'colorcoding_unit',
    colorcoding_sources,
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

fad_unit = executable(
    'fad_unit',
    files('fad_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

unicode_digits_unit = executable(
    'unicode_digits_unit',
    files('unicode_digits_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

tan_unit = executable(
    'tan_unit',
    files('tan_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

test('distortion-warp', distort_unit)
test('clip-regression', clip_unit)
test('furi-extension', furi_unit)
test('multi-border-extension', multiborder_unit)
test('actor-colorcoding', colorcoding_unit)
test('fad-color-fade', fad_unit)
test('unicode-decimal-digits', unicode_digits_unit)
test('text-anchor-alignment', tan_unit)
