# Mangetsu style extensions

`mangetsu-style` comments attach Mangetsu rendering defaults to existing ASS
styles without adding fields to `[V4+ Styles]`. Put the comments immediately
after the `[Events]` `Format:` line, before any dialogue or unrelated comment:

```ass
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\furis70}
Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,<က|စူဇူကီ>
```

Every dialogue using `Default` now starts with 70% furigana scaling. No inline
`\furis70` is necessary. The comment's timestamps, layer, actor and margins do
not control the defaults or their lifetime. Metadata creates no visible event.

The Effect identifier is exactly `mangetsu-style`, case-sensitive and without
braces or trailing spaces. Normal ASS field parsing removes leading spaces.
The **Style field** selects the target using normal ASS event-style matching:
leading `*` characters are ignored, `Default` is case-normalized, other names
are case-sensitive, and the last matching style definition wins. Unknown
styles are ignored with a warning instead of modifying the fallback style.

## Supported properties

The supported set contains typed event-layout defaults and persistent, pointer-free appearance defaults:

| Tags | Meaning |
| --- | --- |
| `\fshp` | Signed extra line spacing; relative operands resolve against the current default |
| `\ortho0`, `\ortho1` | Perspective (default) or orthographic projection |
| `\q0`…`\q3` | Initial wrapping mode (script `WrapStyle` when absent) |
| `\vert0`, `\vert1`, `\vtype0`…`\vtype3`, `\vdir0`…`\vdir2` | Initial native vertical layout, unit policy and column direction |
| `\vsp`, `\vcolsp` | Initial vertical unit and column spacing |
| `\1bsc`…`\10bsc` | Flat waiting karaoke border colors; `\1bsc` aliases `\3sc` inline |
| `\furi`, `\furis`, `\furisx`, `\furisy` | Enable furigana and set reading scale |
| `\furifsp`, `\furistyle` | Reading spacing and style |
| `\furipos(x,y)`, `\furiap` | Reading offset and automatic placement |
| `\furiplaceauto`, `\furichangepos` | Existing automatic reading placement policies |
| `\1bs`…`\10bs`, `\Nbsx`, `\Nbsy` | Native border-layer sizes and axes, N=1…10 |
| `\1bc`…`\10bc`, `\1ba`…`\10ba` | Flat native border colors and alpha |
| `\1bblur`…`\10bblur`, `\1bbe`…`\10bbe` | Per-layer blur and edge blur |
| `\box`, `\boxp`, `\boxpx`, `\boxpy`, `\boxr` | Box mode, padding and corner radius |
| `\bbs`, `\bbc`, `\bba` | First box-border layer size, flat color and alpha |
| `\1bbs`…`\10bbs`, `\Nbbc`, `\Nbba` | Box-border layer defaults, N=1…10 |

All admitted tags use the existing Mangetsu override parser and rendering
semantics. In particular, `\boxp10` sets padding on both axes; `\boxpx10\boxpy20`
sets them separately. `\boxp(10,20,5,5)` still uses its first argument for both
axes, as the current inline tag does; it does **not** define four-sided padding.
Choose the appropriate ordinary ASS BorderStyle for your box as usual.

Native border layers 2…10 add rings outside earlier layers. Setting only a
layer color does not enable its size. For example:

```ass
Comment: 0,0:00:00.00,0:00:00.00,Sign,,0,0,0,mangetsu-style,{\2bs3\2bc&HFFAA00&\2ba&H40&}
Comment: 0,0:00:00.00,0:00:00.00,Box,,0,0,0,mangetsu-style,{\boxpx10\boxpy5\boxr3\bbs2\bbc&HFFFFFF&}
```

Text must contain override blocks in braces. Plain text is ignored. Multiple
blocks and declarations merge in source order, property by property. Later
scalar assignments win, including assignments back to a native default.
**Gradients are the exception:** the first valid source wins independently for
each target’s color and alpha, across the entire initial metadata block. Existing
relative numeric operands resolve once during loading against the style and
earlier declarations; they are not accumulated again on each frame.

Unknown, malformed and unsupported tags are ignored, with verbose diagnostics
when enabled. Unsupported parenthesized tags are skipped as whole units:
`\t(\furis20)` does not apply its child tag. Event timing, positioning,
transforms, fading, clipping, drawing, karaoke, font tags, standard ASS
appearance tags, image fills (`\img` and numbered forms), and pattern
definitions are not metadata properties. The gradient families below are supported. They remain available inline.

## Gradient defaults and source selection

The existing gradient grammars and renderer are used, with these metadata families:

| Family | Supported tags |
| --- | --- |
| Attached color / alpha | \grd, \1grd…\5grd / \1gra…\5gra |
| Four-corner color / alpha | \vc, \1vc…\4vc / \va, \1va…\4va |
| Positioned color | \pgrd, \1pgrd…\5pgrd |
| Native border color / alpha | \Nbgrd, \Nbvc, \Nbpgrd / \Nbga, \Nbva; N=1…10 |
| Box-border color / alpha | \Nbbgrd, \Nbbvc / \Nbbga, \Nbbva; N=1…10 |
| Waiting layer-1 outline | \3svc, \3sgrd (color; active outline alpha) |
| Global positioned alpha | \pgrdalpha (independent RGBA field) |

