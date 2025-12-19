# Live_MultiConv

A JUCE-based VST3 plugin for **multichannel (1–16) “record-as-IR” convolution**.

You record a short snippet of incoming audio into an internal impulse-response buffer, then the plugin immediately switches to convolution using that recorded buffer. The IR can be shaped with fade-in/out while listening.

## Features

- **Discrete multichannel support (1–16 channels)**
  - Channel *N* convolves only channel *N*.
  - If the IR buffer is empty, the plugin passes audio through.
- **Record-as-IR workflow**
  - One-shot record captures the current input into the IR buffer.
  - Recording starts immediately when you click **Record**.
- **Live IR shaping**
  - Fade in/out is applied to the IR (the original recorded IR is preserved).
  - Rebuild happens off the audio thread and the plugin crossfades between old/new IR to reduce artifacts.
- **Mix + Trim**
  - Wet/dry mix (0 = dry, 1 = wet)
  - Output trim in dB
- **State recall**
  - Plugin parameters and the recorded IR buffer are saved/restored with the DAW project.

## Controls

- **Record**: captures input into the IR buffer using the current **Record Length**.
- **Clear Buffer**: clears the recorded IR and returns to pass-through.
- **Record Length (ms)**: length of the IR recording (100–2000 ms).
- **Fade In (%)**: percentage of IR length used for the fade-in shape (0–50%).
- **Fade Out (%)**: percentage of IR length used for the fade-out shape (0–50%).
- **Wet / Dry**: mix between dry input and convolved output.
- **Output Trim (dB)**: output gain after mixing.
- **IR Channels** (label): how many channels are currently stored in the IR buffer.

## Typical REAPER workflow

1. Insert **Live_MultiConv** on a track.
2. Route/arm the track so audio is flowing through the plugin.
3. Click **Record** to capture an IR.
4. Set **Wet / Dry** to taste.
5. Adjust **Fade In/Out** while listening.
6. Click **Clear Buffer** to return to pass-through and re-record.

Notes:
- Channel count is derived from the track/plugin I/O layout at the time of recording.
- If you change the plugin channel layout in the host, the processor resets convolution state to stay safe.

## Building (Windows / Visual Studio 2022)

This repo is generated from **Projucer** and is intended to be built using the normal JUCE + Visual Studio workflow.

### 1) Export the Visual Studio project (Projucer)

1. Open `NewProject.jucer` in **Projucer**.
2. Select the **Visual Studio 2022** exporter.
3. Click **Save Project** (this regenerates `Builds/VisualStudio2022/`).

### 2) Build in Visual Studio (recommended)

1. Open the generated solution if present (typical JUCE export):
  - `Builds/VisualStudio2022/Live_MultiConv.sln`
2. If a `.sln` is not present in your export, open the VST3 project directly:
  - `Builds/VisualStudio2022/Live_MultiConv_VST3.vcxproj`
3. In Visual Studio, set:
  - Configuration: `Release`
  - Platform: `x64`
4. Use **Build → Build Solution** (or **Rebuild Solution**).

Build output (bundle):
- `Builds/VisualStudio2022/x64/Release/VST3/Live_MultiConv.vst3`

### 3) Build from the command line (MSBuild)

First, locate MSBuild using `vswhere`:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\amd64\MSBuild.exe"
```

Then rebuild the VST3 project (Visual Studio/JUCE project references should build dependencies automatically):

```powershell
$msbuild = "C:\Path\To\MSBuild.exe"  # amd64\MSBuild.exe
& $msbuild Builds\VisualStudio2022\Live_MultiConv_VST3.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m
```

### One-command helper script

There is a helper script that finds MSBuild via `vswhere`, builds, and installs the VST3:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_install.ps1
```

It writes a log to:
- `_build_install_log.txt`

## Installing the VST3

Common install locations:

- Per-user (this repo’s default helper script target):
  - `%LOCALAPPDATA%\Programs\Common\VST3\Live_MultiConv.vst3`
- System-wide:
  - `C:\Program Files\Common Files\VST3\Live_MultiConv.vst3`

After copying the bundle, rescan plugins in your DAW.

## Troubleshooting

- **No effect / sounds like pass-through**
  - Ensure an IR is recorded (IR Channels label should be non-empty).
  - Ensure **Wet / Dry** is not at 0.
- **Changing fade parameters causes audible artifacts**
  - This is expected because the IR is being changed; the plugin debounces rebuilds and crossfades to reduce noise, but some change is inherent.
- **Changes don’t appear after rebuilding**
  - In Visual Studio, prefer **Rebuild Solution**.
  - If you’re building from the command line, rebuild the VST3 project.
  - If you still don’t see changes, rebuild `Builds/VisualStudio2022/Live_MultiConv_SharedCode.vcxproj` first, then rebuild `Builds/VisualStudio2022/Live_MultiConv_VST3.vcxproj`.

## Project structure

- DSP + state/parameters: `Source/PluginProcessor.*`
- UI: `Source/PluginEditor.*`
- Projucer project: `NewProject.jucer`
- VS2022 exporter projects: `Builds/VisualStudio2022/*.vcxproj`

## License

Internal / unspecified. Add license terms here if you plan to distribute.
