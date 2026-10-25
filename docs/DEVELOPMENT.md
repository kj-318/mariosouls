# Development (Dark Souls Remastered)

This fork builds the **Dark Souls Remastered** host: `framework/hosts/ds1remastered/`, x64, DX11. It reuses Ted Cassirer's framework, SDK and Mario adapter unchanged. Read [PORT_STATUS](../framework/hosts/ds1remastered/PORT_STATUS.md) for how the host is put together, and [architecture](../framework/README.md) and [AGENTS.md](../AGENTS.md) before changing shared code.

## Prepare dependencies (once)

Use Windows, Git and Python 3.12. The pinned Zig 0.13.0 toolchain, MinHook, libsm64 and SM64 decomp revisions are recorded in [dependencies.lock.json](../dependencies.lock.json).

```powershell
python tools/dependencies.py --setup --download-toolchain
python tools/framework.py build --with-runtime --check-portability
```

The second command builds the x64 Mario adapter and libsm64 runtime that the Remastered package uses:
- `work/framework-build/portability/x86_64-windows-gnu/mario64.dll`
- `.../runtime/sm64.dll`

It also builds the original x86 PTDE host as part of the portability check; this fork doesn't ship that host. Rerun the command after changing adapter, runtime or `patches/libsm64` code.

## Build the Remastered host

```powershell
tools\build_remastered.bat
```

This compiles `test_shaders.hlsl` into `test_shaders_gen.h` with `tools/compile_test_shaders.py`, then runs `framework/hosts/ds1remastered/build.py`. That links `work/framework-build/dinput8_x64.dll` with Zig (`x86_64-windows-gnu`, `-Werror`) against MinHook. The log is in `work/framework-build/last-build.txt`. Building does not touch the game.

To test, copy `dinput8_x64.dll` to the game folder as `dinput8.dll`, next to `DarkSoulsRemastered.exe`, with the `MarioSouls` folder and a ROM. See [installation](INSTALLING.md). Keep a copy of the DLL you replace.

## Package a release

```powershell
tools\package_remastered.bat
```

This writes `work/release/MarioSouls-DSR/` and `work/release/MarioSouls-DSR.zip`. The package holds:
- `dinput8.dll`
- `MarioSouls\mario64.dll`
- `MarioSouls\sm64.dll`
- `MarioSouls\settings-example.txt`
- `README - MarioSouls.txt`

The player supplies the ROM; collision is built from their game on first launch.

## Settings and diagnostics

Built-in defaults are in `kDefaultModeText` (`geometry_pass.cpp`). Overrides are read live from:
- `<game>\MarioSouls\settings.txt` (for players);
- `%LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt` (for development).

`diag=on` enables the expensive probes and extra logging. The log is `%LOCALAPPDATA%\MarioSoulsProbe\remastered_host.log`.

## Collision tools

The host builds collision itself (`collision_build.h`). `prepare_collision.py` and `prepare_breakables.py` in the host folder are the Python reference exporters. The C++ builder's output was checked byte for byte against them. Use them to compare output after changing either side. Their output, like any game data, ROM or capture, stays out of Git.

## Making changes

- Keep native Remastered logic in `framework/hosts/ds1remastered/`, neutral policy in core/SDK, and SM64-specific behavior in the adapter.
- Record behavior and tuning changes in [PORT_STATUS](../framework/hosts/ds1remastered/PORT_STATUS.md) and the [changelog](../CHANGELOG.md). Record new offsets and evidence in [REMASTERED_FINDINGS](REMASTERED_FINDINGS.md).
- Test on disposable characters, offline. Note what was compiled separately from what was checked in game.

## PTDE reference build

The original PTDE host (`framework/hosts/ds1ptde/`, x86/D3D9) is still in the tree as the behavior the port follows. It is built with `python tools/framework.py build` and installed with `MarioSouls.ps1`. This fork does not ship it; use [the original project](https://github.com/TedCassirer/mariosouls) for PTDE.
