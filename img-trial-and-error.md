# IMG Trial And Error

## Goal
Match VSFilterMod output for `\img` (and `\1img..\4img`) in libassmod, especially drawing mode (`\p`).

## Current Status
- `snow` sample: good again after rollback + phase tweaks.
- `black` sample: still has a visible mismatch (looks like a ~1px parity issue).

## Repro Inputs
- VSFilterMod source path:
  - `C:\Users\HP USER\libassmodP\VSFilterMod`
- ASS samples:
  - Snow bubble template (`snow` case)
  - Black bubble template (`black` case)
- Key black-center dialogue (from sample):
  - `\pos(960,529)\bord0\1img(FloatNotice.265.1.png)\p1`
  - shape width is `667.85`
- Key image dimensions:
  - `FloatNotice.265.1.png`: `8x97`
  - `FloatNotice.2.1.png`: `90x74`

## Trial Log

1. Path and host lookup fixes
- Area: parser + tag-image lookup.
- Result: needed for reliability, but did not solve black parity.

2. VSFilter-style texture sampling core
- Area: `render_bitmap_rgba` + `sample_tag_image`.
- Changes: upside-down row access, wrap rules, 1/8 subpixel interpolation with left/up neighbors.
- Result: improved parity, still not final for black.

3. Drawing-mode coverage handling
- Area: `render_bitmap_rgba`.
- Changes: use VSFilter-like 6-bit coverage behavior in draw mode, clamp padding columns.
- Result: helped snow case, black still off.

4. Drawing-run subpixel source
- Area: `render_and_combine_glyphs`, `render_glyph`, `render_glyph_i`.
- Changes: for drawing runs, use position-derived subpixel phase (word position) instead of cache-center subpixel for `\img`.
- Result: improved consistency; black mismatch remains.

5. Positioned drawing basepoint quantization
- Area: positioned-event basepoint and rotation-center logic.
- Changes: added VSFilter-style 1/8-quantized basepoint path for draw+img events.
- Result: user reported snow regression and black still not fixed.

6. Rollback of positioned draw+img basepoint quantization
- Area: removed `is_vsf_draw_img_event` / `get_base_point_vsf_draw_img` path and callsites.
- Reason: isolate regression and restore known-good snow behavior baseline.

7. Draw-mode texture phase rebase from first covered column
- Area: `render_bitmap_rgba`.
- Change: `tex_phase_bias_x` now follows `cov_x0` for draw mode too (not just non-draw mode).
- Reason: black center tile is `8px` wide; 1-column phase drift is very visible.
- Status: pending user retest.

8. Coverage-bound anchor switched to raw mask coverage (`cov > 0`)
- Area: `render_bitmap_rgba` coverage bound scan.
- Change: bound detection no longer uses quantized `cov64` in draw mode.
- Reason: tiny edge coverage could be rounded to 0 in `cov64` and shift the phase origin by 1 column.
- Status: pending user retest.

9. Draw guard-column heuristic for phase anchoring
- Area: `render_bitmap_rgba` draw-mode coverage bound post-processing.
- Change: if first covered column is 0, compare summed mask coverage of col0 vs col1; when col0 is much weaker (`sum0 * 8 < sum1`), treat col0 as guard (`cov_x0=1`, `tex_phase_bias_x=1`).
- Reason: fractional-width vector rectangles can leave tiny AA in guard col0, which anchors texture phase one column too far left.
- Status: pending user retest (final attempt before moving on).

## Verified Facts
- Black center piece is very sensitive because texture width is `8px` and shape width is fractional (`667.85`).
- Snow center width is integer (`562`) and is less sensitive.
- Current issue is likely in draw raster/span parity (not only in parser or path lookup).

## Strong Hypotheses (Not Solved Yet)
1. Drawing raster width/guard-column parity differs from VSFilter for fractional widths.
- VSFilter raster path appears to use a different overlay width/offset strategy than libass bitmap logical bounds.

2. First visible column phase for draw `\img` may still be off by 1 for the black center piece.
- This would be hard to notice on wide textures, but obvious on an `8px` repeating tile.

## Next Experiments
1. Add temporary debug logs (for draw+img only) for:
- `full_w`, `w`, `cov_x0`, `cov_x1`, `src_x`, `subpix_x`, `dst_x`
- same event for snow and black center

2. Trial A:
- derive sampling X from a VSFilter-like span origin (instead of current logical bitmap origin) for draw mode.

3. Trial B:
- make draw-mode sampling width depend on covered span (`cov_x1 - cov_x0 + 1`) when phase is computed.

4. Trial C:
- adjust draw coverage conversion to exact VSFilter mapping formula and retest edge columns.

## Notes
- Local compile is intentionally skipped (machine space constraints).
- Validation flow is GitHub Actions render comparison.
