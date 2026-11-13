#include "plugin.hpp"
#include "dsp/MorelSequencer.hpp"
#include "dsp/MorelSlicer.hpp"
#include "dsp/MorelPlayer.hpp"
#include "dsp/MorelGrain.hpp"
#include "dsp/MorelRecorder.hpp"
#include "dsp/MorelWav.hpp"
#include "MorelLayout.hpp"
#include <osdialog.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>

// Morel: a machine that makes a past moment inhabitable. Records audio, slices it automatically
// by level and plays the slices from an 8-step sequencer.
// Sequencer: dsp/MorelSequencer.hpp, slicing: dsp/MorelSlicer.hpp, CUT playback: dsp/MorelPlayer.hpp,
// GRAIN playback: dsp/MorelGrain.hpp (all Rack-independent, unit-tested).
// Signal contract: docs/modules/Morel-Signalvertrag.md (project docs).
// Recording modes and WAV files: dsp/MorelRecorder.hpp, dsp/MorelWav.hpp.
// Stages 1-3 done: recording MAN/CLK/LVL/RETRO, CUT and GRAIN, loading WAV files, recording saved with the patch.

// Fixed memory per buffer: 60 s at 48 kHz. At higher sample rates the maximum recording time is shorter.
static const int CAPACITY_FRAMES = 60 * 48000;
static const float MIN_REC_SECONDS = 0.05f;
static const float CV_SCALE = 0.1f;   // 1 V = 10 % for 0..1 controls
static const int OVERVIEW_COLS = 512;
// Live waveform while recording: min/max of L + R per block, fixed size (no allocation in process()).
static const int LIVE_MAX_BLOCKS = 48000;

static const char* STORAGE_FILE = "recording.wav";
static const int BAR_CHOICES[4] = {1, 2, 4, 8};

// One recording buffer. Two of them: one plays, the other records.
struct MorelBuffer {
	std::vector<float> left, right;
	int length = 0;                    // valid samples, 0 = empty
	float sampleRate = 48000.f;
	std::atomic<int> generation{0};    // incremented whenever a new recording starts
	// Slice lists, double-buffered: the worker writes the inactive one, then flips sliceCur.
	morel::SliceList slices[2];
	std::atomic<int> sliceCur{0};
	// Waveform overview of L + R for the display (written by the worker before the buffer is published).
	float ovMin[OVERVIEW_COLS] = {};
	float ovMax[OVERVIEW_COLS] = {};
	// Source shown in the display: "REC 001" for recordings, the file name of a loaded sample.
	char name[64] = {};

	const morel::SliceList& currentSlices() const {
		return slices[sliceCur.load()];
	}
};

struct Morel : Module {
	// IDs are stored in patches. Never reorder; only append new entries.
	enum ParamId {
		TEMPO_PARAM,
		RUN_PARAM,
		ADV_PARAM,
		TRIG_PARAM,
		REC_PARAM,
		RND_PARAM,
		DIR_BUTTON_PARAM,
		MODE_BUTTON_PARAM,
		ENUMS(STEP_PARAMS, 8),
		LEN_PARAM,
		SENS_PARAM,
		SPEED_PARAM,
		SPAN_PARAM,
		SCATTER_PARAM,
		GRAIN_PARAM,
		DIR_PARAM,      // selected by DIR_BUTTON_PARAM, no own widget
		MODE_PARAM,     // selected by MODE_BUTTON_PARAM, no own widget
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		RUN_INPUT,
		TRIG_INPUT,
		RND_INPUT,
		LEN_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		REC_INPUT,
		SENS_INPUT,
		VOCT_INPUT,
		SPAN_INPUT,
		SCATTER_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		EOC_OUTPUT,
		STEP_OUTPUT,
		EOS_OUTPUT,
		SLICE_OUTPUT,
		ENV_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT,
		REC_LIGHT,
		RND_LIGHT,
		GRAIN_LIGHT,
		ENUMS(STEP_LIGHTS, 8),
		LIGHTS_LEN
	};

	morel::StepSequencer seq;
	morel::Transport transport;
	morel::CutPlayer player;
	morel::GrainCloud grains;
	bool lastGrainMode = false;
	morel::Rng scatterRng;

	MorelBuffer bufs[2];
	int capacity = 0;                    // allocated samples per channel and buffer
	std::atomic<int> activeBuf{0};
	std::atomic<int> readyBuf{-1};       // analysed recording waiting to become active
	std::atomic<int> analyseRequest{-1}; // buffer the worker should analyse completely
	std::atomic<float> sensTarget{0.5f};
	std::mutex bufMutex;                 // held by the worker while it reads a buffer

	bool recording = false;  // mirror of recLogic.recording() for the display
	bool recArmed = false;   // CLK/LVL waiting (display, light)
	morel::RecorderLogic recLogic;
	morel::StepPeriod stepPeriod;
	int bars = 2;            // CLK and RETRO length in bars (1, 2, 4, 8), saved in the patch JSON

	// RETRO ring buffer, allocated by the worker the first time RETRO is selected
	std::vector<float> ringL, ringR;
	morel::RetroRing ring;
	std::atomic<bool> ringWanted{false}, ringReady{false};
	// RETRO capture request (audio thread -> worker)
	std::atomic<int> retroTarget{-1};
	int retroStart = 0, retroLen = 0;
	float retroRate = 48000.f;
	char retroName[64] = {};
	// WAV load request (UI thread -> worker -> audio thread claims the buffer)
	std::mutex loadMutex;
	std::string loadPath, loadName;
	std::atomic<bool> loadPending{false};
	std::atomic<int> claimRequest{0};
	std::atomic<int> claimGranted{-1};
	char loadError[96] = {};
	std::string savedName;   // name from the patch JSON, used when the stored recording is loaded

	int recBuf = 1;
	int recPos = 0;
	int recCounter = 0;      // numbers the recordings of this module instance (REC 001, REC 002, ...)
	// Live waveform of the recording in progress (written by process(), read by the display).
	float liveMin[LIVE_MAX_BLOCKS] = {};
	float liveMax[LIVE_MAX_BLOCKS] = {};
	int liveBlock = 256;      // samples per block
	float livePeak = 0.f;

	// State for display and outputs
	int curSlice = -1;
	int curSliceCount = 0;
	float sliceVolts = 0.f;
	float envState = 0.f;
	float recBlink = 0.f;
	std::atomic<float> playheadFrac{-1.f};
	float engineRate = 48000.f;

