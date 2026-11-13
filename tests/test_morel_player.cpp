// Standalone tests for the Morel CUT player (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_morel_player.cpp -o build/test_morel_player && ./build/test_morel_player
#include "dsp/MorelPlayer.hpp"
#include <cstdio>
#include <vector>
#include <cmath>

using namespace morel;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static const float SRS[] = {44100.f, 48000.f, 96000.f, 192000.f};

static SampleRef ref(const std::vector<float>& d, float sr) {
	SampleRef s;
	s.left = s.right = d.data();
	s.length = (int) d.size();
	s.sampleRate = sr;
	s.id = 0;
	return s;
}

// Plays n output samples; returns number of samples with |out| > thr, max jump, and EOS count.
struct Run {
	int sounding = 0;
	float maxJump = 0.f;
	int eos = 0;
	float maxAbs = 0.f;
};
static Run play(CutPlayer& p, int n, float speed, float sr, float& last) {
	Run r;
	for (int i = 0; i < n; i++) {
		float l, rr;
		if (p.process(speed, sr, l, rr))
			r.eos++;
		if (std::fabs(l) > 1e-4f)
			r.sounding++;
		r.maxJump = std::fmax(r.maxJump, std::fabs(l - last));
		r.maxAbs = std::fmax(r.maxAbs, std::fabs(l));
		last = l;
	}
	return r;
}

static void testSpanAndFades() {
	for (float sr : SRS) {
		std::vector<float> dc((size_t) sr, 1.f);   // 1 s of DC: any missing fade shows as a jump
		SampleRef s = ref(dc, sr);
		CutPlayer p;
		float last = 0.f;
		// Slice of 0.4 s, SPAN 25 % -> 0.1 s.
		p.trigger(s, 0.0, 0.1 * sr, 0.f, 1.f, sr);
		Run r = play(p, (int) (0.3f * sr), 1.f, sr, last);
		float dur = r.sounding / sr;
		CHECK(std::fabs(dur - 0.1f) < 0.002f, "sr %.0f: SPAN 25 %% of 0.4 s plays 0.1 s, got %.4f", sr, dur);
		CHECK(r.eos == 1, "sr %.0f: one EOS at the SPAN cut, got %d", sr, r.eos);
		float maxStep = 1.f / (FADE_IN_SEC * sr) + 1e-3f;
		CHECK(r.maxJump <= maxStep, "sr %.0f: no jumps at start/end (max %.4f)", sr, r.maxJump);
	}
}

static void testCrossfade() {
	float sr = 48000.f;
	std::vector<float> dc((size_t) sr, 1.f);
	SampleRef s = ref(dc, sr);
	CutPlayer p;
	float last = 0.f;
	p.trigger(s, 0.0, 0.5 * sr, 0.f, 1.f, sr);
	Run a = play(p, (int) (0.05f * sr), 1.f, sr, last);
	p.trigger(s, 0.6 * sr, 0.9 * sr, 0.f, 1.f, sr);
	Run b = play(p, (int) (0.4f * sr), 1.f, sr, last);
	CHECK(a.eos == 0, "no EOS before the step change");
	CHECK(b.eos == 1, "the interrupted slice gives no EOS, the new one does (got %d)", b.eos);
	float maxStep = 1.f / (FADE_IN_SEC * sr) + 1e-3f;
	CHECK(b.maxJump <= maxStep, "no jump at the step change (max %.4f)", b.maxJump);
	CHECK(b.maxAbs < 2.f, "overlap stays below double level (max %.3f)", b.maxAbs);
}

static void testSpeedAndRate() {
	float sr = 48000.f;
	std::vector<float> dc((size_t) sr, 1.f);
	CutPlayer p;
	float last = 0.f;
	p.trigger(ref(dc, sr), 0.0, 0.2 * sr, 0.f, 1.f, sr);
	Run r = play(p, (int) (0.3f * sr), 2.f, sr, last);
	CHECK(std::fabs(r.sounding / sr - 0.1f) < 0.002f, "+1 octave halves the duration, got %.4f", r.sounding / sr);
	// Recording made at 96 kHz, played at 48 kHz: duration stays 0.2 s.
	std::vector<float> dc96((size_t) 96000, 1.f);
	p.trigger(ref(dc96, 96000.f), 0.0, 0.2 * 96000.0, 0.f, 1.f, sr);
	r = play(p, (int) (0.3f * sr), 1.f, sr, last);
	CHECK(std::fabs(r.sounding / sr - 0.2f) < 0.002f, "rate correction keeps the duration, got %.4f", r.sounding / sr);
	// Scatter pitch +12 semitones also halves.
	p.trigger(ref(dc, sr), 0.0, 0.2 * sr, 12.f, 0.5f, sr);
	r = play(p, (int) (0.3f * sr), 1.f, sr, last);
	CHECK(std::fabs(r.sounding / sr - 0.1f) < 0.002f, "+12 semitones halves the duration");
	CHECK(std::fabs(r.maxAbs - 0.5f) < 1e-3f, "hit level is applied");
}

static void testEdgeCases() {
	float sr = 48000.f;
	std::vector<float> dc(1000, 1.f);
	CutPlayer p;
	float l, r;
	p.trigger(ref(dc, sr), 500.0, 500.0, 0.f, 1.f, sr);
	CHECK(!p.voices[p.cur].active, "empty range does not start");
	p.trigger(ref(dc, sr), -100.0, 5000.0, 0.f, 1.f, sr);
	CHECK(p.voices[p.cur].active && p.voices[p.cur].end == 1000.0, "range is clamped to the recording");
	SampleRef none;
	p.trigger(none, 0.0, 100.0, 0.f, 1.f, sr);
	for (int i = 0; i < 2000; i++)
		p.process(1.f, sr, l, r);
	CHECK(std::isfinite(l) && l == 0.f, "silent without data");
	p.trigger(ref(dc, sr), 0.0, 1000.0, 0.f, 1.f, sr);
	p.release(0);
	p.process(1.f, sr, l, r);
	CHECK(l == 0.f, "release silences voices of a rewritten buffer");
	p.trigger(ref(dc, sr), 0.0, 1000.0, 0.f, 1.f, sr);
	p.process(std::nanf(""), sr, l, r);
	CHECK(std::isfinite(l), "NaN speed is safe");
}

int main() {
	testSpanAndFades();
	testCrossfade();
	testSpeedAndRate();
	testEdgeCases();
	if (failures == 0)
		std::printf("All Morel player tests passed.\n");
	return failures == 0 ? 0 : 1;
}
