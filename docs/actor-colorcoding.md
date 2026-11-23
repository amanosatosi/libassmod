# Mangetsu actor colorcoding

Mangetsu colorcoding lets ASS files define actor-based appearance defaults
using normal `Comment` events at the top of `[Events]`. Old renderers ignore
these lines as comments, and deleting the block leaves a normal ASS script.

## Metadata block

Colorcoding metadata is a top contiguous block of `Comment` events immediately
after the `[Events]` `Format:` line. A line belongs to the block only when:

- the event type is `Comment`
- `Effect` is `mangetsu-colorcoding` after trimming surrounding spaces
- `Name` is non-empty

The block may also contain [style-extension metadata](style-extensions.md)
with Effect `mangetsu-style`, in either order. Parsing stops at the first
ordinary event line. Oborodzuki Dialogue carriers can occur within the block. Later matching
comments elsewhere in the file are ignored as normal comments.

Hosts that bypass full ASS text parsing must still pass this metadata to
libassmod. `ass_process_chunk()` uses Matroska-style event packets without a
Dialogue/Comment event-type field, so it cannot safely discover colorcoding
comments by itself. Integrations that strip comment events or construct tracks
directly should feed the top metadata block with
`ass_process_mangetsu_colorcoding_line()` before rendering dialogue events.
Hosts preserving several kinds of initial metadata can instead feed both actor
and style comments through `ass_process_mangetsu_metadata_line()`. The existing
actor-only API, marker matching and whitelist/reset rules are unchanged. The
host must enforce one initial contiguous block for both metadata types.

```ass
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,9:59:59.99,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Alt}
Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\fs42\1c&HFFB6D9&\3c&H7A2E50&}
Dialogue: 0,0:00:01.00,0:00:04.00,Default,Nene,0,0,0,,Hello.
```

## Applied Styles

The reserved actor name `mangetsu-colorcode-applied-styles` defines the style
whitelist for resets and style switches:

```ass
Comment: ...,Default,mangetsu-colorcode-applied-styles,...,mangetsu-colorcoding,{Default}{Alt}{Sign}
```

Style names are parsed from non-empty brace groups and matched exactly.
If the line is present but contains no non-empty style groups, libassmod warns
and treats the whitelist as absent.

If this config line is absent, actor colorcoding applies to the line's initial
style and to bare `\r`, but explicit `\rStyleName` is an escape hatch and does
not reapply actor colorcoding.

If the whitelist is present, actor colorcoding applies whenever the active
style is in the list. This includes the initial style, bare `\r`, and explicit
`\rStyleName`.

## Actor Lines

For normal colorcoding lines, `Name` is the actor name and `Text` contains ASS
override tags. Multiple lines for the same actor are allowed; later lines
replace earlier actor defaults.

Actor colorcoding is a sparse renderer-side appearance patch. It is not a fake
style and it is not prepended to dialogue text. Only explicitly present tags are
applied; missing fields keep inheriting from the active style and native
libassmod defaults.

## Supported Tags

These appearance tags are supported in colorcoding metadata:

```ass
\fn \fs \b \i \u \s
\bord \xbord \ybord
\shad \xshad \yshad
\blur \be
\c \1c \2c \3c \4c \5c
\alpha \1a \2a \3a \4a \5a
\1grd(...)..\5grd(...)
\1gra(...)..\5gra(...)
\vc(...) \1vc(...)..\4vc(...)
\va(...) \1va(...)..\4va(...)
\1bs..\10bs \1bsx..\10bsx \1bsy..\10bsy
\1bc..\10bc \1ba..\10ba
\1bgrd(..)..\10bgrd(..)
\1bga(..)..\10bga(..)
\1bvc(..)..\10bvc(..)
\1bva(..)..\10bva(..)
```

`\vc(...)` is the unnumbered alias for the primary-fill `\1vc(...)` vector
color gradient. Likewise, `\va(...)` aliases the primary-fill `\1va(...)`
vector alpha gradient. The numbered forms target primary, secondary, outline,
and shadow/box paint layers 1 through 4 respectively.

The `\1bs` through `\10bs` tags above are native border-layer size tags. They
are distinct from the line-level `\bs<N>` BorderStyle override.

Forbidden, unknown, layout, timing, drawing, transition, clipping, karaoke, and
motion tags are ignored inside colorcoding blocks. For example, in
`{\1c&HFFB6D9&\pos(100,100)}`, the color is applied and `\pos` is ignored.

