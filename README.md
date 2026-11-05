# WASAPI Streamer

A lightweight, 64-bit VST3 plugin that routes audio from a low-latency ASIO DAW directly into Windows' audio subsystem, allowing Discord screen share, OBS, and other desktop apps to capture your DAW cleanly in stereo without extra drivers.

---

## Development Transparency

This plugin was developed with heavy assistance from artificial intelligence (large language models) for architectural design, C++ code generation, and debugging. 

While the source code has been reviewed, tested, and built strictly around standard real-time audio practices (lock-free buffers, isolated worker threads, and memory-safe resamplers), it is open-source software provided as-is. But feel free to let me know of any bugs you might encounter!

---

## Alternatives

If you prefer not to use software developed with AI assistance, or prefer a virtual-cable-based routing workflow, you can consider checking out **DawAudioStreamer**:
* https://yoruhinot.github.io/DawAudioStreamer/en/

While I've not personally used that plugin, it seems to accomplish similar goals to this one with some extra steps, like installing a Virtual Audio Cable. My own plugin instead just requires an unused hardware device/playback channel in Windows to route to. I know AI is a controversial topic so no fault to anyone not wanting to support my plugin for that reason! Just thought it might be helpful.

---

## Why This Exists

When making music on Windows, low-latency audio interfaces rely on ASIO drivers. ASIO achieves low latency by deliberately bypassing Windows' internal audio engine (WASAPI/MME) to talk directly to your audio interface hardware.

The downside is that Windows has no idea your DAW is generating sound. When you use Discord's application screen share, Discord listens to the Windows audio session attached to your DAW's process. Because ASIO bypasses that session, Discord streams silence.

Historically, the only workarounds were:
* Installing complex third-party kernel drivers (like Voicemeeter or VB-Cable) that often break after Windows updates or introduce sample rate mismatches.
* Hardware loopback cables that tie up physical inputs and outputs on your interface.
* Routing your DAW through your microphone channel, which forces your audio into mono and makes everyone in your voice call hear your playback continuously.
* Using *Voxengo Recorder*, a 32-bit plugin from 2005 bridged through jBridge.

`WASAPI Streamer` was built to replace all of those workarounds with a single, self-contained VST3 that requires zero driver installations, zero administrative privileges, and zero background services.

---

## How It Works

Discord's screen share does not capture what comes out of your physical speakers; it captures any active WASAPI audio stream associated with the application's Process ID.

`WASAPI Streamer` sits as a utility insert on your Master track (or any track you choose) and spins up an isolated background worker thread using native Windows Core Audio APIs. It feeds a digital duplicate of your DAW's audio into any Windows Shared Mode output device you select.

Because the stream originates from inside your DAW's process, Windows tags the audio with that Process ID, and Discord captures it in digital stereo.

By sending this audio to an unused hardware output—such as the built-in speakers on an HDMI/DisplayPort monitor, a motherboard headphone jack, or a secondary output you keep muted—you can stream full-quality stereo audio to Discord without hearing a delayed echo on your end.

---

## Features

* **Driverless Setup:** Runs purely in user space. No kernel-level virtual audio cables, no driver signature enforcement issues, and no system restarts required.
* **Real-Time Decoupled Architecture:** Built around a lock-free Single-Producer Single-Consumer (SPSC) ring buffer. Your DAW's audio thread never waits on Windows audio engine calls, preventing dropouts or crackles in your live project.
* **Adjustable Latency Buffer (10 ms to 3,000 ms):** Allows you to dial in the exact buffer size needed for your hardware. If your secondary output has a stable clock, you can run as low as 10 to 30 ms so audio stays tightly in sync with your screen share video.
* **Dedicated Output Volume Control:** Features an independent volume trim (-60 dB to +6 dB) so you can adjust the loudness of the stream for Discord viewers without touching your DAW's master fader or your studio monitors.
* **Automatic Clock Drift Correction:** Subtly adjusts the consumer read rate using linear interpolation to keep the ASIO clock and the Windows device clock aligned over long multi-hour sessions.
* **DAW Lifecycle and Export Aware:** Detects offline renders (`isNonRealtime`). It automatically pauses during exports, bounces, and track freezes so high-speed render audio is never sent over the stream, then automatically resumes when normal playback returns.
* **Format Resilience:** Automatically queries the destination device's mix format and handles both 32-bit float and 16-bit integer PCM formats, preventing crashes on optical or legacy soundcards.
* **Session Memory with Safe Initialization:** Project files remember your selected output device, latency, and volume settings, but the stream always defaults to a stopped state when you reopen a project to avoid unexpected audio routing.

---

## Installation

1. Download the latest `WASAPI Streamer.zip` from the **Releases** tab.
2. Extract the `WASAPI Streamer.vst3` archive.
3. Copy it into your system VST3 directory: Default is
   `C:\Program Files\Common Files\VST3\`
4. Re-open your DAW, or re-scan plugins inside your DAW.

---

## Quick Start Guide

1. Place `WASAPI Streamer` at the very end of your Master channel chain.
2. Open the plugin window and choose a destination device from the dropdown menu.
   * *Best Practice:* Pick a hardware device you are not currently listening to (such as an unused monitor speaker or an empty motherboard jack) to avoid hearing a slapback delay.
3. Click **START STREAM**.
4. In Discord, share your DAW's application window and ensure **Share Audio** is enabled.

---

## Building from Source

* Requires **Visual Studio 2026** with the **Desktop development with C++** workload installed.
* Requires **JUCE 9** (or Projucer).
* Open `WASAPI Streamer.jucer` in Projucer, select the Visual Studio exporter, and compile under the **Release | x64** configuration.

---

## License

This project is open-source software licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** in compliance with the JUCE framework license.
