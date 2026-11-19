// Standalone tests for the Parzen FX DSP blocks (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Isrc tests/test_parzen_dsp.cpp -o build/test_parzen_dsp && ./build/test_parzen_dsp
#include "dsp/ParzenDsp.hpp"
#include <cstdio>
#include <cmath>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

using namespace parzen;

static void testDrive() {
	// Full-scale input stays near full scale at every drive setting (level compensation).
	for (float db = 0.f; db <= 36.f; db += 6.f) {
		float g = dbToGain(db);
		float y = driveShape(1.f, g, 0.f);
		CHECK(y > 0.99f && y < 1.01f, "drive %.0f dB: full scale -> %.3f", db, y);
		CHECK(std::fabs(driveShape(-1.f, g, 0.f) + y) < 1e-5f, "drive %.0f dB not symmetric at bias 0", db);
	}
	// Zero input gives zero output, also with bias.
	for (float b = -1.f; b <= 1.f; b += 0.5f)
		CHECK(std::fabs(driveShape(0.f, 4.f, b)) < 1e-6f, "bias %.1f: offset at zero input", b);
	// Bias makes the curve asymmetric.
	float pos = driveShape(1.f, 4.f, 0.5f), neg = driveShape(-1.f, 4.f, 0.5f);
	CHECK(std::fabs(pos + neg) > 0.05f, "bias 0.5 should be asymmetric (%.3f / %.3f)", pos, neg);
	// Monotonic.
	float last = -10.f;
	for (float x = -2.f; x <= 2.f; x += 0.01f) {
		float y = driveShape(x, 8.f, 0.3f);
		CHECK(y >= last - 1e-6f, "drive not monotonic at %.2f", x);
		last = y;
	}
}

static void testDist() {
	for (float s = 0.f; s <= 1.f; s += 0.25f)
		for (float x = -3.f; x <= 3.f; x += 0.01f) {
			float y = distShape(x, dbToGain(40.f), s);
			CHECK(std::isfinite(y) && std::fabs(y) <= 1.0001f, "dist out of range: x %.2f shape %.2f -> %f", x, s, y);
		}
	CHECK(std::fabs(distShape(0.5f, 1.f, 0.f) - 0.5f) < 1e-6f, "hard clip should pass small signals");
	CHECK(std::fabs(distShape(2.f, 1.f, 0.f) - 1.f) < 1e-6f, "hard clip should clip at 1");
	// Full fold: input 2 (after gain) folds back to sin(pi) = 0.
	CHECK(std::fabs(distShape(2.f, 1.f, 1.f)) < 1e-5f, "folder at 2 should be 0");
}

static void testCrush() {
	// 16 bits: quantisation error below 1/32768.
	for (float x = -1.f; x <= 1.f; x += 0.0137f)
		CHECK(std::fabs(quantise(x, 16.f) - x) <= 1.f / 32768.f + 1e-7f, "16 bit error too large at %.4f", x);
	// 1 bit: only -1, 0, 1.
	for (float x = -1.f; x <= 1.f; x += 0.05f) {
		float q = quantise(x, 1.f);
		CHECK(q == -1.f || q == 0.f || q == 1.f, "1 bit gave %.3f", q);
	}
	// Hold frequency: full rate at 0, 200 Hz at 1, for several sample rates.
	const float rates[] = {44100.f, 48000.f, 96000.f, 192000.f};
	for (float sr : rates) {
		CHECK(std::fabs(crushHoldFreq(0.f, sr) - sr) < 1e-2f * sr, "rate 0 at %.0f", sr);
		CHECK(std::fabs(crushHoldFreq(1.f, sr) - 200.f) < 0.5f, "rate 1 at %.0f = %.2f", sr, crushHoldFreq(1.f, sr));
		// Count sample changes over one second at rate 1 with a ramp input -> about 200 updates.
		Crusher c;
		int changes = 0;
		float prev = -100.f;
		for (int i = 0; i < (int) sr; i++) {
			float y = c.process((float) i / sr, 16.f, 1.f, sr);
			if (y != prev) changes++;
			prev = y;
		}
		CHECK(changes >= 198 && changes <= 202, "S&H at %.0f Hz: %d updates per second", sr, changes);
		// Full rate passes every sample.
		Crusher c2;
		bool same = true;
		for (int i = 0; i < 1000; i++) {
			float x = std::sin(i * 0.01f);
			if (std::fabs(c2.process(x, 16.f, 0.f, sr) - x) > 1.f / 32768.f + 1e-6f) same = false;
		}
		CHECK(same, "full rate, 16 bit should be transparent at %.0f", sr);
	}
}