`\1grd(angle,color0,color1)` through `\5grd(...)`,
`\1gra(angle,alpha0,alpha1)` through `\5gra(...)`, and their border-layer
forms `\1bgrd(...)` through `\10bgrd(...)` and `\1bga(...)` through
`\10bga(...)` are Mangetsu true gradients with angle control and percentage
stops. Color gradients use ASS BGR values such as `&HBBGGRR&`. Alpha gradients
use ASS alpha bytes such as `&HAA&`, where `&H00&` is opaque and `&HFF&` is
transparent. Stop positions still use percentages, and duplicate colors or
alpha values are preserved to create flat zones.

`\3grd(...)` is an alias for `\1bgrd(...)`; `\3gra(...)` is an alias for
`\1bga(...)`. Resets such as `\2grd()`, `\2gra0`, and `\2bga()` disable only
the matching channel or border layer. Solid color tags disable only their
matching true-gradient color source: for example, `\1c` clears `\1grd`,
`\3c` clears `\3grd`/`\1bgrd`, and `\2bc` clears `\2bgrd`. Solid alpha tags
disable only their matching true-gradient alpha source: for example, `\1a`
clears `\1gra`, `\3a` clears `\3gra`/`\1bga`, and `\2ba` clears `\2bga`.
`\alpha` clears the active alpha-gradient channels affected by normal global
alpha behavior. Font/style changes and `\N` do not split an active Mangetsu
gradient segment.

`\pgrd(x1,y1,x2,y2,angle,stops...)` and its `\1pgrd` alias are primary-fill
fixed-frame color gradients. `\2pgrd`..`\5pgrd` and `\1bpgrd`..`\10bpgrd`
use the corresponding color targets, including actor colorcoding overrides.
They use the same color-stop grammar as `\1grd`.
Actor colorcoding remains the ordinary primary color beneath this source, so it
is used for every positioned-gradient pixel outside the fixed rectangle unless
an ordinary `\c` or `\1c` later overrides it.

Mangetsu true-gradient definitions can be animated with normal ASS `\t(...)`
transforms. Angles use shortest-path interpolation, colors interpolate per RGB
component, alpha gradients interpolate ASS alpha bytes numerically, and
gradients with different stop positions morph through the union of both stop
lists. If a transform targets a gradient while the current channel is solid,
libassmod synthesizes a source gradient using the target stop positions and the
current solid color or alpha value. Animated gradient resets such as
`\t(\1grd())` and `\t(\1gra0)` are ignored safely; reset tags outside `\t`
keep their normal behavior. This applies to Mangetsu `\grd`/`\bgrd` and
`\gra`/`\bga` tags only, not to VSFilterMod-style `\vc`/`\bvc`/`\va`/`\bva`
gradients.

## Reset Behavior

For each dialogue line, libassmod builds state in this order:

1. active ASS style
2. native libassmod extended defaults
3. active style's `mangetsu-style` defaults, if present
4. actor colorcoding patch, if applicable
5. the dialogue line's own inline tags

Blank appearance resets return to the effective default from steps 1-4. For
example, if actor colorcoding sets `\fs42`, then inline `\fs` resets to `42`.
If actor colorcoding sets `\1c&HFFB6D9&`, then inline `\1c` resets to that
color.

`\r` rebuilds the style/default state and then reapplies actor colorcoding
according to the style rules above.

## Multi-Border

Colorcoding works with native multi-border layers and border gradients. Sparse
inheritance is preserved:

- `\2bc&H402030&` changes only layer 2 color
- missing `\2ba` keeps inheriting alpha
- missing `\2bs` does not enable layer 2
- `\2bs5\2bc&H402030&\2ba&H60&` enables layer 2 with explicit color and alpha
- `\Nbc` and `\Nba` set flat values and disable that layer's matching gradient
- `\Nbgrd(...)` sets that layer's Mangetsu true-gradient color source
- `\Nbga(...)` sets that layer's Mangetsu true-gradient alpha source
- `\Nbvc(...)` and `\Nbva(...)` keep their existing four-corner gradient meaning
- `\3a` uses normal mangetsu semantics and applies alpha to all native border
  layers without enabling extra layers or changing sizes

Native border layers 2 through 10 are outer rings. Their `\Nbs`, `\Nbsx`, and
`\Nbsy` values are additional thicknesses outside earlier/lower-numbered
native border layers, so `\2bs3` on a style whose normal outline is
`\bord5`/`Outline=5` creates a 3px layer-2 ring outside the 5px normal outline.

