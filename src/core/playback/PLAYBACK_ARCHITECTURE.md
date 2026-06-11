# Playback / transport architecture — who owns time

Seven classes cooperate to run realtime playback. This is the map of which
thread each one lives on and who drives whom. Every historical A/V-sync bug
lived somewhere in this picture — read this before touching any of them.

```
 main/UI thread          TIME_CRITICAL thread        producer thread
 ──────────────          ───────────────────         ───────────────
 PlaybackController ───► FrameClock ────────────────► FrameProducer
 (transport state         (accumulator-timed          (calls the compositor
  machine: play/pause/     frame ticks; zero-CPU       callback = Composite-
  JKL shuttle/loop/        when paused)                Service::compositeFrame;
  edit-point nav)                                      latest-wins scrub slot;
        │                                              last-good-frame hold)
        │                                                      │ exchange slot
        ▼                                                      ▼
 AudioEngine ──────────► AVSyncClock ◄───────────────  FramePresenter
 (PortAudio/WASAPI        (MASTER CLOCK during          (high-priority display
  callback thread)         playback: audio callback      thread; deadline-driven
                           advance()s it; video reads    during playback, event-
                           it lock-free to pick the      driven while paused;
                           frame to display)             presents via Program-
                                                         Monitor → VulkanViewport)
```

- **PlaybackScheduler** is the facade that owns FrameClock + FrameProducer +
  FramePresenter and wires them together; the UI talks to it (and to
  PlaybackController), never to the three threads directly.
- **FrameScheduler** (a `MediaPool` member, `MediaPool::scheduler()`) is a
  separate concern: it schedules *decode* work (pull-based, bounded
  lookahead, urgent-first, cancel-on-seek) for the prefetch workers. It
  feeds the FrameCache that the compositor reads; it does not touch
  presentation timing. The composite path keeps it aimed via
  `setPlayhead(tick)` (CompositeServiceFrame.cpp).

Rules that must survive any refactor:

1. **Audio is the master clock.** Video never sleeps on its own idea of
   time during playback; it reads AVSyncClock. When there is no audio,
   AVSyncClock free-runs on the system clock.
2. **The clock thread never blocks.** FrameClock fires ticks and returns;
   compositing happens on FrameProducer's thread, presentation on
   FramePresenter's. Anything added to the tick callback must be O(µs).
3. **Scrub requests coalesce.** FrameProducer keeps ONE pending scrub
   request (latest wins). Never queue scrubs.
4. **Presentation never starves.** FramePresenter re-presents the last good
   frame on a miss; FrameProducer republishes its last good frame on cache
   miss. Removing either fallback brings back blank-frame flicker.
5. **Export does not use this machinery.** Export pulls frames synchronously
   through the same CompositeService::compositeFrame with forceExact +
   cache bypass (see export/RenderQueue.cpp); the clocks and threads above
   are preview-only.