static void testFader() {
	const float sr = 48000.f;
	float outs[4] = {0.f, 1.f, 2.f, 3.f};
	ConfigFader f;
	f.reset(1);
	CHECK(f.mix(outs) == 1.f, "reset mix");
	f.start(2, 0.005f, sr);
	int n = (int) (0.005f * sr);
	float last = f.mix(outs);
	bool monotonic = true;
	for (int i = 0; i < n + 2; i++) {
		f.advance();
		float y = f.mix(outs);
		if (y < last - 1e-6f) monotonic = false;
		last = y;
	}
	CHECK(monotonic, "fade 1->2 not monotonic");
	CHECK(std::fabs(f.mix(outs) - 2.f) < 1e-6f && !f.fading(), "fade 1->2 did not finish (%.3f)", f.mix(outs));
	// Interrupt in the middle: no jump at the moment of the new trigger.
	f.start(3, 0.005f, sr);
	for (int i = 0; i < n / 2; i++) f.advance();
	float before = f.mix(outs);
	f.start(1, 0.005f, sr);
	float after = f.mix(outs);
	CHECK(std::fabs(before - after) < 1e-5f, "jump on interrupted fade: %.4f -> %.4f", before, after);
	for (int i = 0; i < n + 2; i++) f.advance();
	CHECK(std::fabs(f.mix(outs) - 1.f) < 1e-6f, "interrupted fade did not end at target (%.3f)", f.mix(outs));
	// Weights add up to 1 during an interrupted fade.
	f.start(2, 0.005f, sr);
	for (int i = 0; i < n / 3; i++) f.advance();
	f.start(3, 0.005f, sr);
	for (int i = 0; i < n / 3; i++) f.advance();
	float sum = 0.f;
	for (int c = 0; c < 4; c++) sum += f.weight(c);
	CHECK(std::fabs(sum - 1.f) < 1e-5f, "weights sum %.4f", sum);
}

static void testScope() {
	const float sr = 48000.f;
	ScopeCapture<216> s;
	s.setWindow(0.01f, sr);
	s.reset();
	// 100 Hz sine: a frame must start on a rising zero crossing, so the first column is near 0 and rising.
	int frames = 0;
	for (int i = 0; i < (int) sr; i++) {
		float x = std::sin(2.f * PI_F * 100.f * i / sr + 1.f);
		if (s.process(x, -x, 0.5f * x)) frames++;
	}
	CHECK(frames > 10, "too few scope frames: %d", frames);
	CHECK(s.front >= 0, "no frame published");
	const auto& fr = s.frames[s.front];
	CHECK(fr.inMin[0] >= -0.05f && fr.inMax[0] <= 0.1f, "frame does not start at the zero crossing (%.3f..%.3f)", fr.inMin[0], fr.inMax[0]);
	CHECK(fr.outMax[0] <= 0.05f, "output column wrong");
	CHECK(fr.out2Min[0] >= -0.03f && fr.out2Max[0] <= 0.05f, "second output column wrong");
	bool r2 = true;
	for (int i = 0; i < 216; i++)
		if (std::fabs(fr.out2Max[i] - 0.5f * fr.inMax[i]) > 1e-5f) r2 = false;
	CHECK(r2, "second output not captured alongside the input");
	// Silence: free running after the timeout.
	ScopeCapture<216> q;
	q.setWindow(0.01f, sr);
	int qf = 0;
	for (int i = 0; i < (int) sr; i++)
		if (q.process(0.f, 0.f, 0.f)) qf++;
	CHECK(qf > 5, "scope does not free-run on silence (%d frames)", qf);
}

static void testFinite() {
	// Extreme settings never produce NaN/Inf.
	const float xs[] = {-10.f, -1.f, -1e-6f, 0.f, 1e-6f, 1.f, 10.f};
	for (float x : xs) {
		CHECK(std::isfinite(driveShape(x, dbToGain(36.f), 1.f)), "drive inf");
		CHECK(std::isfinite(driveShape(x, dbToGain(0.f), -1.f)), "drive inf");
		CHECK(std::isfinite(distShape(x, dbToGain(40.f), 1.f)), "dist inf");
		CHECK(std::isfinite(quantise(x, 1.f)) && std::isfinite(quantise(x, 16.f)), "quantise inf");
	}
	CHECK(levelGain(-60.f) == 0.f, "level -60 should be off");
	CHECK(std::fabs(levelGain(0.f) - 1.f) < 1e-6f, "level 0 dB");
	CHECK(withCv(12.f, 10.f, 0.f, 36.f) == 36.f && withCv(12.f, -10.f, 0.f, 36.f) == 0.f, "cv clamp");
	CHECK(std::fabs(withCv(12.f, 1.f, 0.f, 36.f) - 15.6f) < 1e-4f, "1 V = 10 %%");
}

int main() {
	testDrive();
	testDist();
	testCrush();
	testFader();
	testScope();
	testFinite();
	if (failures == 0) std::printf("test_parzen_dsp: all tests passed\n");
	else std::printf("test_parzen_dsp: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
