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
- `\bs4` - existing libass rectangular event box behavior
- `\bs5` - Mangetsu geometric border mode

`BorderStyle=5` in a style is equivalent to using `\bs5` for events using that
style.

## Mangetsu Geometric Borders

`BorderStyle=5` / `\bs5` enables Mangetsu geometric border mode. This mode is
intended for shape/sign typesetting. It expands glyph and vector outlines as
geometric offset contours with sharp/miter-style joins by default, avoiding the
rounded blob look of legacy ASS borders.

`\bord`, `\xbord`, and `\ybord` still control border thickness. `\3c` and
`\3a` still control normal outline color and alpha unless a native multi-border
layer overrides them.

`\bs5` does not replace multiple borders. It only changes how active border
layers are geometrically generated. When multi-border layers are enabled, each
active outline layer uses the same geometric border mode and keeps the existing
layer ordering.

BorderStyle=4 box-border tags (`\bbs`, `\bbc`, `\bba`, and numbered forms)
apply only to rectangular box rendering and are documented in `docs/box-tags.md`.
