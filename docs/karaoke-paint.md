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
`\rStyleName` unset the secondary source because ASS styles have no field for
it. A parameterless `\3sc` unsets the opt-in state; parameterless vector or
gradient tags fall back to an already configured secondary solid colour.

The extension is opt-in. Without one of these tags, `\k`, `\K`/`\kf`, and
lowercase `\ko` retain their prior outline rendering. `SecondaryColour` is
never inferred as an outline paint. With an explicit source, `\k` switches
the layer-1 outline from secondary to active paint at the segment start and
`\kf` uses the same logical frontier and resolved direction for fill and
outline. Border bitmap thickness does not participate in timing. Lowercase
`\ko` still suppresses every outline before activation and therefore ignores
secondary outline paint while waiting.

Extra numbered border layers do not have secondary public tags. They retain
their active paint under `\k`/`\kf`; this avoids assigning ambiguous meaning
to `\3s*` outside its existing layer-1 counterpart. They are still hidden by
reveal karaoke as described below.

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
