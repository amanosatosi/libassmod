## Development Priorities

* **Do not compile locally.** Builds and compilation are handled by GitHub Actions after changes are made.
* **Prioritize rendering performance.** Mangetsu is a renderer, so avoid unnecessary per-frame/per-glyph work and prefer efficient implementations.
* **Prioritize backward compatibility.** Existing ASS/libass behavior should remain unchanged unless a feature explicitly requires a behavior change. Do not break existing subtitles, tags, scripts, or workflows unnecessarily.
* When adding new features, prefer implementations that are both **fast and compatible** over solutions that are merely simpler to implement.
* Avoid introducing new dependencies or architectural changes when the same result can be achieved within the existing rendering pipeline.
