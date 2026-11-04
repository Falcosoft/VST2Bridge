# CLAUDE.md – VST2Bridge

Short map for Claude. Users read `README.md`; technical details are in `docs/` (see the table in the README).

## Rules

- For every change, check whether `README.md`, the matching file in `docs/` and the release notes (the `body` of "Create GitHub release" in `.github/workflows/build.yml`) need an update, and do it in the same change. Say in the PR what was checked.
- Add a row `| next | | ... |` to `docs/versions.md` for every change that ends up in a release (plain language, newest on top). Never write `r<N>` by hand; the Autobuild workflow numbers the rows.
- `bridge/protocol.h` is shared by the bridge and the helper, which are always shipped together: bump `VB_VERSION` when the layout changes.
- The tag `autobuild-r<N>`, the asset names `VST2Bridge-Patcher-r<N>-{windows.exe,linux.tar.gz,macos-applesilicon.zip,macos-intel.zip}` and `--selftest-out` are a contract between `build.yml` and `utility/selfupdate.py`.
- Run `bash bridge/test/run_tests.sh` (mingw-w64, gcc + libxcb1-dev, Wine) after changing `bridge/`; extend `test/fakevst.c` / `test/testhost.c` / `test/testclap.c` for new paths.
- `stub.c` and `clap.c` build for Windows and (linked together) for Linux / macOS; platform code stays in their `#ifdef _WIN32` blocks and `plat.h`. The helper's `net:` mode (sockets, shared file, lock word) is the Linux / macOS transport; keep it in step with the Windows one.

## Background

Started from issue #1, modeled on the VST2 / CLAP bridge of [SXG-Create-NF](https://github.com/NightFright2k19/SXG-Create-NF) (`clapbridge/`, issue #104 there) and its patcher; the goal is a free replacement for JBridge's automatic bridging (as used by Falcosoft's BASS_VST fork).
