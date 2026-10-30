# Live tuning: which stream settings change a running session

`frame-stream get`, `set` and `ab` change the runtime's settings while a session runs, so two settings can be
compared on the same scene without stopping it. The OXRSys runtime re-reads its toml every 250 ms, but most of the
keys that shape the picture are read only when something is built: the encoder at each headset connect, the stream
layout (encoded size, foveation) when the app's session begins. So each key has a stage:

| stage | takes effect | how `frame-stream` handles it |
|---|---|---|
| **live** | on the next frames | `set` writes it; `ab` can alternate it |
| **reconnect** | at the next headset-client connect | `set --reconnect` / `ab --reconnect` restart the headset client (about 3.5 s of black) |
| **restart** | only in a new app session | refused while an engine runs: `down`, `set`, `up` |

## The keys

Measured (**M**) on the Mac with the release runtime and the Godot test scene, or read from the runtime's code (**C**).

| key | stage | evidence |
|---|---|---|
| `keyframe_interval_sec` | live | **M** 10 → 1: a keyframe every second within 2 s, and back |
| `send_pacing_ms`, `send_pacing_keyframe_ms` | live | **M** 3 → 0: the send spread fell from 2.8 to 0.3 ms within the window |
| `pacing_spin_ms`, `pacing_lock_wait_ms`, `pacing_lock_on`, `pacing_lock_age_ms` | live | **C** read every frame |
| `stage_height_offset_m` | live | **C** read on every locate |
| `client.predict_ms`, `client.stamp_lead_ms` | live | **C** the headset client's own settings, polled about once a second |
| `bitrate_mbps` | reconnect | **M** 50 → 20: no change until a reconnect, then the encoder's log line and the wire both follow |
| `encoder_min_qp` | reconnect | **M** 12 → 30: no change until a reconnect |
| `encoder_preset` | reconnect | **M** balanced → speed after a reconnect |
| `encoder_low_latency`, `foveation_box_filter`, `video_codec`, `abr_mode` | reconnect | **C** read when the encoder is built |
| `resolution_scale`, `foveated_encoding_preset` | restart | **M**/**C** the stream layout is built when the app's session begins |
| `render_device`, `refresh_rate_hz`, `pose_prediction*`, `rtt_*`, `dynamic_resolution_min_scale`, `pacing_thread`, `passthrough_enabled`, `app_alpha_blend_passthrough` | restart | **C** read at session or instance creation |
| `encoder_full_range` | restart | **C** the headset converts by the range it was started with, so only `down` / `up` keeps the two ends agreeing |
| `client_sharpening`, `client_upscaling`, `client_foveation_preset`, `client_reprojection` | restart | **C** sent once, when the runtime starts announcing itself |

`runtime_status.json` shows a toml change at once, even when nothing has applied it yet. The encoder's own
`VideoEncoder: Initialized …` log line is the truth, and it is what `frame-stream get` quotes.

## The verbs

```
frame-stream get [--all | KEY...]          what is on file, and when each key takes effect
frame-stream set KEY=VALUE... [--reconnect] [--label TEXT]
frame-stream ab "K=V,..." "K=V,..." [--every S] [--rounds N] [--reconnect]
```

- `set` refuses an unknown key (the runtime silently keeps its default for a name it does not know), a value that is
  not a plain word, and a restart key while an engine runs. It writes all the keys in one rename, so the runtime never
  reads half a change, waits for the runtime's `Reloaded config` line, and prints `key: old -> new (stage)`.
- `--reconnect` checks that the headset client is up before writing anything, restarts it, waits for the new
  encoder's line and quotes it.
- `ab` alternates two sets of settings. It refuses restart keys, and reconnect keys without `--reconnect`, which also
  raises `--every` to at least 15 s. Each switch is a line in `~/Library/Logs/frame-stream/ab.jsonl` (`unix`, `flip`,
  `arm`, `set`), so a bench or a log read can be split by arm. When it ends (rounds done, Ctrl-C) the settings from
  before the first switch go back, and a key that was not in the file is removed again.
- Every `set` is also a line in `~/Library/Logs/frame-stream/live.jsonl`.

Not yet: bitrate, the QP floor and the encoder preset are the keys most worth comparing, and each switch of
them costs a client restart. A runtime change that rebuilds the encoder in place on a config reload would make them
live, at the cost of one keyframe.
