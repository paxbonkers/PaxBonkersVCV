// Standalone tests for the Morel sequencer logic (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_morel_sequencer.cpp -o build/test_morel_sequencer && ./build/test_morel_sequencer
#include "dsp/MorelSequencer.hpp"
#include <cstdio>
#include <vector>
#include <limits>

using namespace morel;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

// Runs n steps, records 1-based step numbers and EOC flags.
static void run(StepSequencer& s, int n, int len, int dir, std::vector<int>& steps, std::vector<bool>& eoc) {
	for (int i = 0; i < n; i++) {
		eoc.push_back(s.advance(len, dir));
		steps.push_back(s.current() + 1);
	}
}

static bool seqEq(const std::vector<int>& a, const std::vector<int>& b) {
	return a == b;
}

static void testForward() {
	StepSequencer s;
	std::vector<int> st;
	std::vector<bool> e;
	run(s, 11, 5, DIR_FWD, st, e);
	CHECK(seqEq(st, {1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1}), "FWD order");
	for (int i = 0; i < 11; i++)
		CHECK(e[i] == (i == 5 || i == 10), "FWD EOC at step index %d", i);
}

static void testReverse() {
	StepSequencer s;
	std::vector<int> st;
	std::vector<bool> e;
	run(s, 7, 4, DIR_REV, st, e);
	CHECK(seqEq(st, {4, 3, 2, 1, 4, 3, 2}), "REV order, starts at LEN");
	for (int i = 0; i < 7; i++)
		CHECK(e[i] == (i == 4), "REV EOC at %d", i);
}

static void testPendulum() {
	StepSequencer s;
	std::vector<int> st;
	std::vector<bool> e;
	run(s, 9, 4, DIR_PEND, st, e);
	CHECK(seqEq(st, {1, 2, 3, 4, 3, 2, 1, 2, 3}), "PEND without doubled ends");
	for (int i = 0; i < 9; i++)
		CHECK(e[i] == (i == 6), "PEND EOC only on return to step 1, index %d", i);
	StepSequencer s2;
	st.clear();
	e.clear();
	run(s2, 5, 2, DIR_PEND, st, e);
	CHECK(seqEq(st, {1, 2, 1, 2, 1}), "PEND length 2");
	CHECK(!e[0] && !e[1] && e[2] && !e[3] && e[4], "PEND length 2 EOC");
}

static void testLengthOne() {
	for (int d = 0; d < NUM_DIRS; d++) {
		StepSequencer s;
		std::vector<int> st;
		std::vector<bool> e;
		run(s, 4, 1, d, st, e);
		CHECK(seqEq(st, {1, 1, 1, 1}), "length 1 stays on step 1 (dir %d)", d);
		CHECK(!e[0] && e[1] && e[2] && e[3], "length 1 EOC from the second step (dir %d)", d);
	}
}

static void testShuffle() {
	StepSequencer s;
	s.rng.seed(12345);
	int len = 6;
	int prevLast = -1;
	CHECK(!s.advance(len, DIR_SHUF), "first step is no EOC");
	CHECK(s.current() == 0, "first step after reset is step 1");
	// Complete the first permutation.
	std::vector<int> first = {0};
	for (int i = 1; i < len; i++) {
		CHECK(!s.advance(len, DIR_SHUF), "no EOC inside a permutation");
		first.push_back(s.current());
	}
	std::vector<bool> seen(len, false);
	for (int v : first)
		seen[v] = true;
	for (int i = 0; i < len; i++)
		CHECK(seen[i], "first permutation contains step %d", i + 1);
	prevLast = first.back();
	for (int cycle = 0; cycle < 200; cycle++) {
		std::vector<bool> got(len, false);
		for (int i = 0; i < len; i++) {
			bool eoc = s.advance(len, DIR_SHUF);
			CHECK(eoc == (i == 0), "SHUF EOC exactly at the first step of a permutation");
			if (i == 0)
				CHECK(s.current() != prevLast, "no repetition across permutations");
			got[s.current()] = true;
		}
		for (int i = 0; i < len; i++)
			CHECK(got[i], "each step once per permutation");
		prevLast = s.current();
	}
	// LEN change starts a new permutation (with EOC).
	CHECK(s.advance(3, DIR_SHUF), "LEN change starts a new permutation");
	CHECK(s.current() < 3, "new permutation within new length");
}

static void testLengthShrink() {
	StepSequencer f;
	for (int i = 0; i < 7; i++)
		f.advance(8, DIR_FWD);
	CHECK(f.advance(4, DIR_FWD) && f.current() == 0, "FWD: shrink below position goes to step 1 with EOC");
	StepSequencer r;
	r.advance(8, DIR_REV); // step 8
	CHECK(r.advance(4, DIR_REV) && r.current() == 3, "REV: shrink below position goes to step LEN");
	StepSequencer p;
	for (int i = 0; i < 6; i++)
		p.advance(8, DIR_PEND);
	CHECK(p.advance(3, DIR_PEND) && p.current() == 0, "PEND: shrink below position goes to step 1");
}

static void testResetAndDirChange() {
	StepSequencer s;
	for (int i = 0; i < 3; i++)
		s.advance(8, DIR_FWD);
	s.reset();
	CHECK(!s.advance(8, DIR_FWD) && s.current() == 0, "after reset the next step is step 1, no EOC");
	s.reset();
	CHECK(!s.advance(8, DIR_REV) && s.current() == 7, "REV: after reset the next step is step LEN, no EOC");
	s.reset();
	CHECK(!s.advance(3, DIR_REV) && s.current() == 2, "REV with LEN 3 starts at step 3");
	// Switch from FWD to PEND mid-run: pendulum continues forward.
	StepSequencer t;
	t.advance(8, DIR_FWD);
	t.advance(8, DIR_FWD); // step 2
	t.advance(8, DIR_PEND);
	CHECK(t.current() == 2, "PEND continues forward from the current step");
}

static void testSliceMapping() {
	CHECK(sliceIndex(0.5f, 0) == -1, "no recording");
	CHECK(sliceIndex(0.f, 1) == 0 && sliceIndex(1.f, 1) == 0, "one slice");
	CHECK(sliceIndex(0.f, 5) == 0 && sliceIndex(0.19f, 5) == 0 && sliceIndex(0.2f, 5) == 1, "five slices, zone edges");
	CHECK(sliceIndex(1.f, 5) == 4, "top value maps to the last slice");
	CHECK(sliceIndex(0.999f, 32) == 31 && sliceIndex(1.f, 32) == 31, "32 slices");
	CHECK(sliceIndex(std::numeric_limits<float>::quiet_NaN(), 5) == 0, "NaN counts as 0");
	for (int i = 0; i < NUM_STEPS; i++)
		CHECK(sliceIndex(defaultStepValue(i), 8) == i, "defaults play slices 1..8 in order");
	CHECK(sliceVoltage(0, 1) == 0.f && sliceVoltage(0, 5) == 0.f && sliceVoltage(4, 5) == 10.f, "SLICE voltage ends");
	CHECK(std::fabs(sliceVoltage(2, 5) - 5.f) < 1e-5f, "SLICE voltage middle");
}

int main() {
	testForward();
	testReverse();
	testPendulum();
	testLengthOne();
	testShuffle();
	testLengthShrink();
	testResetAndDirChange();
	testSliceMapping();
	if (failures == 0)
		std::printf("All Morel sequencer tests passed.\n");
	return failures == 0 ? 0 : 1;
}