	dsp::SchmittTrigger runInTrigger, clockTrigger, trigInTrigger, resetTrigger, rndInTrigger, recInTrigger;
	dsp::BooleanTrigger advButton, trigButton, rndButton, recButton, dirButton, modeButton;
	dsp::PulseGenerator stepPulse, eocPulse, eosPulse, rndLightPulse;
	dsp::ClockDivider lightDivider;

	std::thread worker;
	std::atomic<bool> workerRun{true};

	Morel() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(TEMPO_PARAM, std::log2(zenon::TEMPO_MIN_SPM), std::log2(zenon::TEMPO_MAX_SPM),
			std::log2(480.f), "Tempo", " steps/min", 2.f);
		configSwitch(RUN_PARAM, 0.f, 1.f, 0.f, "Run", {"Stopped", "Running"});
		configButton(ADV_PARAM, "Advance one step");
		configButton(TRIG_PARAM, "Trigger (replay current step)");
		configButton(REC_PARAM, "Record (start / stop)");
		configButton(RND_PARAM, "Randomize all steps");
		configButton(DIR_BUTTON_PARAM, "Direction (press to cycle)");
		configButton(MODE_BUTTON_PARAM, "Recording mode (press to cycle)");
		for (int i = 0; i < morel::NUM_STEPS; i++)
			configParam(STEP_PARAMS + i, 0.f, 1.f, morel::defaultStepValue(i), "Step " + std::to_string(i + 1) + " slice", "%", 0.f, 100.f);
		configParam(LEN_PARAM, 1.f, (float) morel::NUM_STEPS, (float) morel::NUM_STEPS, "Length", " steps");
		paramQuantities[LEN_PARAM]->snapEnabled = true;
		configParam(SENS_PARAM, 0.f, 1.f, 0.5f, "Slice sensitivity", "%", 0.f, 100.f);
		configParam(SPEED_PARAM, -2.f, 2.f, 0.f, "Speed", " oct");
		configParam(SPAN_PARAM, 0.01f, 1.f, 1.f, "Span (part of the slice that plays)", "%", 0.f, 100.f);
		configParam(SCATTER_PARAM, 0.f, 1.f, 0.f, "Scatter", "%", 0.f, 100.f);
		configSwitch(GRAIN_PARAM, 0.f, 1.f, 0.f, "Play mode", {"CUT", "GRAIN"});
		std::vector<std::string> dirLabels;
		for (int d = 0; d < morel::NUM_DIRS; d++)
			dirLabels.push_back(morel::dirName(d));
		configSwitch(DIR_PARAM, 0.f, morel::NUM_DIRS - 1, morel::DIR_FWD, "Direction", dirLabels);
		configSwitch(MODE_PARAM, 0.f, morel::NUM_REC_MODES - 1, morel::REC_MAN, "Recording mode", {"MAN", "CLK", "LVL", "RETRO"});

		configInput(CLOCK_INPUT, "Clock (replaces the internal clock)");
		configInput(RESET_INPUT, "Reset");
		configInput(RUN_INPUT, "Run toggle trigger");
		configInput(TRIG_INPUT, "Trigger (replay current step)");
		configInput(RND_INPUT, "Randomize trigger");
		configInput(LEN_INPUT, "Length CV (1 V = 1 step)");
		configInput(IN_L_INPUT, "Audio left");
		configInput(IN_R_INPUT, "Audio right (left is used when unpatched)");
		configInput(REC_INPUT, "Record start / stop trigger");
		configInput(SENS_INPUT, "Sensitivity CV (1 V = 10 %)");
		configInput(VOCT_INPUT, "Speed (1 V/oct)");
		configInput(SPAN_INPUT, "Span CV (1 V = 10 %)");
		configInput(SCATTER_INPUT, "Scatter CV (1 V = 10 %)");

		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		configOutput(EOC_OUTPUT, "End of cycle");
		configOutput(STEP_OUTPUT, "Step trigger");
		configOutput(EOS_OUTPUT, "End of slice");
		configOutput(SLICE_OUTPUT, "Slice index (0 V first, 10 V last)");
		configOutput(ENV_OUTPUT, "Envelope follower");

		configLight(RUN_LIGHT, "Running");
		configLight(REC_LIGHT, "Recording");
		configLight(GRAIN_LIGHT, "Grain mode");

		lightDivider.setDivision(16);
		scatterRng.seed((uint32_t) (random::u32() | 1u));
		seq.rng.seed((uint32_t) (random::u32() | 1u));
		grains.rng.seed((uint32_t) (random::u32() | 1u));

