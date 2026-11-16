# Changelog (modulove fork, branch `modulove-techno`)

Base: upstream `dchwebb/Punck` 06cc991. Revisions r1–r28 were flashed and tested on one module (STM32H743 rev V, 400 MHz) unless noted.

**r28** — Level pots: audio taper, unity at 75 %, drive (saturation) in the last part of the travel. Pan per voice. Cold code moved out of ITCM.
**r27** — Settings save: flash error flags cleared correctly, erase request ended, every record verified, retry on an erased sector.
**r26** — Plaits synth code in ITCM, hot lookup tables in DTCM (bongo peak per hit 2548 → 1438 cycles).
**r25** — Plaits FM / VA / waveshaping with the low-pass gate: snare 7 *Bongo*, toms 4 *FM*, 5 *VA stab*, 6 *Fold stab*.
**r23–r24** — Plaits string (Karplus-Strong): snare 6 *String*, toms 3 *Pluck*.
**r22** — 480 MHz made opt-in (off): r21 did not start standalone in the case.
**r20–r21** — Coefficient caches for the Plaits analog snare and hi-hats (bit-identical output), LED writes only on change, 480 MHz (now opt-in).
**r18–r19** — NaN/inf protection on every stage; classic snare decay capped (level could grow to inf).
**r15–r17** — USB: ZLP packet count, EP0 SETUP re-arm, per-endpoint transmit flags (console hangs under load).
**r14** — Claps band pass limited and cleared per hit.
**r13** — Wobbly pitch fixed: I2S FIFO threshold, frame written first, audio-rate watchdog.
**r9–r12** — Underruns: modal set-up spread over 4 samples, one hit set-up per sample, audio code in ITCM; fault record + auto restart, reset reason.
**r8** — Console output no longer drops lines.
**r7** — Key & scale.
**r6** — Settings save themselves in the background (appended records, newest wins).
**r5** — Random zone on kick / hat Decay.
**r4** — Bass engine on the toms, modal snare, exact 48 kHz clock (PLL3).
**r3** — Engine LED blinks, long press in Sequence mode, clean clipper, CPU fixes.
**r2** — Switchable engines (long press), Plaits kicks/snares/hats, Rumble kick, master compressor + clipper, CPU meter, Tone Lab master section, PC renderer.
**r1** — MIDI drum map for the BeatStep Pro (ch 10), chromatic channel per voice, MIDI channel setting, hi-hat random decay + ring mod, Tone Lab, SWD and sample tools.
