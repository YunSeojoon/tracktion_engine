# AI music editor fork

Fork: https://github.com/YunSeojoon/tracktion_engine

Upstream: https://github.com/Tracktion/tracktion_engine

Development branch: `ai-editor`.

## Current starting point

The Tracktion DemoRunner is the first executable. It includes MIDI,
playback and plugin-hosting examples, with VST3 hosting enabled. AI editing is
not implemented yet. Its MSVC build explicitly uses UTF-8 source decoding and
C++ exception unwinding so the Release build works on non-English Windows.
The recording error fallback is compiled only when JUCE's exception catcher is
enabled, avoiding MSVC's unreachable-code error with the default configuration.

## Build on Windows

Requires Visual Studio 2022 with Desktop development with C++, a Windows SDK,
CMake, and the JUCE submodule at the commit recorded by this repository.

Run in PowerShell from the repository root:

```powershell
git submodule update --init --recursive
cmake -S examples/DemoRunner -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target DemoRunner --parallel 4
& ./build/DemoRunner_artefacts/Release/DemoRunner.exe
```

The local checkout initially uses shallow history. Use `git fetch --unshallow`
if older history becomes necessary. `origin` points to the fork; `upstream`
points to Tracktion.

Verified on Windows with MSVC 19.44.35223: Release build succeeded and
DemoRunner initialised a responsive window. Actual audio output, third-party
VST3 loading, rendering and state restoration have not been tested yet.

## First product milestone

Load one installed VST3 instrument, create eight MIDI bars, render a WAV,
change one parameter, render again, compare the audio, and restore the original
state. Verify save/reload before extending the editor UI or adding an agent.

Keep upstream license notices intact. Tracktion Engine and JUCE have separate
licenses; see the upstream README and the JUCE license files.