		allocate();
		worker = std::thread([this]() { workerLoop(); });
	}

	~Morel() {
		workerRun = false;
		if (worker.joinable())
			worker.join();
	}

	// Reserves CAPACITY_FRAMES per channel and buffer once (constructor only).
	void allocate() {
		for (int b = 0; b < 2; b++) {
			bufs[b].left.assign(CAPACITY_FRAMES, 0.f);
			bufs[b].right.assign(CAPACITY_FRAMES, 0.f);
		}
		capacity = CAPACITY_FRAMES;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		// Recordings keep their own sample rate and are played back with rate correction.
		engineRate = e.sampleRate;
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		seq.reset();
		transport = morel::Transport();
		player.stopAll();
		grains.stopAll();
		recording = false;
		recArmed = false;
		recLogic.reset();
		bars = 2;
		loadError[0] = 0;
		// Initialize deletes the recording (signal contract).
		for (int b = 0; b < 2; b++) {
			bufs[b].generation++;
			bufs[b].length = 0;
			bufs[b].name[0] = 0;
		}
		readyBuf = -1;
		recCounter = 0;
		curSlice = -1;
		curSliceCount = 0;
		sliceVolts = 0.f;
	}

	// --- Patch: settings in JSON, recording as WAV in the patch storage directory

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "bars", json_integer(bars));
		json_object_set_new(root, "recCounter", json_integer(recCounter));
		const MorelBuffer& b = bufs[activeBuf.load()];
		json_object_set_new(root, "name", json_string(b.length > 0 ? b.name : ""));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "bars");
		if (j && json_is_integer(j)) {
			int v = (int) json_integer_value(j);
			for (int c : BAR_CHOICES)
				if (c == v)
					bars = v;
		}
		j = json_object_get(root, "recCounter");
		if (j && json_is_integer(j))
			recCounter = std::max(0, (int) json_integer_value(j));
		j = json_object_get(root, "name");
		savedName = (j && json_is_string(j)) ? json_string_value(j) : "";
	}

	// Thread-safe with process() (SDK). Writes the playing material as float32 WAV, scaled 5 V -> 1.0.
	void onSave(const SaveEvent& e) override {
		std::lock_guard<std::mutex> lock(bufMutex);
		const MorelBuffer& b = bufs[activeBuf.load()];
		if (b.length <= 0) {
			std::string dir = getPatchStorageDirectory();
			std::string path = system::join(dir, STORAGE_FILE);
			if (system::isFile(path))
				system::remove(path);
			return;
		}
		std::vector<float> l(b.left.begin(), b.left.begin() + b.length);
		std::vector<float> r(b.right.begin(), b.right.begin() + b.length);
		for (int i = 0; i < b.length; i++) {
			l[i] *= 0.2f;
			r[i] *= 0.2f;
		}
		std::string path = system::join(createPatchStorageDirectory(), STORAGE_FILE);
		std::string err;
		if (!morel::writeWavFloat(path, l.data(), r.data(), b.length, (int) b.sampleRate, err))
			WARN("Morel: could not save %s: %s", path.c_str(), err.c_str());
	}

	// Thread-safe with process() (SDK): loads the stored recording directly into the playing buffer.
	void onAdd(const AddEvent& e) override {
		std::string path = system::join(getPatchStorageDirectory(), STORAGE_FILE);
		if (!system::isFile(path))
			return;
		std::lock_guard<std::mutex> lock(bufMutex);
		int t = activeBuf.load();
		MorelBuffer& b = bufs[t];
		morel::WavInfo info;
		if (!morel::readWav(path, b.left.data(), b.right.data(), capacity, info) || info.frames <= 0) {
			WARN("Morel: could not load %s: %s", path.c_str(), info.error.c_str());
			return;
		}
		for (int i = 0; i < info.frames; i++) {
			b.left[i] *= 5.f;
			b.right[i] *= 5.f;
		}
		b.generation++;
		b.sampleRate = info.sampleRate;
		std::snprintf(b.name, sizeof(b.name), "%s", savedName.empty() ? "REC" : savedName.c_str());
		b.length = info.frames;
		analyseRequest = t;
	}

	// --- Worker thread: analysis and re-slicing, never in process()

	void analyse(int b, float sens, bool full) {
		std::lock_guard<std::mutex> lock(bufMutex);
		MorelBuffer& buf = bufs[b];
		int gen = buf.generation.load();
		int len = buf.length;
		if (len <= 0)
			return;
		int next = 1 - buf.sliceCur.load();
		morel::computeSlices(buf.left.data(), buf.right.data(), len, buf.sampleRate, sens, buf.slices[next]);
		if (full) {
			for (int c = 0; c < OVERVIEW_COLS; c++) {
				int a = (int) ((long long) len * c / OVERVIEW_COLS);
				int z = std::max(a + 1, (int) ((long long) len * (c + 1) / OVERVIEW_COLS));
				float lo = 0.f, hi = 0.f;
				for (int i = a; i < z && i < len; i++) {
					float s = buf.left[i] + buf.right[i];
					if (!std::isfinite(s))
						continue;
					lo = std::min(lo, s);
					hi = std::max(hi, s);
				}
				buf.ovMin[c] = lo;
				buf.ovMax[c] = hi;
			}
		}
		if (buf.generation.load() != gen)
			return;   // a new recording started meanwhile: drop the result
		buf.sliceCur = next;
		if (full)
			readyBuf = b;
	}

	// RETRO: copy the requested frames from the ring buffer into the claimed buffer, then analyse.
	void doRetro(int t) {
		{
			std::lock_guard<std::mutex> lock(bufMutex);
			MorelBuffer& b = bufs[t];
			ring.copy(retroStart, retroLen, b.left.data(), b.right.data());
			b.sampleRate = retroRate;
			std::snprintf(b.name, sizeof(b.name), "%s", retroName);
			b.length = retroLen;
		}
		analyseRequest = t;
	}

	// WAV load: ask the audio thread for a free buffer, read the file into it, then analyse.
	void doLoad() {
		std::string path, name;
		{
			std::lock_guard<std::mutex> lock(loadMutex);
			path = loadPath;
			name = loadName;
		}
		claimGranted = -1;
		claimRequest = 1;
		int g = -1;
		for (int i = 0; i < 200 && workerRun; i++) {   // up to 2 s
			g = claimGranted.load();
			if (g >= 0)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		claimRequest = 0;
		if (g < 0) {
			std::snprintf(loadError, sizeof(loadError), "load: busy (recording?)");
			return;
		}
		morel::WavInfo info;
		bool ok;
		{
			std::lock_guard<std::mutex> lock(bufMutex);
			MorelBuffer& b = bufs[g];
			ok = morel::readWav(path, b.left.data(), b.right.data(), capacity, info);
			if (ok && info.frames >= (int) (MIN_REC_SECONDS * info.sampleRate)) {
				// WAV files are scaled to +-1; Morel records +-5 V audio, so loaded files play at 5 V.
				for (int i = 0; i < info.frames; i++) {
					b.left[i] *= 5.f;
					b.right[i] *= 5.f;
				}
				b.sampleRate = info.sampleRate;
				std::snprintf(b.name, sizeof(b.name), "%s", name.c_str());
				b.length = info.frames;
			}
			else if (ok) {
				ok = false;
				info.error = "file too short";
			}
		}
		if (!ok) {
			std::snprintf(loadError, sizeof(loadError), "load: %s", info.error.c_str());
			return;
		}
		loadError[0] = 0;
		analyseRequest = g;
	}

	void workerLoop() {
		float lastSens[2] = {-1.f, -1.f};
		int lastGen[2] = {-1, -1};
		auto lastReslice = std::chrono::steady_clock::now();
		while (workerRun) {
			if (ringWanted.load() && !ringReady.load()) {
				ringL.assign(CAPACITY_FRAMES, 0.f);
				ringR.assign(CAPACITY_FRAMES, 0.f);
				ring.attach(ringL.data(), ringR.data(), CAPACITY_FRAMES);
				ringReady = true;
			}
			int rt = retroTarget.load();
			if (rt >= 0) {
				doRetro(rt);
				retroTarget = -1;
			}
			if (loadPending.load()) {
				doLoad();
				loadPending = false;
			}
			int req = analyseRequest.exchange(-1);
			if (req >= 0) {
				float s = sensTarget.load();
				analyse(req, s, true);
				lastSens[req] = s;
				lastGen[req] = bufs[req].generation.load();
			}
			else {
				auto now = std::chrono::steady_clock::now();
				if (now - lastReslice >= std::chrono::milliseconds(50)) {
					float s = sensTarget.load();
					int cand[2] = {activeBuf.load(), readyBuf.load()};
					for (int k = 0; k < 2; k++) {
						int b = cand[k];
						if (b < 0 || bufs[b].length <= 0 || lastGen[b] != bufs[b].generation.load())
							continue;
						if (std::fabs(s - lastSens[b]) > 0.01f) {
							analyse(b, s, false);
							lastSens[b] = s;
							lastReslice = now;
						}
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	// --- Audio thread

	float in(int id) {
		return morel::finiteOr(inputs[id].getVoltage(), 0.f);
	}

	void randomize() {
		for (int i = 0; i < morel::NUM_STEPS; i++)
			params[STEP_PARAMS + i].setValue(random::uniform());
		rndLightPulse.trigger(0.1f);
	}

	void cycleParam(int id, int count) {
		int v = (int) std::lround(params[id].getValue());
		params[id].setValue((float) ((v + 1) % count));
	}

	void startRecording() {
		recBuf = claimFreeBuffer();
		recPos = 0;
		livePeak = 0.f;
		liveBlock = std::max(256, (int) std::ceil(capacity / (float) (LIVE_MAX_BLOCKS - 1)));
		recording = true;
	}

	void stopRecording() {
		recording = false;
		MorelBuffer& b = bufs[recBuf];
		if (recPos < (int) (MIN_REC_SECONDS * engineRate))
			return;   // too short: discarded
		b.length = recPos;
		b.sampleRate = engineRate;
		recCounter++;
		snprintf(b.name, sizeof(b.name), "REC %03d", recCounter % 1000);
		analyseRequest = recBuf;
	}

	// Takes over the buffer that is not playing for new material (recording, RETRO, file).
	int claimFreeBuffer() {
		int t = 1 - activeBuf.load();
		if (readyBuf.load() == t)
			readyBuf = -1;
		player.release(t);
		grains.release(t);
		bufs[t].generation++;
		bufs[t].length = 0;
		return t;
	}

	// RETRO: the last `bars` bars from the ring buffer become the new recording (copied by the worker).
	void requestRetro(bool running, bool clockConnected, float spm) {
		if (!ringReady.load() || retroTarget.load() >= 0)
			return;
		double internal = 60.0 / std::max(spm, 1.f) * engineRate;
		double period = (!running && !clockConnected) ? internal : stepPeriod.get(internal);
		int want = (int) std::min((double) capacity, bars * morel::STEPS_PER_BAR * period);
		int start, n;
		ring.newest(want, start, n);
		if (n < (int) (MIN_REC_SECONDS * engineRate))
			return;
		int t = claimFreeBuffer();
		retroStart = start;
		retroLen = n;
		retroRate = engineRate;
		recCounter++;
		std::snprintf(retroName, sizeof(retroName), "REC %03d", recCounter % 1000);
		retroTarget = t;
	}

	// UI thread: load a WAV file (worker does the reading).
	void requestLoad(const std::string& path, const std::string& name) {
		{
			std::lock_guard<std::mutex> lock(loadMutex);
			loadPath = path;
			loadName = name;
		}
		loadPending = true;
	}

	void triggerStep() {
		MorelBuffer& b = bufs[activeBuf.load()];
		if (b.length <= 0) {
			curSlice = -1;
			curSliceCount = 0;
			sliceVolts = 0.f;
			return;
		}
		morel::SliceList sl = b.currentSlices();   // copy: the worker may publish a new list later
		int n = sl.count;
		int idx = morel::sliceIndex(params[STEP_PARAMS + seq.current()].getValue(), n);
		curSlice = idx;
		curSliceCount = n;
		sliceVolts = morel::sliceVoltage(idx, n);
		if (idx < 0)
			return;
		double start = sl.sliceStart(idx);
		double sliceLen = sl.sliceEnd(idx) - start;
		morel::SampleRef s;
		s.left = b.left.data();
		s.right = b.right.data();
		s.length = b.length;
		s.sampleRate = b.sampleRate;
		s.id = activeBuf.load();
		if (params[GRAIN_PARAM].getValue() > 0.5f) {
			// GRAIN: the cloud moves to the new slice; a still sounding CUT voice fades out.
			grains.setSlice(s, start, start + sliceLen);
			player.trigger(morel::SampleRef(), 0.0, 0.0, 0.f, 0.f, engineRate);
			return;
		}
		// CUT: running grains finish with their window, no new grains.
		grains.stopSpawning();
		float span = morel::clampf(params[SPAN_PARAM].getValue() + in(SPAN_INPUT) * CV_SCALE, 0.01f, 1.f);
		float scatter = morel::clampf(params[SCATTER_PARAM].getValue() + in(SCATTER_INPUT) * CV_SCALE, 0.f, 1.f);
		double end = start + span * sliceLen;
		float semis = 0.f, level = 1.f;
		if (scatter > 0.f) {
			// CUT scatter: later start (up to 20 ms), pitch (up to +-1 semitone), level (down to -6 dB).
			double offset = scatterRng.uniform() * 0.02 * scatter * b.sampleRate;
			start = std::min(start + offset, end - 1.0);
			semis = (scatterRng.uniform() * 2.f - 1.f) * scatter;
			level = std::pow(10.f, -6.f * scatter * scatterRng.uniform() / 20.f);
		}
		player.trigger(s, start, end, semis, level, engineRate);
	}

	void process(const ProcessArgs& args) override {
		const float dt = args.sampleTime;
		engineRate = args.sampleRate;

		// --- Buttons
		if (rndButton.process(params[RND_PARAM].getValue() > 0.5f) | rndInTrigger.process(inputs[RND_INPUT].getVoltage(), 0.1f, 1.f))
			randomize();
		if (dirButton.process(params[DIR_BUTTON_PARAM].getValue() > 0.5f))
			cycleParam(DIR_PARAM, morel::NUM_DIRS);
		if (modeButton.process(params[MODE_BUTTON_PARAM].getValue() > 0.5f))
			cycleParam(MODE_PARAM, morel::NUM_REC_MODES);
		sensTarget = morel::clampf(params[SENS_PARAM].getValue() + in(SENS_INPUT) * CV_SCALE, 0.f, 1.f);

		// --- Transport
		if (runInTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 1.f))
			params[RUN_PARAM].setValue(params[RUN_PARAM].getValue() > 0.5f ? 0.f : 1.f);
		bool running = params[RUN_PARAM].getValue() > 0.5f;
		transport.setRunning(running);

		bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		if (reset) {
			seq.reset();
			transport.reset();
		}

		int len = morel::lengthFrom(params[LEN_PARAM].getValue(), in(LEN_INPUT));
		int dir = (int) std::lround(params[DIR_PARAM].getValue());
		bool clockEdge = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		bool advEdge = advButton.process(params[ADV_PARAM].getValue() > 0.5f);
		float spm = morel::stepsPerMinute(params[TEMPO_PARAM].getValue(), 0.f);
		int factor = morel::clockFactorIndex(params[TEMPO_PARAM].getValue(), 0.f);
		bool tick = transport.process(inputs[CLOCK_INPUT].isConnected(), clockEdge, advEdge, spm, dt, factor);
		bool eoc = false;
		if (tick) {
			eoc = seq.advance(len, dir);
			if (eoc)
				eocPulse.trigger(1e-3f);
			stepPulse.trigger(1e-3f);
		}

		// --- Recording: MAN / CLK / LVL / RETRO (after the transport, so CLK starts exactly on a step)
		stepPeriod.process(tick);
		int recMode = (int) std::lround(params[MODE_PARAM].getValue());
		ringWanted = ringWanted.load() || recMode == morel::REC_RETRO;
		float inL = in(IN_L_INPUT);
		float inR = inputs[IN_R_INPUT].isConnected() ? in(IN_R_INPUT) : inL;
		if (recMode == morel::REC_RETRO && ringReady.load())
			ring.write(inL, inR);
		bool recEdge = recButton.process(params[REC_PARAM].getValue() > 0.5f);
		recEdge |= recInTrigger.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f);
		morel::RecEvent re = recLogic.process(recMode, recEdge, tick, std::max(std::fabs(inL), std::fabs(inR)), bars);
		if (re.stop && recording)
			stopRecording();
		if (re.start)
			startRecording();
		if (re.retro)
			requestRetro(running, inputs[CLOCK_INPUT].isConnected(), spm);
		recArmed = recLogic.armed();
		if (recording) {
			MorelBuffer& b = bufs[recBuf];
			if (recPos < capacity) {
				b.left[recPos] = inL;
				b.right[recPos] = inR;
				float sum = inL + inR;
				int blk = std::min(recPos / liveBlock, LIVE_MAX_BLOCKS - 1);
				if (recPos % liveBlock == 0) {
					liveMin[blk] = sum;
					liveMax[blk] = sum;
				}
				else {
					liveMin[blk] = std::min(liveMin[blk], sum);
					liveMax[blk] = std::max(liveMax[blk], sum);
				}
				livePeak = std::max(livePeak, std::fabs(sum));
				recPos++;
			}
			else {
				stopRecording();
				recLogic.forceStop();
			}
		}
		// A file is waiting for a free buffer (never while recording or armed)
		if (claimRequest.load() == 1 && !recording && !recArmed && claimGranted.load() < 0)
			claimGranted = claimFreeBuffer();

		// --- Switch to a new recording: at EOC or RESET, immediately when stopped or empty
		int ready = readyBuf.load();
		if (ready >= 0 && !(recording && ready == recBuf)) {
			int act = activeBuf.load();
			if (ready != act && (eoc || reset || !running || bufs[act].length <= 0)) {
				activeBuf = ready;
				readyBuf = -1;
			}
			else if (ready == act) {
				readyBuf = -1;
			}
		}

		bool hit = tick;
		hit |= trigButton.process(params[TRIG_PARAM].getValue() > 0.5f);
		hit |= trigInTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		if (hit)
			triggerStep();

		// --- Playback: CUT voices and grain cloud (both run, so switching modes crossfades)
		bool grainMode = params[GRAIN_PARAM].getValue() > 0.5f;
		if (grainMode != lastGrainMode) {
			lastGrainMode = grainMode;
			if (grainMode) {
				if (curSlice >= 0)
					triggerStep();   // continue the current slice as a grain cloud right away
			}
			else {
				grains.stopSpawning();
			}
		}
		float oct = morel::clampf(params[SPEED_PARAM].getValue() + in(VOCT_INPUT), -5.f, 5.f);
		float speedMul = std::pow(2.f, oct);
		float span = morel::clampf(params[SPAN_PARAM].getValue() + in(SPAN_INPUT) * CV_SCALE, morel::SPAN_MIN, 1.f);
		float scatter = morel::clampf(params[SCATTER_PARAM].getValue() + in(SCATTER_INPUT) * CV_SCALE, 0.f, 1.f);
		float outL, outR, gL, gR;
		bool eos = player.process(speedMul, engineRate, outL, outR);
		eos |= grains.process(span, scatter, speedMul, engineRate, gL, gR);
		if (eos)
			eosPulse.trigger(1e-3f);
		outL += gL;
		outR += gR;
		outputs[OUT_L_OUTPUT].setVoltage(outL);
		outputs[OUT_R_OUTPUT].setVoltage(outR);

		// Envelope follower of max(|L|, |R|): attack 1 ms, release 50 ms, x2.
		float x = std::max(std::fabs(outL), std::fabs(outR));
		float c = x > envState ? std::exp(-dt / 0.001f) : std::exp(-dt / 0.05f);
		envState = c * envState + (1.f - c) * x;
		outputs[ENV_OUTPUT].setVoltage(envState * 2.f);

		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(dt) ? 10.f : 0.f);
		outputs[STEP_OUTPUT].setVoltage(stepPulse.process(dt) ? 10.f : 0.f);
		outputs[EOS_OUTPUT].setVoltage(eosPulse.process(dt) ? 10.f : 0.f);
		outputs[SLICE_OUTPUT].setVoltage(sliceVolts);

		// --- Lights and display state (not audio-rate critical)
		if (lightDivider.process()) {
			float ldt = dt * lightDivider.getDivision();
			lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);
			recBlink += ldt;
			if (recBlink >= 0.5f)
				recBlink -= 0.5f;
			lights[REC_LIGHT].setBrightness(recording ? 1.f : (recArmed ? (recBlink < 0.25f ? 1.f : 0.f) : 0.f));
			lights[RND_LIGHT].setBrightness(rndLightPulse.process(ldt) ? 1.f : 0.f);
			lights[GRAIN_LIGHT].setBrightness(params[GRAIN_PARAM].getValue() > 0.5f ? 1.f : 0.f);
			int cur = seq.current();
			for (int i = 0; i < morel::NUM_STEPS; i++) {
				float br = i == cur ? 1.f : (i < len ? 0.15f : 0.f);
				lights[STEP_LIGHTS + i].setBrightness(br);
			}
			playheadFrac = grainMode ? grains.playhead() : player.playhead();
		}
	}
};


// ---------------------------------------------------------------------------------------------
// Widgets

// Dark display in the style of the VCV core modules. Works without a module (browser preview).
struct MorelDisplay : widget::Widget {
	Morel* module = nullptr;
	std::string fontPath = asset::system("res/fonts/ShareTechMono-Regular.ttf");

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 4.f);
		nvgFillColor(args.vg, nvgRGB(0x1b, 0x1c, 0x20));
		nvgFill(args.vg);
		nvgStrokeWidth(args.vg, 1.f);
		nvgStrokeColor(args.vg, nvgRGB(0x0e, 0x0e, 0x10));
		nvgStroke(args.vg);
		Widget::draw(args);
	}

	void text(NVGcontext* vg, float x, float y, const std::string& s, NVGcolor col, int align, float size) {
		nvgFontSize(vg, size);
		nvgFillColor(vg, col);
		nvgTextAlign(vg, align);
		nvgText(vg, x, y, s.c_str(), NULL);
	}

	void drawLive(NVGcontext* vg, float x0, float ww, float mid, float half, float oldSeconds, NVGcolor col) {
		int blockLen = std::max(module->liveBlock, 1);
		int blocks = std::min(module->recPos / blockLen + 1, LIVE_MAX_BLOCKS);
		float recSeconds = module->recPos / std::max(module->engineRate, 1.f);
		float widthFrac = (oldSeconds > 0.f && recSeconds < oldSeconds) ? recSeconds / oldSeconds : 1.f;
		int cols = std::max(1, (int) std::lround(OVERVIEW_COLS * widthFrac));
		float peak = std::max(module->livePeak, 1e-6f);
		nvgBeginPath(vg);
		for (int c = 0; c < cols; c++) {
			int a = (int) ((long long) blocks * c / cols);
			int z = std::max(a + 1, (int) ((long long) blocks * (c + 1) / cols));
			float lo = 0.f, hi = 0.f;
			for (int k = a; k < z && k < blocks; k++) {
				lo = std::min(lo, module->liveMin[k]);
				hi = std::max(hi, module->liveMax[k]);
			}
			lo = morel::clampf(lo / peak, -1.f, 1.f);
			hi = morel::clampf(hi / peak, -1.f, 1.f);
			float x = x0 + ww * (c + 0.5f) / OVERVIEW_COLS;
			nvgMoveTo(vg, x, mid - hi * half);
			nvgLineTo(vg, x, mid - lo * half + 0.5f);
		}
		nvgStrokeColor(vg, nvgTransRGBA(col, 0xd0));
		nvgStrokeWidth(vg, std::max(0.6f, ww / OVERVIEW_COLS));
		nvgStroke(vg);
		// Record head
		float hx = x0 + ww * cols / (float) OVERVIEW_COLS;
		nvgBeginPath(vg);
		nvgMoveTo(vg, hx, mid - half);
		nvgLineTo(vg, hx, mid + half);
		nvgStrokeColor(vg, col);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
		if (!font || font->handle < 0)
			font = APP->window->uiFont;
		if (!font || font->handle < 0)
			return;
		nvgFontFaceId(vg, font->handle);

		const NVGcolor fg = nvgRGB(0xe6, 0xe6, 0xe6);
		const NVGcolor dim = nvgRGB(0x8a, 0x8d, 0x96);
		const NVGcolor wave = nvgRGB(0x9f, 0xb4, 0xd8);
		const NVGcolor accent = nvgRGB(0xf4, 0x7c, 0x6c);
		const float W = box.size.x, H = box.size.y;
		const float pad = 8.f;
		const float top = 17.f, bottom = H - 17.f;
		const float mid = (top + bottom) / 2.f, half = (bottom - top) / 2.f;

		// Values (defaults for the browser preview)
		int dir = morel::DIR_FWD, mode = morel::REC_MAN, step = 0, len = 8, slice = 0, count = 8;
		bool grain = false, rec = false, running = false, clock = false, armed = false;
		std::string err;
		float recSec = 0.f, sens = 0.5f, playhead = -1.f, spm = 480.f;
		int factor = 0;
		const MorelBuffer* buf = nullptr;
		morel::SliceList sl;
		if (module) {
			dir = (int) std::lround(module->params[Morel::DIR_PARAM].getValue());
			mode = (int) std::lround(module->params[Morel::MODE_PARAM].getValue());
			grain = module->params[Morel::GRAIN_PARAM].getValue() > 0.5f;
			step = module->seq.current();
			len = morel::lengthFrom(module->params[Morel::LEN_PARAM].getValue(), module->inputs[Morel::LEN_INPUT].getVoltage());
			rec = module->recording;
			armed = module->recArmed;
			err = module->loadError;
			recSec = module->recPos / std::max(module->engineRate, 1.f);
			running = module->params[Morel::RUN_PARAM].getValue() > 0.5f;
			clock = module->inputs[Morel::CLOCK_INPUT].isConnected();
			spm = morel::stepsPerMinute(module->params[Morel::TEMPO_PARAM].getValue(), 0.f);
			factor = morel::clockFactorIndex(module->params[Morel::TEMPO_PARAM].getValue(), 0.f);
			sens = module->sensTarget.load();
			playhead = module->playheadFrac.load();
			buf = &module->bufs[module->activeBuf.load()];
			if (buf->length > 0)
				sl = buf->currentSlices();
			else
				buf = nullptr;
			slice = module->curSlice;
			count = buf ? sl.count : 0;
		}

		// Top line
		// Source name: loaded file or "REC nnn". While recording: the number being recorded, in red.
		std::string source;
		NVGcolor sourceCol = fg;
		if (module && rec) {
			char nb[16];
			snprintf(nb, sizeof(nb), "REC %03d", (module->recCounter + 1) % 1000);
			source = nb;
			sourceCol = accent;
		}
		else if (module) {
			source = buf ? std::string(buf->name) : "";
		}
		else {
			source = "REC 001";
		}
		if (source.size() > 28)
			source = source.substr(0, 27) + "...";
		float nameEnd = pad;
		if (!source.empty()) {
			text(vg, pad, 11.f, source, sourceCol, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 9.f);
			float bounds[4];
			nvgFontSize(vg, 9.f);
			nameEnd = pad + nvgTextBounds(vg, 0, 0, source.c_str(), NULL, bounds);
		}
		std::string status = std::string(grain ? "GRAIN" : "CUT") + "   " + morel::dirName(dir) + "   REC " + morel::recModeName(mode);
		if (mode == morel::REC_CLK || mode == morel::REC_RETRO)
			status += " " + std::to_string(module ? module->bars : 2) + " BAR";
		text(vg, W - pad, 11.f, status, dim, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, 8.f);
		// Next to the name: armed state or a load error
		if (armed)
			text(vg, nameEnd + 6.f, 11.f, mode == morel::REC_LVL ? "ARMED  -  waiting for signal" : "ARMED  -  next step", accent, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 7.f);
		else if (!err.empty())
			text(vg, nameEnd + 6.f, 11.f, err, accent, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 7.f);

		// Wave area
		nvgSave(vg);
		nvgScissor(vg, pad, top, W - 2 * pad, bottom - top);
		float x0 = pad, x1 = W - pad, ww = x1 - x0;
		if (module && !buf) {
			if (!rec)
				text(vg, W / 2.f, mid + 3.f, "empty  -  press REC", dim, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 9.f);
		}
		else {
			// Waveform: real overview or a static demo pattern for the browser.
			float peak = buf ? std::max(sl.envPeak, 1e-6f) : 1.f;
			nvgBeginPath(vg);
			for (int c = 0; c < OVERVIEW_COLS; c++) {
				float lo, hi;
				if (buf) {
					lo = buf->ovMin[c] / peak;
					hi = buf->ovMax[c] / peak;
				}
				else {
					float ph = std::fmod(c / 64.f, 1.f);
					float a = std::exp(-ph * 6.f) * (0.5f + 0.5f * std::fabs(std::sin(c * 0.7f)));
					lo = -a;
					hi = a;
				}
				lo = morel::clampf(lo, -1.f, 1.f);
				hi = morel::clampf(hi, -1.f, 1.f);
				float x = x0 + ww * (c + 0.5f) / OVERVIEW_COLS;
				nvgMoveTo(vg, x, mid - hi * half);
				nvgLineTo(vg, x, mid - lo * half + 0.5f);
			}
			nvgStrokeColor(vg, wave);
			nvgStrokeWidth(vg, std::max(0.6f, ww / OVERVIEW_COLS));
			nvgStroke(vg);

			// Current slice highlight and markers
			int n = buf ? sl.count : 8;
			int length = buf ? sl.length : 8;
			for (int i = 0; i < n; i++) {
				float sx = x0 + ww * (buf ? sl.sliceStart(i) : i) / (float) std::max(length, 1);
				float ex = x0 + ww * (buf ? sl.sliceEnd(i) : i + 1) / (float) std::max(length, 1);
				if (i == slice) {
					nvgBeginPath(vg);
					nvgRect(vg, sx, top, ex - sx, bottom - top);
					nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x18));
					nvgFill(vg);
				}
				nvgBeginPath(vg);
				nvgMoveTo(vg, sx + 0.5f, top);
				nvgLineTo(vg, sx + 0.5f, bottom);
				nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x50));
				nvgStrokeWidth(vg, 1.f);
				nvgStroke(vg);
				if (ex - sx > 9.f)
					text(vg, sx + 2.f, top + 7.f, std::to_string(i + 1), i == slice ? fg : dim, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 6.5f);
			}

			// Threshold line (SENS 0 % = peak, 100 % = -48 dB)
			float thr = std::pow(10.f, -48.f * sens / 20.f);
			for (int sgn = -1; sgn <= 1; sgn += 2) {
				float y = mid - sgn * thr * half;
				nvgBeginPath(vg);
				for (float x = x0; x < x1; x += 6.f) {
					nvgMoveTo(vg, x, y);
					nvgLineTo(vg, std::min(x + 3.f, x1), y);
				}
				nvgStrokeColor(vg, nvgRGBA(0xf4, 0x7c, 0x6c, 0xa0));
				nvgStrokeWidth(vg, 0.8f);
				nvgStroke(vg);
			}

			// Playhead
			if (playhead >= 0.f && playhead <= 1.f) {
				float px = x0 + ww * playhead;
				nvgBeginPath(vg);
				nvgMoveTo(vg, px, top);
				nvgLineTo(vg, px, bottom);
				nvgStrokeColor(vg, fg);
				nvgStrokeWidth(vg, 1.f);
				nvgStroke(vg);
			}
		}
		// Recording in progress: live waveform in red on top of the current material.
		// Time scale: the length of the current recording (if any), so the red wave runs across it
		// in real time; a longer new recording is squeezed to the full width.
		if (module && rec)
			drawLive(vg, x0, ww, mid, half, buf ? buf->length / std::max(buf->sampleRate, 1.f) : 0.f, accent);
		nvgRestore(vg);

		// Bottom line
		char s[64];
		snprintf(s, sizeof(s), "STEP %d/%d", step + 1, len);
		text(vg, pad, H - 5.f, s, fg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 9.f);
		if (count > 0)
			snprintf(s, sizeof(s), "SLICE %d/%d", slice >= 0 ? slice + 1 : 1, count);
		else
			snprintf(s, sizeof(s), "SLICE -");
		text(vg, pad + 62.f, H - 5.f, s, fg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, 9.f);
		if (rec) {
			snprintf(s, sizeof(s), "REC %.1f s", recSec);
			text(vg, W / 2.f + 34.f, H - 5.f, s, accent, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, 9.f);
		}
		static const char* FACTORS[5] = {"/4", "/2", "x1", "x2", "x4"};
		if (clock)
			snprintf(s, sizeof(s), "CLK %s", FACTORS[factor - zenon::CLOCK_FACTOR_MIN]);
		else
			snprintf(s, sizeof(s), "%.1f BPM", spm / 4.f);
		text(vg, W - pad, H - 5.f, s, running ? fg : dim, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, 9.f);

		Widget::drawLayer(args, layer);
	}
};

