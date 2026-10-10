# Gradient targets and rendering limits

All gradients render through premultiplied RGBA. Use automatic rendering or
`ass_render_frame_rgba`; legacy `ASS_Image` output preserves masks and a solid
fallback, and cannot carry varying RGB. Alpha is ASS inverse alpha (`00`
opaque, `FF` transparent). Color and alpha sources remain independent.

| Target | Vector color / alpha | Attached Mangetsu color / alpha | Positioned field |
| --- | --- | --- | --- |
| Primary fill | `\vc`, `\1vc` / `\va`, `\1va` | `\grd`, `\1grd` / `\1gra` | `\pgrd`, `\1pgrd` color only |
| Secondary karaoke fill | `\2vc` / `\2va` | `\2grd` / `\2gra` | `\2pgrd` color |
| First ordinary outline | `\3vc`, `\1bvc` / `\3va`, `\1bva` | `\3grd`, `\1bgrd` / `\3gra`, `\1bga` | `\3pgrd`, `\1bpgrd` color |
| Native glyph-outline layers 1–10 | `\Nbvc` / `\Nbva` | `\Nbgrd` / `\Nbga` | `\Nbpgrd` color, N=1..10 |
| Ordinary shadow or BS4 fill | `\4vc` / `\4va` | `\4grd` / `\4gra` | `\4pgrd` color |
| BS4 outward rings 1–10 | `\Nbbvc` / `\Nbbva` | `\Nbbgrd` / `\Nbbga` | None |
| Underline and strikeout | No dedicated fifth vector family | `\5grd` / `\5gra` | `\5pgrd` color |
| Waiting first karaoke outline | `\3svc` (color) | `\3sgrd` (color); first-outline alpha | None |

Box layer-1 aliases are `\bbvc`, `\bbva`, `\bbgrd`, `\bbga`. Box fill
and ring paint are captured with the first visible content's geometry. A ring
without explicit gradient paint falls back to solid box RGB/alpha, not to the
fill's gradient. See [box tags](box-tags.md).

Vector corner order is top left, top right, bottom left, bottom right. Attached
Mangetsu syntax is `(angle,first_color[,position%,color...],last_color)`;
alpha uses the same stop grammar with inverse-alpha values. Named colors are
the shared `$white`/`$shiro`/`$siro`, `$black`/`$kuro` shortcuts. Stops may
repeat positions. Colors interpolate component-wise without gamma correction.
Angles and stop sets interpolate inside `\t` using the existing stop-union
rules. A solid color tag replaces only color; a solid alpha tag replaces only
alpha. Empty gradient parentheses and inline `0` reset that source. `\r` and
`\rStyleName` restore style paint. Invalid arguments preserve valid state.

Four-corner coordinates retain the existing logical-bitmap convention.
Attached gradients follow their target's final rendered bounds; their angles
use screen axes, not a locally warped texture. Native border rings use their
own occupied mask bounds, including blur. Shadows use their shadow bounds.
Primary and decoration paint use their corresponding rendered masks. Clip and
karaoke slices preserve the original field. All `\Npgrd` / `\Nbpgrd` fields
stay stationary in video/script-frame coordinates, including during native
scrolling; outside each rectangle that target's ordinary color remains active.
See [positioned gradient syntax, resets and examples](position-gradient.md).

Paint follows the existing shaping/layout pipeline: fallback fonts, Japanese,
Myanmar marks, normal and gyaku-furigana, wrapping, native vertical text,
columns, and supported curved-text layout retain their masks and attached
paint. Furigana uses inherited per-glyph paint; horizontal alignment and
`\furichangepos0/1/2` clearance do not add another paint pipeline. Karaoke
splits coverage rather than gradient state; `\kO` hides every paint layer
until activation. Waiting-outline paint applies only to the first outline.
Reversed progressive wipes select logical primary/secondary paint sources
together with their fallback alpha; explicit fifth-channel decoration paint
continues through both waiting and active phases. Box-ring solid overrides
inside `\t` interpolate the existing gradient stops toward the solid value.

Existing layout restrictions still apply: native vertical furigana placement
is unavailable; curved events with furigana, semantic columns, or scrolling
fall back to normal layout; semantic columns ignore karaoke timing. Gradients
do not enable unsupported layout combinations or positioned alpha fields. Image fills and pattern paint keep their
existing precedence; no image, cycle, or polka tags are added for box rings.

`test/gradient_surface_selftest.c` compares actual target pixels and masks for
BS4 transforms/clips, independent box rings, all ten native borders, BS5 blur,
decorations, inherited furigana paint, karaoke visibility, animation/reverse
seek, and stationary scrolling fields. Existing colorcoding, image-fill,
pattern, karaoke, layout, ownership, and decoration regression suites provide
additional coverage. `.github/workflows/gradient-audit.yml` builds and runs
the entire Meson regression suite with ASan/UBSan and a shared-library build.
Positioned-target regressions also assert visible RGB on supported targets,
independent simultaneous channels and borders, paint replacement/reset, animated
coordinates/angles/stops, malformed input, drawing masks, clipping, fades and
outside fallback. CI results establish validation.

## Audit findings

The BS4 temporary bitmap path previously carried only a solid color and blend
mode, dropping fourth-channel gradient metadata. Box-border paint also lacked
an independent gradient namespace and was omitted when RGBA requirements were
recomputed from text alone. Captured box paint now accompanies geometry into
the existing sampler, including its fade and blend state.

Attached outline and shadow gradients previously used base-text bounds, while
different native rings could share padded composite allocations. Target bounds
now come from the shadow bitmap or the occupied ring mask. This changes
Mangetsu attached domains; four-corner VSFilterMod coordinates remain unchanged.
The positioned-gradient rectangle previously subtracted scrolling displacement,
moving a documented fixed-frame field. Its script-frame mapping now stays fixed.

Malformed vector arguments previously passed through permissive color parsing
and could replace valid paint. Validation now happens before state changes.
Reversed progressive fills previously swapped only legacy colors, leaving RGBA
gradient sources and fallback alpha in the wrong phase. Decoration fifth-layer
gradients were transferred only to the active primary phase. Both now retain
their intended logical paint source through the common karaoke paths.

Regression fixtures preserve existing restrictions: inline reading overrides
select karaoke rather than arbitrary visual style; ASS suppresses fill-only
shadows behind fully invisible fill; eight-value distortion places P0 last;
and ordinary single outlines have different mask-subtraction semantics from
native composite rings. Tests isolate reading bands and compare like geometry
rather than changing those established behaviors.

The inner-ring allocation-independence fixture permits one alpha unit of blur
rounding and two RGB units after unpremultiplication at opacity at least 128.
CoreText/macOS produces those quantization differences when a larger outer
ring changes the composite allocation. The fixture checks every mask pixel,
the sampled colors in the common covered region, and both gradient endpoints;
it does not require byte-identical blurred allocations. Other mask/color
comparisons retain exact equality where the geometry and allocation are equal.
