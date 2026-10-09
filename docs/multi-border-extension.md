# Experimental multi-border extension

This fork supports experimental numbered ASS override tags for native multiple
outline rings:

```ass
\Nbs<size>
\Nbsx<size>
\Nbsy<size>
\Nbc&HBBGGRR&
\Nba&HAA&
\Nbvc(<c0>,<c1>,<c2>,<c3>)
\Nbva(<a0>,<a1>,<a2>,<a3>)
```

`N` is a border layer number from 1 through 10. Layer 1 is the normal ASS
outline. Layers 2 through 10 are additional outer outline layers.

Layer-1 size tags use normal ASS border thickness semantics. For layers 2
through 10, `\Nbs`, `\Nbsx`, and `\Nbsy` are additional thicknesses outside the
previous native border layer, not cumulative outer radii. Users do not need to
write cumulative sizes:

```ass
{\bord5\2bs1}Text
{\bord5\2bs5}Text
```

The first example creates a 1px second outer ring outside a 5px normal outline.
The second creates a 5px second outer ring outside a 5px normal outline.

For example, `\1bs2\2bs5\3bs4` has cumulative outer radii 2, 7, and 11.
Missing intermediate layers contribute zero thickness, so an enabled later
layer still renders outside the previous visible outline.

Numbered `\Nbs` tags are border-layer size tags. They are distinct from the
line-level `\bs<N>` BorderStyle override documented in
`docs/borderstyle-tags.md`.
They are also distinct from BorderStyle=4 box-border tags such as `\Nbbs`,
`\Nbbc`, and `\Nbba`, which are documented in `docs/box-tags.md`.

Layer-1 tags are aliases for existing ASS tags:

```ass
\bord  == \1bs
\xbord == \1bsx
\ybord == \1bsy
\3c    == \1bc
\3vc   == \1bvc
\3va   == \1bva
```

The legacy ASS outline alpha tag `\3a` controls transparency for all native
border layers, from layer 1 through layer 10. For example, `\3a&H80&` makes
every native border layer semitransparent, and bare `\3a` resets every native
border layer to the active style `OutlineColour` alpha. It does not enable
extra border layers or change their sizes.

Numbered border alpha tags remain layer-specific. `\1ba` controls only layer 1,
and `\Nba` controls only layer `N`.

Alpha uses normal ASS inverse-alpha semantics:

```ass
&H00& = opaque
&HFF& = transparent
```

Additional layers are rendered as non-overlapping rings. For example, if layer 1
has thickness 2 and layer 2 has thickness 6, layer 2 is rendered as a 6px ring
outside the layer-1 expanded mask. This avoids stacking a semitransparent outer
border under an inner border.

Border gradients use the same four-corner vector-gradient format as `\1vc`
through `\4vc` and `\1va` through `\4va`, with normal ASS BGR colors and
inverse-alpha values. `\Nbc` disables the color gradient for layer `N`, and
`\Nba` disables the alpha gradient for layer `N`. The legacy `\3a` tag disables
alpha gradients for every native border layer. Gradients require the RGBA
rendering path; legacy `ASS_Image` output keeps a flat fallback color.

Attached Mangetsu `\Nbgrd` / `\Nbga` gradients use the occupied bounds of
the corresponding rendered border ring, including its scaling and blur.
Composite allocation padding shared with other rings does not enlarge that
gradient domain. Color and alpha share geometry but retain independent stops
and angles. VSFilterMod four-corner coordinates keep their existing behavior.
The waiting-outline `\3sgrd` uses the first outline's bounds and remains
confined to that layer. Box rings have a separate `\Nbbgrd` / `\Nbbga`
namespace documented in [box tags](box-tags.md).

Examples:

```ass
{\bord2\3c&HFFFFFF&\3a&H00&\2bs6\2bc&H000000&\2ba&H80&}Text
{\1bs2\1bc&HFFFFFF&\1ba&H00&\2bs6\2bc&H000000&\2ba&H80&}Text
{\1bs2\1bc&HFFFFFF&\2bs5\2bc&H000000&\3bs4\3bc&H202020&\3ba&HAA&}Text
{\1bs2\1bvc(&HFFFFFF&,&HCCCCCC&,&HFFFFFF&,&HCCCCCC&)\2bs7\2bvc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\2bva(&H20&,&H80&,&H20&,&H80&)}Text
```

The layer count is fixed at 10. `\Nbblur<value>` and `\Nbbe<value>` set Gaussian
blur and edge blur for native border layer `N` (1 through 10). Each border
inherits global `\blur` and `\be` unless that layer has an explicit numbered
override. Layer 1 is the normal ASS outline; its numbered settings do not
change the fill or other border layers. Explicit zero disables that filter for
the selected layer, even when the global value is nonzero. Bare `\Nbblur` and
`\Nbbe` clear their respective overrides, and `\r` clears all numbered
overrides.

For example, `{\blur2\bord2\2bs5\2bblur6}Text` blurs layer 2 by 6 while the
fill and layer 1 inherit 2. `{\blur4\bord2\2bs5\2bblur0}Text` keeps layer 2
sharp while the other parts inherit 4. Border rings are separated before their
individual filters are applied, so differently blurred borders have clean
underlying geometry. The shadow continues to use global blur/BE semantics;
there are no per-layer shadow tags.

`\shad`, `\xshad`, and `\yshad` cast one shadow from the complete outer
silhouette of all enabled native border layers.

Override state is persistent like normal ASS tags. `\r` resets layer 1 to the
active style outline and disables layers 2 through 10.

When `BorderStyle=5` / `\bs5` is active, every enabled native border layer uses
Mangetsu geometric border generation. Layer sizes, colors, alpha, gradients,
and ordering keep the same multi-border semantics. Extreme acute joins are
beveled when they exceed the geometric miter limit, preventing text glyph
spikes while preserving ordinary sharp corners.
