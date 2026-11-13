// Standalone tests for the Morel WAV reader/writer (no Rack needed). Writes temporary files to build/.
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_morel_wav.cpp -o build/test_morel_wav && ./build/test_morel_wav [tmpdir]
#include "dsp/MorelWav.hpp"
#include <cstdio>
#include <vector>
#include <cmath>
#include <string>

using namespace morel;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static std::string dir = "build";

// Writes a PCM/float WAV by hand (independent of writeWavFloat).
static void writeRaw(const std::string& path, int format, int bits, int channels, int rate, const std::vector<double>& samples, bool extensible = false, bool junk = false) {
	FILE* f = std::fopen(path.c_str(), "wb");
	int bytes = bits / 8, block = bytes * channels;
	uint32_t dataBytes = (uint32_t) samples.size() * bytes;
	auto w32 = [&](uint32_t v) { unsigned char b[4] = {(unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24)}; std::fwrite(b, 1, 4, f); };
	auto w16 = [&](uint16_t v) { unsigned char b[2] = {(unsigned char) v, (unsigned char) (v >> 8)}; std::fwrite(b, 1, 2, f); };
	uint32_t fmtSize = extensible ? 40 : 16;
	std::fwrite("RIFF", 1, 4, f);
	w32(4 + 8 + fmtSize + (junk ? 8 + 3 + 1 : 0) + 8 + dataBytes);
	std::fwrite("WAVE", 1, 4, f);
	if (junk) {
		std::fwrite("JUNK", 1, 4, f);
		w32(3);
		std::fwrite("abc\0", 1, 4, f);   // odd size + pad byte
	}
	std::fwrite("fmt ", 1, 4, f);
	w32(fmtSize);
	w16(extensible ? 0xFFFE : format);
	w16(channels);
	w32(rate);
	w32(rate * block);
	w16(block);
	w16(bits);
	if (extensible) {
		w16(22); w16(bits); w32(3);
		w16(format);
		unsigned char guid[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
		std::fwrite(guid, 1, 14, f);
	}
	std::fwrite("data", 1, 4, f);
	w32(dataBytes);
	for (double s : samples) {
		if (format == 3 && bits == 32) { float v = (float) s; uint32_t u; std::memcpy(&u, &v, 4); w32(u); }
		else if (format == 3) { uint64_t u; std::memcpy(&u, &s, 8); w32((uint32_t) u); w32((uint32_t) (u >> 32)); }
		else if (bits == 8) { unsigned char v = (unsigned char) std::lround(s * 127 + 128); std::fwrite(&v, 1, 1, f); }
		else if (bits == 16) w16((uint16_t) (int16_t) std::lround(s * 32767));
		else if (bits == 24) { int32_t v = (int32_t) std::lround(s * 8388607); unsigned char b[3] = {(unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16)}; std::fwrite(b, 1, 3, f); }
		else w32((uint32_t) (int32_t) std::lround(s * 2147483647.0));
	}
	std::fclose(f);
}

static void testRoundTrip() {
	std::vector<float> l(1000), r(1000);
	for (int i = 0; i < 1000; i++) {
		l[i] = std::sin(i * 0.01f) * 3.7f;    // values beyond +-1 survive (Rack voltages)
		r[i] = -l[i];
	}
	std::string p = dir + "/morel_test_rt.wav", err;
	CHECK(writeWavFloat(p, l.data(), r.data(), 1000, 96000.f, err), "write: %s", err.c_str());
	std::vector<float> a(2000), b(2000);
	WavInfo info;
	CHECK(readWav(p, a.data(), b.data(), 2000, info), "read: %s", info.error.c_str());
	bool same = info.frames == 1000 && info.channels == 2 && info.sampleRate == 96000.f && !info.truncated;
	for (int i = 0; same && i < 1000; i++)
		same = a[i] == l[i] && b[i] == r[i];
	CHECK(same, "float round trip is bit exact (frames %d)", info.frames);
	CHECK(readWav(p, a.data(), b.data(), 300, info) && info.frames == 300 && info.truncated, "truncation to capacity");
	std::remove(p.c_str());
}

static void testFormats() {
	struct Case { int format, bits, channels; bool ext, junk; double tol; };
	Case cases[] = {{1, 16, 1, false, false, 1e-4}, {1, 16, 2, false, true, 1e-4}, {1, 24, 2, false, false, 1e-6},
		{1, 32, 2, true, false, 1e-8}, {1, 8, 1, false, false, 2e-2}, {3, 32, 2, true, false, 1e-7}, {3, 64, 3, false, false, 1e-7}};
	for (const Case& c : cases) {
		std::vector<double> s;
		for (int i = 0; i < 500; i++)
			for (int ch = 0; ch < c.channels; ch++)
				s.push_back(0.8 * std::sin(i * 0.05 + ch));
		std::string p = dir + "/morel_test_fmt.wav";
		writeRaw(p, c.format, c.bits, c.channels, 44100, s, c.ext, c.junk);
		std::vector<float> a(600), b(600);
		WavInfo info;
		bool ok = readWav(p, a.data(), b.data(), 600, info);
		bool close = ok && info.frames == 500 && info.sampleRate == 44100.f;
		for (int i = 0; close && i < 500; i++) {
			double el = s[i * c.channels], er = c.channels > 1 ? s[i * c.channels + 1] : el;
			close = std::fabs(a[i] - el) <= c.tol + 1e-6 && std::fabs(b[i] - er) <= c.tol + 1e-6;
		}
		CHECK(close, "format %d, %d bit, %d ch%s: %s (frames %d, a[1] %f expected %f)", c.format, c.bits, c.channels, c.ext ? " (extensible)" : "", info.error.c_str(), info.frames, a[1], s[c.channels]);
		std::remove(p.c_str());
	}
}

static void testErrors() {
	std::vector<float> a(10), b(10);
	WavInfo info;
	CHECK(!readWav(dir + "/does_not_exist.wav", a.data(), b.data(), 10, info), "missing file fails");
	std::string p = dir + "/morel_test_bad.wav";
	FILE* f = std::fopen(p.c_str(), "wb");
	std::fwrite("RIFFxxxxWAVEfmt ", 1, 16, f);
	std::fclose(f);
	CHECK(!readWav(p, a.data(), b.data(), 10, info), "truncated header fails: %s", info.error.c_str());
	f = std::fopen(p.c_str(), "wb");
	std::fwrite("hello world, not audio", 1, 22, f);
	std::fclose(f);
	CHECK(!readWav(p, a.data(), b.data(), 10, info), "non-WAV fails");
	writeRaw(p, 1, 12, 1, 44100, std::vector<double>(10, 0.0));
	CHECK(!readWav(p, a.data(), b.data(), 10, info), "unsupported bit depth fails");
	std::remove(p.c_str());
}

int main(int argc, char** argv) {
	if (argc > 1)
		dir = argv[1];
	testRoundTrip();
	testFormats();
	testErrors();
	if (failures == 0)
		std::printf("All Morel WAV tests passed.\n");
	return failures == 0 ? 0 : 1;
}
