ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/libass

# Using gnu99 to gain compatibility between const and
# non-const array types without needing to jump to ISO C23.
# Otherwise intended to be compatible with ISO C.
#
# Similarly we only strictly _need_ ISO C APIs, but we’ll use
# some additional APIs from e.g. GNU and POSIX when available.
# Unfortunately defining _POSIX_C_SOURCE may end up hiding additional
# non-POSIX interfaces even if their respective feature macro is set too.
# However, POSIX interfaces are usually implicitly enabled
# with other feature macros, thus omit the POSIX one.
AM_CFLAGS = -std=gnu99 -D_GNU_SOURCE -D_XPLATFORM_SOURCE

EXTRA_DIST = libass.pc.in Changelog MAINTAINERS RELEASEVERSION ltnasm.sh \
             docs/curved-text.md docs/distort-tag.md docs/warp-text-anchor.md \
             docs/native-vertical.md test/native_vertical_render_selftest.c \
             test/native_vertical_layout_selftest.c \
             test/myanmar_syllable_selftest.c \
             test/curved_text_selftest.c test/distort_test.c \
             test/distort_selftest.c \
             docs/blend-tag.md test/blend_selftest.c \
             samples/blend-basic.ass samples/blend-alpha.ass \
             samples/blend-components.ass samples/blend-order.ass \
             samples/blend-img.ass samples/blend-reset.ass \
             samples/blend-video.ass \
             docs/native-chat.md samples/chat-modes.ass \
             test/chat_selftest.c test/chat_render_selftest.c

EXTRA_DIST += docs/native-scroll.md samples/scroll-lines.ass \
              test/scroll_selftest.c test/scroll_render_selftest.c

EXTRA_DIST += test/clip_transform_selftest.c
EXTRA_DIST += docs/furigana-extension.md test/furi_selftest.c \
              test/fonts/furi-tight-ascent.ttf test/fonts/furi-tight-descent.ttf
EXTRA_DIST += docs/decoration-fallback.md test/decoration_fallback_selftest.c \
              test/fonts/README.md test/fonts/generate-decoration-fonts.py \
              test/fonts/decoration-primary-a.ttf test/fonts/decoration-primary-b.ttf \
              test/fonts/decoration-fallback.ttf test/fonts/decoration-complete.ttf
EXTRA_DIST += docs/rnd-tags.md test/rnd_test.c test/rnd_selftest.c \
              test/decoration_rnd_selftest.c

# Meson build setup
EXTRA_DIST += gen_defs.py meson_options.txt meson.build \
              libass/meson.build libass/ass/meson.build \
              fuzz/meson.build checkasm/meson.build \
              compare/meson.build \
              profile/meson.build test/meson.build

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libass.pc

nasm_verbose = $(nasm_verbose_$(V))
nasm_verbose_ = $(nasm_verbose_$(AM_DEFAULT_VERBOSITY))
nasm_verbose_0 = @echo "  NASM    " $@;

lib_LTLIBRARIES =
noinst_LTLIBRARIES =
noinst_PROGRAMS =
check_PROGRAMS =
TESTS =

include libass/Makefile_library.am
include Makefile_util.am

# Tests
if ENABLE_FUZZ
check: check-art-fuzz
.PHONY: check-art-fuzz
check-art-fuzz: fuzz/fuzz
	@if [ -z '$(ART_SAMPLES)' ] ; then \
		echo "ART_SAMPLES location not set; cannot run regression tests!"; \
	else \
		cd '$(ART_SAMPLES)'/crash/ ; \
		./run-all.sh '$(abs_top_builddir)'/fuzz/fuzz ; \
	fi
endif

if ENABLE_COMPARE
check: check-art-compare
.PHONY: check-art-compare
check-art-compare: compare/compare
	@if [ -z '$(ART_SAMPLES)' ] ; then \
		echo "ART_SAMPLES location not set; cannot run regression tests!"; \
	else \
		cd '$(ART_SAMPLES)'/regression/ ; \
		./run-all.sh '$(abs_top_builddir)'/compare/compare ; \
	fi
endif