Unnumbered box aliases (\bbgrd, \bbga, \bbvc, \bbva) select box ring 1.
See [gradient targets and rendering limits](gradient-support.md) and
[positioned fields](position-gradient.md) for argument syntax, limits,
coordinate spaces and fallback behavior. These tags do not enable a disabled
native or box ring; give it a size as usual. There are no new inline tags or
new higher-layer waiting gradient families.

For every target, **first valid gradient source wins in metadata**. This includes
repeated tags from the same family, different gradient families, and declarations
in later comments. Color and alpha select independently. Aliases share a slot:
\grd/\1grd, \3grd/\1bgrd/\3vc/\1bvc, and their alpha counterparts
cannot compete as independent sources. Fill, secondary fill, shadow/BS4 fill,
decorations, extra native rings, box rings, waiting outline and global alpha
retain independent slots. Invalid or malformed sources do not claim a slot.
Empty/zero gradient resets in metadata declare no source and do not clear a
previous winner. Later solid border colors/alpha remain fallback assignments;
they do not erase a winning gradient or reserve its slot.

~~~ass
Comment: 0,0:00:00.00,0:00:00.00,Sign,,0,0,0,mangetsu-style,{\fshp5\ortho1\grd(45,&HFF0000&,&H0000FF&)}
Comment: 0,0:00:00.00,0:00:00.00,Sign,,0,0,0,mangetsu-style,{\vc(&HFFFFFF&,&H000000&,&HFFFFFF&,&H000000&)\1gra(0,&H00&,&H80&)}
~~~

Here the attached color wins over the later vector color; alpha is independently
accepted. Splitting or joining those comments does not change the result.

**Inline dialogue remains last-valid-source-wins.** Inline \vc, \grd, or
\img can replace inherited color paint. Image fills are still unsupported
inside style metadata. Bare and named \r restore the target style’s compiled
paint before the existing actor patch. Empty vector/attached gradient tags and
attached inline 0 resets restore the inherited source for that target/channel,
when configured. Without a configured gradient, the historical reset behavior
is unchanged. Empty positioned color tags retain their restriction to a current
positioned source, then restore the style’s source; they do not disturb an
unrelated current attached/vector source. Empty \pgrdalpha() restores the
inherited global field. Waiting-outline resets restore its inherited paint.
Resetting one channel preserves the other channel. Effective fill/native-border
reset defaults include applicable Actor Colorcoding patches, including an actor
solid source replacing the style gradient. The actor whitelist is unchanged.

All varying RGB paint requires RGBA or automatic host rendering. Legacy ASS_Image
output keeps the existing solid fallback and cannot express varying RGB; it is
not a gradient-capable output format. Attached domains follow the existing
rendered bounds; positioned fields remain in script/frame coordinates through
movement, scrolling, transforms and clipping.

## Inheritance, Actor Colorcoding and resets

Dialogue state is initialized in this order:

1. Ordinary ASS style and native Mangetsu defaults.
2. The active style's compiled extension defaults.
3. The existing Actor Colorcoding patch, when applicable.
4. Explicit dialogue override tags, including normal inline animation.

Parameterless `\fshp` restores the effective style line spacing (zero without
metadata); `\fshp` is distinct from `\fsvp`, `\fsp` and native `\vsp`.
It accepts signed and relative numeric operands using the existing line-height
calculation. Parameterless `\ortho` restores the style projection mode (0
without metadata). Metadata accepts only integers 0 and 1; both properties
follow bare and named appearance resets. Explicit dialogue values win.

Thus `\furis50` overrides an inherited 70 in its normal inline scope. Bare `\r`
restores the dialogue's original style and its extensions. `\rOtherStyle`
uses `OtherStyle` and its own extensions, or native defaults if none exist.
Parameterless `\furis` restores both inherited scale axes; `\furisx` and
`\furisy` restore only their own inherited axis. Other supported parameterless
furigana controls restore the active style's spacing, placement, enabled state
and layout policy as well. Explicit `\furis50` remains an explicit 50%, even
when the inherited value is 70%. Without metadata the native 50% defaults and
other native furigana defaults are unchanged. Border size/color empty resets
use the effective defaults already used by the normal parser. Parameterless
`\Nbsc` restores the active style’s waiting color for that layer, or unsets
it when none is configured. `\r` and named resets restore these appearance
defaults too; layers remain disabled until their size is enabled. Waiting
paint inherits the active layer’s alpha, fade and alpha gradients.

Wrapping and vertical controls select the **initial event layout** separately
from appearance. Neither bare nor named `\r` changes them. Inline `\q`
overrides wrapping; an empty or invalid inline `\q` restores the initial
style’s wrapping default (or script `WrapStyle`). Metadata only accepts modes
0–3. Vertical inline controls override the inherited values before visible
text, and retain their usual restrictions after text or inside transforms.
Parameterless vertical controls keep their existing inline native-default
semantics; they are not appearance-style resets. See [native vertical layout](native-vertical.md).

