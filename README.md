# Audio Monitor — maurojuniorr Rebuild

Unofficial macOS-focused rebuild of [Exeldro's Audio Monitor plugin](https://github.com/exeldro/obs-audio-monitor) for OBS Studio.

This fork keeps the original plugin features and adds stability work for setups that monitor one source on multiple audio devices. The current Rebuild v5 moves CoreAudio `AudioQueue` operations away from OBS's audio callback, adds bounded buffering with a 90 ms pre-roll, uses short fades when playback starts or runs out of data, and improves output lifecycle synchronization.

![Screenshot](media/screenshot.png)

## Download

[Download the latest universal macOS build](https://github.com/maurojuniorr/obs-audio-monitor/releases/latest/download/audio-monitor-maurojuniorr-v5-universal.zip)

The binary contains both Apple Silicon (`arm64`) and Intel (`x86_64`) architectures.

## Install on macOS

1. Close OBS Studio.
2. Extract the downloaded ZIP.
3. Copy `audio-monitor.plugin` to `~/Library/Application Support/obs-studio/plugins/`.
4. Reopen OBS Studio.
5. Add **Audio Monitor** as a filter on the desired audio source and select the output device.

Back up or remove an older `audio-monitor.plugin` before copying this build. OBS should show **maurojuniorr Rebuild v5** in the filter information and startup log.

## Main Rebuild v5 changes

- dedicated worker thread owns macOS `AudioQueue` creation, start, retry and disposal;
- OBS's producer callback only copies samples into a bounded ring buffer;
- multiple outputs and repeated source disable/enable no longer invoke blocking CoreAudio operations on the capture thread;
- 90 ms pre-roll compensates for mismatched OBS and device buffer cadence;
- 5 ms fade-in and fade-to-zero reduce clicks on resume and underrun;
- bounded retry behavior for `AudioQueueStart` failures, including `-66681`;
- safer output-device lifecycle and UI/audio synchronization;
- complete Brazilian Portuguese labels;
- universal Apple Silicon and Intel build.

The simulated macOS regression test covers two outputs, 20 reactivation cycles, synchronous callback re-entry, bounded failures, cleanup, overflow, underruns and mismatched buffer cadence. Real-device testing was also performed with two monitoring outputs and repeated source reactivation.

## Source branches

- `rebuild/maurojuniorr-v5`: branded fork build distributed here.
- `fix/macos-audioqueue-stability`: clean technical contribution submitted upstream as [pull request #113](https://github.com/exeldro/obs-audio-monitor/pull/113).

## Build from source

The original OBS plugin build workflow is documented by the [upstream project](https://github.com/exeldro/obs-audio-monitor). This rebuild uses the same project structure and produces a universal macOS plugin bundle.

## Credits and license

Audio Monitor was created and is maintained upstream by [Exeldro](https://github.com/exeldro). This repository is a GPL-2.0 fork and preserves the original license and authorship. Rebuild changes and macOS stability testing by [maurojuniorr](https://github.com/maurojuniorr).

For the official release and upstream support, visit the [original Audio Monitor page](https://obsproject.com/forum/resources/audio-monitor.1186/).
