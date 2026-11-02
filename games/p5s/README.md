# Persona 5 Strikers (Switch, Unity/IL2CPP)

The glue to run the player's own Strikers dump with this runtime, the way `games/p5r` does for Persona 5 Royal:
the APK holds no game code or data; the player picks their own game file and `prod.keys` on the phone.

## Status: starting point, not a working port

Nothing here has been run on a phone or on a real Strikers dump. What exists:

| Piece | State |
|---|---|
| `game.cpp` | Metadata only. Program ID and system resource size are read from the player's game file (`runtime/src/svc_process.cpp`); any build is accepted (`main_build_id` is null). No hooks, no patches. |
| `AndroidManifest.xml`, `java/` | The Royal launcher renamed (package `io.github.p5sandroid`, data folder `Internal storage/Persona 5 Strikers`). No build ID check. The touch buttons are Royal's (`runtime/src/touch_layout.cpp`). |
| `import/` | On-phone import: opens the game file, extracts NVN shaders from Unity data and hands them to the translator (`libspirvharness.so`). **The Unity side is written from the documented formats and tested only on synthetic data** (`tools/test_scan.py`). If it finds nothing it writes `import-report.txt` in the game's data folder saying what it scanned. |
| `tools/unity_shaders.py` | The same extraction as a PC research tool (UnityPy), for looking at a dump. |
| `tools/test_scan.py` | Builds `import/p5s_scan_cli.cpp` and checks it on synthetic Shader objects and UnityFS bundles. |

What is certainly still missing: whatever services, SDK calls and NVN features Strikers uses that the runtime does not
implement yet (they show in the log as unknown services/commands), Unity-specific shader
details, audio and video middleware, and performance work. Expect an iterative loop of run, read the log, fix.

## What a dump looks like (from a file listing of the base game's NSP)

Unity, IL2CPP (`Data/Managed/Metadata/global-metadata.dat`, no managed DLLs: the game code is in the ExeFS main).
Besides `globalgamemanagers*`, `level0`, `sharedassets0.assets`, `resources.assets` and `Data/Resources/unity_builtin_extra`
there are 6,826 Addressables bundles (`StreamingAssets/aa/Switch/<32 hex>.bundle`) and 1,837 CRI files
(`aa/Switch/CriStreamingData/*.acb|.awb|.usm` followed by a hash, plus `Crisound_cmn.acb`): the importer skips the CRI
folder and scans everything else. CRI ADX2 (audio) and Sofdec (`.usm` movies) run as game code, so the runtime's audio
and video services will matter. Shaders are in the bundles and in the files above; which, and how they are compressed,
is what the first import report will say.

## Build

Linux / Colab: `bash android/colab/colab_build.sh p5s` (sets up the SDK/NDK, the translator and the APK).
The notebook `android/colab/p5s_build.ipynb` does this from a clean Colab session.
Windows: as the root README, with `android\build_apk.ps1 -Game p5s -Nce` (after the translator library step there).

Use only a game file you dumped from your own console. Do not share the APK together with game data or keys.

## Logs (no adb needed)

Both are plain text files in `Internal storage/Persona 5 Strikers/` (a file manager shows them):
- `nx.log`: the game's run, written line by line (so it survives a crash); the run before it is `nx.log.prev`.
  Only the non-release build writes it (`android/build_apk_nce.sh` without `--release`, which is what the Colab notebook builds).
- `import-report.txt`: what the shader import scanned, and why it failed if it did.

## Rebuilding without losing anything

- The APK gets a new `versionCode` on every build (`android/build_apk_nce.sh`), so installing a new build updates the
  installed app in place, as long as it is signed with the same key. The Colab notebook keeps that key in Google Drive
  (`MyDrive/p5s/p5s.keystore`, `ANDROID_KEYSTORE`); keep that file, or the next build has to be uninstalled first.
- The translated shaders are not in the APK: they are in the data folder (`Internal storage/Persona 5 Strikers/spirv/`),
  and the launcher imports again only when `spirv/import.txt` does not name the picked game file. A new build does not
  touch it. (Uninstalling removes only the app's own folder under `Android/data`, so the shaders survive that too; the
  keys and the game file are picked again.)
