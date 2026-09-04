# Unicode decimal digits in ASS numbers

Mangetsu accepts Unicode decimal digits (`General_Category=Nd`) anywhere ASS
defines a decimal integer or floating-point number. Each digit is converted by
its Unicode decimal value before the existing, locale-independent ASS number
grammar is applied.

For example, all of these represent decimal 20:

```text
20   ၂၀   ٢٠   ۲۰   २०   ２０   ๒๐   ២០
```

Digit scripts may be mixed. `1၂٣４` is parsed as 1234. ASCII punctuation keeps
its normal meaning, so `၁၂၃.၅` is 123.5, but locale-specific punctuation is not
introduced as an alternate decimal separator.

The shared parsing applies to:

- numeric override-tag arguments, including Mangetsu tags such as `\scale` and
  multi-border layers;
- decimal fields in `Dialogue:` and `Comment:` events, including layer,
  timestamps, and margins;
- integer and floating-point fields in `[V4+ Styles]`, including font size,
  glyph scales, spacing, angle, border values, alignment, margins, and encoding;
- numeric `[Script Info]` values such as `PlayResX`, `PlayResY`, `LayoutResX`,
  `LayoutResY`, `WrapStyle`, and `Timer`.

Examples:

```ass
[Script Info]
PlayResX: ၆၄၀
PlayResY: ٣٦٠

[V4+ Styles]
Style: Default,Arial,４８,&H00FFFFFF,...

[Events]
Dialogue: ०,٠:٠٠:٠٠.٠٠,٠:٠٠:٠٢.٠٠,Default,,၁၀၀,๑๐๐,៥០,,{\pos(၆၄၀,၃၆၀)\scale၁၂၅}Text
```

Hexadecimal color and alpha fields retain their existing ASCII hexadecimal
grammar (`0-9`, `A-F`, and `a-f`). Arbitrary text metadata is not normalized.
