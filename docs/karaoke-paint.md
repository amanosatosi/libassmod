# Karaoke Paint Extensions

Mangetsu adds opt-in waiting paint for the ordinary layer-1 outline:

```ass
\3sc&HBBGGRR&
\3svc(c1,c2,c3,c4)
\3sgrd(angle,color1,color2,...)
```

These are the waiting counterparts of `\3c`, `\3vc`, and `\3grd`.
They select one mutually replacing colour source: a later valid secondary
solid, four-corner vector, or Mangetsu gradient source wins. Outline alpha
continues to use the normal active outline alpha state. `\r` and
`\rStyleName` restore the target style’s compiled waiting-color default, or
unset the secondary source when none is configured. A parameterless `\3sc`
(or `\1bsc`) restores that flat default or unsets the opt-in state; parameterless vector or
gradient tags fall back to an already configured secondary solid colour.

The extension is opt-in. Without one of these tags, `\k`, `\K`/`\kf`, and
lowercase `\ko` retain their prior outline rendering. `SecondaryColour` is
never inferred as an outline paint. With an explicit source, `\k` switches
the layer-1 outline from secondary to active paint at the segment start and
`\kf` uses the same logical frontier and resolved direction for fill and
outline. Border bitmap thickness does not participate in timing. Lowercase
`\ko` still suppresses every outline before activation and therefore ignores
secondary outline paint while waiting.

Native layers 1–10 accept `\Nbsc&HBBGGRR&` as a flat waiting source.
`\1bsc` aliases `\3sc`; rich `\3svc`/`\3sgrd` sources remain layer-1-only.
Higher layers store only a color and enable bit, without per-layer gradient
objects. Each enabled source uses the existing `\k` activation or `\kf`
frontier, including RTL, vertical, furigana and gyaku-furi ownership. Waiting
paint inherits that ring’s active alpha, fades and alpha gradients while
replacing its active color source. A color never enables a disabled ring.
Unconfigured rings retain their old active paint. `\ko` suppression and
`\kO` reveal still apply to all rings.

Flat `\Nbsc` values are supported in [style metadata](style-extensions.md).
An empty `\Nbsc` restores the active style’s value or unsets that layer.
Explicit inline colors win until reset; named resets use the destination style.
Actor Colorcoding remains a separate sparse patch with its existing whitelist;
it may override active ring colors but does not acquire waiting-color tags.

## Reveal karaoke

Uppercase `\kO` is a case-sensitive, instantaneous reveal effect. Before a
segment's start, its fill, decorations, layer-1 outline, additional borders,
and shadow produce no pixels. At the start, the complete segment uses its
normal active paint and user alpha. `\kO` does not show secondary fill or
outline paint and has no progressive sweep or rotation reversal.

Durations and `\kt` use the existing karaoke timeline. Visibility is derived
from the current event-relative time on every render, so seeking and arbitrary
frame order do not create persistent reveal state. Transforms continue to be
evaluated at the current timestamp while content is hidden.

Furigana uses the same event-level timeline and shaped region ownership as
ordinary phase-1 furigana karaoke. Empty `\kO` segments advance time without
acquiring a base region, ownership can continue beyond `>`, and karaoke tags
in the base side remain ignored. RTL reveal ownership follows visual shaping;
only `\kf` has an internal directional sweep.
