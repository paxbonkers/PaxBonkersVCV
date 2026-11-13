# Zenon

<p align="center">
  <img src="images/Zenon-panel.png" alt="Zenon module panel" width="640">
</p>

*Polymetric percussion.* 36 HP, monophonic. Part of the [PaxBonkers](../README.md) plugin by pax.bonkers.

- [Concept](#concept)
- [Getting started](#getting-started)
- [Operation](#operation)
- [Technical specifications](#technical-specifications)
- [Patch ideas](#patch-ideas)

## Concept

Zeno's paradox has Achilles chasing a tortoise. He runs faster, yet each time he reaches the point where the tortoise was, it has moved on a little. Zenon takes the picture of two runners at different speeds who drift apart and meet again, and turns it into a drum sequencer.

Zenon is a percussion synthesizer with a built-in step sequencer, in the tradition of the Moog DFAM without copying it. Two 8-step tracks run on top of each other. On the DFAM the two rows are wired to pitch and velocity. On Zenon each track chooses its own destination and has its own length. A 5-step pitch line against a 7-step filter line repeats only after 35 steps. Every step triggers a voice with two oscillators, noise, a filter and three decay envelopes.

## Getting started

1. Patch AUDIO to your mixer or audio interface, at low volume.
2. Press RUN. Zenon plays at 120 BPM in sixteenths with the internal clock.
3. Turn the step knobs of track A (default destination PITCH) to make a melody, and the step knobs of track B (default VEL) to set accents.
4. Set track A to length 5 and track B to length 7 and listen to the pattern shift.

## Operation

### Transport

| Control / port | Function |
|---|---|
| TEMPO | Internal clock, 40–800 steps/min, default 480 (120 BPM in sixteenths). With CLOCK patched the knob becomes a divider/multiplier: /4, /2, ×1, ×2, ×4. Its travel is split into five equal zones with ×1 in the middle |
| TEMPO CV | Internal clock: 1 V doubles the tempo. External clock: 1 V moves one factor step, limited to /4 … ×4 |
| RUN button / input | Starts and stops the sequencer. A trigger at the input toggles RUN. With the internal clock the first step sounds at once, with an external clock on the next pulse |
| ADV | One step forward, also while stopped |
| CLOCK | External clock. Replaces the internal clock. Pulses count only while RUN is on |
| TRIG button / input | Plays the current step again without advancing |
| RESET | Both tracks start at step 1 with the next step. The internal clock restarts its phase and the divider starts counting again |

Every step, whether from the internal clock, an external clock or ADV, advances both tracks and triggers the voice.

### Tracks A and B

| Control / port | Range | Function |
|---|---|---|
| DEST | 7 destinations | What the track modulates. The small display shows the name. Default A = PITCH, B = VEL |
| LENGTH | 1–8 steps | Length of the track, default 8 |
| LEN CV | 1 V = 1 step | Added to LENGTH, rounded and limited to 1–8 |
| AMOUNT | −100 … +100 % | Depth and direction, default +100 % |
| Steps 1–8 | 0–100 % | Step values. Default A 0 %, B 100 % |
| Step LEDs | | Current step bright, steps within the length dimmed, steps beyond it off |
| EOC | trigger | End of cycle, see below |
| RND button / input | trigger | New random values for all 8 steps of the track, including those beyond the length |

Destinations at AMOUNT +100 % (s = step value from 0 to 1):

| DEST | Effect |
|---|---|
| PITCH | Both oscillators, up to +4 octaves (s × 4) |
| OSC 2 | Oscillator 2 only, up to +4 octaves |
| FM | Added to the FM amount, total limited to 0–100 % |
| CUTOFF | Up to +6 octaves |
| FLT DEC | Added to the filter decay slider, limited to its range |
| AMP DEC | Added to the amp decay slider, limited to its range |
| VEL | Hit level. At +100 % the level equals the step value; at −100 % a high step is quiet |

Negative AMOUNT reverses the direction, so PITCH goes down instead of up. If both tracks use the same destination, their contributions add up.

A track advances one position per step and wraps at its length. If you shorten a track below its current position, it jumps to step 1 on the next step.

EOC fires when a track jumps from its last step back to step 1, at the same moment as step 1. It also fires when a shorter length cuts a cycle off. It does not fire on the first step after start or reset. At length 1 it fires on every step from the second one on.

### Voice

| Control / port | Range | Default | Function |
|---|---|---|---|
| OSC 1 FREQ | 8 Hz – 4.2 kHz | 65 Hz | Frequency of oscillator 1 |
| OSC 2 FREQ | 8 Hz – 4.2 kHz | 131 Hz | Frequency of oscillator 2 |
| SQR / TRI | switch | TRI | Waveform of each oscillator |
| PITCH 1 / PITCH 2 | 1 V/oct | | Added to the oscillator frequency |
| FM 1→2 + CV | 0–100 %, 1 V = 10 % | 0 % | Exponential FM of oscillator 2 by oscillator 1, up to ±4 octaves |
| SYNC | switch | off | Hard sync of oscillator 2 to oscillator 1 |
| MIX OSC 1 / OSC 2 / NOISE | sliders, 0–100 % | 80 / 0 / 0 % | Levels into the filter |
| EXT | audio, ±5 V | | Replaces the noise when patched; the NOISE slider sets its level |
| CUTOFF + CV | 20 Hz – 20 kHz, 1 V/oct | 2 kHz | Filter cutoff |
| RES | 0–100 % | 20 % | Resonance, close to self-oscillation at 100 % |
| LP / HP | switch | LP | Low-pass or high-pass, 12 dB/oct |
| LEVEL + CV | 0–100 %, 0–10 V | 80 % | Output level. A patched CV scales the knob (CV / 10 V) |

### Decay

| Envelope | Decay | Default | AMT |
|---|---|---|---|
| PITCH | 5 ms – 1 s | 15 % | ±4 octaves on both oscillators, default +30 % |
| FILTER | 5 ms – 2 s | 30 % | ±6 octaves on the cutoff, default +40 % |
| AMP | 10 ms – 3 s | 35 % | none; the level comes from VEL |

All three envelopes restart on every hit with a 0.5 ms attack from their current level, then fall exponentially. Decay times are measured to −60 dB.

### Outputs

| Output | Signal |
|---|---|
| OSC 1, OSC 2 | Raw oscillators, ±5 V |
| P.ENV, F.ENV | Pitch and filter envelope, 0–10 V |
| A.ENV | Amp envelope × velocity, 0–10 V |
| TRK A, TRK B | Current step value of each track, 0–10 V, independent of DEST and AMOUNT. Holds until the next step |
| TRIG | 10 V trigger, 1 ms, on every hit |
| EOC A, EOC B | 10 V trigger, 1 ms. They sit in the track rows, not in the output field |
| AUDIO | Voice after filter and amp, about ±4.3 V at default settings |

## Technical specifications

| | |
|---|---|
| Width | 36 HP |
| Polyphony | Mono. Only channel 0 of each input is read; all outputs have one channel |
| Trigger and clock inputs | Schmitt trigger, high at ≥ 1 V, low at ≤ 0.1 V |
| Trigger outputs | 10 V, 1 ms |
| CV inputs | Added to the knob. NaN or Inf counts as 0 V |
| Oscillators | Square band-limited with PolyBLEP, triangle calculated directly. Phase reset on every hit. Hard sync with sub-sample position; the sync jump itself is not band-limited |
| Filter | 2-pole state-variable filter (TPT), 12 dB/oct, stable at all sample rates. The band-pass state saturates softly at about ±2.5 internal units, which limits the resonance peak |
| Output level | Internal 1.0 = 8 V. No hard clipping; in extreme settings (all mix sliders up, square wave, RES 100 %, oscillator on the cutoff frequency) peaks reach about 20 V |
| Clock multiplier | Extra steps are spread evenly, based on the time between the last two clock pulses. Before the second pulse only the pulses themselves count. A gap longer than 4 periods counts as a restart and keeps the old period |
| Clock divider | Every Nth pulse is a step, counted from the first pulse after start or reset |
| Sample rate | All times and filter coefficients follow the current sample rate. Tested at 44.1, 48, 96 and 192 kHz |
| Saved with the patch | All knobs, switches, step values and RUN. Track positions and the divider count are not saved; both tracks start at step 1 after loading |
| Initialize | All parameters to default, sequencer to step 1, envelopes to 0 |
| Bypass | All outputs silent |

The DEST order (PITCH, OSC 2, FM, CUTOFF, FLT DEC, AMP DEC, VEL) is stored in patches and will not change.

## Patch ideas

- Polymeter: A on PITCH with length 5, B on CUTOFF with length 7. The pattern repeats every 35 steps.
- Generative: patch EOC A into RND B. Track B gets new values each time track A completes a cycle.
- Clocked: patch a clock into CLOCK and use TEMPO for half or double time.
- Bass drum: OSC 1 low, PITCH AMT up, a short pitch decay, and track B on VEL for accents.
