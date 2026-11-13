#pragma once
#include <cmath>
#include "MorelPlayer.hpp"
#include "MorelSequencer.hpp"

// GRAIN playback of Morel: overlapping Hann-windowed grains read from the current slice.
// A read head scans slowly through the slice; SPAN sets the grain size, SCATTER spreads position and pitch.
// Rack-independent, unit-tested in tests/test_morel_grain.cpp. See docs/modules/Morel-Signalvertrag.md.
// Real-time safe: fixed number of voices, no allocation, no I/O.

namespace morel {

static const int GRAIN_VOICES = 4;
static const float GRAIN_MIN_SEC = 0.01f;     // grain size at SPAN minimum
static const float GRAIN_SCAN_RATE = 0.25f;   // read head speed relative to playback speed
static const float GRAIN_SCATTER_SEMIS = 7.f;
static const float SPAN_MIN = 0.01f;

// Grain size in source samples: exponential from 10 ms (SPAN 1 %) to the whole slice (SPAN 100 %).
inline double grainSize(float span, double sliceLen, float sampleRate) {
	double minLen = GRAIN_MIN_SEC * sampleRate;
	if (!(sliceLen > 1.0))
		return 1.0;
	if (sliceLen <= minLen)
		return sliceLen;
	float t = (clampf(finiteOr(span, 1.f), SPAN_MIN, 1.f) - SPAN_MIN) / (1.f - SPAN_MIN);
	return minLen * std::pow(sliceLen / minLen, (double) t);
}

struct Grain {
	bool active = false;
	SampleRef s;
	double pos = 0.0;      // source position
	double rate = 1.0;     // source samples per output sample (without SPEED)
	float phase = 0.f;     // 0..1 through the window
	float phaseInc = 0.f;  // per output sample at speed 1
};

struct GrainCloud {
	Grain grains[GRAIN_VOICES];
	SampleRef s;           // current slice source
	double sliceStart = 0.0, sliceLen = 0.0;
	bool hasSlice = false;
	double scan = 0.0;     // read head, relative to the slice start (source samples)
	double untilNext = 0.0;// output samples until the next grain starts
	Rng rng;

	void stopAll() {
		for (int i = 0; i < GRAIN_VOICES; i++)
			grains[i].active = false;
		hasSlice = false;
	}

	// No new grains; running grains finish with their window.
	void stopSpawning() {
		hasSlice = false;
	}

	void release(int id) {
		for (int i = 0; i < GRAIN_VOICES; i++)
			if (grains[i].active && grains[i].s.id == id)
				grains[i].active = false;
		if (hasSlice && s.id == id)
			hasSlice = false;
	}

	// New step: grains now come from [start, end). Running grains finish naturally (crossfade).
	void setSlice(const SampleRef& src, double start, double end) {
		if (!src.left || src.length <= 1 || end - start < 2.0 || start < 0.0 || end > src.length) {
			hasSlice = false;
			return;
		}
		s = src;
		sliceStart = start;
		sliceLen = end - start;
		hasSlice = true;
		scan = 0.0;
		untilNext = 0.0;   // first grain starts immediately
	}

	// Read head position as a fraction of the recording, -1 if silent.
	float playhead() const {
		if (!hasSlice || s.length <= 0)
			return -1.f;
		return (float) ((sliceStart + scan) / s.length);
	}

	// span, scatter: 0..1; speedMul: 2^octaves from SPEED and V/OCT.
	// Returns true when the read head wraps to the slice start (EOS in GRAIN mode).
	bool process(float span, float scatter, float speedMul, float engineRate, float& outL, float& outR) {
		outL = outR = 0.f;
		bool eos = false;
		if (!(engineRate > 0.f) || !(speedMul > 0.f) || !std::isfinite(speedMul))
			return false;
		scatter = clampf(finiteOr(scatter, 0.f), 0.f, 1.f);

		if (hasSlice) {
			double baseRate = (double) s.sampleRate / engineRate * speedMul;
			double size = grainSize(span, sliceLen, s.sampleRate);
			// Spawn: one grain every half grain duration (2x overlap, Hann windows sum to 1).
			untilNext -= 1.0;
			if (untilNext <= 0.0) {
				float semis = (rng.uniform() * 2.f - 1.f) * GRAIN_SCATTER_SEMIS * scatter;
				double rate = (double) s.sampleRate / engineRate * std::pow(2.f, semis / 12.f);
				double offset = (rng.uniform() * 2.f - 1.f) * 0.5 * sliceLen * scatter;
				double rel = scan + offset;
				double maxRel = sliceLen - size;
				if (maxRel < 0.0)
					maxRel = 0.0;
				rel = std::fmod(rel, sliceLen);
				if (rel < 0.0)
					rel += sliceLen;
				if (rel > maxRel)
					rel = maxRel;
				for (int i = 0; i < GRAIN_VOICES; i++) {
					Grain& g = grains[i];
					if (g.active)
						continue;
					g.active = true;
					g.s = s;
					g.pos = sliceStart + rel;
					g.rate = rate;
					g.phase = 0.f;
					g.phaseInc = (float) (rate / size);
					break;
				}
				// Duration of a grain at speed 1 in output samples, without the scatter pitch
				double durOut = size / ((double) s.sampleRate / engineRate * speedMul);
				untilNext += std::fmax(durOut * 0.5, 1.0);
			}
			// Read head
			scan += GRAIN_SCAN_RATE * baseRate;
			if (scan >= sliceLen) {
				scan = std::fmod(scan, sliceLen);
				eos = true;
			}
		}

		for (int i = 0; i < GRAIN_VOICES; i++) {
			Grain& g = grains[i];
			if (!g.active)
				continue;
			float w = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * g.phase);
			outL += w * hermite(g.s.left, g.s.length, g.pos);
			outR += w * hermite(g.s.right, g.s.length, g.pos);
			g.pos += g.rate * speedMul;
			g.phase += g.phaseInc * speedMul;
			if (g.phase >= 1.f || g.pos >= g.s.length)
				g.active = false;
		}
		return eos;
	}
};

} // namespace morel
