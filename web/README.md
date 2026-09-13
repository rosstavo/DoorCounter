# Live web tuner

A browser dashboard for dialling in the detection constants at the doorway. It
shows both PIR sensors in real time, lets you drag the timings and see the
effect on the very next walk-through, and hands you the `config.h` block to
paste back.

```
ESP32 (tuner.ino)  --USB serial-->  Vite dev server  --WebSocket-->  browser
```

## Run it

```sh
scripts/flash.sh tuner          # put the tuning firmware on the board
cd web && npm install           # once
npm run dev
```

Open the **Local** URL on the laptop, or the **Network** URL on your phone — the
dev server binds to the LAN so the laptop can stay tethered to the board at the
doorway while you read the traces on a phone in your hand. Both can be open at
once; every client sees the same stream and any of them can move the sliders.

The serial port is auto-detected and reopened if the board is reset or
replugged. Override it with `PORT=/dev/cu.usbserial-XXXX npm run dev`.

When you're done, flash the real firmware back: `scripts/flash.sh firmware`.

## What's on screen

| Panel | What it tells you |
|---|---|
| **OUTER / INNER lamps** | live pin level. At rest both should read LOW — a channel that sits HIGH or flickers with nobody near it is a wiring or power problem, not a tuning one. |
| **STATE** | `IDLE` → `ARMED <sensor> <ms>` → back to `IDLE`, plus the `DEBOUNCE` lock-out and the warm-up countdown. This is the state machine itself, live. |
| **Live sensor trace** | the last few seconds of both channels as a logic trace, with a marker and a delta bracket at every decision. This is where you *see* why a pass was counted or discarded. |
| **Counters** | entries, exits, and the two discard buckets. |
| **Walk-through deltas** | every counted delta plotted against the current window, with min / median / max and a suggested window. |

## How to actually tune it

The trap: **a window that's too narrow hides the data you need.** If a real
walk-through takes 600 ms and the window is 400 ms, you never see "600" — the
pass lands in *discarded noise* and the deltas you can see are capped at 400.
Tuning downward from a wide window is the way round it.

1. Let the PIRs settle. They need ~60 s from power-on regardless of what the
   tuner's own warm-up slider says.
2. Hit **Widen to sample** (window → 1500 ms). Now every pass gets measured.
3. Walk through a dozen times, both directions, at the pace real customers use —
   include a slow amble and someone pausing in the doorway.
4. Read the deltas panel. **Apply suggested window** sets 1.5× the largest
   observed delta, rounded to 10 ms, which is the rule the PRD calls for.
5. Walk through again and confirm each pass lands in entries/exits rather than
   the discard buckets. Nudge `Debounce` up if one slow person registers twice.
6. **Copy** the `config.h` block, paste it into `door_counter/config.h`, and
   `scripts/flash.sh firmware`.

The state machine the tuner drives is `door_counter/Detector.h` — the same file
the production firmware compiles in, not a copy — so the numbers transfer
exactly. `test/run.sh` covers that shared logic.

## Wire protocol

Newline-delimited JSON out of the device, plain-text commands in. Both are
documented at the top of [`tuner/tuner.ino`](../tuner/tuner.ino), and the
commands are typeable by hand in any serial monitor if you'd rather skip the
browser:

```
set window 600     set debounce 1000     set simultaneous 10
set warmup 5000    get     reset     skipwarmup     ping
```

## Notes

- Only one program can hold the serial port. Close `scripts/monitor.py` (and the
  Arduino IDE's Serial Monitor) before `npm run dev`, or the bridge reports
  "Resource busy" and keeps retrying.
- `npm run dev` is the only supported mode: `npm run build` produces a static
  bundle with no serial bridge behind it, so the page would have nothing to talk to.
- The tuner firmware has no Wi-Fi, no Google Sheets and no filesystem writes. It
  counts nothing permanently and cannot corrupt the shop's stored data.
