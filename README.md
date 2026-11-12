# Freeloader for RE_Kenshi

Freeloader predicts the camera's next region and requests it through Kenshi's background loader. It uses [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) and [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib).

## Build

Use the Visual C++ 2010 x64 compiler, Windows SDK 7.1, and the [KenshiLib example dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps). The build script expects the shared Kenshi project directory to contain the extracted toolchain and the KenshiLib dependencies.

```powershell
.\tools\build-plugin.ps1 -KenshiProjects 'C:\path\to\KenshiProjects' -KenshiRoot 'C:\path\to\Kenshi'
```

The build verifies the Steam Kenshi 1.0.65 executable in the RE_Kenshi directory and stages the x64 DLL, its symbols, the loader manifest, and default settings under build/releases. Place the DLL, manifest, and INI beside the matching Freeloader mod data and enable Freeloader in the Kenshi launcher. The target runtime uses RE_Kenshi 0.3.5 and KenshiLib 0.5.0.

## License

Freeloader source is licensed under [GNU GPL version 3 only](LICENSE). External dependencies retain their respective licenses.
