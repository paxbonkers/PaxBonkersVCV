#pragma once
#include <cmath>
#include <vector>
#include <algorithm>

// Level-based auto-slicing of Morel, unit-tested in tests/test_morel_slicer.cpp.
// See docs/modules/Morel-Signalvertrag.md, section "Auto-Slicing".
// NOT real-time safe (uses std::vector): run it on a worker thread, never in process().

namespace morel {

static const int MAX_SLICES = 32;
static const int FALLBACK_SLICES = 8;

struct SliceList {
	int count = 0;                 // number of slices, 0 = no recording
	int length = 0;                // recording length in samples
	int start[MAX_SLICES] = {};    // first sample of each slice, ascending, start[0] = 0
	float envPeak = 0.f;           // peak of the analysis envelope (L + R), for the threshold display

	int sliceStart(int i) const {
		return (i >= 0 && i < count) ? start[i] : 0;
	}
	// One past the last sample of slice i.
	int sliceEnd(int i) const {
		if (i < 0 || i >= count)
			return 0;
		return (i + 1 < count) ? start[i + 1] : length;
	}
};

struct SlicerConfig {
	float attackMs = 1.f;
	float releaseMs = 30.f;
	float rangeDb = 48.f;        // SENS 100 % = -48 dB below the peak
	float hysteresisDb = 6.f;
	float minSliceMs = 40.f;
	float snapBackMs = 5.f;      // marker moves back to the nearest zero crossing within this time
	float strengthWindowMs = 10.f;
};

// Splits the recording into 1..MAX_SLICES slices.
// left/right: recording (right may be null = mono), length in samples, sens 0..1.
inline void computeSlices(const float* left, const float* right, int length, float sampleRate, float sens,
                          SliceList& out, const SlicerConfig& cfg = SlicerConfig()) {
	out = SliceList();
	out.length = length > 0 ? length : 0;
	if (length <= 0 || !(sampleRate > 0.f))
		return;
	if (!std::isfinite(sens))
		sens = 0.5f;
	sens = std::min(1.f, std::max(0.f, sens));

	// Mono sum and envelope.
	std::vector<float> mono(length), env(length);
	for (int i = 0; i < length; i++) {
		float s = left[i] + (right ? right[i] : left[i]);
		mono[i] = std::isfinite(s) ? s : 0.f;
	}
	float a = std::exp(-1.f / (cfg.attackMs * 0.001f * sampleRate));
	float r = std::exp(-1.f / (cfg.releaseMs * 0.001f * sampleRate));
	float e = 0.f, peak = 0.f;
	for (int i = 0; i < length; i++) {
		float x = std::fabs(mono[i]);
		float c = x > e ? a : r;
		e = c * e + (1.f - c) * x;
		env[i] = e;
		peak = std::max(peak, e);
	}

	out.envPeak = peak;
	int minLen = std::max(1, (int) std::lround(cfg.minSliceMs * 0.001f * sampleRate));

	struct Cand {
		int pos;
		float strength;
	};
	std::vector<Cand> cands;
	if (peak > 1e-9f) {
		float thr = peak * std::pow(10.f, -cfg.rangeDb * sens / 20.f);
		float rearm = thr * std::pow(10.f, -cfg.hysteresisDb / 20.f);
		int snap = (int) std::lround(cfg.snapBackMs * 0.001f * sampleRate);
		int win = std::max(1, (int) std::lround(cfg.strengthWindowMs * 0.001f * sampleRate));
		bool armed = true;
		float floorEnv = 0.f;   // lowest envelope since re-arming
		for (int i = 1; i < length; i++) {
			if (!armed) {
				if (env[i] < rearm) {
					armed = true;
					floorEnv = env[i];
				}
				continue;
			}
			floorEnv = std::min(floorEnv, env[i]);
			if (env[i] >= thr && env[i - 1] < thr) {
				armed = false;
				float top = env[i];
				for (int k = i; k < std::min(length, i + win); k++)
					top = std::max(top, env[k]);
				int p = i;
				for (int k = i; k > std::max(0, i - snap); k--) {
					if ((mono[k - 1] <= 0.f && mono[k] > 0.f) || (mono[k - 1] >= 0.f && mono[k] < 0.f) || mono[k] == 0.f) {
						p = k;
						break;
					}
				}
				cands.push_back({p, top - floorEnv});
			}
		}
	}

	// Minimum slice length: against the fixed start at 0, the end, and between candidates
	// (of two candidates that are too close, the stronger one stays).
	std::vector<Cand> kept;
	for (const Cand& c : cands) {
		if (c.pos < minLen || c.pos > length - minLen)
			continue;
		if (!kept.empty() && c.pos - kept.back().pos < minLen) {
			if (c.strength > kept.back().strength)
				kept.back() = c;
			continue;
		}
		kept.push_back(c);
	}
	// At most MAX_SLICES slices (including the one at 0): keep the strongest candidates.
	if ((int) kept.size() > MAX_SLICES - 1) {
		std::stable_sort(kept.begin(), kept.end(), [](const Cand& x, const Cand& y) { return x.strength > y.strength; });
		kept.resize(MAX_SLICES - 1);
		std::sort(kept.begin(), kept.end(), [](const Cand& x, const Cand& y) { return x.pos < y.pos; });
	}

	if (kept.empty()) {
		// Fallback: equal slices (fewer if the recording is very short).
		int n = std::max(1, std::min(FALLBACK_SLICES, length / minLen));
		out.count = n;
		for (int i = 0; i < n; i++)
			out.start[i] = (int) ((long long) length * i / n);
		return;
	}
	out.count = 1 + (int) kept.size();
	out.start[0] = 0;
	for (size_t i = 0; i < kept.size(); i++)
		out.start[i + 1] = kept[i].pos;
}

} // namespace morel
