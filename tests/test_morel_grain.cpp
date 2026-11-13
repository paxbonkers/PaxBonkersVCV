// Standalone tests for the Morel GRAIN engine (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_morel_grain.cpp -o build/test_morel_grain && ./build/test_morel_grain
#include "dsp/MorelGrain.hpp"
#include <cstdio>
#include <vector>
#include <cmath>

using namespace morel;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static const float SRS[] = {44100.f, 48000.f, 96000.f, 192000.f};

static SampleRef ref(const std::vector<float>& d, float sr, int id = 0) {
	SampleRef s;
	s.left = s.right = d.data();
	s.length = (int) d.size();
	s.sampleRate = sr;
	s.id = id;
	return s;
}

static void testGrainSize() {
	float sr = 48000.f;
	CHECK(std::fabs(grainSize(0.01f, sr, sr) - 0.01 * sr) < 1.0, "SPAN 1 %% = 10 ms");
	CHECK(std::fabs(grainSize(1.f, sr, sr) - sr) < 1.0, "SPAN 100 %% = whole slice");
	double mid = grainSize(0.5f, sr, sr);
	CHECK(mid > 0.01 * sr && mid < sr, "SPAN 50 %% in between");
	CHECK(grainSize(0.3f, 100.0, sr) == 100.0, "slice shorter than 10 ms: grain = slice");
	CHECK(std::isfinite(grainSize(std::nanf(""), sr, sr)), "NaN SPAN is safe");
}

// Constant level: a DC recording must give a (nearly) constant output with 2x Hann overlap.
static void testConstantLevel() {
	for (float sr : SRS) {
		std::vector<float> dc((size_t) sr, 1.f);
		for (float span : {0.05f, 0.3f, 0.7f}) {
			GrainCloud c;
			c.setSlice(ref(dc, sr), 0.0, sr * 0.5);
			float l, r, lo = 10.f, hi = -10.f;
			int n = (int) (1.0f * sr);
			for (int i = 0; i < n; i++) {
				c.process(span, 0.f, 1.f, sr, l, r);
				if (i > n / 2) {
					lo = std::fmin(lo, l);
					hi = std::fmax(hi, l);
				}
			}
			CHECK(lo > 0.93f && hi < 1.07f, "sr %.0f span %.2f: level %.3f .. %.3f", sr, span, lo, hi);
		}
	}
}

static void testEosAndScan() {
	float sr = 48000.f;
	std::vector<float> dc((size_t) sr, 1.f);
	GrainCloud c;
	c.setSlice(ref(dc, sr), 0.0, sr * 0.1);   // 100 ms slice, scan at 25 % -> wraps every 400 ms
	float l, r;
	int eos = 0, first = -1;
	for (int i = 0; i < (int) (1.0f * sr); i++)
		if (c.process(0.2f, 0.f, 1.f, sr, l, r)) {
			eos++;
			if (first < 0)
				first = i;
		}
	CHECK(eos == 2, "read head wraps twice per second, got %d", eos);
	CHECK(std::abs(first - (int) (0.4f * sr)) < 2, "first wrap after 400 ms, got %d samples", first);
	CHECK(c.playhead() >= 0.f && c.playhead() <= 0.1f, "playhead stays inside the slice");
}

static void testSliceChangeAndStop() {
	float sr = 48000.f;
	std::vector<float> dc((size_t) sr, 1.f);
	GrainCloud c;
	c.setSlice(ref(dc, sr), 0.0, sr * 0.3);
	float l, r, last = 0.f, maxJump = 0.f;
	for (int i = 0; i < (int) (0.5f * sr); i++) {
		if (i == (int) (0.25f * sr))
			c.setSlice(ref(dc, sr), sr * 0.5, sr * 0.8);
		c.process(0.4f, 0.f, 1.f, sr, l, r);
		if (i > 2000)
			maxJump = std::fmax(maxJump, std::fabs(l - last));
		last = l;
	}
	CHECK(maxJump < 0.02f, "no jump at the slice change (max %.4f)", maxJump);
	// stop spawning: sound dies out within one grain
	c.stopSpawning();
	for (int i = 0; i < (int) (0.5f * sr); i++)
		c.process(0.4f, 0.f, 1.f, sr, l, r);
	CHECK(l == 0.f && c.playhead() < 0.f, "silent after the last grain finished");
	c.setSlice(ref(dc, sr, 1), 0.0, 1000.0);
	c.process(0.4f, 0.f, 1.f, sr, l, r);
	c.release(1);
	c.process(0.4f, 0.f, 1.f, sr, l, r);
	CHECK(l == 0.f, "release silences grains of a rewritten buffer");
}

static void testExtremes() {
	float sr = 48000.f;
	std::vector<float> sine((size_t) sr);
	for (size_t i = 0; i < sine.size(); i++)
		sine[i] = std::sin(2.f * 3.14159265f * 440.f * i / sr);
	float spans[] = {0.01f, 1.f}, scatters[] = {0.f, 1.f}, speeds[] = {1.f / 32.f, 1.f, 32.f};
	bool finite = true;
	float peak = 0.f;
	for (float sp : spans)
		for (float sc : scatters)
			for (float spd : speeds) {
				GrainCloud c;
				c.setSlice(ref(sine, sr), 1000.0, 30000.0);
				float l, r;
				for (int i = 0; i < 48000; i++) {
					c.process(sp, sc, spd, sr, l, r);
					finite = finite && std::isfinite(l) && std::isfinite(r);
					peak = std::fmax(peak, std::fabs(l));
				}
			}
	CHECK(finite, "finite output at all extremes");
	CHECK(peak < 2.5f, "peak bounded (%.3f)", peak);
	GrainCloud c;
	float l, r;
	c.setSlice(SampleRef(), 0.0, 100.0);
	c.process(0.5f, 0.5f, 1.f, sr, l, r);
	CHECK(l == 0.f, "no data: silent");
	c.setSlice(ref(sine, sr), 0.0, 1000.0);
	CHECK(!c.process(0.5f, 0.5f, std::nanf(""), sr, l, r) && l == 0.f, "NaN speed is safe");
}

static void testScatterReproducible() {
	float sr = 48000.f;
	std::vector<float> ramp((size_t) sr);
	for (size_t i = 0; i < ramp.size(); i++)
		ramp[i] = (float) i / sr;
	GrainCloud a, b;
	a.setSlice(ref(ramp, sr), 0.0, sr * 0.5);
	b.setSlice(ref(ramp, sr), 0.0, sr * 0.5);
	b.rng.seed(777);
	float la, ra, lb, rb, diff = 0.f;
	for (int i = 0; i < 24000; i++) {
		a.process(0.2f, 0.f, 1.f, sr, la, ra);
		b.process(0.2f, 0.f, 1.f, sr, lb, rb);
		diff = std::fmax(diff, std::fabs(la - lb));
	}
	CHECK(diff < 1e-6f, "SCATTER 0: random seed has no influence (diff %.6f)", diff);
}

int main() {
	testGrainSize();
	testConstantLevel();
	testEosAndScan();
	testSliceChangeAndStop();
	testExtremes();
	testScatterReproducible();
	if (failures == 0)
		std::printf("All Morel grain tests passed.\n");
	return failures == 0 ? 0 : 1;
}
