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

Layer-1 tags are aliases for existing ASS tags:

```ass
\bord  == \1bs
\xbord == \1bsx
\ybord == \1bsy
\3c    == \1bc
\3a    == \1ba
\3vc   == \1bvc
\3va   == \1bva
```

Alpha uses normal ASS inverse-alpha semantics:

```ass
&H00& = opaque
&HFF& = transparent
```

Additional layers are rendered as non-overlapping rings. For example, if layer 1
has size 2 and layer 2 has size 6, layer 2 is rendered only in the area outside
the layer-1 expanded mask. This avoids stacking a semitransparent outer border
under an inner border.

Border gradients use the same four-corner vector-gradient format as `\1vc`
through `\4vc` and `\1va` through `\4va`, with normal ASS BGR colors and
inverse-alpha values. `\Nbc` disables the color gradient for layer `N`, and
`\Nba` disables the alpha gradient for layer `N`. Gradients require the RGBA
rendering path; legacy `ASS_Image` output keeps a flat fallback color.

Examples:

```ass
{\bord2\3c&HFFFFFF&\3a&H00&\2bs6\2bc&H000000&\2ba&H80&}Text
{\1bs2\1bc&HFFFFFF&\1ba&H00&\2bs6\2bc&H000000&\2ba&H80&}Text
{\1bs2\1bc&HFFFFFF&\2bs5\2bc&H000000&\3bs9\3bc&H202020&\3ba&HAA&}Text
{\1bs2\1bvc(&HFFFFFF&,&HCCCCCC&,&HFFFFFF&,&HCCCCCC&)\2bs7\2bvc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\2bva(&H20&,&H80&,&H20&,&H80&)}Text
```

For this first version, the layer count is fixed at 10. There are no per-layer
blur or shadow tags; existing `\blur`, `\be`, `\shad`, `\xshad`, and `\yshad`
behavior is reused.

Override state is persistent like normal ASS tags. `\r` resets layer 1 to the
active style outline and disables layers 2 through 10.
