# macOS worker regression test

The test includes the production macOS backend and substitutes AudioQueue
creation, allocation, enqueue, start and disposal. It plays no sound and does
not modify OBS settings. Requires macOS, Xcode, configured plugin dependencies,
and OBS installed at `/Applications/OBS.app`.

From the repository root:

```sh
clang -std=c17 -g -fsanitize=address,undefined \
  -I .deps/Frameworks/libobs.framework/Headers -I .deps/include/obs \
  -F /Applications/OBS.app/Contents/Frameworks tests/macos-worker-test.c \
  -framework libobs -framework AudioToolbox -framework CoreFoundation \
  -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build_macos/macos-worker-test
build_macos/macos-worker-test
```

Replace `-fsanitize=address,undefined` with `-fsanitize=thread` for a separate
ThreadSanitizer run (do not combine AddressSanitizer and ThreadSanitizer).

Coverage: interleaving, gain/mono, bounded overflow, ring wraparound, silence
padding, two workers, 20 stop/start cycles during delayed starts, synchronous
callback reentry, disposal callbacks, three-attempt failure cap, explicit
recovery, queue cleanup, and no intercepted AudioQueue calls on the producer.

These simulated tests do not establish real-device stability. Before publishing
a release or PR, test two physical outputs, repeated source disable/enable,
scene switching, device unplug/replug, mute/volume changes and OBS shutdown.

The worker keeps the queue running with silence during source underruns.
Explicit start/stop requests are asynchronous. After three consecutive queue
failures in one generation it stops retrying until a new start request. Monitor
destruction joins the worker, so a stalled OS disposal can still delay teardown,
although normal capture no longer invokes AudioQueue APIs.

v5 also tests 10 virtual seconds of 1024-frame input packets against 1440-frame
output buffers, including 10 ms arrival jitter. The old v4 fill policy inserts
1888 silent frames in this schedule; v5 has no underruns after pre-roll. Tests
verify 5 ms fade-in, fade-to-zero at underruns (including near-boundary cases),
and playback of short notifications. The reserve target is 90 ms, with a wait
cap of six output buffers for sounds shorter than the reserve. This increases
monitoring latency; it does not change the original source audio returned to OBS.

Rebuild attribution: maurojuniorr, https://github.com/maurojuniorr.
Original project authorship and license remain unchanged.
