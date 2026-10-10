---
name: add-previewer
description: How to add a new way of previewing files in 26Viewer — an ImageDecoder, a StructureParser, or a new preview widget/tab in the Qt app — while keeping light/dark theming and background loading correct. Use when adding texture formats, data viewers, audio/3D previews, or changing PreviewPane.
---

# Adding a previewer

Pick the lightest option that works:

| Need | Implement | Lives in |
|---|---|---|
| File → picture | `viewer::ImageDecoder` (returns RGBA8 `Image`) | core if generic, game module otherwise |
| File → labelled byte regions | `viewer::StructureParser` (returns `StructureNode` tree) | same |
| Something new (audio, 3D, tables) | a widget + a field in `PreviewResult` | `src/app/preview/` |

## Decoder / parser (no UI code needed)

1. Subclass in the right module; `canDecode(ext, data)` must be cheap (extension or magic).
2. Register it in `registerContent()` (game) or `registerBuiltinContent()` (core, `ContentRegistry.cpp`).
   Later registrations win, so game decoders override built-ins.
3. Pixel formats: reuse `image::decodeSurface()` / `fromD3DFormat()`; add new
   `PixelFormat`s in `core/Image.*` with a unit test in `tests/test_image.cpp`.
4. The app picks it up automatically (`PreviewPane` → `loadPreview()`).

## New preview widget

1. Widget in `src/app/preview/`, added to `src/app/CMakeLists.txt`.
2. Heavy work (decode, parse) goes in `loadPreview()` (worker thread) and its result into
   `PreviewResult`; `applyResult()` (UI thread) only hands results to widgets.
3. Add it to the Preview stack or as a new tab in `PreviewPane`'s constructor; keep the
   tab-selection logic in `applyResult()` consistent (`Tab` enum, `m_preferredTab`).
4. Theming: never hard-code colours. Read `palette()` at paint time, rebuild cached
   brushes/pixmaps on `QEvent::PaletteChange` (see `ImageCanvas::changeEvent`).
5. Icons: `icons::theme(QIcon::ThemeIcon::…, fallback)` or an SVG in `resources/icons/`.
6. Verify with screenshots in both themes (`build-and-run` skill, `--tab` option).

## Limits

Preview reads are capped (`kLooseReadLimit`, `kPackedPreviewLimit` in `PreviewPane.cpp`);
very large files should get a streaming/lazy viewer instead of raising the caps.
