#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>

// Minimal WAV reader/writer for Morel (no external library).
// Reads PCM 8/16/24/32-bit and IEEE float 32/64-bit, also WAVE_FORMAT_EXTENSIBLE, any channel count
// (mono is copied to both sides, more than two channels: the first two are used).
// Writes 32-bit float stereo. Unit-tested in tests/test_morel_wav.cpp.
// NOT real-time safe (file I/O): call from the worker or the UI thread only.

namespace morel {

struct WavInfo {
	int frames = 0;          // frames written to the destination
	int channels = 0;
	float sampleRate = 0.f;
	bool truncated = false;  // the file was longer than the destination
	std::string error;
};

namespace wavdetail {
inline uint32_t u32(const unsigned char* p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
inline uint16_t u16(const unsigned char* p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}
inline void put32(unsigned char* p, uint32_t v) {
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
inline void put16(unsigned char* p, uint16_t v) {
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}
inline float sampleAt(const unsigned char* p, int format, int bits) {
	if (format == 3) {
		if (bits == 32) {
			uint32_t u = u32(p);
			float f;
			std::memcpy(&f, &u, 4);
			return std::isfinite(f) ? f : 0.f;
		}
		uint64_t u = (uint64_t) u32(p) | ((uint64_t) u32(p + 4) << 32);
		double d;
		std::memcpy(&d, &u, 8);
		return std::isfinite(d) ? (float) d : 0.f;
	}
	switch (bits) {
		case 8: return (p[0] - 128) / 128.f;
		case 16: return (int16_t) u16(p) / 32768.f;
		case 24: {
			int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24) >> 8;
			return v / 8388608.f;
		}
		default: return (int32_t) u32(p) / 2147483648.f;
	}
}
} // namespace wavdetail

// Reads `path` into left/right (each with room for `capacity` frames). Samples are scaled to +-1.
inline bool readWav(const std::string& path, float* left, float* right, int capacity, WavInfo& info) {
	using namespace wavdetail;
	info = WavInfo();
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		info.error = "cannot open file";
		return false;
	}
	unsigned char hdr[12];
	if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
		std::fclose(f);
		info.error = "not a WAV file";
		return false;
	}
	int format = 0, channels = 0, bits = 0, blockAlign = 0;
	uint32_t rate = 0;
	bool haveFmt = false;
	while (true) {
		unsigned char ch[8];
		if (std::fread(ch, 1, 8, f) != 8) {
			std::fclose(f);
			info.error = haveFmt ? "no audio data" : "no format chunk";
			return false;
		}
		uint32_t size = u32(ch + 4);
		if (std::memcmp(ch, "fmt ", 4) == 0) {
			unsigned char fmt[40] = {};
			uint32_t n = size < 40 ? size : 40;
			if (size < 16 || std::fread(fmt, 1, n, f) != n) {
				std::fclose(f);
				info.error = "broken format chunk";
				return false;
			}
			if (size > n)
				std::fseek(f, (long) (size - n), SEEK_CUR);
			format = u16(fmt);
			channels = u16(fmt + 2);
			rate = u32(fmt + 4);
			blockAlign = u16(fmt + 12);
			bits = u16(fmt + 14);
			if (format == 0xFFFE && size >= 26)
				format = u16(fmt + 24);   // sub format GUID starts with the format code
			haveFmt = true;
		}
		else if (std::memcmp(ch, "data", 4) == 0) {
			if (!haveFmt) {
				std::fclose(f);
				info.error = "no format chunk";
				return false;
			}
			bool okFormat = (format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
				(format == 3 && (bits == 32 || bits == 64));
			int bytes = bits / 8;
			if (!okFormat || channels < 1 || blockAlign < channels * bytes || rate < 1000 || rate > 768000) {
				std::fclose(f);
				info.error = "unsupported WAV format";
				return false;
			}
			long long total = size / (uint32_t) blockAlign;
			int frames = total > capacity ? capacity : (int) total;
			info.truncated = total > capacity;
			unsigned char buf[8192];
			int perRead = (int) (sizeof(buf) / blockAlign);
			int done = 0;
			while (done < frames) {
				int want = frames - done < perRead ? frames - done : perRead;
				size_t got = std::fread(buf, (size_t) blockAlign, (size_t) want, f);
				for (size_t k = 0; k < got; k++) {
					const unsigned char* p = buf + k * blockAlign;
					float l = sampleAt(p, format, bits);
					float r = channels > 1 ? sampleAt(p + bytes, format, bits) : l;
					left[done + k] = l;
					right[done + k] = r;
				}
				done += (int) got;
				if ((int) got < want)
					break;   // file shorter than its header says: keep what was read
			}
			std::fclose(f);
			info.frames = done;
			info.channels = channels;
			info.sampleRate = (float) rate;
			if (done <= 0) {
				info.error = "no audio data";
				return false;
			}
			return true;
		}
		else {
			std::fseek(f, (long) (size + (size & 1)), SEEK_CUR);
		}
	}
}

// Writes a 32-bit float stereo WAV.
inline bool writeWavFloat(const std::string& path, const float* left, const float* right, int frames, float sampleRate, std::string& error) {
	using namespace wavdetail;
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) {
		error = "cannot write file";
		return false;
	}
	uint32_t dataBytes = (uint32_t) frames * 8u;
	unsigned char h[58];
	std::memcpy(h, "RIFF", 4);
	put32(h + 4, 50 + dataBytes);
	std::memcpy(h + 8, "WAVE", 4);
	std::memcpy(h + 12, "fmt ", 4);
	put32(h + 16, 18);
	put16(h + 20, 3);                                 // IEEE float
	put16(h + 22, 2);                                 // stereo
	put32(h + 24, (uint32_t) std::lround(sampleRate));
	put32(h + 28, (uint32_t) std::lround(sampleRate) * 8u);
	put16(h + 32, 8);
	put16(h + 34, 32);
	put16(h + 36, 0);                                 // cbSize
	std::memcpy(h + 38, "fact", 4);
	put32(h + 42, 4);
	put32(h + 46, (uint32_t) frames);
	std::memcpy(h + 50, "data", 4);
	put32(h + 54, dataBytes);
	bool ok = std::fwrite(h, 1, 58, f) == 58;
	unsigned char buf[8192];
	int perWrite = (int) (sizeof(buf) / 8);
	for (int i = 0; ok && i < frames; i += perWrite) {
		int n = frames - i < perWrite ? frames - i : perWrite;
		for (int k = 0; k < n; k++) {
			float l = std::isfinite(left[i + k]) ? left[i + k] : 0.f;
			float r = std::isfinite(right[i + k]) ? right[i + k] : 0.f;
			uint32_t ul, ur;
			std::memcpy(&ul, &l, 4);
			std::memcpy(&ur, &r, 4);
			put32(buf + k * 8, ul);
			put32(buf + k * 8 + 4, ur);
		}
		ok = std::fwrite(buf, 8, (size_t) n, f) == (size_t) n;
	}
	ok = (std::fclose(f) == 0) && ok;
	if (!ok)
		error = "write failed";
	return ok;
}

} // namespace morel