`\Nbva(...)` remains the existing four-corner/vector alpha-gradient tag.
`\Nbga(...)` is the Mangetsu true alpha-gradient tag with angle and percentage
stops. If both are used on the same border layer, the later tag wins for that
layer's alpha source.


## Shared Oborodzuki Dialogue carriers

Use these Dialogue events to share an ASS file between Oborodzuki preview and
Mangetsu encoding. Both renderers intercept the carriers, including any text
outside override blocks.

| Field | Preferred identifier | Legacy alias |
| --- | --- | --- |
| Effect | `OBRCC` | `oborodzuki-colorcoding` |
| Effect | `OBRS` | `oborodzuki-style` |
| Actor / Name on an `OBRCC` event | `OBRCCS` | `oborodzuki-colorcode-applied-styles` |

`OBRS` extends the style named in the Style field. `OBRCCS` supplies the style
whitelist for timed actor patches. The last whitelist declaration replaces
the previous one. Style extensions and whitelists are track-wide. The
Dialogue and Comment carriers can be interleaved at the top of `[Events]`.
Dialogue carriers can also appear later, while the Mangetsu Comment block is
still limited to the top of `[Events]`.

At each rendered video frame, the last matching actor declaration in
source order whose ASS Start and End bracket the frame wins. Start is
inclusive and End is exclusive. A visible line changes appearance as
declarations begin and expire. When a declaration ends, the next active
one resumes. Each patch supplies only its own properties.
The effective order is the ASS style, extended style defaults, the selected
actor patch, then inline overrides.

When a timed patch is unavailable for the actor or current style, Mangetsu
checks its original global Comment actor defaults and their own whitelist.
Comment timestamps still have no effect. Bare `\r` and empty appearance tags
restore effective defaults. Without an `OBRCCS` whitelist, a named `\rStyle`
skips the timed patch. With a whitelist, a named reset reapplies the same
selected patch when the destination style is listed.

```ass
Dialogue: 0,0:00:00.00,9:59:59.99,Default,,0,0,0,OBRS,{\furis70}
Dialogue: 0,0:00:00.00,9:59:59.99,Default,OBRCCS,0,0,0,OBRCC,{Default}{Alt}
Dialogue: 0,0:00:00.00,9:59:59.99,Default,NOELLE,0,0,0,OBRCC,{\2bs5\c&HD59B3E&\3c&HFFFFFF&\2bc&H724725&}
Dialogue: 0,0:05:00.00,0:10:00.00,Default,NOELLE,0,0,0,OBRCC,{\c&HAC5CCC&}
Dialogue: 0,0:07:00.00,0:08:00.00,Default,NOELLE,0,0,0,OBRCC,{\c&HFABB41&\1a&H20&}
Dialogue: 0,0:07:30.00,0:08:30.00,Default,NOELLE,0,0,0,,<今日|きょう> မြန်မာနိုင်ငံ
```

The line starting at 07:30 uses the special patch until 08:00, then
changes to the transformation patch while still visible. The normal
appearance resumes at 10:00.

### Filtered CSRI preview

Aegisub can open a fresh CSRI instance with just the Dialogue events
active on the requested frame. The actor declarations needed for that
frame are active by their ordinary ASS times. Track-wide style and
whitelist carriers should span the preview range so the host includes
them in filtered loads. Editing an actor carrier's Start or End changes
the interval immediately when the editor refreshes the frame.
[See Aegisub's frame loading](https://github.com/TypesettingTools/Aegisub/blob/master/src/async_video_provider.cpp#L51-L69).

Mangetsu handles the shared Dialogue carriers through ASS file loading,
`ass_process_data()` and `ass_process_chunk()`. For Matroska packets, actor
declarations use increasing ReadOrder for precedence and take priority
over actor declarations in the header. Feed global style and whitelist
declarations in source order, preferably with the header. Packet metadata
survives event pruning and `ass_flush_events()` so previously received
declarations remain available across seeks. Use a new track to replace the
script or its metadata. A demuxer that begins midstream must supply the
metadata needed for that seek position. An unseen packet cannot affect
the result until it is ingested.

Rendering comparisons use inline references within each renderer. Text
shaping, rasterization and some paint implementations can produce different
pixels between renderers even when both choose the same appearance.
