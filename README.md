# MiniEQ

A minimal, lowest-RAM, standalone system-wide equalizer for Windows —
built for Bluetooth devices (Sony WH-1000XM4, Tribit XSound Go, …) but works
with any output. **One app, one install, no dependencies.**

> **Status: scaffold.** The architecture, DSP, COM plumbing, registration,
> and UI are all written, but this was authored on a Linux machine and has
> **not been compile-tested**. The first build on a Windows laptop will
> surface any issues — the code follows Microsoft's documented APO APIs and
> is modeled on the official SwapAPO sample. See "Build" below.

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
  extra RAM, zero extra latency (minimum-phase IIR), and it processes audio
  *before* Bluetooth transmission — so it works with every BT device without
  any vendor protocol.
- The **UI only runs when you open it**. Sliders write to shared memory; the
  APO's real-time thread picks up changes via a lock-free sequence counter
  (no locks, no syscalls, no COM on the audio thread).
- Each device gets its own channel, so **per-device EQ memory is automatic**:
  your XM4 keeps its curve, the Tribit keeps its own.

## Repo layout

```
apo/                  MiniEQ_APO.dll -- the system effect
  eq_apo.h/.cpp       CEqApo: IAudioProcessingObject + RT + Configuration
  dsp.h/.cpp          5-band biquad peaking EQ (RBJ cookbook), TDF-II
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

1. Copy both files somewhere permanent, e.g. `C:\Program Files\MiniEQ\`.
   (The registered DLL path must not move afterwards.)
2. **Elevated** command prompt in that folder:
   ```
   regsvr32 MiniEQ_APO.dll
   ```
   This writes the COM class + APO declaration to HKLM.
3. Run `MiniEQ.exe`, pick your Bluetooth device, click **Attach to this
   device** (approves one admin prompt). This writes the SFX slot in that
   endpoint's `FxProperties`.
4. Restart audio playback (the engine loads the APO when the stream starts).
   Move sliders — EQ applies live.

To remove: detach from the UI, then elevated `regsvr32 /u MiniEQ_APO.dll`.

## Design notes

- **Bands:** 60 / 230 / 910 / 3600 / 14000 Hz, Q = 1.0, ±12 dB, plus master
  trim ±12 dB and a bypass switch. Change in `shared/settings_channel.h`
  + `apo/dsp.cpp`.
- **Format support:** IEEE float32 only (what the engine feeds SFX APOs in
  shared mode). Other formats are rejected at negotiation time.
- **Real-time safety:** `APOProcess` never blocks, allocates, or touches COM;
  coefficient recomputation on settings change is pure arithmetic.
- **Persistence:** per-device curves live in `%APPDATA%\MiniEQ\devices.ini`;
  the APO itself stays flat until the UI pushes settings.

## Roadmap

- [ ] First Windows compile + smoke test on a real BT endpoint
- [ ] Verify attach/detach across device reconnects (BT endpoints can re-enumerate)
- [ ] Consider auto-attach for newly seen devices (opt-in)
- [ ] UI polish pass (custom-drawn sliders, dark mode) — structure is ready
- [ ] Simple installer (one elevated setup instead of manual `regsvr32`)
- [ ] 10-band option

## License

MIT — see [LICENSE](LICENSE). Built on Microsoft's documented APO interfaces
(the SwapAPO sample pattern); all DSP and UI code here is original.
