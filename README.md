# Ultimate Split-Screen — v0.1.1 source

Full source for the Windows Steam mod for LEGO Star Wars: The Complete Saga.
Published for review of the [Nexus release](https://www.nexusmods.com/legostarwarsthecompletesaga/mods/221).

The two C++ files, build script, launcher BAT and runtime notices are copied unchanged from the uploaded v0.1.1 archive. The source is laid out at the repository root so it can be built directly after cloning or extracting GitHub's source ZIP.

## Build

The original release build instructions are in [BUILD.txt](BUILD.txt). No game files are needed to compile.

1. On 64-bit Windows, download `llvm-mingw-20260908-ucrt-x86_64.zip` from the [official LLVM-MinGW 20260908 release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260908). This is the release toolchain: Clang 23.1.1, LLVM commit `6dfe1677ab8dffbc6ec13d53a1e0215d75147689`.
2. Extract that toolchain, for example to `C:\llvm-mingw`. Its `bin` folder must contain both `i686-w64-mingw32-clang++.exe` and `llvm-strip.exe`.
3. Clone this repository or use Code > Download ZIP and extract it. Open PowerShell in the folder containing `build.ps1`, `attach.cpp`, and `split.cpp`.
4. Run:

```powershell
.\build.ps1 -Compiler 'C:\llvm-mingw\bin\i686-w64-mingw32-clang++.exe'
```

Adjust the compiler path to your extracted folder. The script compiles and strips the x86 DLL and launcher into `build\TCS-Split-Prototype.dll` and `build\Enable-Split.exe`. The compiler includes its required Windows headers and libraries; a separate Visual Studio SDK is unnecessary.

To verify the toolchain ZIP:

```powershell
Get-FileHash 'C:\Downloads\llvm-mingw-20260908-ucrt-x86_64.zip' -Algorithm SHA256
```

Expected SHA-256: `1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6`.

To compare the compiled binaries with the Nexus release:

```powershell
Get-FileHash .\build\Enable-Split.exe, .\build\TCS-Split-Prototype.dll -Algorithm SHA256
```

Expected values are in [SHA256SUMS.txt](SHA256SUMS.txt). A rebuild with this toolchain on November 15, 2026 matched both uploaded v0.1.1 binaries byte for byte.

## Files and runtime behavior

- `attach.cpp` builds `Enable-Split.exe`. It locates `LEGOStarWarsSaga.exe`, checks the supported executable's size and SHA-256, then loads the accompanying mod DLL into that running game using Windows process APIs (`OpenProcess`, `VirtualAllocEx`, `WriteProcessMemory`, `CreateRemoteThread` and `LoadLibraryW`) and calls its `EnableSplit` export. `--launch` first starts the game through `steam://rungameid/32440` when needed and waits for its renderer. This loader is why the package contains an EXE.
- `split.cpp` builds `TCS-Split-Prototype.dll`. It validates expected game instructions and menu entries before applying session memory hooks for two world views, independent third-person cameras, input, visibility, HUD and video settings. It writes diagnostics to `%TEMP%\TCS-Split-Prototype.log`.
- `build.ps1` contains the complete compiler commands; it builds both files from these two C++ sources with static runtime linkage.
- `Launch Ultimate Split-Screen.bat` runs the EXE with `--launch`. Copy it beside both rebuilt binaries to use it. Running the mod requires the supported installed Steam game, app 32440, build 1017673. The required game EXE hash is in `BUILD.txt`. Game files are not distributed here.
- `LLVM.txt`, `COPYING.MinGW-w64-runtime.txt` and `COPYING.winpthreads.txt` preserve the bundled runtime notices from the release.

The mod targets Windows. Playtesting covered the first level and Cantina interactions; full-game stability and Proton/Steam Deck compatibility remain unverified.

## Credits

Alex directed the project and performed hands-on co-op playtesting. OpenAI Codex generated the implementation and release text under human direction. Thanks to the [OpenSagaDev saga-pc contributors](https://github.com/opensagadev/saga-pc) for the GOG PC decompilation used as a reference while tracing the Steam build. Third-party runtime notices retain their original terms.