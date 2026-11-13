# Lillebror

<p align="center">
  <img src="images/Lillebror.png" alt="Lillebror module panel" width="300">
</p>

*Three drum voices, the little brother of Grids.* 16 HP, monophonic. Part of the [PaxBonkers](../README.md) plugin by pax.bonkers.

- [Concept](#concept)
- [Getting started](#getting-started)
- [Operation](#operation)
- [Technical specifications](#technical-specifications)
- [Patch ideas](#patch-ideas)
- [Known limits](#known-limits)

## Concept

*Lillebror* is Swedish for "little brother". The name comes from Astrid Lindgren's *Bröderna Lejonhjärta* (*The Brothers Lionheart*), a story about two brothers who only make sense together.

The big brother is Mutable Instruments Grids, available in VCV Rack as Valley's [Topograph](https://library.vcvrack.com/Valley/Topograph), among others. Grids moves through a map of drum patterns and sends triggers and accents for kick, snare and hi-hat. It makes no sound itself.

Lillebror turns those six signals into sound. Its three columns match the three Grids channels, so you patch them one to one:

| Grids / Topograph | Lillebror |
|---|---|
| Trig 1 / BD | KICK · TRIG |
| Trig 2 / SD | SNARE · TRIG |
| Trig 3 / HH | HI-HAT · TRIG |
| Acc 1 / 2 / 3 | ACC of the same column |

The lion next to the name is a nod to the Lionhearts. The big brother may get his own module later.

The sound takes loose inspiration from percussion synths such as ALM's Tyso Daiko. Each voice morphs between waveforms instead of using a fixed sine, and an accent changes the character of a hit as well as its level.

Lillebror contains no code from Grids. Mutable Instruments and Grids are names of their respective owners.

## Getting started

1. Place Topograph (or another Grids) and Lillebror next to each other and give Topograph a clock.
2. Patch the three trigger outputs and the three accent outputs into the matching TRIG and ACC inputs.
3. Patch MIX to your mixer or audio interface, at low volume.
4. Start the clock and move through the pattern map with Map X and Map Y.

Lillebror works with any trigger source. A sequencer gate into KICK · TRIG plays the kick on its own.

## Operation

### Voices and controls

| | KICK | SNARE | HI-HAT |
|---|---|---|---|
| TUNE | 19–155 Hz, default 55 Hz | 64–509 Hz, default 180 Hz | cluster base 141–1131 Hz, default 400 Hz |
| WAVE | sine → triangle → saw → square | sine → triangle → saw → square (body) | waveform of the six cluster oscillators |
| 2nd row left | SWEEP: pitch drop at the start, up to 4 octaves | TONE: body ↔ noise | COLOR: metallic cluster ↔ noise |
| 2nd row right | CLICK: short noise click | SNAP: short noise burst | BRIGHT: high-pass 400 Hz – 7 kHz |
| DECAY | 30 ms – 2 s | 30 ms – 1 s | 10 ms – 1.5 s |

Decay is the time the voice takes to fall by 60 dB.

The kick is a morphing oscillator with a fast exponential pitch sweep. With WAVE at sine it is a classic electronic kick; towards saw and square it becomes a buzzing bass drum or a zap.

The snare has a morphing body tone with a short pitch drop, band-passed noise and a snap burst. The body decays faster than the noise, as it does on a real snare.

The hi-hat uses six oscillators at the inharmonic ratios of the classic 808 hi-hat, mixed with noise and sent through a high-pass. WAVE moves the oscillators from square (hard and metallic) to sine (soft and bell-like).

### Inputs, outputs and buttons

| Port | Description |
|---|---|
| TRIG | Trigger. Rising edge at ≥ 1 V, released at ≤ 0.1 V |
| ACC | Accent. Counts if it is high (≥ 1 V) at the trigger or up to 1 ms after it |
| TUNE CV | Added to TUNE, 1 V/oct. Knob plus CV is limited to ±3 octaves |
| WAVE CV | Added to WAVE, 1 V = 10 % |
| DEC CV | Added to DECAY, 1 V = 10 % |
| MUTE | Latching button. The red light shows a muted voice |
| OUT | Voice output, about ±5 V for a normal hit |
| MIX | Sum of the three voices × 0.6. Muted voices are left out |

### Accent

An accented hit is 6 dB louder, and each voice changes in its own way. The kick decays 1.5 times longer and sounds fuller. The snare decays in 0.7 of the time with 1.5 times the snap, so it gets shorter and snappier. The hi-hat decays twice as long and opens up.

With Grids, the accents shape the groove instead of only the volume.

### Mute

MUTE fades the voice out and back in over 5 ms, so it does not click. The voice keeps running internally while muted. The mute state is saved with the patch.

## Technical specifications

| | |
|---|---|
| Width | 16 HP |
| Polyphony | Mono. Only channel 0 of each input is read; all outputs have one channel |
| Trigger inputs | Schmitt trigger, high at ≥ 1 V, low at ≤ 0.1 V |
| Accent window | ACC is read at the trigger and up to 1 ms after it, so small timing differences between cables do not matter |
| CV inputs | Read continuously: WAVE CV can morph the waveform during a hit and DEC CV can lengthen or shorten a hit that is already sounding. NaN or Inf is ignored |
| Retrigger | The envelope restarts with a 0.5 ms attack from its current level. Kick and snare reset their oscillator phase for a consistent attack |
| Kick | Sweep time constant 30 ms. CLICK is noise filtered to 3–12 kHz, 1.5 ms long |
| Snare | Body pitch drop +0.5 octave over 12 ms. Noise band-pass 1.2–9 kHz. Snap burst 8 ms |
| Hi-hat | Two-pole high-pass 400 Hz – 7 kHz (BRIGHT), followed by a low-pass at 16 kHz |
| Aliasing | Saw and square are band-limited with PolyBLEP. The triangle is calculated directly; its soft corners produce very little aliasing |
| Levels | About ±5 V for a normal hit, +6 dB with accent. Outputs are not clipped: extreme settings can exceed ±5 V, and accents ±10 V |
| Sample rate | All times and filters follow the current sample rate. Noise is level-compensated (√(fs / 48 kHz)), so the voices sound the same at 44.1 and 192 kHz |
| Real-time safety | The audio thread does no memory allocation, file I/O or locking |
| Defaults | Kick: 55 Hz, sine, sweep 50 %, click 30 %, decay 40 %. Snare: 180 Hz, sine, tone 60 %, snap 50 %, decay 35 %. Hi-hat: 400 Hz, square, color 30 %, bright 50 %, decay 15 %. All voices unmuted |
| Saved with the patch | All knobs and the mute buttons |
| Bypass | All outputs silent |

## Patch ideas

- Classic: Topograph into Lillebror and MIX into the audio interface, then explore the map with Map X and Map Y.
- Breathing hats: a slow LFO into HI-HAT · WAVE CV moves the hats between metallic and soft.
- Bass line: a sequencer's pitch CV into KICK · TUNE CV and its gate into KICK · TRIG, with WAVE towards saw and a long DECAY.
- Accents: turn up the Grids accent density and listen to the kick swell and the hats open.

## Known limits

- With WAVE at sine and BRIGHT fully up, the hi-hat is almost silent, because the high-pass removes the low sine cluster.
- At the default settings the hi-hat is quieter than kick and snare (about 3 V peak).
