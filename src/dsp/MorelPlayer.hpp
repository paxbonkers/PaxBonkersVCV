#pragma once
#include <cmath>

// CUT playback of Morel: plays the front part (SPAN) of a slice once, with short fades, and
// crossfades when a new step arrives while the previous slice still sounds.
// Rack-independent, unit-tested in tests/test_morel_player.cpp. See docs/modules/Morel-Signalvertrag.md.
// Real-time safe: no allocation, no I/O.

namespace morel {

static const float FADE_IN_SEC = 0.0005f;
static const float FADE_OUT_SEC = 0.003f;
static const float XFADE_SEC = 0.003f;

// A recording as seen by the player. The data must stay valid while a voice plays it.
struct SampleRef {
	const float* left = nullptr;
	const float* right = nullptr;   // never null if left is set (mono recordings point both to the same data)
	int length = 0;
	float sampleRate = 48000.f;
	int id = -1;                    // buffer index, used to silence voices when a buffer is rewritten
};

// 4-point Hermite interpolation, indices clamped to the data.
inline float hermite(const float* d, int len, double pos) {
	int i = (int) std::floor(pos);
	float t = (float) (pos - i);
	int i0 = i - 1, i1 = i, i2 = i + 1, i3 = i + 2;
	if (i0 < 0) i0 = 0;
	if (i1 < 0) i1 = 0;
	if (i2 < 0) i2 = 0;
	if (i3 < 0) i3 = 0;
	if (i0 >= len) i0 = len - 1;
	if (i1 >= len) i1 = len - 1;
	if (i2 >= len) i2 = len - 1;
	if (i3 >= len) i3 = len - 1;
	float xm1 = d[i0], x0 = d[i1], x1 = d[i2], x2 = d[i3];
	float c1 = 0.5f * (x1 - xm1);
	float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
	float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
	return ((c3 * t + c2) * t + c1) * t + x0;
}

struct CutVoice {
	bool active = false;
	bool killing = false;
	SampleRef s;
	double pos = 0.0;
	double end = 0.0;
	float pitchRatio = 1.f;   // recording rate / engine rate * scatter pitch
	float level = 1.f;
	float fadeIn = 0.f;
	float kill = 1.f;
};

struct CutPlayer {
	CutVoice voices[2];
	int cur = 0;

	void stopAll() {
		voices[0].active = voices[1].active = false;
	}

	// Silences every voice that reads buffer id (before that buffer is overwritten).
	void release(int id) {
		for (int i = 0; i < 2; i++)
			if (voices[i].active && voices[i].s.id == id)
				voices[i].active = false;
	}

	// Starts [start, end) of the sample. semis: pitch offset of this hit, level: gain of this hit.
	void trigger(const SampleRef& s, double start, double end, float semis, float level, float engineRate) {
		CutVoice& old = voices[cur];
		if (old.active)
			old.killing = true;
		cur ^= 1;
		CutVoice& v = voices[cur];
		v = CutVoice();
		if (!s.left || s.length <= 1 || !(engineRate > 0.f))
			return;
		if (start < 0.0)
			start = 0.0;
		if (end > s.length)
			end = s.length;
		if (end - start < 1.0)
			return;
		v.active = true;
		v.s = s;
		v.pos = start;
		v.end = end;
		v.pitchRatio = s.sampleRate / engineRate * std::pow(2.f, semis / 12.f);
		v.level = level;
	}

	// Position (0..1 of the recording) of the current voice, -1 if silent.
	float playhead() const {
		const CutVoice& v = voices[cur];
		if (!v.active || v.killing || v.s.length <= 0)
			return -1.f;
		return (float) (v.pos / v.s.length);
	}

	// speedMul: playback speed from SPEED and V/OCT (2^octaves).
	// Returns true when the current voice reached the end of its slice / SPAN cut (EOS).
	bool process(float speedMul, float engineRate, float& outL, float& outR) {
		outL = outR = 0.f;
		bool eos = false;
		float inInc = 1.f / (FADE_IN_SEC * engineRate);
		float killDec = 1.f / (XFADE_SEC * engineRate);
		float fadeOutSamples = FADE_OUT_SEC * engineRate;
		for (int i = 0; i < 2; i++) {
			CutVoice& v = voices[i];
			if (!v.active)
				continue;
			double rate = (double) v.pitchRatio * speedMul;
			if (!(rate > 0.0)) {
				v.active = false;
				continue;
			}
			float remaining = (float) ((v.end - v.pos) / rate);   // in output samples
			float g = v.level * v.fadeIn * v.kill * std::fmin(1.f, remaining / fadeOutSamples);
			if (g < 0.f)
				g = 0.f;
			outL += g * hermite(v.s.left, v.s.length, v.pos);
			outR += g * hermite(v.s.right, v.s.length, v.pos);
			v.fadeIn = std::fmin(1.f, v.fadeIn + inInc);
			if (v.killing) {
				v.kill -= killDec;
				if (v.kill <= 0.f)
					v.active = false;
			}
			v.pos += rate;
			if (v.pos >= v.end) {
				if (!v.killing && i == cur)
					eos = true;
				v.active = false;
			}
		}
		return eos;
	}
};

} // namespace morel
