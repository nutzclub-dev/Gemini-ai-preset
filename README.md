# ProjectM SDL3 AI Visualizer (Visual Studio 2022)

## Quick Start
1. Run `setup_project.ps1` in PowerShell to download `miniaudio.h` and the SDL3 VC package.
2. Install projectM and curl via vcpkg:
   ```cmd
   vcpkg install curl:x64-windows projectm:x64-windows
   ```
3. In Visual Studio 2022:
   - **File -> Open -> Folder...** -> select this folder.
   - Build target: `x64-Debug` or `x64-Release`.
4. Put your `song.mp3` in the output build folder alongside the executable.
5. Put your Gemini API Key in `api.key` (Line 1).
6. Press F5 to run!

## Controls
- **F1**: Save current preset to `./save/preset_XXXX.milk`
- **F2**: Asynchronously query AI to mutate the preset with music reactivity
- **ESC**: Exit






STEP 1 
Dump a valide song.mp3 exe folder

STEP 2
edit add valid api.key   
GEMINI_API_KEY   (create in google ai studio)
GROK_API_KEU


STEP 3   edit optimize ai prompt what you want ai do to this preset (max lent 500-1000 charactere)
prompt.txt

