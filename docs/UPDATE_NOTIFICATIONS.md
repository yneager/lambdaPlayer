# In-app updates

LAMBDA checks for updates after startup and every six hours. Stable and test channels are separate. Clicking **Update** downloads the Windows runtime ZIP, checks its SHA-256 and size, prepares a complete replacement beside the installation, then closes and restarts LAMBDA. No browser or installer is involved in updating.

The app saves playback when closing normally. Library/settings/watch-later data stay in their existing profile folder. The updater also preserves the uninstaller and files added to a portable app directory. Preparing an update therefore requires space for the download and a full second copy of the application.

The previous directory is kept until the new app signals that it has started successfully. Startup failure restores and launches the previous version. Locked files or a restricted installation folder produce an error and keep the existing version. Download cancellation leaves the app running. The UI shows progress and release notes inside the app.

## Distribution

GitHub Releases is the default backend. Users never need a GitHub account, browser, domain or hosting subscription. A downloadable package still needs an online source. The CI release workflow publishes the installer, complete ZIP, checksums and a generic feed. GitHub API asset SHA-256 digests are required; an asset without a digest is ignored.

To use independent HTTPS hosting, place `update-source.json` alongside LambdaPlayer.exe:

```json
{"feedUrl":"https://updates.example.com/lambda/update-feed.json"}
```

The manifest format is:

```json
{"schemaVersion":1,"releases":[{"version":"v0.2.8","prerelease":false,"url":"https://updates.example.com/lambda/player.zip","sha256":"64 hexadecimal characters","size":123456789,"notes":"Release notes"}]}
```

`packaging/build-test-package.ps1 -UpdateBaseUrl https://updates.example.com/lambda` emits a ZIP and `update-feed.json` with the real hash and size. Retain stable and test entries in your hosted feed if both channels should remain available. GitHub releases are paged to the latest 100 entries.

The updater uses HTTPS transport plus a trusted feed checksum. This does not provide independent publisher signature verification if the hosting account is compromised. Keep release publishing credentials protected.

## Testing and rollout

Version `v0.2.8` includes this updater. The already released `v0.2.7-test.1` does not contain this updater, so existing users need one installation of an updater-enabled version. Later versions can be installed through **Update**.

Qt tests cover versions, channels, cached notifications, feed failures, independent manifests and missing/invalid checksums. The Windows transaction test covers directory replacement, preservation, restart health, rollback, path traversal and incomplete archives. It uses an isolated probe app, never the user's running LAMBDA installation.
