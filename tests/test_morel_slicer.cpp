// Standalone tests for the Morel auto-slicer (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_morel_slicer.cpp -o build/test_morel_slicer && ./build/test_morel_slicer
#include "dsp/MorelSlicer.hpp"
#include "dsp/MorelSequencer.hpp"
#include <cstdio>
#include <vector>
#include <cmath>

using namespace morel;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static const float SRS[] = {44100.f, 48000.f, 96000.f, 192000.f};

// Adds a decaying noise burst (tau 20 ms, 100 ms long) at time t with amplitude a.
static void addClick(std::vector<float>& buf, float sr, float t, float a, Rng& rng) {
	int start = (int) (t * sr);
	int n = (int) (0.1f * sr);
	for (int i = 0; i < n && start + i < (int) buf.size(); i++) {
		float env = std::exp(-(i / sr) / 0.02f);
		buf[start + i] += a * env * (rng.uniform() * 2.f - 1.f);
	}
}

static void testClicks() {
	for (float sr : SRS) {
		Rng rng;
		std::vector<float> buf((size_t) (2.0f * sr), 0.f);
		float times[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
		for (float t : times)
			addClick(buf, sr, t, 2.f, rng);
		SliceList sl;
		computeSlices(buf.data(), nullptr, (int) buf.size(), sr, 0.5f, sl);
		CHECK(sl.count == 7, "sr %.0f: 6 clicks + start = 7 slices, got %d", sr, sl.count);
		for (int i = 1; i < sl.count && i < 7; i++) {
			float t = sl.start[i] / sr;
			float onset = times[i - 1];
			CHECK(t <= onset + 0.002f && t >= onset - 0.006f, "sr %.0f: slice %d at %.4f s, onset %.3f", sr, i + 1, t, onset);
		}
	}
}

static void testSensMonotonic() {
	float sr = 48000.f;
	Rng rng;
	std::vector<float> buf((size_t) (2.0f * sr), 0.f);
	float levels[] = {2.f, 1.f, 0.2f, 0.02f, 0.5f, 0.05f};
	for (int i = 0; i < 6; i++)
		addClick(buf, sr, 0.25f + 0.25f * i, levels[i], rng);
	int prev = 0;
	for (int k = 0; k <= 10; k++) {
		SliceList sl;
		computeSlices(buf.data(), buf.data(), (int) buf.size(), sr, k / 10.f, sl);
		CHECK(sl.count >= prev || sl.count == FALLBACK_SLICES, "more SENS never gives fewer slices (sens %d0 %%: %d < %d)", k, sl.count, prev);
		if (sl.count != FALLBACK_SLICES)
			prev = sl.count;
	}
	SliceList hi;
	computeSlices(buf.data(), nullptr, (int) buf.size(), sr, 1.f, hi);
	CHECK(hi.count == 7, "SENS 100 %% finds all six clicks, got %d", hi.count);
	SliceList lo;
	computeSlices(buf.data(), nullptr, (int) buf.size(), sr, 0.f, lo);
	CHECK(lo.count == 2 || lo.count == FALLBACK_SLICES, "SENS 0 %% keeps only the loudest click, got %d", lo.count);
}

static void testHysteresis() {
	// Tone that starts at 0.5 s and ripples by about 3 dB at 20 Hz; the threshold sits inside the ripple.
	float sr = 48000.f;
	std::vector<float> buf((size_t) (1.5f * sr), 0.f);
	for (int i = (int) (0.5f * sr); i < (int) buf.size(); i++) {
		float t = i / sr;
		float am = 0.85f + 0.15f * std::sin(2.f * 3.14159265f * 20.f * t);
		buf[i] = am * std::sin(2.f * 3.14159265f * 440.f * t);
	}
	SliceList sl;
	computeSlices(buf.data(), nullptr, (int) buf.size(), sr, 1.5f / 48.f, sl);
	CHECK(sl.count == 2, "ripple inside the hysteresis gives one onset, got %d slices", sl.count);
}

static void testMinLength() {
	float sr = 48000.f;
	Rng rng;
	std::vector<float> buf((size_t) (1.0f * sr), 0.f);
	addClick(buf, sr, 0.3f, 2.f, rng);
	// Second, stronger hit 20 ms later. The first one decays only 20 ms, so make it short.
	int s2 = (int) (0.32f * sr);
	for (int i = 0; i < (int) (0.003f * sr); i++)
		buf[s2 + i] += 4.f * (rng.uniform() * 2.f - 1.f);
	SliceList sl;
	computeSlices(buf.data(), nullptr, (int) buf.size(), sr, 1.f, sl);
	for (int i = 1; i < sl.count; i++)
		CHECK(sl.start[i] - sl.start[i - 1] >= (int) (0.04f * sr) - 1, "slices are at least 40 ms long");
	CHECK(sl.length - sl.start[sl.count - 1] >= (int) (0.04f * sr) - 1, "last slice is at least 40 ms long");
}

static void testMaxSlices() {
	float sr = 44100.f;
	Rng rng;
	std::vector<float> buf((size_t) (11.0f * sr), 0.f);
	for (int i = 0; i < 50; i++)
		addClick(buf, sr, 0.2f + 0.2f * i, 1.f + 0.1f * i, rng);
	SliceList sl;
	computeSlices(buf.data(), nullptr, (int) buf.size(), sr, 0.5f, sl);
	CHECK(sl.count == MAX_SLICES, "at most 32 slices, got %d", sl.count);
	// The strongest (= latest) clicks stay: the 31 strongest are clicks 20..50, the first at 4.0 s.
	CHECK(sl.count > 1 && std::fabs(sl.start[1] / sr - 4.0f) < 0.01f, "strongest clicks kept, first at %.3f s", sl.count > 1 ? sl.start[1] / sr : -1.f);
	for (int i = 1; i < sl.count; i++)
		CHECK(sl.start[i] > sl.start[i - 1], "starts ascending");
}

static void testFallback() {
	float sr = 48000.f;
	std::vector<float> sine((size_t) sr, 0.f);
	for (size_t i = 0; i < sine.size(); i++)
		sine[i] = std::sin(2.f * 3.14159265f * 220.f * i / sr);
	SliceList sl;
	computeSlices(sine.data(), nullptr, (int) sine.size(), sr, 0.5f, sl);
	CHECK(sl.count == FALLBACK_SLICES, "steady tone falls back to 8 equal slices, got %d", sl.count);
	CHECK(sl.start[4] == (int) sine.size() / 2, "equal slices");
	std::vector<float> silence((size_t) sr, 0.f);
	computeSlices(silence.data(), silence.data(), (int) silence.size(), sr, 0.5f, sl);
	CHECK(sl.count == FALLBACK_SLICES, "silence falls back to 8 slices");
	std::vector<float> shortRec((size_t) (0.1f * sr), 0.f);
	computeSlices(shortRec.data(), nullptr, (int) shortRec.size(), sr, 0.5f, sl);
	CHECK(sl.count == 2, "100 ms recording gives two 50 ms slices, got %d", sl.count);
	computeSlices(nullptr, nullptr, 0, sr, 0.5f, sl);
	CHECK(sl.count == 0, "empty recording has no slices");
	CHECK(sl.sliceEnd(0) == 0 && sl.sliceStart(5) == 0, "out-of-range queries are safe");
}

static void testNonFinite() {
	float sr = 48000.f;
	Rng rng;
	std::vector<float> buf((size_t) sr, 0.f);
	addClick(buf, sr, 0.5f, 2.f, rng);
	buf[100] = std::nanf("");
	buf[200] = INFINITY;
	SliceList sl;
	computeSlices(buf.data(), nullptr, (int) buf.size(), sr, std::nanf(""), sl);
	CHECK(sl.count == 2, "NaN/Inf samples and NaN SENS are handled, got %d", sl.count);
}

int main() {
	testClicks();
	testSensMonotonic();
	testHysteresis();
	testMinLength();
	testMaxSlices();
	testFallback();
	testNonFinite();
	if (failures == 0)
		std::printf("All Morel slicer tests passed.\n");
	return failures == 0 ? 0 : 1;
}
