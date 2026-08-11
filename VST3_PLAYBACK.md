# VST3 instrument playback

This MuseScore 3 fork hosts 64-bit VST3 instruments on Windows. The host follows
the MuseScore 4.0 architecture while adapting it to MuseScore 3's synthesizer,
MIDI channel, Mixer, and state systems.

## Build

VST3 support is enabled by default for Windows builds. The build pins Steinberg
VST3 SDK 3.7.7 (`358b72ee61bc67fb4592b0d492e0c6a1211ebf11`) and fetches it when
`VST3_SDK_PATH` is not supplied. To use an existing checkout, either set the
`VST3_SDK_PATH` environment variable or pass the matching CMake cache option.

For the Visual Studio advanced-build workflow:

1. Close Visual Studio before reconfiguring.
2. Run `msvc_build.bat clean` when changing build options or SDK paths.
3. Run `msvc_build.bat relwithdebinfo` (or `release`).
4. Run `msvc_build.bat installrelwithdebinfo` (or `install`).

The installed executable is `msvc.install_x64/bin/MuseScore3Evo.exe`.

## Use

All VST3 controls are on **View > Synthesizer > VST3**. Select an instrument in
the list and choose **Load selected** to assign it to the score's piano playback
channels (or the first pitched part when there is no piano). Choose
**Open editor...** to open its native VST3 interface. **Use FluidSynth** returns
those channels to the default FluidSynth piano.

MuseScore scans the system VST3 locations defined by the SDK at startup. Use
**Add folder...**, **Remove folder**, and **Rescan** on the same VST3 page to
manage additional locations.

The host supplies note, controller, pressure, pitch-bend, transport, and tempo
data. Component and controller states are saved in the synthesizer state so a
score can restore the selected instance's settings.

Only VST3 instrument classes are listed; audio effects are deliberately excluded.
