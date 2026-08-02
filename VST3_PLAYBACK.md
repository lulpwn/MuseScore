# VST3 piano playback

This MuseScore 3 fork can host one external VST3 instrument and route selected score parts to it.
The implementation is currently enabled on Windows and uses the official MIT-licensed Steinberg
VST3 SDK.

## Run the packaged build

Launch `bin\\MuseScore3Evo.exe` from the source checkout's `msvc.install_x64` folder. Keep the
executable in that installed folder: the adjacent Qt DLLs and the resource directories are required
at runtime. Do not run an executable directly from a CMake build directory.

## Build

Configure the normal MuseScore Windows build with `VST3_HOST=ON` (the Windows default). CMake
downloads the pinned VST3 SDK revision during its first configure. Use a 64-bit MuseScore build for
64-bit VST3 plug-ins.

To explicitly disable the feature, configure with `-DVST3_HOST=OFF`.

## Use

1. Open **View > Synthesizer** and select the **VST3** tab.
2. Select an instrument from **Available plug-ins**. The list is cached between sessions; use
   **Rescan** only after installing or removing plug-ins. Rescan checks the standard Windows VST3
   locations, including `C:\\Program Files\\Common Files\\VST3`. Bundles that contain effects but
   no instrument class are excluded.
3. Leave **Use this instrument for piano playback** checked and click **Load selected**. MuseScore
   automatically changes piano channels from FluidSynth to the loaded VST3 patch. The same patch
   remains available for manual selection in the Mixer.
4. Use **Open editor** in the VST3 tab to choose the piano/preset and adjust the plug-in.
   The editor is an independent, non-modal window, so it can be minimized or placed behind
   MuseScore like any other application window.
5. Save the synthesizer settings to the score or store them as the application defaults.

The **Playback performance** section controls PortAudio's buffer size. **512 samples** is the
recommended balance. Use **1024 samples** if playback still misses or stutters; use **256** only
when lower audition latency matters. Restart MuseScore after changing this setting. The audio log
reports the selected buffer, measured PortAudio CPU load, and output-underflow count.

Loaded VST3 instruments remain active while their editor is open, allowing their own on-screen
keyboards and audition controls to produce sound. With playback stopped and the editor closed,
the host suspends a silent instrument after two seconds to save CPU. Unloading the instrument
restores piano channels to FluidSynth.

The plug-in path, class ID, component state, and controller state are persisted in MuseScore's
synthesizer state. MIDI notes, note-off events, sustain and other CCs, channel pressure,
polyphonic pressure, and pitch bend are forwarded to the instrument. The first VST3 output bus is
mixed into MuseScore's stereo master bus.

### Pedal reset gap

The advanced preference `io/midi/pedalEventsMinTicks` controls the real repedal gap between connected
or overlapping score pedal lines. CC64-off remains exactly on their end/start boundary; the following
CC64-on is delayed by the configured number of ticks. A first or isolated pedal line still engages on
its notated start. The default gap is one tick. Both `240` and the legacy-style `-240` are interpreted
as a 240-tick gap. At 120 BPM, 24 ticks is about 25 ms, 48 ticks is about 50 ms, and 240 ticks is about
250 ms. The preference is read at application startup. When playback starts or seeks, MuseScore
chases the final CC64 state at or before that tick. This activates a pedal line beginning exactly on
the selected bar as well as one that was already active, without having to approach it from an
earlier bar. The state is reasserted after the first VST3 processing block so plug-in startup resets
cannot overwrite it.

## Current scope

- VST3 instruments only; legacy VST2 DLLs are intentionally unsupported.
- One hosted instrument instance, designed for the Piano part. Other parts can continue using
  FluidSynth or Zerberus.
- In-process hosting means the plug-in architecture must match MuseScore (normally x64), and a
  crashing plug-in can also terminate MuseScore.
- The plug-in receives MuseScore's actual play/stop state and score tempo in its VST3 process
  context. The time signature is currently reported as 4/4.
- Multiple VST3 instances are a possible future extension.
