# CLAUDE.md – VST2Bridge

Short map for Claude. Users read `README.md`; technical details are in `docs/` (see the table in the README).

## Rules

- For every change, check whether `README.md`, the matching file in `docs/` and the release notes (the `body` of "Create GitHub release" in `.github/workflows/build.yml`) need an update, and do it in the same change. Say in the PR what was checked.
- Add a row `| next | | ... |` to `docs/versions.md` for every change that ends up in a release (plain language, newest on top). Never write `r<N>` by hand; the Autobuild workflow numbers the rows.
- `bridge/protocol.h` is shared by the bridge and the helper, which are always shipped together: bump `VB_VERSION` when the layout changes.
- The tag `autobuild-r<N>`, the asset name `VST2Bridge-Patcher-r<N>-windows.exe` and `--selftest-out` are a contract between `build.yml` and `utility/selfupdate.py`.
- Run `bash bridge/test/run_tests.sh` (mingw-w64 + Wine) after changing `bridge/`; extend `test/fakevst.c` / `test/testhost.c` for new paths.

## Background

Started from issue #1, modeled on the VST2 / CLAP bridge of [SXG-Create-NF](https://github.com/NightFright2k19/SXG-Create-NF) (`clapbridge/`, issue #104 there) and its patcher; the goal is a free replacement for JBridge's automatic bridging (as used by Falcosoft's BASS_VST fork).