// Small knob with detents (LEN).
struct MorelSnapSmallKnob : RoundSmallBlackKnob {
	MorelSnapSmallKnob() {
		snap = true;
	}
};

struct MorelWidget : ModuleWidget {
	MorelWidget(Morel* module) {
		setModule(module);
		// Panel concept A (12.11.) in Philipp's style. Panel and positions come from one layout definition:
		// the generator writes res/Morel.svg and src/MorelLayout.hpp together (see docs/Panel-Design-Regeln.md).
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Morel.svg")));
		namespace L = morel_layout;
		auto at = [](const L::Pos& p) { return mm2px(Vec(p.x, p.y)); };

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		MorelDisplay* disp = createWidget<MorelDisplay>(at(L::DISPLAY_POS));
		disp->box.size = mm2px(Vec(L::DISPLAY_SIZE.x, L::DISPLAY_SIZE.y));
		disp->module = module;
		addChild(disp);

		// Transport and sequencer controls, CV inputs directly below their control
		addInput(createInputCentered<PJ301MPort>(at(L::CLOCK_IN), module, Morel::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(at(L::RESET_IN), module, Morel::RESET_INPUT));
		addParam(createLightParamCentered<VCVLightBezelLatch<GreenLight>>(at(L::RUN), module, Morel::RUN_PARAM, Morel::RUN_LIGHT));
		addInput(createInputCentered<PJ301MPort>(at(L::RUN_IN), module, Morel::RUN_INPUT));
		addParam(createParamCentered<VCVBezel>(at(L::ADV), module, Morel::ADV_PARAM));
		addParam(createParamCentered<VCVBezel>(at(L::TRIG), module, Morel::TRIG_PARAM));
		addInput(createInputCentered<PJ301MPort>(at(L::TRIG_IN), module, Morel::TRIG_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(at(L::TEMPO), module, Morel::TEMPO_PARAM));
		addParam(createParamCentered<MorelSnapSmallKnob>(at(L::LEN), module, Morel::LEN_PARAM));
		addInput(createInputCentered<PJ301MPort>(at(L::LEN_IN), module, Morel::LEN_INPUT));
		addParam(createParamCentered<VCVBezel>(at(L::DIR), module, Morel::DIR_BUTTON_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(at(L::RND), module, Morel::RND_PARAM, Morel::RND_LIGHT));
		addInput(createInputCentered<PJ301MPort>(at(L::RND_IN), module, Morel::RND_INPUT));

		// Steps
		const L::Pos steps[8] = {L::STEP_1, L::STEP_2, L::STEP_3, L::STEP_4, L::STEP_5, L::STEP_6, L::STEP_7, L::STEP_8};
		const L::Pos leds[8] = {L::STEP_LED_1, L::STEP_LED_2, L::STEP_LED_3, L::STEP_LED_4,
			L::STEP_LED_5, L::STEP_LED_6, L::STEP_LED_7, L::STEP_LED_8};
		for (int i = 0; i < morel::NUM_STEPS; i++) {
			addParam(createParamCentered<RoundBlackKnob>(at(steps[i]), module, Morel::STEP_PARAMS + i));
			addChild(createLightCentered<SmallLight<WhiteLight>>(at(leds[i]), module, Morel::STEP_LIGHTS + i));
		}

		// Record
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(at(L::REC), module, Morel::REC_PARAM, Morel::REC_LIGHT));
		addInput(createInputCentered<PJ301MPort>(at(L::REC_IN), module, Morel::REC_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(at(L::SENS), module, Morel::SENS_PARAM));
		addInput(createInputCentered<PJ301MPort>(at(L::SENS_CV), module, Morel::SENS_INPUT));
		addParam(createParamCentered<VCVBezel>(at(L::MODE), module, Morel::MODE_BUTTON_PARAM));

		// Play
		addParam(createParamCentered<RoundBlackKnob>(at(L::SPEED), module, Morel::SPEED_PARAM));
		addInput(createInputCentered<PJ301MPort>(at(L::VOCT), module, Morel::VOCT_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(at(L::SPAN), module, Morel::SPAN_PARAM));
		addInput(createInputCentered<PJ301MPort>(at(L::SPAN_CV), module, Morel::SPAN_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(at(L::SCATTER), module, Morel::SCATTER_PARAM));
		addInput(createInputCentered<PJ301MPort>(at(L::SCATTER_CV), module, Morel::SCATTER_INPUT));
		addParam(createLightParamCentered<VCVLightBezelLatch<WhiteLight>>(at(L::GRAIN), module, Morel::GRAIN_PARAM, Morel::GRAIN_LIGHT));

		// Patch row: audio in | trigger and CV outputs | audio out
		addInput(createInputCentered<PJ301MPort>(at(L::IN_L), module, Morel::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(at(L::IN_R), module, Morel::IN_R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::STEP_OUT), module, Morel::STEP_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::EOC), module, Morel::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::EOS), module, Morel::EOS_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::SLICE), module, Morel::SLICE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::ENV), module, Morel::ENV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::OUT_L), module, Morel::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(at(L::OUT_R), module, Morel::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Morel* module = dynamic_cast<Morel*>(this->module);
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		std::vector<std::string> dirs;
		for (int d = 0; d < morel::NUM_DIRS; d++)
			dirs.push_back(morel::dirName(d));
		menu->addChild(createIndexSubmenuItem("Direction", dirs,
			[=]() { return (size_t) std::lround(module->params[Morel::DIR_PARAM].getValue()); },
			[=](size_t i) { module->params[Morel::DIR_PARAM].setValue((float) i); }));
		menu->addChild(createIndexSubmenuItem("Recording mode", {"MAN", "CLK", "LVL", "RETRO"},
			[=]() { return (size_t) std::lround(module->params[Morel::MODE_PARAM].getValue()); },
			[=](size_t i) { module->params[Morel::MODE_PARAM].setValue((float) i); }));
		menu->addChild(createIndexSubmenuItem("Bars (CLK, RETRO)", {"1", "2", "4", "8"},
			[=]() {
				for (size_t i = 0; i < 4; i++)
					if (BAR_CHOICES[i] == module->bars)
						return i;
				return (size_t) 1;
			},
			[=](size_t i) { module->bars = BAR_CHOICES[i]; }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load WAV...", "", [=]() {
			osdialog_filters* filters = osdialog_filters_parse("WAV:wav,WAV");
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
			osdialog_filters_free(filters);
			if (!path)
				return;
			std::string p = path;
			std::free(path);
			module->requestLoad(p, system::getFilename(p));
		}));
	}
};


Model* modelMorel = createModel<Morel, MorelWidget>("Morel");