```ass
Comment: 0,0:00:00.00,0:00:00.00,Vertical,,0,0,0,mangetsu-style,{\q2\vert1\vtype3\vdir2\vsp2\vcolsp8}
Comment: 0,0:00:00.00,0:00:00.00,Karaoke,,0,0,0,mangetsu-style,{\2bs3\2bc&HFFFFFF&\2bsc&HFF0000&}
```

Actor Colorcoding keeps its existing syntax, actor matching and reset policy.
Its color patch takes priority where both metadata systems specify a border
color. The Actor Colorcoding whitelist is unchanged: `\Nbsc` and event-layout
controls are not actor properties. Adding `\furis70` does not touch any actor colors. Without an actor
style whitelist, bare `\r` reapplies the actor patch and named resets remain
the existing escape hatch. With a whitelist, the patch applies on either
reset only when the destination style is allowed. See
[Actor Colorcoding](actor-colorcoding.md).

Both recognized metadata types may be interleaved in the initial block, in
either order. An unrelated comment or dialogue closes this block for both
systems; later matching comments and later `Format:` lines do not reopen it.
Unrelated comments retain their ordinary ignored behavior.

## Loading and compatibility

The implementation compiles metadata during ASS text loading into a sparse
patch indexed by style ID in private track storage. Rendering applies only
those fields, before Actor Colorcoding; it never concatenates dialogue strings
or reparses extension comments. Freeing styles or tracks frees their patches.
Flushing dialogue packets retains the owning track's style defaults. Typed
layout defaults are applied before font selection and shaping, once per event
initialization, and are excluded from appearance resets. Typed furigana defaults live in the compiled patch; dialogue resets borrow an
immutable pointer, with a shared native constant for unextended styles. Rich
paint uses a separate sparse, track-owned array of typed sources with copied
stops/coordinates. No parser text is retained. Only selected paint records are
applied; unextended styles allocate/copy no new rich paint arrays. Global alpha
uses the existing event-owned immutable mask snapshot so frame preparation
does not mutate track defaults. Style replacement and track destruction free
the owned paint records.

Standard ASS style records and the public style/track layouts are unchanged.
Files remain ordinary ASS, and other renderers ignore these comments and use
their ordinary defaults. They need their own inline equivalents for the same
appearance. Hosts can feed the initial comments through full ASS text loading
(`ass_read_memory`, `ass_read_file`, or `ass_process_data`). Matroska packets
passed to `ass_process_chunk` have no Comment/Dialogue distinction and cannot
discover this metadata themselves. An integration supplying comments separately
can call the additive public API after loading its style headers:

```c
ass_process_mangetsu_metadata_line(track, "Default", "", "mangetsu-style",
                                  "{\\furis70}", 1, 1);
ass_process_mangetsu_metadata_line(track, "Default", "Nene",
                                  "mangetsu-colorcoding", "{\\1c&HFFB6D9&}", 1, 1);
```

The final arguments mean **is a Comment** and **is in the initial block**.
The host must close the single shared metadata block at an unrelated comment or
dialogue and pass these flags accurately. Return values are 1 for recognized
metadata, 0 for unrelated/out-of-block input, and -1 on failure. An unknown style
is recognized but unapplied and warns. Input strings are compiled/copied and
may be freed immediately. The original `ass_process_mangetsu_colorcoding_line`
API and symbol remain available, with unchanged behavior. Do not feed the same
declaration through both APIs.

Metadata updates must not race rendering. Load it before rendering, or update
it between completed render passes. Metadata is not an inline animation; the
next reset/render pass uses the replacement defaults.

## Regression and performance checks

`style-extensions` compares real Burmese glyphs, geometry, colors and pixels
with equivalent inline tags in both legacy and RGBA output. It covers empty
resets, named resets, mixed metadata, actor precedence, gyaku-furi, vertical
readings, karaoke, separately fed host metadata and track/style ownership.
The furigana workflow builds and runs the full available Meson suite under
ASan/UBSan, then reruns the metadata/ownership test with leak detection enabled.

Its performance job builds this revision and baseline `423f526` with the same
release compiler, dependencies and attached fonts. A public-API harness excludes
loading/font setup, warms 100 frames, then measures CPU time for 1000 frames
containing 12 active events. Seven trials alternate revision order on one CPU.
It measures ordinary ASS, ordinary ASS with standard overrides, and inline
furigana, inherited furigana, native vertical layout, and multi-border karaoke
without secondary sources, inline/inherited RGBA gradients and native multi-border
RGBA gradients; inherited furigana is also compared with inline furigana. Geometry
checksums must agree. The job publishes medians, percentage changes and every
raw sample. Shared-runner timing is informational and must be reviewed before
claiming that a regression has been ruled out.

The gradient-metadata benchmark renders the baseline through equivalent inline
tags because that revision cannot inherit gradients. Gradient workloads use
RGBA and verify geometry plus sampled pixel checksums; ordinary/furigana
workloads retain the existing legacy harness. Raw trial results are published.
