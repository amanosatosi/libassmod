test_src = files('test.c')

scroll_unit = executable('scroll_unit',
    files('scroll_selftest.c', '../libass/ass_scroll.c'),
    include_directories: incs, dependencies: deps, install: false)
test('native-scroll-cues', scroll_unit)

scroll_render_unit = executable('scroll_render_unit',
    files('scroll_render_selftest.c'),
    include_directories: incs, dependencies: deps,
    link_with: libass_for_tools, install: false)
test('native-scroll-render', scroll_render_unit,
    env: {'SCROLL_TEST_FONT': join_paths(meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
    timeout: 120)

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

rnd_math_unit = executable('rnd_math_unit',
    files('rnd_test.c', '../libass/ass_rnd.c'),
    include_directories: incs, dependencies: deps, install: false)
rnd_render_unit = executable('rnd_render_unit',
    files('rnd_selftest.c'),
    include_directories: incs, dependencies: deps,
    link_with: libass_for_tools, install: false)
test('vsfiltermod-rnd-math', rnd_math_unit)
test('vsfiltermod-rnd-render', rnd_render_unit,
    env: {'RND_TEST_FONT': join_paths(meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
    timeout: 120)

distort_render_unit = executable(
    'distort_render_unit',
    files('distort_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

override_annotations_unit = executable(
    'override_annotations_unit',
    files('override_annotations_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

curved_text_unit = executable(
    'curved_text_unit',
    files('curved_text_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

relative_numbers_unit = executable(
    'relative_numbers_unit',
    files('relative_numbers_selftest.c'),
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

clip_transform_unit = executable(
    'clip_transform_unit',
    files('clip_transform_selftest.c'),
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

karaoke_paint_unit = executable(
    'karaoke_paint_unit',
    files('karaoke_paint_selftest.c'),
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
    colorcoding_sources += [gradient_test_src, config_h]
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

decoration_rnd_unit = executable(
    'decoration_rnd_unit',
    files('decoration_rnd_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

decoration_fallback_args = []
if get_option('default_library') != 'shared'
    # Exact outline checks use private helpers; public rendering checks also
    # run against the shared library on Windows.
    decoration_fallback_args += ['-DDECO_TEST_INTERNAL']
endif
decoration_fallback_unit = executable('decoration_fallback_unit',
    files('decoration_fallback_selftest.c'),
    include_directories: incs, dependencies: deps,
    c_args: decoration_fallback_args,
    link_with: libass_for_tools, install: false)
test('selected-font-decoration-fallback', decoration_fallback_unit,
    args: [
        join_paths(meson.current_source_dir(), 'fonts', 'decoration-primary-a.ttf'),
        join_paths(meson.current_source_dir(), 'fonts', 'decoration-primary-b.ttf'),
        join_paths(meson.current_source_dir(), 'fonts', 'decoration-fallback.ttf'),
        join_paths(meson.current_source_dir(), 'fonts', 'decoration-complete.ttf'),
    ])

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

bs4_geometry_unit = executable(
    'bs4_geometry_unit',
    files('bs4_geometry_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

gradient_surface_unit = executable(
    'gradient_surface_unit',
    files('gradient_surface_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)
test('gradient-surface-pixels', gradient_surface_unit, timeout: 180)

mangetsu_motion_scale_unit = executable(
    'mangetsu_motion_scale_unit',
    files('mangetsu_motion_scale_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

# This test intentionally checks private allocation provenance. Run it only
# where the test links against libass internals; shared Windows exports only
# the public libass ABI.
if get_option('default_library') != 'shared'
    repeated_event_render_unit = executable('repeated_event_render_unit',
        files('repeated_event_render_selftest.c'),
        include_directories: incs, dependencies: deps,
        link_with: libass_for_tools, install: false)
    test('repeated-paint-event-render', repeated_event_render_unit,
        args: [
            join_paths(meson.current_source_dir(), 'fonts', 'decoration-primary-a.ttf'),
            join_paths(meson.current_source_dir(), 'fonts', 'decoration-primary-b.ttf'),
            join_paths(meson.current_source_dir(), 'fonts', 'decoration-fallback.ttf'),
        ], timeout: 180)
    native_vertical_layout_unit = executable(
        'native_vertical_layout_unit',
        files('native_vertical_layout_selftest.c'),
        install: false,
        include_directories: incs,
        dependencies: deps,
        link_with: libass_for_tools,
    )
    test('native-vertical-layout', native_vertical_layout_unit)

    rgba_ownership_unit = executable(
        'rgba_ownership_unit',
        files('rgba_ownership_selftest.c'),
        install: false,
        include_directories: incs,
        dependencies: deps,
        link_with: libass_for_tools,
    )

    shaper_chain_cleanup_unit = executable(
        'shaper_chain_cleanup_unit',
        files('shaper_chain_cleanup_selftest.c'),
        install: false,
        include_directories: incs,
        dependencies: deps,
        link_with: libass_for_tools,
    )
endif

test('distortion-warp', distort_unit)
test('distortion-render', distort_render_unit, timeout: 120)
test('curved-text-render', curved_text_unit, timeout: 120)
test('relative-numbers', relative_numbers_unit, timeout: 120)
test('override-annotations', override_annotations_unit, timeout: 120)
test('clip-regression', clip_unit)
# Preserve the full case/configuration/backend matrix while bounding each
# sanitized test's workload. Case progress is printed before every comparison
# so a timeout also identifies the active geometry and output path.
foreach config : [0, 1]
    foreach rgba : [0, 1]
        backend = rgba == 1 ? 'rgba' : 'legacy'
        foreach batch : [0, 1]
            # Large native RGBA tiles are CPU-heavy under ASan/UBSan. Keep
            # their deadline separate from contention with the other tests.
            large_rgba = config == 1 and rgba == 1
            test('clip-transform-regression-@0@-@1@-@2@'.format(backend, config, batch),
                clip_transform_unit,
                args: [config.to_string(), rgba.to_string(), batch.to_string()],
                env: {'CLIP_TRANSFORM_TEST_FONT': join_paths(
                    meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
                timeout: large_rgba ? 300 : 120,
                is_parallel: not large_rgba)
        endforeach
    endforeach
endforeach
test('furi-extension', furi_unit,
    env: {'FURI_TEST_FONT': join_paths(
        meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
    timeout: 120,
)
foreach fixture : ['ascent', 'descent']
    test('furi-tight-' + fixture, furi_unit,
        args: ['--geometry-only'],
        env: {
            'FURI_TEST_FONT': join_paths(meson.current_source_dir(), 'fonts',
                'furi-tight-' + fixture + '.ttf'),
            'FURI_TEST_FAMILY': 'Furi Tight ' + (fixture == 'ascent' ? 'Ascent' : 'Descent'),
        },
        timeout: 120)
endforeach

pattern_paint_unit = executable(
    'pattern_paint_unit',
    files('pattern_paint_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)

blend_unit = executable(
    'blend_unit',
    files('blend_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)
test('karaoke-paint-extension', karaoke_paint_unit, timeout: 120)
test('multi-border-extension', multiborder_unit)
test('text-pattern-paint', pattern_paint_unit, timeout: 120)
test('actor-colorcoding', colorcoding_unit)
test('fad-color-fade', fad_unit)
test('rnd-custom-decoration-lifetime', decoration_rnd_unit,
    env: {'RND_TEST_FONT': join_paths(meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
    timeout: 180)
if get_option('rgba-ownership-extended-stress')
    test('rnd-custom-decoration-lifetime-extended', decoration_rnd_unit,
        args: ['--extended'],
        env: {'RND_TEST_FONT': join_paths(meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
        timeout: 1800, is_parallel: false)
endif
test('unicode-decimal-digits', unicode_digits_unit)
test('text-anchor-alignment', tan_unit)
test('bs4-transformed-geometry', bs4_geometry_unit)
test('mangetsu-motion-scale', mangetsu_motion_scale_unit)

myanmar_syllable_unit = executable(
    'myanmar_syllable_unit',
    files('myanmar_syllable_selftest.c', '../libass/ass_myanmar.c'),
    install: false,
    include_directories: incs,
)
test('myanmar-layout-syllables', myanmar_syllable_unit)

native_vertical_render_unit = executable(
    'native_vertical_render_unit',
    files('native_vertical_render_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)
test('native-vertical-render', native_vertical_render_unit, timeout: 120)
test('vsfiltermod-blend', blend_unit)

chat_unit = executable(
    'chat_unit',
    files('chat_selftest.c', '../libass/ass_chat.c'),
    install: false,
    include_directories: incs,
)
test('native-chat-parser', chat_unit)

chat_render_unit = executable(
    'chat_render_unit',
    files('chat_render_selftest.c'),
    install: false,
    include_directories: incs,
    dependencies: deps,
    link_with: libass_for_tools,
)
test('native-chat-render', chat_render_unit,
    env: {'FURI_TEST_FONT': join_paths(
        meson.project_source_root(), 'compare', 'test', 'font1.ttf')},
    timeout: 120)
if get_option('default_library') != 'shared'
    test('rgba-ownership-lifetime', rgba_ownership_unit, timeout: 120)
    test('shaper-chain-cleanup', shaper_chain_cleanup_unit)
    if get_option('rgba-ownership-extended-stress')
        # The ASan/UBSan workload is intentionally long. It is enabled only
        # by the scheduled/manual stress workflow, not regular PR CI.
        test('rgba-ownership-lifetime-extended', rgba_ownership_unit,
             args: ['--extended'], timeout: 3600, is_parallel: false)
    endif
endif
