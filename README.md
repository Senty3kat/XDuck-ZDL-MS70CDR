# XDuck — cross-sidechain duck / gate / texture for Zoom MS-70CDR

[Русская версия](README.ru.md)

![XDuck on the MS-70CDR screen](docs/screen.png)

**Release 1.19.** Custom `.ZDL` effect for the **Zoom MS-70CDR** (firmware 2.10).
A two-channel sidechain processor: the level of one input controls the volume of the
other. Plug an instrument into **IN L** and a drum machine, kick or loop into **IN R**,
and the instrument pumps, gates or follows the drums.

It also pairs with its sister effect **[sForm](https://github.com/Senty3kat/sForm-ZDL-MS70CDR)**,
a shapeable LFO / mono synth: sForm → XDuck with Route = `sForm` makes XDuck follow
sForm's waveform sample by sample (tremolo, trance gate, auto-pan, audio-rate
modulation locked to the note you play). See [Working with sForm](#working-with-sform).

> **MS-70CDR only.** XDuck relies on the CDR's two separate inputs (IN L / IN R).
> Other MultiStomp models have a mono input and the effect has nothing to key from.

## Install

1. Download [`release/XDuck.ZDL`](release/XDuck.ZDL) and [`release/XDuck.png`](release/XDuck.png)
   (the PNG is the Effect Manager thumbnail; keep it next to the ZDL).
2. Close Ableton Live and any browser tab that uses Web MIDI: they grab the pedal's
   MIDI port and Zoom Effect Manager stops seeing the pedal.
3. Add XDuck with Zoom Effect Manager. It is in the **Dynamics** category, effect ID **501**.
   - Effect Manager does not replace an effect with the same ID: delete an older XDuck 1.16+
     first.
   - XDuck **1.13 and older** used ID 486 in the Delay category (it clashed with another
     custom pack). That copy is a different effect for the pedal: delete it by hand, and
     re-add XDuck to old patches.
4. Put XDuck **first** in the patch so the key on IN R reaches it unprocessed
   (or right after sForm, see below).

If the pedal ever hangs: power off, hold the left knob, power on, *All Initialize*, footswitch.

## Screen

One value box at the bottom left. Instead of a knob it shows a symbol of the first knob of the
current page: on page 1 that is **Type** — a duck for `Duck`, a pulse for `Gate`, waves for `Tex`.

## Controls (3 pages)

| Page | Knob | What it does |
|---|---|---|
| 1 | **Type** | `Duck` — key loud → target quieter (pumping). `Gate` — key loud → target opens. `Tex` — the target's volume follows the key's envelope continuously, both signals mixed to both outputs (one-band-vocoder feel). |
| 1 | **Route** | `X` — R controls L and L controls R. `R>L` — R controls L, R passes. `R<L` — L controls R, L passes. `KeyR` — R controls L, R is not heard (key only). `KeyL` — L controls R, L is not heard. `sForm` — R carries sForm's LFO, L follows it (see below). `sFPan` — as `sForm`, but the right output follows the LFO 180° later: auto-pan. |
| 1 | **Freq** | What the detector listens to: `Full`, or a band around 40, 60, 100, 150, 230, 360, 570, 880, 1.4k, 2.1k, 3.3k, 5.1k, 8k Hz (kick ≈ 60–100, snare ≈ 150–230, hats ≈ 5.1k–8k). The audio itself is not filtered. |
| 2 | **Thrsh** | Key threshold, 0…60 = 0…−60 dB. Higher number reacts to a quieter key. |
| 2 | **Range** | Maximum gain reduction, 0…60 dB (duck depth / gate floor). |
| 2 | **Ratio** | Duck: compression slope, 100 = limiter. Gate: steepness, 100 = hard gate. Tex: 100 = dB-for-dB following. |
| 3 | **Attk** | 0.1…100 ms. Duck: how fast it ducks. Gate/Tex: how fast it opens. |
| 3 | **Hold** | 0…500 ms. Holds the key level after a hit: duck stays down, gate stays open, no chatter. |
| 3 | **Rels** | 5 ms…2.5 s. Duck: recovery (the pump). Gate/Tex: how fast it closes. |

Output is always 100 % processed (no Mix).

### Quick starts

- **Sidechain pump (Benassi-style):** Type `Duck`, Route `KeyR`, Freq `60`, Thrsh 40, Range 40, Ratio 80, Attk 15, Hold 10, Rels 45.
- **Trance gate from a hi-hat line:** Type `Gate`, Route `KeyR`, Freq `5.1k`, Hold 5, Rels 20.
- **Two players ducking each other:** Type `Duck`, Route `X`.

## Working with sForm

Chain: **sForm → XDuck** (sForm directly before XDuck), sForm **Out = `XDuck`**,
XDuck **Route = `sForm`** or **`sFPan`**. sForm sends its waveform on the R channel;
XDuck reads it every sample, with no detector and no envelope, so it follows the wave
exactly at any rate — from a 100-second swell to audio-rate modulation, or at the pitch
of the note you play (sForm Note = `in…`). The processed L goes to both outputs.

In these routes:

| Type | Volume of L |
|---|---|
| `Duck` | 1 − wave (louder when the wave is low) |
| `Tex` | follows the wave |
| `Gate` | opens where the wave crosses **Thrsh** (30 = mid-wave, lower = open longer); **Ratio** = steepness (0 = just follows the wave, 100 = hard on/off) |

**Range** sets the depth for all three. Attk, Hold, Rels and Freq are not used.
`sFPan` gives each side its own copy of the wave, half a cycle apart — an auto-panner
(it exists only in these routes, where L feeds both outputs).

## Hardware notes found while building it

- The effect buffer (`ctx[5]`) carries the real IN L and IN R in its two 8-sample halves.
- The "raw input" buffer (`ctx[4]`) carries **only IN L** — its R half is silent — so a
  pre-chain R key does not exist. Hence "put XDuck first".
- Firmware value boxes must stay in **one horizontal row**; a vertical column drew a stray
  bar. A box at x = 0 hides under the "effect on the left" tab — XDuck's box is at x = 14,
  like the stock first box.
- Parameter names longer than 5 characters overlap on the pedal display.
- A knob with more than 102 positions (max > 101) made Effect Manager fail with
  `FS.bin size mismatch`.

## Building from source

XDuck is built with the toolchain of
[themanro/ZoomMultistompZDL](https://github.com/themanro/ZoomMultistompZDL)
(a fork of [repeat98/ZoomMultistompZDL](https://github.com/repeat98/ZoomMultistompZDL))
and the TI C6000 compiler 8.5.0.LTS (ships with Code Composer Studio).

1. Clone the themanro fork and copy `src/xduck/` to `src/custom/xduck/`.
2. Copy the files from `toolchain-patches/` over the same paths in the fork (they are full
   files, based on the fork as of 2026-10-18; newer fork versions may need a merge). They add:
   - `build/linker.py` — a single value box (`allow_fewer_knob_boxes`) and a custom knob
     picture (`knob_pic`, `knob_frames`, stock "style 12");
   - `build/selector_metadata.py` — the on-pedal words for Type / Route / Freq (and sForm's);
   - `build/parameter_display.py` — the 5-letter pedal names Thrsh / Attk / Rels;
   - `src/airwindows/common/pack_covers.py` and `custom_covers.py` — the XDuck and sForm
     screens and an optional `version` argument for covers.
3. In `src/custom/xduck/build.py` set `TI_ROOT` to your compiler and `out_dir` to where
   you want the ZDL, then run `python src/custom/xduck/build.py`.

The DSP (`xduck.c`) follows the fork's safe-DSP rules: no switch / jump tables, no math
library, no divides (log2/exp2 by bit tricks, Newton reciprocal), no static arrays,
state in the `ctx[3]` arena, `ctx[11]`/`ctx[12]` shuttle preserved.

The screen art (`xduck_art.json`) is a 1-bit conversion of a reference picture drawn
for this effect; the value-box symbols are in `knob_icons.py`.

## History

See [CHANGELOG.md](CHANGELOG.md) (in Russian).

## License

MIT, same as the toolchain it is built on — see [LICENSE](LICENSE).
