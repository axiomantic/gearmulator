# Nord Modular G2 Editor & Web Architecture: Understanding Document

**Swarm Workspace:** `gearmulator-rg2`  
**Stage:** Phase 3 Dialectical Pump (Stage 1 Grounded Research)  
**Scope:** Clavia G2 Hardware/Editor v1.40 vs. Peter van der Noord's Web Editor (`nordmodulareditor.com`) vs. Red Gecko 2 Transport Bridge

---

## 1. Executive Summary & Architectural Overview

The Nord Modular G2 combines a DSP-accelerated calculation engine (Motorola DSP56311) with a desktop GUI editor (Clavia G2-Edit v1.40) over USB. Peter van der Noord (`@petervdn`) created a browser-based reimplementation (nordmodulareditor.com/g2-patch-editor), which allows patching directly in modern web browsers.

However, two major barriers have constrained the web editor's reach:
1. **The WebUSB Monoculture:** The web editor relies exclusively on Chromium-only `navigator.usb`. Safari (macOS and iPadOS) and Mozilla Firefox refuse to implement WebUSB for security/fingerprinting reasons. Consequently, iPad users—the ideal audience for tactile touch patching—have been completely excluded.
2. **DAW & Emulator Isolation:** Virtual software emulators like **Red Gecko 2** (running standalone or as VST3/AU/CLAP plugins in DAWs such as Reaper, Live, or Logic) cannot register virtual USB devices in user space on macOS without restricted Apple DriverKit entitlements.

To solve this:
- **Red Gecko 2 In-Engine Server (`rg2Lib`)**: Port 7777 natively handles both HTTP GET static file serving (serving the Web Editor) and RFC 6455 WebSocket upgrades (for both browser sessions and Wine `setupapi.dll`).
- **In-Bundle Native Transport**: The editor bundle served by Red Gecko 2 (`/Users/eek/Documents/The Usual Suspects/RedGecko2/webeditor/`) injects a `NativeMockUSBDevice` over loopback WebSocket (`ws://127.0.0.1:7777`), enabling instant cross-browser operation without browser extensions.
- **Upstream PR Package (`source/claudia/rg2/rg2WebBridge/upstream_patch/`)**: Clean TypeScript code formally decoupling `G2Transport` into `WebUsbG2Transport` and `WebSocketG2Transport`.

---

## 2. Clavia G2-Edit v1.40 Specification

### 2.1 Hardware Models & Synthesis Engine
- **Hardware Models:** G2 (3-octave, 24 encoders, 4 LCDs), G2X (5-octave, dual DSP cards), G2 Engine (1U 19" rack-mount).
- **DSP Topology:** Base unit 4x Motorola DSP56311 (~100–150 MHz). Expansion adds 4 DSPs (8 DSPs total).
- **Multitimbral Architecture:** 4 independent slots (A, B, C, D). Each slot has a polyphonic Voice Area (Common Voice Area - CVA) and a monophonic FX Area.
- **Signal Rates:** Audio Rate (Red connectors, full sample rate), Control Rate (Blue connectors, modulation rate), Logic Rate (Yellow connectors, binary/pulse).

### 2.2 Clavia G2-Edit Advanced Sound Design Tools
1. **Patch Mutator (Interactive Genetic Evolution):**
   - Genetic breeding of sounds from a "Mother" patch.
   - User sets mutation probability (0–100%) and mutation range.
   - Evolutionary tree of offspring.
   - Mutation Exclusion Masks (protects outputs, pitch trackers, compressors from mutating into deafening noise).
2. **Patch Adjuster (Category Macro Scaling & Randomizer):**
   - Macro offsets across all modules of a specific functional group (e.g. scaling all Attack times, Filter Cutoffs, FM depths simultaneously).
   - Global Randomizer slider with percentage depth.
3. **Variations & Parameter Lock:**
   - 8 Variations per patch (instant preset switching without voice stealing or DSP reload).
   - Parameter Lock: locks parameter value across variation changes.
4. **Morph Groups (8 Sources):**
   - Sources: Mod Wheel, Velocity, Aftertouch, Pitch Bend, Expression Pedal, Sustain Pedal, Assign 1, Assign 2.
   - Visual morph range arcs around rotary knobs in the UI.
5. **Multitimbral Performance Mode (`.prf2`):**
   - Visual graphical keyboard split manager (up to 4 zones with draggable split points).
   - Per-slot MIDI channel, velocity curves, octave transposition, dynamic voice reserve.
   - Global Performance Parameter pages.

---

## 3. Web Editor Capabilities & Gap Analysis

### 3.1 Implemented Modules (168 Unique Modules)
168 modules implemented across 16 categories: Delay (10), Env (9), FX (9), Filter (14), In/Out (11), LFO (5), Level (14), Logic (10), MIDI (7), Mixer (16), Note (8), Osc (19), Random (6), Seq (5), Shaper (7), Switch (16), Test (2).

### 3.2 Missing & Unmapped Modules (23 Modules)
Top priority missing modules:
1. `SwS&H` (Sample & Hold) & `SwT&H` (Track & Hold)
2. `FilterShelvEQ` (2-band Shelving Equalizer)
3. `IOOutBusA` & `IOOutBusB` (Common Voice Area sub-buses)
4. `OscPulse` (Dedicated pulse width oscillator) & `OscSync` (Hard-sync slave oscillator)
5. `EnvDX` (4-rate 4-level DX7-style envelope)
6. `SeqA` (Step and control sequencer A)
7. `AudioIn` & `BusIn` (Physical audio and inter-slot bus inputs)

### 3.3 Functional Gaps
- Genetic Patch Mutator: Missing in Web Editor.
- Patch Adjuster / Macro Scaler: Missing in Web Editor.
- Parameter Lock: Missing in Web Editor UI.
- Morph Knob Arcs: Missing in Web Editor UI.
- Performance Mode (.prf2) & Keyboard Split Editor: Missing in Web Editor.

---

## 4. Work Tracks for Swarm Execution

- **Track 1 (`@implementer` / `agy-worker-4`)**: Native WebSocket Transport & Multi-Instance Auto-Port Upstream PR.
- **Track 2 (`@dsp-modules` / `agy-worker-7`)**: Missing Core G2 Modules (`SwS&H`, `FilterShelvEQ`, `IOOutBusA`, `OscPulse`, `OscSync`).
- **Track 3 (`@mutator-lead` / `agy-worker-8`)**: Genetic Patch Mutator & Macro Adjuster / Randomizer Port.
- **Track 4 (`@perf-architect` / `agy-worker-9`)**: Multitimbral Performance Mode (`.prf2`) & 88-Key Split Editor.
- **Track Wine (`@wine-verifier` / `agy-worker-10`)**: Clavia G2-Edit v1.40 Windows Editor Loopback Verification & DAW Multi-Track Testing.
- **Audit & Gatekeeping (`@auditor` / `agy-worker-5`)**: Zero-Mirage Verification & Two-Key Gatekeeper.
- **Upstream Packaging & CI (`@devex` / `agy-worker-6`)**: Upstream PR Packaging & CI Automation.
- **System Architecture (`@architect` / `agy-worker-3`)**: Master Architecture & Dialectical Synthesis.
