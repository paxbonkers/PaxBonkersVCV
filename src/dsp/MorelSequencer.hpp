#pragma once
#include <cmath>
#include <cstdint>
#include "ZenonSequencer.hpp"

// Rack-independent sequencer logic of Morel (one 8-step track with four directions),
// unit-tested in tests/test_morel_sequencer.cpp. See docs/modules/Morel-Signalvertrag.md.
// Transport (internal clock, clock divider/multiplier) is shared with Zenon.
// Real-time safe: no allocation, no I/O, fixed-size state.

namespace morel {

static const int NUM_STEPS = 8;

using zenon::clampf;
using zenon::finiteOr;
using zenon::lengthFrom;
using zenon::Transport;
using zenon::stepsPerMinute;
using zenon::clockFactorIndex;

// Directions. The order is stored in patches (DIR param value): never reorder.
enum Dir {
	DIR_FWD = 0,
	DIR_REV,
	DIR_PEND,
	DIR_SHUF,
	NUM_DIRS
};

inline const char* dirName(int d) {
	static const char* names[NUM_DIRS] = {"FWD", "REV", "PEND", "SHUF"};
	return (d >= 0 && d < NUM_DIRS) ? names[d] : "";
}

// Small deterministic PRNG (xorshift32), real-time safe.
struct Rng {
	uint32_t s = 0x9E3779B9u;
	void seed(uint32_t v) {
		s = v ? v : 0x9E3779B9u;
	}
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	// Uniform in [0, 1).
	float uniform() {
		return (next() >> 8) * (1.f / 16777216.f);
	}
	// Uniform integer in [0, n), n >= 1.
	int below(int n) {
		return n <= 1 ? 0 : (int) (next() % (uint32_t) n);
	}
};

// Position of the track. Step values and length are read from the controls.
struct StepSequencer {
	int pos = -1;        // -1 = before the first step: the next advance() lands on step 1 (REV: step LEN)
	int pendDir = 1;     // pendulum direction, +1 or -1
	int perm[NUM_STEPS];
	int permLen = 0;     // length the current shuffle permutation was made for, 0 = none
	int permIdx = 0;     // next index in perm
	int lastDir = DIR_FWD;
	Rng rng;

	StepSequencer() {
		for (int i = 0; i < NUM_STEPS; i++)
			perm[i] = i;
	}

	void reset() {
		pos = -1;
		pendDir = 1;
		permLen = 0;
		permIdx = 0;
	}

	int current() const {
		return pos < 0 ? 0 : pos;
	}

	// Moves one step. Returns true at the end of a cycle (EOC), see the signal contract.
	// The very first step after reset is step 1 (REV: step LEN) and never an EOC.
	bool advance(int len, int dir) {
		if (len < 1)
			len = 1;
		if (len > NUM_STEPS)
			len = NUM_STEPS;
		if (dir < 0 || dir >= NUM_DIRS)
			dir = DIR_FWD;
		if (dir != lastDir) {
			// A newly selected shuffle starts with a fresh permutation, the pendulum moves forward.
			permLen = 0;
			pendDir = 1;
			lastDir = dir;
		}
		int old = pos;
		if (old < 0) {
			pos = (dir == DIR_REV) ? len - 1 : 0;
			pendDir = 1;
			if (dir == DIR_SHUF) {
				// Start a permutation that begins with step 1.
				makePermutation(len, -1);
				int at = 0;
				for (int i = 0; i < len; i++)
					if (perm[i] == 0)
						at = i;
				int t = perm[0];
				perm[0] = perm[at];
				perm[at] = t;
				permIdx = 1;
			}
			return false;
		}
		switch (dir) {
			default:
			case DIR_FWD: {
				pos = old + 1;
				if (pos >= len)
					pos = 0;
				return pos == 0;
			}
			case DIR_REV: {
				if (old >= len) {
					pos = len - 1;
					return true;
				}
				pos = old - 1;
				if (pos < 0) {
					pos = len - 1;
					return true;
				}
				return false;
			}
			case DIR_PEND: {
				if (len == 1 || old >= len) {
					pos = 0;
					pendDir = 1;
					return true;
				}
				int next = old + pendDir;
				if (next >= len) {
					pendDir = -1;
					next = old - 1;
				}
				if (next < 0) {
					pendDir = 1;
					next = 1;
				}
				pos = next;
				return pos == 0;
			}
			case DIR_SHUF: {
				bool eoc = false;
				if (permLen != len || permIdx >= permLen) {
					makePermutation(len, old);
					eoc = true;
				}
				pos = perm[permIdx++];
				return eoc;
			}
		}
	}

	// Fisher-Yates permutation of 0..len-1. If len > 1, the first element differs from avoidFirst.
	void makePermutation(int len, int avoidFirst) {
		for (int i = 0; i < len; i++)
			perm[i] = i;
		for (int i = len - 1; i > 0; i--) {
			int j = rng.below(i + 1);
			int t = perm[i];
			perm[i] = perm[j];
			perm[j] = t;
		}
		if (len > 1 && perm[0] == avoidFirst) {
			int j = 1 + rng.below(len - 1);
			int t = perm[0];
			perm[0] = perm[j];
			perm[j] = t;
		}
		permLen = len;
		permIdx = 0;
	}
};

// Step value (0..1) to slice index for n slices; -1 if there is no recording.
inline int sliceIndex(float value, int n) {
	if (n <= 0)
		return -1;
	int i = (int) std::floor(clampf(finiteOr(value, 0.f), 0.f, 1.f) * n);
	return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

// SLICE output: 0 V for the first slice to 10 V for the last.
inline float sliceVoltage(int index, int n) {
	if (n <= 1 || index <= 0)
		return 0.f;
	if (index >= n - 1)
		return 10.f;
	return 10.f * index / (float) (n - 1);
}

// Default step value: step i (0-based) = (i + 0.5) / 8, so the steps play the slices in order.
inline float defaultStepValue(int i) {
	return (i + 0.5f) / NUM_STEPS;
}

} // namespace morel
