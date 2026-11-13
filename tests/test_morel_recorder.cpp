// Standalone tests for the Morel recording modes, ring buffer and step period (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_morel_recorder.cpp -o build/test_morel_recorder && ./build/test_morel_recorder
#include "dsp/MorelRecorder.hpp"
#include <cstdio>
#include <vector>
#include <cmath>

using namespace morel;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static void testMan() {
	RecorderLogic r;
	RecEvent e = r.process(REC_MAN, true, false, 0.f, 2);
	CHECK(e.start && r.recording(), "MAN: press starts");
	e = r.process(REC_MAN, false, true, 0.f, 2);
	CHECK(!e.stop && r.recording(), "MAN: steps do not stop");
	e = r.process(REC_CLK, false, false, 0.f, 2);
	CHECK(r.recording(), "MAN: changing the mode keeps a running recording");
	e = r.process(REC_CLK, true, false, 0.f, 2);
	CHECK(e.stop && !r.recording(), "press stops");
}

static void testClk() {
	for (int bars : {1, 2, 4, 8}) {
		RecorderLogic r;
		RecEvent e = r.process(REC_CLK, true, false, 0.f, bars);
		CHECK(r.armed() && !e.start, "CLK: press arms");
		for (int i = 0; i < 10; i++)
			r.process(REC_CLK, false, false, 0.f, bars);
		CHECK(r.armed(), "CLK: stays armed without a step");
		e = r.process(REC_CLK, false, true, 0.f, bars);
		CHECK(e.start && r.recording(), "CLK: next step starts");
		int steps = 0;
		bool stopped = false;
		for (int i = 0; i < 1000 && !stopped; i++) {
			e = r.process(REC_CLK, false, (i % 7) == 6, 0.f, bars);
			if ((i % 7) == 6)
				steps++;
			stopped = e.stop;
		}
		CHECK(stopped && steps == bars * STEPS_PER_BAR, "CLK: stops after %d bars (%d steps)", bars, steps);
	}
	RecorderLogic r;
	r.process(REC_CLK, true, false, 0.f, 2);
	RecEvent e = r.process(REC_CLK, true, false, 0.f, 2);
	CHECK(e.cancelled && !r.armed(), "CLK: press while armed cancels");
	r.process(REC_CLK, true, false, 0.f, 2);
	e = r.process(REC_LVL, false, false, 0.f, 2);
	CHECK(e.cancelled && !r.armed(), "mode change while armed cancels");
	r.process(REC_CLK, true, false, 0.f, 2);
	r.process(REC_CLK, false, true, 0.f, 2);
	e = r.process(REC_CLK, true, false, 0.f, 2);
	CHECK(e.stop, "CLK: press while recording stops early");
	RecorderLogic s;
	e = s.process(REC_CLK, true, true, 0.f, 1);
	CHECK(e.start, "CLK: press and step in the same sample start at once");
}

static void testLvl() {
	RecorderLogic r;
	r.process(REC_LVL, true, false, 0.f, 2);
	RecEvent e = r.process(REC_LVL, false, false, 0.1f, 2);
	CHECK(r.armed() && !e.start, "LVL: below -30 dBFS stays armed");
	e = r.process(REC_LVL, false, false, std::nanf(""), 2);
	CHECK(r.armed(), "LVL: NaN does not start");
	e = r.process(REC_LVL, false, false, 0.2f, 2);
	CHECK(e.start && r.recording(), "LVL: crossing the threshold starts");
	e = r.process(REC_LVL, false, true, 0.f, 2);
	CHECK(r.recording(), "LVL: steps do not stop");
	e = r.process(REC_LVL, true, false, 0.f, 2);
	CHECK(e.stop, "LVL: press stops");
}

static void testRetroEvent() {
	RecorderLogic r;
	RecEvent e = r.process(REC_RETRO, true, false, 0.f, 4);
	CHECK(e.retro && !r.recording() && !r.armed(), "RETRO: press captures at once");
	r.process(REC_MAN, true, false, 0.f, 4);
	r.forceStop();
	CHECK(!r.recording(), "forceStop ends a recording");
}

static void testStepPeriod() {
	StepPeriod p;
	CHECK(p.get(123.0) == 123.0, "fallback before the first period");
	for (int i = 1; i <= 1000; i++)
		p.process(i % 100 == 0);
	CHECK(p.get(0.0) == 100.0, "period 100, got %.1f", p.get(0.0));
	for (int i = 1; i <= 1000; i++)
		p.process(i == 1000);
	CHECK(p.get(0.0) == 100.0, "long gap keeps the old period");
	for (int i = 1; i <= 300; i++)
		p.process(i % 150 == 0);
	CHECK(p.get(0.0) == 150.0, "tempo change is followed, got %.1f", p.get(0.0));
}

static void testRing() {
	std::vector<float> l(10), r(10);
	RetroRing ring;
	int start, len;
	ring.newest(5, start, len);
	CHECK(len == 0, "empty ring yields nothing");
	ring.write(1.f, 1.f);
	ring.attach(l.data(), r.data(), 10);
	for (int i = 0; i < 4; i++)
		ring.write((float) i, (float) -i);
	ring.newest(8, start, len);
	CHECK(len == 4 && start == 0, "only filled frames (len %d start %d)", len, start);
	for (int i = 4; i < 23; i++)
		ring.write((float) i, (float) -i);
	ring.newest(6, start, len);
	std::vector<float> dl(6), dr(6);
	ring.copy(start, len, dl.data(), dr.data());
	bool ok = len == 6;
	for (int k = 0; k < 6; k++)
		ok = ok && dl[k] == (float) (17 + k) && dr[k] == (float) -(17 + k);
	CHECK(ok, "newest 6 frames after wrapping, oldest first");
	ring.newest(50, start, len);
	CHECK(len == 10, "at most the capacity");
}

int main() {
	testMan();
	testClk();
	testLvl();
	testRetroEvent();
	testStepPeriod();
	testRing();
	if (failures == 0)
		std::printf("All Morel recorder tests passed.\n");
	return failures == 0 ? 0 : 1;
}
