# Recorded retail runs

Every human run recorded in the retail game with `tools/retail/record_session.py`
(first made for the tesseract port, 2026-09-07 to 2026-10-02), gzipped. Replay one:

```bash
python -m tools.retail.replay --trace recordings/20260930_213855_escape_overlay_session.jsonl.gz
```

`replay.py` with no `--trace` takes the newest. Each file is one JSON record per
line: a `meta` line, then `sample` (one per retail frame: the camera, and the pawn's
own `px/py/pz` capsule centre, `vx/vy/vz`, `move_name`, `physics`), `key` (every key
the game window received) and, from telemetry v6, `sound` records. The `v` column is
the telemetry version: v5 added the controller's own view (`cyaw`/`cpitch`, which
the replay uses), v6 the animation and sound records. The v4 runs have no
controller view; their camera record is the view, and in `20260907_002207` that
record froze at a device Reset, so `trace.stamp_look` reads the view off the pawn's
velocity instead.

| file | map | recorded | play (s) | pawn frames | key presses | v | gz |
|---|---|---|---:|---:|---:|---|---:|
| `20260907_002207_escape_run.jsonl.gz` | escape_p | 2026-09-07 01:22:07 | 963 | 43063 | 57 | v4 | 0.5 MB |
| `20260920_145610_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-20 15:56:11 | 204 | 12256 | 37 | v5 | 0.3 MB |
| `20260920_185901_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-20 19:59:01 | 126 | 7507 | 87 | v5 | 0.4 MB |
| `20260921_212130_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-21 22:21:30 | 95 | 5681 | 72 | v5 | 0.3 MB |
| `20260922_020306_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-22 03:03:07 | 17 | 1029 | 2 | v5 | 0.0 MB |
| `20260923_014206_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-23 02:42:07 | 1799 | 107879 | 80 | v5 | 2.2 MB |
| `20260923_203118_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-23 21:31:18 | 158 | 9503 | 95 | v5 | 0.3 MB |
| `20260925_154619_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-25 16:46:19 | 832 | 50295 | 21 | v6 | 1.1 MB |
| `20260925_160622_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-25 17:06:22 | 60 | 3656 | 54 | v6 | 0.3 MB |
| `20260926_101753_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-26 11:17:53 | 106 | 6347 | 23 | v6 | 0.2 MB |
| `20260926_102131_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-26 11:21:31 | 104 | 6263 | 56 | v6 | 0.3 MB |
| `20260926_164012_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-26 17:40:12 | 80 | 4825 | 34 | v6 | 0.3 MB |
| `20260926_204019_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-26 21:40:19 | 139 | 7568 | 107 | v6 | 0.3 MB |
| `20260929_214813_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-29 22:48:13 | 184 | 11109 | 149 | v6 | 0.8 MB |
| `20260930_213855_escape_overlay_session.jsonl.gz` | escape_p | 2026-09-30 22:38:55 | 161 | 9684 | 86 | v6 | 0.5 MB |
| `20261002_102817_edge_pt1.jsonl.gz` | edge_p | 2026-10-02 11:28:17 | 211 | 11508 | 114 | v6 | 0.5 MB |
