# Extended \fad Color Fade

libassmod supports normal ASS `\fad` syntax:

```ass
\fad(fade_in_ms,fade_out_ms)
```

It also supports an extended four-argument form:

```ass
\fad(fade_in_ms,fade_out_ms,start_color,end_color)
```

The first two arguments use normal ASS `\fad` timing. `start_color` controls
the fade-in side, and `end_color` controls the fade-out side. Colors use normal
ASS BGR syntax such as `&HFFFFFF&`, `&H000000&`, and `&H0000FF&`.

An empty color argument keeps classic alpha fading for that side:

```ass
\fad(300,300,,&H000000&)
\fad(300,300,&HFFFFFF&,)
```

Add `+a` to a color argument to combine the color fade with classic alpha fade:

```ass
\fad(300,300,&HFFFFFF&+a,&H000000&+a)
```

Without `+a`, a non-empty color argument performs color fade only for that side.
With `+a`, it performs both color fade and alpha fade. If a color argument is
malformed, that side falls back to classic alpha fade.

Color fade is a final visible-color pass. libassmod first computes the normal
appearance from styles, actor colorcoding defaults, inline tags, `\t`
transforms, gradients, native multi-border state, and box state. It then mixes
the final visible RGB color from or toward the `\fad` color during the active
fade window.

This means `\t` still computes the normal color for the frame, but extended
`\fad` has final priority during the fade window:

```ass
{\1c&H0000FF&\t(0,500,\1c&H00FF00&)\fad(300,300,&HFFFFFF&+a,&H000000&+a)}Text
```

The RGB pass affects visible color channels, including primary text, secondary
karaoke color when visible, outline/border, shadow, enabled native multi-border
layers, sampled native border gradients, and BorderStyle=4 box color when box
mode is enabled. It does not enable inactive components: a zero-size native
border layer remains hidden, and disabled box mode does not make the box color
visible.

Examples:

```ass
{\fad(300,300)}Classic alpha fade
{\fad(300,300,&HFFFFFF&,&H000000&)}White-to-normal, normal-to-black RGB fade
{\fad(300,300,,&H000000&)}Classic alpha fade-in, black RGB fade-out
{\fad(300,300,&HFFFFFF&,)}White RGB fade-in, classic alpha fade-out
{\fad(300,300,&HFFFFFF&+a,&H000000&+a)}RGB and alpha fade on both sides
```
