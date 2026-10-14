# Global positioned alpha: `\pgrdalpha`

`\pgrdalpha` adds one fixed spatial opacity mask to non-box subtitle artwork.
It requires Mangetsu's premultiplied RGBA output. Ordinary ASS alpha, attached
alpha gradients, fades, and source-image alpha remain independent.

```ass
\pgrdalpha(x1,y1,x2,y2,angle,alpha1[,position%,alpha2,...],lastAlpha)
```

The rectangle uses finite ASS script coordinates and the same script-to-frame
mapping as [positioned color gradients](position-gradient.md): PlayRes, output
size, storage resolution, margins and pixel aspect ratio. Endpoints are
normalized, so reversed corners work. Negative/off-frame coordinates are valid.
An empty-area rectangle applies no mask. Sampling uses final pixel centers and
the same inclusive rectangle boundary as `\pgrd`; pixels outside retain their
original RGBA exactly. The field stays fixed while text moves, scrolls, rotates,
distorts, warps or is displaced by collision avoidance.

Angle and stops follow the existing positioned-gradient grammar. Angle 0 runs
left to right; 90 runs top to bottom. At least two stops are required. Optional
intermediate percentage/alpha pairs have sorted offsets from 0% through 100%,
including the existing duplicate-offset behavior and maximum stop count.

Alpha stops use the strict Mangetsu byte parser:

- `00`, `80`, `FF`, `&H80&` are ASS hexadecimal (80 means decimal 128).
- `$0` through `$255` are explicit decimal bytes; Unicode decimal digits after
  `$` are supported.
- 0 means opaque; 255 means transparent. Values such as `$256`, `$abc`, `$`,
  `$12xyz`, or hexadecimal `100` are invalid stops.

See [alpha compatibility contracts](rgba-rendering.md#mangetsu-color-and-alpha-values).
The tag does not change legacy scalar-alpha parsing, color arguments, timings,
coordinates, percentage offsets, or standard decimal `\fade` parameters.

```ass
{\pgrdalpha(100,100,1820,900,90,$0,$255)}Hello!
{\pgrdalpha(100,100,1820,900,0,$255,50%,$128,$0)}Three stops
{\1a$100\pgrdalpha(100,100,800,800,90,$0,$255)}Preserve ordinary alpha
```

Inside the rectangle, opacity multiplies:

```text
normal opacity = 255 - effective alpha
mask opacity = 255 - positioned alpha
result opacity = normal opacity * mask opacity / 255
```

The renderer applies the mask after existing coverage, alpha, fades, image
sampling and clipping, with integer rounding. Premultiplied RGB is scaled with
opacity. An opaque mask preserves the original paint exactly; a transparent
mask hides it. Already transparent paint stays transparent. Outside the
rectangle, no multiplication or edge extension occurs.

## Surfaces and overlapping artwork

The shared field affects primary and secondary/karaoke text, waiting outlines,
all native borders, shadows, fifth-color decorations, underlines/strikethroughs,
furigana/gyaku-furigana, drawings, image-filled text/outlines, color and alpha
gradients, polka/cycling paint, and transformed text. Phone-chat text and native
receipt ink are also eligible.

It excludes BS3/BS4 background boxes, box shadows/outlines/border layers,
rounded/transformed box paint, phone-chat bubble backgrounds/borders, and
background headers/panels. Exemption belongs to each box surface: its associated
text remains masked. Ordinary alpha/fades still affect box paint as before.

For normal source-over output, overlapping non-box surfaces sharing a mask are
adjusted for original coverage from surfaces above. Their finished artwork gets
one common spatial fade, rather than accumulating opacity at overlapping
borders, shadows and glyphs. This uses a bounded scanline workspace and retains
existing tile order, without a full-frame temporary or flattening box paint.
Scopes with different masks and separate dialogue events remain independent.
Small byte-rounding differences can occur when recompositing multiple tiles.

Non-normal `\blend` tiles retain their original blend modes and straight RGB
sidecars, with mask-adjusted source coverage. Those blend operations still run
in the existing painter order against the playback background; they are not
flattened into a background-independent color image.

```ass
{\bord5\shad6\1c&HFFFFFF&\3c&H0000FF&\4c&HFF0000&\pgrdalpha(100,100,1820,900,0,$0,$255)}One fade across white fill, red outline and blue shadow
{\1gra(0,$0,$200)\pgrdalpha(100,100,800,800,90,$0,$255)}Two independent alpha fields
{\img(example.png)\pgrdalpha(100,100,800,800,90,$0,$255)}Preserve image alpha
```

This differs from layer-specific `\Ngra`: `\gra` supplies a layer's alpha;
`\pgrdalpha` multiplies the completed non-box paint without replacing that
layer's alpha source. There are no `\Npgrdalpha` or numbered-border variants.

## Scope, reset and animation

The override follows the text stream. Changes create immutable snapshots for
subsequent glyphs, including their decorations/readings. Reset only this effect:

```ass
{\pgrdalpha(100,100,800,800,90,$0,$255)}Masked {\pgrdalpha()}Normal
```

`\pgrdalpha()` leaves ordinary alpha, layer gradients, colors, images, borders,
and other paint intact. `\r` clears the transient mask with the style reset.
Malformed arguments or parentheses preserve the previous valid mask; failure
is never an empty reset.

`\t` interpolates coordinates, shortest-path angle, merged stop positions and
alpha-stop values using the existing positioned-gradient framework:

```ass
{\pgrdalpha(100,100,800,800,90,$0,$0)\t(0,1000,\pgrdalpha(100,100,800,800,90,$0,$255))}Animate opacity
```

With no source mask, the transform starts from opaque stops over the target
rectangle, matching positioned-gradient initialization. As with `\pgrd`, empty
resets inside `\t` are ignored. Animate to opaque stops to remove its visual
effect, then use an ordinary reset. Repeated frames and seeking reconstruct
state without accumulating opacity; mask changes participate in RGBA change
detection. No mutable mask state is retained in returned hardware-ready tiles.
