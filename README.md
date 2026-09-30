# MiniEQ

A minimal, lowest-RAM, standalone system-wide equalizer for Windows —
built for Bluetooth devices (Sony WH-1000XM4, Tribit XSound Go, …) and any
earphone/headphone connected via aux, USB-C or Bluetooth. **One app, one
install, no dependencies.**

Open the app and the current output device's name is shown big at the top —
it auto-selects the system default output, and the list refreshes live when
devices are plugged in or unplugged.

> **Status: working, with known limits.** Built by CI on Windows
> (VS2022/MSVC) on every push to `main`; APO verified loading and processing
> inside `audiodg.exe` on Realtek speakers and a USB-C earphone (heartbeat
> advancing, stream locked 2ch @ 48/96 kHz). Honest limits: IEEE float32 only
> (what the engine feeds SFX APOs in shared mode — other formats are declined
> at negotiation), SFX placement (RAW streams bypass SFX APOs), and Bluetooth
> endpoint re-enumeration across reconnects is a known weak point (roadmap).
> No driver, no service, no audio-service restarts — the app never touches
> the Windows Audio service.

## How it works

Windows lets a program register an **APO (Audio Processing Object)**: a small
DSP DLL that the audio engine (`audiodg.exe`) loads for a chosen output
device. MiniEQ is exactly two pieces:

```
┌─────────────┐   sliders/presets    ┌──────────────────────┐
│  MiniEQ.exe │ ──────────────────► │  named shared memory  │
│  (tiny UI,  │   per-device state   │  "Local\MiniEQ_<dev>" │
│   ~MBs RAM) │                      └──────────┬───────────┘
└─────────────┘                                 │ lock-free read
                                                ▼
                                     ┌──────────────────────┐
                                     │ MiniEQ_APO.dll (SFX) │
                                     │ 5× biquad peaking EQ │
                                     │ loaded by audiodg.exe│
                                     │ for the BT endpoint  │
                                     └──────────────────────┘
```

- The **DSP runs inside the already-running audio engine**: effectively zero
  extra RAM and no added block latency (minimum-phase IIR, `GetLatency() = 0`;
  the per-buffer processing itself still costs CPU), and it processes audio
  *before* Bluetooth transmission — so it works with BT devices without any
  vendor protocol, wherever Windows feeds the SFX APO float32 in shared mode.
- The **UI only runs when you open it**. Sliders write to shared memory; the
  APO's real-time thread picks up changes via a lock-free sequence counter
  (no locks, no syscalls, no COM on the audio thread).
- Each device gets its own channel, so **per-device EQ memory is automatic**:
  your XM4 keeps its curve, the Tribit keeps its own.

## Repo layout

```
apo/                  MiniEQ_APO.dll -- the system effect
  eq_apo.h/.cpp       CEqApo: IAudioProcessingObject + RT + Configuration
  dsp.h/.cpp          5/10-band biquad peaking EQ (RBJ cookbook), TDF-II
  dllmain.cpp         DllMain, class factory, DllRegisterServer
  registration.h/.cpp one-time HKLM registration + per-device attach/detach
  guids.h             our CLSID + property-store context GUID
  eqapo.def           DLL exports
app/                  MiniEQ.exe -- native Win32 UI, no frameworks
  main.cpp            window: device picker, sliders, presets, attach button
  audio_devices.*     MMDevice endpoint enumeration
  settings_link.*     shared-memory writer + %APPDATA% per-device persistence
shared/
  settings_channel.*  EqSettings struct + mapping-name derivation (both sides)
```

## Build

Requirements: **Windows 10/11, Visual Studio 2022 (MSVC), Windows SDK, CMake**.
No WDK, no ATL, no third-party libraries. MIT licensed throughout.

```powershell
cd minieq
cmake -S . -B build -A x64
cmake --build build --config Release
```

This produces `build\apo\Release\MiniEQ_APO.dll` and `build\app\Release\MiniEQ.exe`.

## Install & test

**Easy path:** every push to `main` builds an MSI on GitHub Actions — download
`MiniEQ-0.1.0-x64.msi` from the latest
[build run's artifacts](https://github.com/vkhawse2/minieq/actions/workflows/build.yml)
(requires GitHub login), run it, and skip straight to step 3. The installer
puts both files in `C:\Program Files\MiniEQ\` and registers the APO DLL.

Manual path:

1. Copy both files somewhere permanent, e.g. `C:\Program Files\MiniEQ\`.
   (The registered DLL path must not move afterwards.)
2. **Elevated** command prompt in that folder:
   ```
   regsvr32 MiniEQ_APO.dll
   ```
   This writes the COM class + APO declaration to HKLM.
3. Run `MiniEQ.exe` — your current output device (aux / USB-C / Bluetooth)
   is already selected with its name shown at the top. Click **Attach to
   this device** (approves one admin prompt). This writes the SFX slot in
   that endpoint's `FxProperties`.
4. Restart audio playback (the engine loads the APO when the stream starts).
   Move sliders — EQ applies live.

To remove: detach from the UI, then elevated `regsvr32 /u MiniEQ_APO.dll`.

## Design notes

- **Bands:** settings toggle for 5 bands (60 / 230 / 910 / 3.6k / 14k Hz) or
  10 bands (31 Hz – 16 kHz), Q = 1.0, ±12 dB, plus master trim ±12 dB and a
  bypass switch. Change in `shared/settings_channel.h` + `apo/dsp.cpp`.
- **Virtualization (optional, off by default):** bs2b-style Bauer crossfeed
  for headphone listening (default 700 Hz / 4.5 dB setting, derived per the
  bs2b theory). It runs before the EQ bands and only on stereo streams.
  Implemented with lazy allocation — the APO keeps zero extra state and
  skips the stage entirely until the toggle is turned on, so an untouched
  toggle costs no RAM and no CPU.
- **Format support:** IEEE float32 only (what the engine feeds SFX APOs in
  shared mode). Other formats are rejected at negotiation time.
- **Real-time safety:** `APOProcess` never blocks, allocates, touches COM, or
  performs I/O; coefficient recomputation on settings change is pure
  arithmetic. UI→APO settings use a seqlock protocol (odd/even 64-bit
  counter, `Interlocked*` atomics) with bounded reader retries.
- **Persistence:** per-device curves live in `%APPDATA%\MiniEQ\devices.ini`;
  the APO itself stays flat until the UI pushes settings.

## Roadmap

- [x] Windows compile via CI (VS2022/MSVC) + smoke test on real endpoints
- [x] WiX MSI installer (per-push artifacts from CI)
- [ ] Verify attach/detach across device reconnects (BT endpoints can re-enumerate)
- [ ] Consider auto-attach for newly seen devices (opt-in)
- [ ] UI polish pass (custom-drawn sliders, dark mode) — structure is ready

## License

MIT — see [LICENSE](LICENSE). Built on Microsoft's documented APO interfaces
(the SwapAPO sample pattern); all DSP and UI code here is original.
