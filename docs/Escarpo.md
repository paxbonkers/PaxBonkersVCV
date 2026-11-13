# Escarpo

<p align="center">
  <img src="images/Escarpo.png" alt="Escarpo module panel" width="145">
</p>

*Clock swing, drift and burst.* 8 HP, monophonic. Part of the [PaxBonkers](../README.md) plugin by pax.bonkers.

- [Concept](#concept)
- [Getting started](#getting-started)
- [Operation](#operation)
- [Technical specifications](#technical-specifications)
- [Patch ideas](#patch-ideas)

## Concept

The name comes from Jean-Honoré Fragonard's painting *L'Escarpolette* (*The Happy Accidents of the Swing*, 1767), which shows a young woman on a swing in mid-flight. Escarpo swings a clock in the same way, away from the beat and back.

Escarpo delays every incoming clock pulse by a fraction of the measured clock period. Because the delay is a fraction of the period, it stays musical at any tempo. With a fixed delay the clock lays back; with an LFO or random CV on the delay input it starts to drift, breathe and swing against the original. Escarpo can also turn each pulse into a burst of evenly spaced pulses (a ratchet).

## Getting started

1. Patch a clock into CLOCK.
2. Patch THRU to one drum voice (a kick, for example) and OUT to another (a hi-hat).
3. Turn DELAY up a few percent and listen to the hats fall behind the beat.
4. Patch an LFO into the delay CV input and open ATTEN to make the delay move.

## Operation

### Controls

| Control | Range | Default | Function |
|---|---|---|---|
| DELAY | 0–90 % of the clock period | 0 % | Delay applied to every clock pulse |
| ATTEN | −100 … +100 % | 0 % | Amount and polarity of the delay CV |
| BURST | 1–8, stepped | 1 | Pulses per incoming clock pulse; 1 = off |

### Inputs and outputs

| Port | Description |
|---|---|
| CLOCK | Clock or gate input |
| RESET | Clears pending pulses and bursts and forgets the measured tempo |
| CV (delay) | Added to DELAY through ATTEN. 1 V = 10 % of the period |
| CV (burst) | Added to BURST. 1 V = 1 step, rounded and limited to 1–8 |
| THRU | The clock input, unchanged |
| OUT | Delayed clock or burst, 0 V / 10 V. The LED shows this output |

### How the delay works

Escarpo measures the tempo from the last two rising clock edges. The first pulse after start or reset has no measurement yet, so it passes through without delay and without burst.

The delay and the burst count are read once per pulse, when the pulse arrives. Modulation never moves a pulse that is already on its way.

With BURST at 1, both edges of the input pulse are delayed by the same time, so the gate length of the input is kept. With BURST above 1, Escarpo spreads N pulses evenly across one clock period, starting at the delayed time. Each burst pulse is half the burst interval long. Burst pulses that would start after the next expected clock pulse are dropped.

Pulses never overtake each other. If the next clock arrives before a delayed pulse has gone out, Escarpo sends that pulse at once as a short trigger and cuts off the rest of a running burst.

When the clock stops, the last measured period stays valid and the output that belongs to the last clock pulse still goes out. To silence Escarpo at once, patch the run/stop trigger of your clock into RESET. A gap longer than four periods counts as a restart rather than a new tempo, so the first pulse after stopping a sequencer is not delayed by several seconds.

### Context menu

Quantize delay snaps the total delay (knob plus CV) to musical fractions of the period: 0, 1/6, 1/4, 1/3, 1/2, 2/3 and 3/4. With a random CV on the delay input, the timing jumps between related positions instead of sliding. The setting is saved with the patch.

## Technical specifications

| | |
|---|---|
| Width | 8 HP |
| Polyphony | Mono. Only channel 0 of each input is read |
| Clock and reset inputs | Schmitt trigger, high at ≥ 1 V, low at ≤ 0.1 V |
| OUT | 0 V / 10 V. Before every new rising edge there is at least one low sample |
| THRU | The input voltage, 1:1 |
| Delay | Total = DELAY + CV × ATTEN, 1 V = 10 % of the period, limited to 0–90 % (with Quantize, to the nearest grid value up to 3/4) |
| Burst gate length | 50 % of the burst interval |
| Pulse that is overtaken | Sent at once as a 1 ms trigger |
| CV inputs | Unpatched inputs are 0 V. NaN or Inf is caught |
| Sample rate | All times are calculated in seconds from the current sample rate; changing it is safe |
| Saved with the patch | All knobs and Quantize delay (`quantizeDelay` in the patch data; missing or invalid means off) |
| Initialize | Parameters to default, Quantize off, timing state cleared |
| Bypass | CLOCK goes straight to THRU and OUT |

## Patch ideas

- Kick on THRU, hi-hat on OUT, DELAY at a few percent: laid-back hats.
- A slow sine LFO into the delay CV with ATTEN at about 30 %: a groove that breathes.
- Random sample-and-hold into the delay CV with Quantize delay on: shifting offbeats that stay on the grid.
- A square LFO into the burst CV: single hits and ratchets take turns.
