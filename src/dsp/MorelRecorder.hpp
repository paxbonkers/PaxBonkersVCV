#pragma once
#include <cmath>

// Recording modes of Morel (MAN, CLK, LVL, RETRO), the RETRO ring buffer and the step period estimate.
// Rack-independent, unit-tested in tests/test_morel_recorder.cpp. See docs/modules/Morel-Signalvertrag.md.
// Real-time safe: no allocation, no I/O (the ring buffer memory is provided from outside).

namespace morel {

// Order is stored in patches (MODE param value): never reorder.
enum RecMode {
	REC_MAN = 0,
	REC_CLK,
	REC_LVL,
	REC_RETRO,
	NUM_REC_MODES
};

inline const char* recModeName(int m) {
	static const char* names[NUM_REC_MODES] = {"MAN", "CLK", "LVL", "RETRO"};
	return (m >= 0 && m < NUM_REC_MODES) ? names[m] : "MAN";
}

static const int STEPS_PER_BAR = 16;
static const float LVL_THRESHOLD_V = 0.158114f;   // -30 dBFS relative to 5 V

// What the module has to do in this sample.
struct RecEvent {
	bool start = false;      // begin writing into the record buffer
	bool stop = false;       // finish the recording (keep it)
	bool retro = false;      // take the last bars from the ring buffer
	bool cancelled = false;  // an armed CLK/LVL recording was cancelled
};

// State machine of the REC button / REC input.
// MAN: press starts, press stops. CLK: press arms, the next step starts, it stops by itself after `bars` bars
// (bars x 16 steps) or earlier on a press. LVL: press arms, the input crossing -30 dBFS starts, a press stops.
// RETRO: a press captures the last bars from the ring buffer at once. A press while armed cancels.
// Changing the mode while armed cancels as well; a running recording continues.
struct RecorderLogic {
	enum State {
		IDLE,
		ARMED,
		RECORDING
	};
	State state = IDLE;
	int activeMode = REC_MAN;
	int ticksLeft = 0;

	bool armed() const {
		return state == ARMED;
	}
	bool recording() const {
		return state == RECORDING;
	}
	void reset() {
		state = IDLE;
		ticksLeft = 0;
	}
	// The record buffer is full: stop without a new decision.
	void forceStop() {
		state = IDLE;
	}

	// mode: current MODE, recEdge: REC pressed or triggered, tick: a sequencer step happened in this sample,
	// level: max(|L|, |R|) of the input in volts, bars: 1, 2, 4 or 8.
	RecEvent process(int mode, bool recEdge, bool tick, float level, int bars) {
		RecEvent e;
		if (mode < 0 || mode >= NUM_REC_MODES)
			mode = REC_MAN;
		if (bars < 1)
			bars = 1;
		if (state == ARMED && mode != activeMode) {
			state = IDLE;
			e.cancelled = true;
		}
		if (recEdge) {
			if (state == RECORDING) {
				state = IDLE;
				e.stop = true;
				return e;
			}
			if (state == ARMED) {
				state = IDLE;
				e.cancelled = true;
				return e;
			}
			activeMode = mode;
			if (mode == REC_MAN) {
				state = RECORDING;
				e.start = true;
				return e;
			}
			if (mode == REC_RETRO) {
				e.retro = true;
				return e;
			}
			state = ARMED;   // CLK or LVL: wait (may start in this very sample below)
		}
		if (state == ARMED) {
			bool go = (activeMode == REC_CLK && tick) || (activeMode == REC_LVL && std::isfinite(level) && level >= LVL_THRESHOLD_V);
			if (go) {
				state = RECORDING;
				ticksLeft = bars * STEPS_PER_BAR;
				e.start = true;
			}
			return e;
		}
		if (state == RECORDING && activeMode == REC_CLK && tick) {
			if (--ticksLeft <= 0) {
				state = IDLE;
				e.stop = true;
			}
		}
		return e;
	}
};

// Duration of one sequencer step in samples, measured between ticks.
// A gap longer than 4 periods (stop, restart) keeps the old value.
struct StepPeriod {
	double since = 0.0;
	double period = 0.0;

	void process(bool tick) {
		since += 1.0;
		if (tick) {
			if (since > 1.0 && (period <= 0.0 || since <= 4.0 * period))
				period = since;
			since = 0.0;
		}
	}
	double get(double fallback) const {
		return period > 0.0 ? period : fallback;
	}
};

// Stereo ring buffer for RETRO. The audio thread writes every sample; the worker copies the newest frames.
struct RetroRing {
	float* left = nullptr;
	float* right = nullptr;
	int capacity = 0;
	int pos = 0;            // next write index
	int filled = 0;         // valid frames, up to capacity

	void attach(float* l, float* r, int cap) {
		left = l;
		right = r;
		capacity = cap;
		pos = 0;
		filled = 0;
	}
	bool ready() const {
		return left && right && capacity > 0;
	}
	void write(float l, float r) {
		if (!ready())
			return;
		left[pos] = l;
		right[pos] = r;
		if (++pos >= capacity)
			pos = 0;
		if (filled < capacity)
			filled++;
	}
	// Start index and length of the newest n frames (n is clamped to what is available).
	void newest(int n, int& start, int& length) const {
		if (n > filled)
			n = filled;
		if (n < 0)
			n = 0;
		length = n;
		start = pos - n;
		if (start < 0)
			start += capacity;
	}
	// Copies `length` frames from `start` (wrapping) into dl/dr.
	void copy(int start, int length, float* dl, float* dr) const {
		if (!ready())
			return;
		int i = start;
		for (int k = 0; k < length; k++) {
			dl[k] = left[i];
			dr[k] = right[i];
			if (++i >= capacity)
				i = 0;
		}
	}
};

} // namespace morel
