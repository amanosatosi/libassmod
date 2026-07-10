# BorderStyle Override Tags

This fork supports a line-level BorderStyle override tag:

```ass
\bs<N>
```

`\bs` is resolved like `\an`: the first valid `\bs<N>` tag found in an event
controls the whole rendered event, even if it appears after visible text.
Later `\bs` tags in the same event are ignored. Bare tags, malformed values,
and unsupported values are ignored safely. If no valid `\bs<N>` tag is present,
the active style's `BorderStyle` value is used.
Because `\bs` is line-level, `\r` resets do not switch away from the first
valid `\bs<N>` selected for the event.

Supported values are:

- `\bs1` - normal legacy ASS outline rendering
- `\bs3` - existing opaque box behavior
- `\bs4` - BorderStyle=4 transformed event-box behavior
- `\bs5` - Mangetsu geometric border mode

`BorderStyle=5` in a style is equivalent to using `\bs5` for events using that
style.

## BorderStyle=4 Event Geometry

`BorderStyle=4` and `\bs4` create one event-level background box, not a box
per glyph. The box is calculated from the event's local layout geometry before
padding and event transforms are applied, then it follows the event through
positioning, rotation, scaling, shear, 3D projection, distortion, and active
clipping. Its box-border layers use that same transformed geometry.

When an event changes geometry after visible content has begun, the one BS4 box
uses the canonical geometry state of the first visible glyph or drawing rather
than splitting into multiple boxes. Per-glyph randomized boundary effects do
not reshape the single box. See `docs/box-tags.md` for the padding order,
clip behavior, supported examples, and this intentional limitation.

## Mangetsu Geometric Borders

`BorderStyle=5` / `\bs5` enables Mangetsu geometric border mode. This mode is
intended for shape/sign typesetting. It expands glyph and vector outlines as
geometric offset contours with sharp/miter-style joins by default, avoiding the
rounded blob look of legacy ASS borders.

To avoid long spike artifacts on normal font outlines, geometric joins use a
conservative miter limit of 2.0 times the border width. Corners within that
limit remain sharp; sharper or near-degenerate joins fall back to bevels.

`\bord`, `\xbord`, and `\ybord` still control border thickness. `\3c` and
`\3a` still control normal outline color and alpha unless a native multi-border
layer overrides them.

`\bs5` does not replace multiple borders. It only changes how active border
layers are geometrically generated. When multi-border layers are enabled, each
active outline layer uses the same geometric border mode and keeps the existing
layer ordering.

BorderStyle=4 box-border tags (`\bbs`, `\bbc`, `\bba`, and numbered forms)
apply only to rectangular box rendering and are documented in `docs/box-tags.md`.
