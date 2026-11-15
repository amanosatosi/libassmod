# Perspective corner semantics

The first perspective feature will use **relative corners**. The four pairs
describe positions in the event's current logical layout rectangle. The
rectangle follows text, font, scale, spacing, and wrapping edits. Corner values
stay the same. Border, shadow, and blur extents do not define the rectangle.
Changing the aspect ratio can therefore change the apparent slope of an edge.

Fixed script-coordinate corners are a possible separate mode for shots where
the pins must stay on a surface as the text changes. In that mode the source
rectangle would still follow layout, and the destination would stay fixed.
The first feature will have a single relative interpretation.

Both policies can use the same homography solver. It takes a source rectangle
and four already-resolved destination points in TL, TR, BR, BL order. For
relative corners, resolve each pair `(u, v)` to
`(left + u * width, top + v * height)` in the output plane. An editor may use
that rule to retain the authored pairs across layout changes. Tag syntax and
parser behavior are outside this prototype.

## Prototype scope

The isolated Meson test target `perspective-math` builds the solver and the
outline projection helper. The renderer and public ABI do not link the helper.
The helper preserves affine outline segments and flattens projective curves in
homogeneous coordinates. Its destination-space tolerance is in outline units
(1/64 pixel), plus at most 0.71 outline units of endpoint rounding. It checks
convexity, projective depth, subdivision and segment limits, and estimated
tile-padded raster area for one outline. Whole-event limits and fallback belong
to renderer integration.

The tests cover the difference from bilinear mapping, both corner policies
across a resize, curve approximation against independently sampled source
curves, identity and mirroring, invalid quads, and mask coverage through the
existing rasterizer. Existing GitHub Actions Meson jobs run the target when
configured with tests and a static libass library.
