Gradient notes:
- Use `ass_render_frame_rgba` to get per-pixel premultiplied RGBA (`ASS_ImageRGBA`); free with `ass_free_images_rgba`.
- RGBA buffers are RGBA8888 premultiplied; `type` matches `ASS_Image.type` for fill/outline/shadow.
- Gradient mapping restarts on each rendered line break (`\N` or wrapping) while the enabled gradient state persists until a uniform `\dc`/`\da` tag or an empty `\dvc`/`\dva` disables it.
- Uniform color/alpha tags override and disable the matching gradient layer; `\r` resets gradients to style defaults.
- Implementation behavior was verified against VSFilterMod for compatibility; implementation is original to keep ISC licensing.
