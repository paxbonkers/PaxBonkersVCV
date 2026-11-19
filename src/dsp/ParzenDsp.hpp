// Parzen FX - Rack-independent DSP building blocks (drive, distortion, bit crusher,
// lane crossfade, scope capture). Signals are normalised: 1.0 = 5 V.
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace parzen {

static const float PI_F = 3.14159265358979f;

inline float dbToGain(float db) {
	return std::pow(10.f, db / 20.f);
}

// Level knob in dB; the bottom end of the range means "off" (-inf).
inline float levelGain(float db, float offBelowDb = -59.5f) {
	return db <= offBelowDb ? 0.f : dbToGain(db);
}

inline float clampf(float x, float lo, float hi) {
	return std::max(lo, std::min(hi, x));
}

// Adds CV to a knob value: 1 V = 10 % of the knob range, result clamped to the range.
inline float withCv(float knob, float cvVolts, float lo, float hi) {
	return clampf(knob + cvVolts * 0.1f * (hi - lo), lo, hi);
}

// ------------------------------------------------------------------ DRIVE (soft saturation)
// y = (tanh(g x + b) - tanh(b)) / tanh(g)
// - b shifts the curve: asymmetric clipping, even harmonics. Subtracting tanh(b) removes the static offset.
// - dividing by tanh(g) keeps a full-scale (5 V) input at about full scale, so more drive changes
//   the character more than the level.
inline float driveShape(float x, float gain, float bias) {
	float norm = std::tanh(gain);
	if (norm < 1e-6f)
		norm = 1e-6f;
	return (std::tanh(gain * x + bias) - std::tanh(bias)) / norm;
}

// ------------------------------------------------------------------ DIST (hard clip -> wavefolder)
// shape 0 = hard clip, 1 = sine wavefolder, crossfaded in between. Output stays within [-1, 1].
inline float distShape(float x, float gain, float shape) {
	float v = gain * x;
	float clip = clampf(v, -1.f, 1.f);
	float fold = std::sin(0.5f * PI_F * v);
	return clip + shape * (fold - clip);
}

// One-pole DC blocker (used after DRIVE, where BIAS creates signal-dependent DC).
struct DcBlocker {
	float x1 = 0.f, y1 = 0.f, r = 0.9993f;
	void setCutoff(float hz, float sampleRate) {
		r = 1.f - 2.f * PI_F * hz / sampleRate;
		r = clampf(r, 0.9f, 0.99999f);
	}
	void reset() {
		x1 = y1 = 0.f;
	}
	float process(float x) {
		float y = x - x1 + r * y1;
		x1 = x;
		y1 = y;
		return y;
	}
};

// ------------------------------------------------------------------ CRUSH (sample & hold + quantiser)
// rate 0 = full sample rate, 1 = 200 Hz (exponential). bits 1..16, continuous.
inline float crushHoldFreq(float rate, float sampleRate) {
	float lo = std::min(200.f, sampleRate);
	return sampleRate * std::pow(lo / sampleRate, clampf(rate, 0.f, 1.f));
}

inline float quantise(float x, float bits) {
	float q = std::pow(2.f, clampf(bits, 1.f, 16.f) - 1.f);   // steps per polarity
	return std::round(x * q) / q;
}

struct Crusher {
	float phase = 1.f;   // >= 1 means: take a new sample now
	float held = 0.f;
	void reset() {
		phase = 1.f;
		held = 0.f;
	}
	float process(float x, float bits, float rate, float sampleRate) {
		float inc = crushHoldFreq(rate, sampleRate) / sampleRate;
		if (phase >= 1.f) {
			phase -= std::floor(phase);
			held = x;
		}
		phase += inc;
		return quantise(held, bits);
	}
};

// ------------------------------------------------------------------ crossfade between configurations
// Configurations: 0 = SIM (full chain), 1..3 = SEQ lane DRIVE / DIST / CRUSH.
// A change starts a linear fade from the current mix to the new target. A change during a fade
// continues from the current audible mix (no jump): the old pair is frozen into "from".
struct ConfigFader {
	int from = 1, to = 1;
	float pos = 1.f;      // 0 = only "from", 1 = only "to"
	float step = 0.f;     // per sample
	// While a fade is interrupted we keep the previous mix as a fixed blend of two configs.
	int prevA = 1, prevB = 1;
	float prevPos = 1.f;
	bool frozen = false;

	void reset(int cfg) {
		from = to = prevA = prevB = cfg;
		pos = prevPos = 1.f;
		frozen = false;
	}
	void start(int cfg, float fadeSeconds, float sampleRate) {
		if (cfg == to && pos >= 1.f)
			return;
		if (pos < 1.f) {
			// interrupt: remember the audible blend of (from, to) as the new source
			prevA = from;
			prevB = to;
			prevPos = pos;
			frozen = true;
		}
		else {
			frozen = false;
		}
		from = to;
		to = cfg;
		pos = 0.f;
		step = 1.f / std::max(1.f, fadeSeconds * sampleRate);
	}
	bool fading() const {
		return pos < 1.f;
	}
	// Mix the outputs of all four configurations.
	float mix(const float* cfgOut) {
		float src = frozen ? cfgOut[prevA] + prevPos * (cfgOut[prevB] - cfgOut[prevA]) : cfgOut[from];
		float y = src + pos * (cfgOut[to] - src);
		return y;
	}
	void advance() {
		if (pos < 1.f) {
			pos += step;
			if (pos >= 1.f) {
				pos = 1.f;
				frozen = false;
				from = to;
			}
		}
	}
	// Weight of a configuration in the current mix (for lamps).
	float weight(int cfg) const {
		float w = 0.f;
		if (frozen) {
			if (prevA == cfg) w += (1.f - pos) * (1.f - prevPos);
			if (prevB == cfg) w += (1.f - pos) * prevPos;
		}
		else if (from == cfg) {
			w += 1.f - pos;
		}
		if (to == cfg) w += pos;
		return w;
	}
};

// ------------------------------------------------------------------ scope capture
// Captures COLS columns of min/max for three signals (input L, output L, output R). A capture starts on a rising
// zero crossing of the input, or after a timeout (free running). Finished frames are published
// into one of two buffers; `front` names the buffer the UI may read. Only the display reads it,
// so a torn frame is harmless.
template <int COLS>
struct ScopeCapture {
	struct Frame {
		float inMin[COLS], inMax[COLS], outMin[COLS], outMax[COLS], out2Min[COLS], out2Max[COLS];
	};
	Frame frames[2];
	int writeIdx = 0;
	int front = -1;          // read with an atomic in the module; plain here for testability
	int col = 0;
	int sub = 0;
	int samplesPerCol = 1;
	bool capturing = false;
	int waited = 0;
	float lastIn = 0.f;
	float cMinI = 0.f, cMaxI = 0.f, cMinO = 0.f, cMaxO = 0.f, cMinO2 = 0.f, cMaxO2 = 0.f;

	void setWindow(float seconds, float sampleRate) {
		samplesPerCol = std::max(1, (int) std::lround(seconds * sampleRate / COLS));
	}
	void reset() {
		capturing = false;
		col = sub = waited = 0;
		lastIn = 0.f;
	}
	// Returns true when a frame was completed (and published).
	bool process(float in, float out, float out2) {
		if (!capturing) {
			bool rising = lastIn <= 0.f && in > 0.f;
			lastIn = in;
			waited++;
			if (rising || waited > 2 * samplesPerCol * COLS) {
				capturing = true;
				col = sub = waited = 0;
			}
			else {
				return false;
			}
		}
		if (sub == 0) {
			cMinI = cMaxI = in;
			cMinO = cMaxO = out;
			cMinO2 = cMaxO2 = out2;
		}
		else {
			cMinI = std::min(cMinI, in);
			cMaxI = std::max(cMaxI, in);
			cMinO = std::min(cMinO, out);
			cMaxO = std::max(cMaxO, out);
			cMinO2 = std::min(cMinO2, out2);
			cMaxO2 = std::max(cMaxO2, out2);
		}
		if (++sub >= samplesPerCol) {
			Frame& f = frames[writeIdx];
			f.inMin[col] = cMinI;
			f.inMax[col] = cMaxI;
			f.outMin[col] = cMinO;
			f.outMax[col] = cMaxO;
			f.out2Min[col] = cMinO2;
			f.out2Max[col] = cMaxO2;
			sub = 0;
			if (++col >= COLS) {
				front = writeIdx;
				writeIdx ^= 1;
				capturing = false;
				lastIn = in;
				return true;
			}
		}
		return false;
	}
};

} // namespace parzen
