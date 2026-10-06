# Repeated painted event geometry

Ordinary events with the same parsed text geometry can reuse completed layout
and composite masks within a frame. Paint and rectangular clips are applied
again for each event. The legacy image ABI and painter order are preserved.

## Work avoided

The existing composite cache shares alpha masks, but its lookup occurs after
shaping, outline retrieval, layout, transform quantization, glyph bitmap
lookups, component array allocation, and composite key hashing. Repeated events
previously repeated those stages even when every composite lookup hit.

The frame memo retains one completed geometry result, references to existing
composite values, and an owned copy of its compact input identity. It does not
retain shaped glyph chains or duplicate bitmap buffers. A hit bypasses font
fallback selection, shaping, layout, and bitmap/composite key construction.
Parsing and comparison of normalized state still happen for every event.

## Admission and identity

Admission accepts ordinary text with leading override blocks and one uniform
solid paint state. The supported overrides are listed in
`libass/ass_event_reuse.h`. Unknown overrides are rejected, including geometry
transforms inside `\t`; rectangular clip transforms are admitted. Inline style
or paint changes, drawings, vector clips, decorations, vertical fonts, karaoke,
randomization, jitter, distortion, furigana, curved text, gradients, patterns,
image fills, columns, scrolling, chat, and actor colour coding use the normal
pipeline. This closed admission list is essential to the identity's scope.

The key is built after parsing and style-run splitting, before shaping:

* The zero-initialized `GlyphInfo` prefix up to `c` supplies symbols, font object
  identity, font size, initial metrics/positions and run boundaries.
* The range from `line` up to `distort_enabled` supplies normalized spacing,
  scale and scale correction, rotation, shear, blur, shadow, border geometry,
  and ordinary layout/effect inputs. Bold, italic and decoration flags are
  included separately, along with normalized border layer geometry.
* Event-wide inputs include style index, margins, alignment, justification,
  wrapping, encoding, explicit/positioned/collision state, position, origin,
  font/scale/spacing, rotations, shear, borders, shadows, blur, render scales,
  and override flags. The exact list is in `repeated_context_key`.
* Primary and secondary alpha distinguish opaque, partially transparent and
  fully transparent input. Those classes preserve `FILTER_FILL_IN_BORDER`
  and `FILTER_FILL_IN_SHADOW`. Other solid alpha and RGB values are paint only.

Renderer, track, shaping configuration and font selection remain constant
within a frame. Every new frame discards the memo before updating fonts or
cutting caches, so changes between frames cannot produce a stale hit.

## Paint, clips and ownership

On a hit each retained run receives current colours, base colours and border
paint from the freshly parsed event. Rectangular clip conversion uses the same
helper as the ordinary path. Rectangular `\iclip` retains its inverse splitting
semantics. Animated rectangles are evaluated by the normal parser for the
current time. Vector clips and clip extensions are deliberately excluded.

Each output image keeps its own position, crop and colour and references its
composite mask through existing image ownership. No images are merged across
events. Fully transparent solid paint is skipped before allocating output
nodes, copying uncached regions, or creating RGBA tiles, including individual
karaoke halves. Extended paint with possible replacement opacity is left to
its normal path. Invisible layers remain in composite geometry because other
events may paint those same masks visibly.

The memo owns only key/run storage, with a cap of the smaller of 2 MiB and
1/16 of the configured composite cache budget. Composite references use
`ass_cache_inc_ref` / `ass_cache_dec_ref`. Storage is released at the next frame,
an ineligible event or geometry replacement, renderer reconfiguration, font
selection changes, cache limit changes, and renderer destruction. Outstanding
legacy images retain their own mask references independently.

## Deterministic coverage

`repeated_event_render_selftest.c` uses 576 events, three repository-owned
fixture fonts, two shaping levels, both image APIs, and multiple frame times.
Reference controls disable reuse and retain transparent output. Tests compare
ordered visible tiles and every mask/RGBA byte, verify exclusion boundaries,
and retain image references across frame/configuration changes.

Diagnostics separate shaping calls, geometry passes, glyph bitmap preparation
requests, composite lookups, actual bitmap/composite constructions, emitted
nodes, hits and memo storage. CI prints separate reference/optimized CPU times;
timing never determines pass/fail. The focused GitHub Actions workflow runs
release and ASan/UBSan configurations; the test also joins regular static Meson
test configurations.
