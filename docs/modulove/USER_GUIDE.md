# Punck modulove firmware — user guide

Firmware branch `modulove-techno` (r28). Everything below works standalone; the Tone Lab and the console are optional.

## Panel

| Control | Function |
|---|---|
| Voice button, **hold ~0.7 s** | Next sound engine for that voice (Play and Sequence mode). The LED blinks the engine number, then a preview hit plays. Saved automatically. |
| Toggle switch | Middle = Play. In Sequence, a button whose pattern is empty plays its sound. |
| **Level pots** (kick, snare, hats, sampler A/B) | 0–75 %: level, audio taper (noon −10.6 dB, 60 % −5.8 dB, 70 % −1.8 dB). **75 % (≈ 2 o'clock) = full level.** 77–100 %: **drive** — saturation blended in, pushed harder towards full CW; the peak stays at full scale, density rises (strength: Tone Lab *Pot drive*, default 12 dB). |
| Kick Attack / Decay, Snare Filter / Tuning, Hats Decay | Per engine, see below. Kick and hat Decay: the top 10 % is a *random zone* (mostly normal hits, a few long ones). |

## Engines

**Mutable** = Mutable Instruments Plaits/Rings code (MIT). Engine numbers = LED blinks.

| Voice | # | Engine | Source | Panel |
|---|---|---|---|---|
| **Kick** | 1 | Classic | Punck | Attack = low-pass, Decay = length |
| | 2 | Deep (808) | **Mutable** analog bass drum | Attack = punch → self-FM → drive |
| | 3 | Punch (909) | **Mutable** synthetic bass drum | Attack = FM envelope / dirt |
| | 4 | FM | modulove | Attack = FM depth |
| | 5 | Rumble | modulove (BOHM-style) | Attack = groove (rumble, sub, ghost kicks), Decay = length |
| **Snare** | 1 | Classic | Punck | Filter = low-pass, Tuning ×0.5–1.5 |
| | 2 | 808 | **Mutable** analog snare | Filter = snappy, Tuning = pitch ×0.5–2 |
| | 3 | 909 | **Mutable** synthetic snare | Filter = snappy, Tuning = pitch |
| | 4 | Rim / clave | modulove | Filter = snap noise, Tuning = pitch |
| | 5 | Modal | **Mutable** modal resonator (Rings/Plaits) | Filter = mallet brightness, Tuning = pitch |
| | 6 | String | **Mutable** string (Karplus-Strong) | Filter = brightness, Tuning = pitch |
| | 7 | Bongo | **Mutable** FM + low-pass gate | Filter = FM amount, Tuning = pitch |
| **Hats** | 1 | Classic metallic | Punck + ring-mod grit | Decay (closed ↔ open) |
| | 2 | 808 | **Mutable** hi-hat (square noise) | Decay |
| | 3 | 909 | **Mutable** hi-hat (ring-mod noise) | Decay |
| **Toms** (MIDI only) | 1 | Toms | Punck | — |
| | 2 | Bass | modulove (saw/pulse + sub, 24 dB LP) | — |
| | 3 | Pluck | **Mutable** string | — |
| | 4 | FM | **Mutable** 2-op FM + LPG | — |
| | 5 | VA stab | **Mutable** virtual analog + LPG | — |
| | 6 | Fold stab | **Mutable** waveshaper/wavefolder + LPG | — |
| Claps (MIDI only) | – | Punck | — | — |
| Sampler A / B | – | Punck | — | Speed, Voice, Level |

Toms engines 2–6 play from MIDI (chromatic ch 14, drum notes 40/41) or **Follow**: automatic notes on the 16ths after every kick (offbeat, rolling KBBB, …; tempo from MIDI clock, sequencer or kick interval). The toms engine is chosen in the Tone Lab (`engine:6,e` on the console).

## MIDI

| Voice | Drum map (ch 10, BeatStep Pro drum track) | Chromatic channel (note 60 = configured pitch) |
|---|---|---|
| Kick | 36 | 11 |
| Snare | 37–38 (38 = longer) | 12 |
| Claps | 39 | 15 |
| Toms | 40–41 | 14 |
| Hats | 42 / 43 / 44 = closed / half / open | 13 |
| Sampler A | 45–47 (bank A 1–3) | 16 (sample chosen by the Voice pot) |
| Sampler B | 48–87 (bank B) | – |

Change with `midichn:x` (drum map channel, 0 = omni) and `pitchchn:v,c` (voice 1–7, 0 = off). Higher drum-map notes play longer variants.

## Master bus and mix

Input trim → glue compressor (drums, samples, reverb; the **kick bypasses it** by default) → anti-aliased clipper on everything → output trim.
Defaults: threshold −12 dB, 3:1, 15 ms / 120 ms, makeup +3 dB, clipper *clean* +2 dB, output +0.7 dB (= DAC full scale). Guideline: keep *Into clip* at 0…+3 dB; more is audible crunch.

- **Pan** per voice (constant power, mono sum −0.1 dB at the defaults): hats 0.2 R, claps and sampler A 0.15 L, the rest centred.
- **Ducking** by the kick: available, off by default (the Rumble kick ducks its own space).
- **Key & scale**: global root + 11 scales; per voice off / root / root+5th / scale / pentatonic. Tonal voices, MIDI notes and the snare Tuning pot snap to the key.

## Tone Lab (`WebEditor/tonelab.html`)

Open in Chrome (Web MIDI) → *Connect* → *Read*. Engine menus with per-engine knobs, presets (e.g. *String snare*, *Bongo groove*, *Dub pluck*, *Dub stab VA*, *Fold stab*, *Hypnotic rumble*), master section with live meter (gain reduction, into clip, output, CPU). Changes are sent live; *Save to Punck* or autosave keeps them. Only one program can use the Punck MIDI port at a time (close the Tone Lab before using the Python tools).

## Settings

Saved automatically about 3 s after a change (at most every 10 s), in the background with audio running; every save is read back and verified. Console `cfg` shows the storage state and failed saves. Settings from stock firmware (config v5/v6) are not read: first boot starts from defaults.

## Console (USB serial, e.g. COM port at 230400)

`status` (switch, engines) · `engine:v,e` · `cpu` (load, meter, cycles per block) · `mix` / `mix:i,v` · `cfg` · `pots` · `crash` (reset reason, last fault) · `midimap`, `midichn:x`, `pitchchn:v,c` · `play:v,vel,o` · `help` for the full list.
