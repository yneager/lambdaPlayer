# Motrix download client

Copied from Motrix Turbo 2.0.0-beta.46 (MIT, copyright Dr_rOot).
The TypeScript sources here are unmodified copies of the download RPC client,
JSON-RPC protocol, pause reconciliation and byte formatting utilities.
`tools/download-engine/generate-client.mjs` removes TypeScript and imports to
generate the bundled browser script. Regenerate with Node 24 or newer after
changing the copied sources.

Before configuring a Windows build, run
`node tools/download-engine/fetch-engine.mjs --platform win32 --arch x64`.
CMake copies the verified engine beside the player executable; CI and local
packaging include it and the license notices.

The browser WebSocket transport and Qt process/file-dialog bindings are in
`resources/vui/downloads.js` and `src/downloadsbridge.*`. Qt engine arguments
are adapted from Motrix's `Aria2ConfigBuilder.buildArgs`; the base config,
fetch script and SHA-256 lockfile are copied from Motrix. The Qt bindings use
the pinned engine's SQLite persistence. Downloads continue during playback;
closing the player shuts down the engine and saves its state.

The Downloads view uses LAMBDA's existing Home/Add-ons components. It exposes
URL/magnet and torrent addition, progress, pause/resume, cancellation, removal
of history entries, download folder selection and playback of completed files.
Motrix's plugin, browser-extension, proxy settings and media processing layers
are not included.

See `licenses/Motrix-LICENSE.txt` and `THIRD_PARTY_NOTICES.md`.
