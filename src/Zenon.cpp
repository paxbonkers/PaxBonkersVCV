#include "plugin.hpp"
#include "dsp/ZenonSequencer.hpp"
#include "dsp/ZenonVoice.hpp"

// Zenon: polymetric percussion. Two 8-step tracks (A, B) with their own length and a free
// destination each, driving a two-oscillator percussion voice.
// Sequencer logic: dsp/ZenonSequencer.hpp, sound: dsp/ZenonVoice.hpp (both Rack-independent, unit-tested).
// Signal contract: docs/modules/Zenon-Signalvertrag.md (project docs).

static const float OSC_VOLTS = 5.f;
static const float ENV_VOLTS = 10.f;
static const float TRACK_VOLTS = 10.f;
// Internal audio 1.0 = 8 V, so a hit with default settings peaks at about 4.3 V.
static const float AUDIO_VOLTS = 8.f;
static const float EXT_VOLTS = 5.f;
static const float CV_SCALE = 0.1f;   // 1 V = 10 % for 0..1 controls
static const float C4_HZ_DISPLAY = 261.6256f;

struct Zenon;

// TEMPO shows steps/min with the internal clock and the clock factor when CLOCK is patched.
struct TempoQuantity : ParamQuantity {
	std::string getDisplayValueString() override;
	std::string getUnit() override;
};

struct Zenon : Module {
	// IDs are stored in patches. Never reorder; only append new entries.
	enum ParamId {
		TEMPO_PARAM,
		RUN_PARAM,
		ADV_PARAM,
		TRIG_PARAM,
		ENUMS(STEP_PARAMS, 16),     // track * 8 + step
		ENUMS(DEST_PARAMS, 2),
		ENUMS(LENGTH_PARAMS, 2),
		ENUMS(AMOUNT_PARAMS, 2),
		ENUMS(RND_PARAMS, 2),
		OSC1_FREQ_PARAM,
		OSC1_WAVE_PARAM,
		OSC2_FREQ_PARAM,
		OSC2_WAVE_PARAM,
		FM_PARAM,
		SYNC_PARAM,
		MIX_OSC1_PARAM,
		MIX_OSC2_PARAM,
		MIX_NOISE_PARAM,
		CUTOFF_PARAM,
		RES_PARAM,
		FILTER_MODE_PARAM,
		LEVEL_PARAM,
		PITCH_DEC_PARAM,
		FILTER_DEC_PARAM,
		AMP_DEC_PARAM,
		PITCH_AMT_PARAM,
		FILTER_AMT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TEMPO_CV_INPUT,
		RUN_INPUT,
		CLOCK_INPUT,
		TRIG_INPUT,
		RESET_INPUT,
		ENUMS(LENGTH_CV_INPUTS, 2),
		ENUMS(RND_INPUTS, 2),
		PITCH1_INPUT,
		PITCH2_INPUT,
		FM_CV_INPUT,
		EXT_INPUT,
		CUTOFF_CV_INPUT,
		LEVEL_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OSC1_OUTPUT,
		OSC2_OUTPUT,
		PITCH_ENV_OUTPUT,
		FILTER_ENV_OUTPUT,
		AMP_ENV_OUTPUT,
		ENUMS(TRACK_OUTPUTS, 2),
		TRIG_OUTPUT,
		AUDIO_OUTPUT,
		// Appended: end-of-cycle trigger per track
		ENUMS(EOC_OUTPUTS, 2),
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT,
		ENUMS(RND_LIGHTS, 2),
		ENUMS(STEP_LIGHTS, 16),
		LIGHTS_LEN
	};

	zenon::Track tracks[zenon::NUM_TRACKS];
	zenon::Transport transport;
	zenon::Voice voice;

	dsp::SchmittTrigger runInTrigger, clockTrigger, trigInTrigger, resetTrigger;
	dsp::SchmittTrigger rndInTrigger[zenon::NUM_TRACKS];
	dsp::BooleanTrigger advButton, trigButton;
	dsp::BooleanTrigger rndButton[zenon::NUM_TRACKS];
	dsp::PulseGenerator trigPulse;
	dsp::PulseGenerator eocPulse[zenon::NUM_TRACKS];
	dsp::PulseGenerator rndLightPulse[zenon::NUM_TRACKS];
	dsp::ClockDivider lightDivider;
	bool runInputGate = false;   // context menu "RUN input", saved in the patch JSON
	bool runInWasHigh = false;

	Zenon() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// TEMPO is stored as log2(steps per minute) and displayed in steps per minute.
		// With an external clock the same knob selects /4, /2, x1, x2, x4 (see TempoQuantity).
		configParam<TempoQuantity>(TEMPO_PARAM, std::log2(zenon::TEMPO_MIN_SPM), std::log2(zenon::TEMPO_MAX_SPM),
			std::log2(480.f), "Tempo", " steps/min", 2.f);
		configSwitch(RUN_PARAM, 0.f, 1.f, 0.f, "Run", {"Stopped", "Running"});
		configButton(ADV_PARAM, "Advance one step");
		configButton(TRIG_PARAM, "Trigger (replay current step)");

		std::vector<std::string> destLabels;
		for (int d = 0; d < zenon::NUM_DESTS; d++)
			destLabels.push_back(zenon::destName(d));
		const char* trackNames[zenon::NUM_TRACKS] = {"Track A", "Track B"};
		for (int t = 0; t < zenon::NUM_TRACKS; t++) {
			std::string n = trackNames[t];
			for (int i = 0; i < zenon::NUM_STEPS; i++)
				configParam(STEP_PARAMS + t * zenon::NUM_STEPS + i, 0.f, 1.f, t == 0 ? 0.f : 1.f,
					n + " step " + std::to_string(i + 1), "%", 0.f, 100.f);
			configSwitch(DEST_PARAMS + t, 0.f, zenon::NUM_DESTS - 1, t == 0 ? zenon::DEST_PITCH : zenon::DEST_VEL,
				n + " destination", destLabels);
			configParam(LENGTH_PARAMS + t, 1.f, (float) zenon::NUM_STEPS, (float) zenon::NUM_STEPS, n + " length", " steps");
			paramQuantities[LENGTH_PARAMS + t]->snapEnabled = true;
			configParam(AMOUNT_PARAMS + t, -1.f, 1.f, 1.f, n + " amount", "%", 0.f, 100.f);
			configButton(RND_PARAMS + t, n + " randomize all steps");
			configInput(LENGTH_CV_INPUTS + t, n + " length CV (1 V = 1 step)");
			configInput(RND_INPUTS + t, n + " randomize trigger");
			configOutput(TRACK_OUTPUTS + t, n + " step value (0-10 V)");
			configOutput(EOC_OUTPUTS + t, n + " end of cycle (trigger when the track returns to step 1)");
			configLight(RND_LIGHTS + t, n + " randomized");
		}

		// Oscillator frequency in octaves relative to C4, displayed in Hz.
		configParam(OSC1_FREQ_PARAM, -5.f, 4.f, -2.f, "Osc 1 frequency", " Hz", 2.f, C4_HZ_DISPLAY);
		configSwitch(OSC1_WAVE_PARAM, 0.f, 1.f, 1.f, "Osc 1 wave", {"Square", "Triangle"});
		configParam(OSC2_FREQ_PARAM, -5.f, 4.f, -1.f, "Osc 2 frequency", " Hz", 2.f, C4_HZ_DISPLAY);
		configSwitch(OSC2_WAVE_PARAM, 0.f, 1.f, 1.f, "Osc 2 wave", {"Square", "Triangle"});
		configParam(FM_PARAM, 0.f, 1.f, 0.f, "FM 1 > 2", "%", 0.f, 100.f);
		configSwitch(SYNC_PARAM, 0.f, 1.f, 0.f, "Hard sync osc 2 to osc 1", {"Off", "On"});
		configParam(MIX_OSC1_PARAM, 0.f, 1.f, 0.8f, "Mix osc 1", "%", 0.f, 100.f);
		configParam(MIX_OSC2_PARAM, 0.f, 1.f, 0.f, "Mix osc 2", "%", 0.f, 100.f);
		configParam(MIX_NOISE_PARAM, 0.f, 1.f, 0.f, "Mix noise / ext", "%", 0.f, 100.f);
		// Cutoff stored as log2(Hz), displayed in Hz.
		configParam(CUTOFF_PARAM, std::log2(20.f), std::log2(20000.f), 11.f, "Cutoff", " Hz", 2.f);
		configParam(RES_PARAM, 0.f, 1.f, 0.2f, "Resonance", "%", 0.f, 100.f);
		configSwitch(FILTER_MODE_PARAM, 0.f, 1.f, 0.f, "Filter mode", {"Low-pass", "High-pass"});
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level", "%", 0.f, 100.f);
		// Decay sliders 0..1, displayed as time to -60 dB: min * (max/min)^x.
		configParam(PITCH_DEC_PARAM, 0.f, 1.f, 0.15f, "Pitch decay", " s",
			zenon::PITCH_DEC_MAX / zenon::PITCH_DEC_MIN, zenon::PITCH_DEC_MIN);
		configParam(FILTER_DEC_PARAM, 0.f, 1.f, 0.3f, "Filter decay", " s",
			zenon::FILTER_DEC_MAX / zenon::FILTER_DEC_MIN, zenon::FILTER_DEC_MIN);
		configParam(AMP_DEC_PARAM, 0.f, 1.f, 0.35f, "Amp decay", " s",
			zenon::AMP_DEC_MAX / zenon::AMP_DEC_MIN, zenon::AMP_DEC_MIN);
		configParam(PITCH_AMT_PARAM, -1.f, 1.f, 0.3f, "Pitch envelope amount", "%", 0.f, 100.f);
		configParam(FILTER_AMT_PARAM, -1.f, 1.f, 0.4f, "Filter envelope amount", "%", 0.f, 100.f);

		configInput(TEMPO_CV_INPUT, "Tempo CV (1 V = double tempo; with external clock 1 V = next factor)");
		configInput(RUN_INPUT, "Run (trigger toggles or gate, see context menu)");
		configInput(CLOCK_INPUT, "Clock (replaces the internal clock)");
		configInput(TRIG_INPUT, "Trigger (replay current step)");
		configInput(RESET_INPUT, "Reset");
		configInput(PITCH1_INPUT, "Osc 1 pitch (1 V/oct)");
		configInput(PITCH2_INPUT, "Osc 2 pitch (1 V/oct)");
		configInput(FM_CV_INPUT, "FM CV (1 V = 10 %)");
		configInput(EXT_INPUT, "External audio (replaces noise)");
		configInput(CUTOFF_CV_INPUT, "Cutoff CV (1 V/oct)");
		configInput(LEVEL_CV_INPUT, "Level CV (0-10 V)");

		configOutput(OSC1_OUTPUT, "Osc 1");
		configOutput(OSC2_OUTPUT, "Osc 2");
		configOutput(PITCH_ENV_OUTPUT, "Pitch envelope");
		configOutput(FILTER_ENV_OUTPUT, "Filter envelope");
		configOutput(AMP_ENV_OUTPUT, "Amp envelope (with velocity)");
		configOutput(TRIG_OUTPUT, "Trigger (every hit)");
		configOutput(AUDIO_OUTPUT, "Audio");
		configLight(RUN_LIGHT, "Running");

		lightDivider.setDivision(16);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		for (int t = 0; t < zenon::NUM_TRACKS; t++)
			tracks[t].reset();
		transport = zenon::Transport();
		voice.reset();
		runInputGate = false;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "runInputGate", json_boolean(runInputGate));
		return root;
	}

	// Missing or invalid: trigger mode (behaviour of Zenon up to 2.2.1).
	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "runInputGate");
		runInputGate = j && json_is_boolean(j) && json_is_true(j);
	}

	float in(int id) {
		return zenon::finiteOr(inputs[id].getVoltage(), 0.f);
	}

	float stepValue(int t) {
		return params[STEP_PARAMS + t * zenon::NUM_STEPS + tracks[t].current()].getValue();
	}

	void randomize(int t) {
		for (int i = 0; i < zenon::NUM_STEPS; i++)
			params[STEP_PARAMS + t * zenon::NUM_STEPS + i].setValue(random::uniform());
		rndLightPulse[t].trigger(0.1f);
	}

	void process(const ProcessArgs& args) override {
		const float dt = args.sampleTime;

		// --- Transport
		// RUN input: trigger mode toggles on every rising edge; gate mode follows the level
		// (rising edge = run, falling edge = stop). The RUN button works in both modes.
		bool runRise = runInTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 1.f);
		bool runFall = runInWasHigh && !runInTrigger.isHigh();
		runInWasHigh = runInTrigger.isHigh();
		if (runInputGate) {
			if (runRise)
				params[RUN_PARAM].setValue(1.f);
			if (runFall)
				params[RUN_PARAM].setValue(0.f);
		}
		else if (runRise) {
			params[RUN_PARAM].setValue(params[RUN_PARAM].getValue() > 0.5f ? 0.f : 1.f);
		}
		bool running = params[RUN_PARAM].getValue() > 0.5f;
		transport.setRunning(running);

		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			for (int t = 0; t < zenon::NUM_TRACKS; t++)
				tracks[t].reset();
			transport.reset();
		}

		int lengths[zenon::NUM_TRACKS];
		for (int t = 0; t < zenon::NUM_TRACKS; t++) {
			lengths[t] = zenon::lengthFrom(params[LENGTH_PARAMS + t].getValue(), in(LENGTH_CV_INPUTS + t));
			bool rnd = rndButton[t].process(params[RND_PARAMS + t].getValue() > 0.5f);
			rnd |= rndInTrigger[t].process(inputs[RND_INPUTS + t].getVoltage(), 0.1f, 1.f);
			if (rnd)
				randomize(t);
		}

		bool clockEdge = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		bool advEdge = advButton.process(params[ADV_PARAM].getValue() > 0.5f);
		float spm = zenon::stepsPerMinute(params[TEMPO_PARAM].getValue(), in(TEMPO_CV_INPUT));
		int factor = zenon::clockFactorIndex(params[TEMPO_PARAM].getValue(), in(TEMPO_CV_INPUT));
		bool tick = transport.process(inputs[CLOCK_INPUT].isConnected(), clockEdge, advEdge, spm, dt, factor);
		if (tick) {
			for (int t = 0; t < zenon::NUM_TRACKS; t++)
				if (tracks[t].advance(lengths[t]))
					eocPulse[t].trigger(1e-3f);
		}
		bool hit = tick;
		hit |= trigButton.process(params[TRIG_PARAM].getValue() > 0.5f);
		hit |= trigInTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);

		// --- Track modulation (evaluated every sample, so knob moves act immediately)
		int dest[zenon::NUM_TRACKS];
		float s[zenon::NUM_TRACKS], a[zenon::NUM_TRACKS];
		for (int t = 0; t < zenon::NUM_TRACKS; t++) {
			dest[t] = zenon::destFrom(params[DEST_PARAMS + t].getValue());
			s[t] = stepValue(t);
			a[t] = params[AMOUNT_PARAMS + t].getValue();
		}
		zenon::Mods m = zenon::computeMods(dest, s, a);

		if (hit) {
			voice.trigger(m.velGain);
			trigPulse.trigger(1e-3f);
		}

		// --- Voice controls
		zenon::VoiceControls c;
		c.osc1Oct = params[OSC1_FREQ_PARAM].getValue() + in(PITCH1_INPUT) + m.pitchOct;
		c.osc2Oct = params[OSC2_FREQ_PARAM].getValue() + in(PITCH2_INPUT) + m.pitchOct + m.osc2Oct;
		c.osc1Square = params[OSC1_WAVE_PARAM].getValue() < 0.5f;
		c.osc2Square = params[OSC2_WAVE_PARAM].getValue() < 0.5f;
		c.fm = zenon::clampf(params[FM_PARAM].getValue() + in(FM_CV_INPUT) * CV_SCALE + m.fm, 0.f, 1.f);
		c.sync = params[SYNC_PARAM].getValue() > 0.5f;
		c.mixOsc1 = params[MIX_OSC1_PARAM].getValue();
		c.mixOsc2 = params[MIX_OSC2_PARAM].getValue();
		c.mixNoise = params[MIX_NOISE_PARAM].getValue();
		c.extConnected = inputs[EXT_INPUT].isConnected();
		c.ext = in(EXT_INPUT) / EXT_VOLTS;
		c.cutoffLog2 = params[CUTOFF_PARAM].getValue() + in(CUTOFF_CV_INPUT) + m.cutoffOct;
		c.res = params[RES_PARAM].getValue();
		c.highpass = params[FILTER_MODE_PARAM].getValue() > 0.5f;
		float level = params[LEVEL_PARAM].getValue();
		if (inputs[LEVEL_CV_INPUT].isConnected())
			level *= zenon::clampf(in(LEVEL_CV_INPUT) / 10.f, 0.f, 1.f);
		c.level = level;
		c.pitchDec = params[PITCH_DEC_PARAM].getValue();
		c.filterDec = zenon::clampf(params[FILTER_DEC_PARAM].getValue() + m.filterDec, 0.f, 1.f);
		c.ampDec = zenon::clampf(params[AMP_DEC_PARAM].getValue() + m.ampDec, 0.f, 1.f);
		c.pitchAmt = params[PITCH_AMT_PARAM].getValue();
		c.filterAmt = params[FILTER_AMT_PARAM].getValue();

		zenon::VoiceOutputs o = voice.process(c, dt);

		outputs[OSC1_OUTPUT].setVoltage(o.osc1 * OSC_VOLTS);
		outputs[OSC2_OUTPUT].setVoltage(o.osc2 * OSC_VOLTS);
		outputs[PITCH_ENV_OUTPUT].setVoltage(o.pitchEnv * ENV_VOLTS);
		outputs[FILTER_ENV_OUTPUT].setVoltage(o.filterEnv * ENV_VOLTS);
		outputs[AMP_ENV_OUTPUT].setVoltage(o.ampEnv * ENV_VOLTS);
		outputs[AUDIO_OUTPUT].setVoltage(o.audio * AUDIO_VOLTS);
		outputs[TRIG_OUTPUT].setVoltage(trigPulse.process(dt) ? 10.f : 0.f);
		for (int t = 0; t < zenon::NUM_TRACKS; t++) {
			outputs[TRACK_OUTPUTS + t].setVoltage(s[t] * TRACK_VOLTS);
			outputs[EOC_OUTPUTS + t].setVoltage(eocPulse[t].process(dt) ? 10.f : 0.f);
		}

		// --- Lights (not audio-rate critical)
		if (lightDivider.process()) {
			float ldt = dt * lightDivider.getDivision();
			lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);
			for (int t = 0; t < zenon::NUM_TRACKS; t++) {
				lights[RND_LIGHTS + t].setBrightness(rndLightPulse[t].process(ldt) ? 1.f : 0.f);
				int cur = tracks[t].current();
				for (int i = 0; i < zenon::NUM_STEPS; i++) {
					float b = i == cur ? 1.f : (i < lengths[t] ? 0.15f : 0.f);
					lights[STEP_LIGHTS + t * zenon::NUM_STEPS + i].setBrightness(b);
				}
			}
		}
	}
};


static const char* CLOCK_FACTOR_NAMES[5] = {"/4", "/2", "x1", "x2", "x4"};

std::string TempoQuantity::getDisplayValueString() {
	Zenon* m = dynamic_cast<Zenon*>(module);
	if (m && m->inputs[Zenon::CLOCK_INPUT].isConnected()) {
		int i = zenon::clockFactorIndex(getValue(), 0.f) - zenon::CLOCK_FACTOR_MIN;
		return std::string(CLOCK_FACTOR_NAMES[i]) + " (external clock)";
	}
	return ParamQuantity::getDisplayValueString();
}

std::string TempoQuantity::getUnit() {
	Zenon* m = dynamic_cast<Zenon*>(module);
	if (m && m->inputs[Zenon::CLOCK_INPUT].isConnected())
		return "";
	return ParamQuantity::getUnit();
}


// ---------------------------------------------------------------------------------------------
// Widgets

template <typename TBase = GrayModuleLightWidget>
struct TZenonOrangeLight : TBase {
	TZenonOrangeLight() {
		this->addBaseColor(nvgRGB(0xff, 0x8a, 0x3d));
	}
};
template <typename TBase = GrayModuleLightWidget>
struct TZenonTealLight : TBase {
	TZenonTealLight() {
		this->addBaseColor(nvgRGB(0x3f, 0xd0, 0xb8));
	}
};
using ZenonOrangeLight = TZenonOrangeLight<>;
using ZenonTealLight = TZenonTealLight<>;

// Small knob with detents (DEST).
struct ZenonSnapKnob : RoundSmallBlackKnob {
	ZenonSnapKnob() {
		snap = true;
	}
};

// Trimpot with detents (LENGTH).
struct ZenonSnapTrimpot : Trimpot {
	ZenonSnapTrimpot() {
		snap = true;
	}
};

// Two-position horizontal switch (SQR/TRI, SYNC, LP/HP).
struct ZenonSwitch : app::SvgSwitch {
	ZenonSwitch() {
		shadow->opacity = 0.f;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/ZenonSwitch_0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/ZenonSwitch_1.svg")));
	}
};

// Short sliders that match the panel rails (36 px for the mixer, 46 px for the decays).
struct ZenonSlider36 : app::SvgSlider {
	ZenonSlider36() {
		setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/ZenonSlider36.svg")));
		setHandleSvg(Svg::load(asset::plugin(pluginInstance, "res/ZenonSliderHandle.svg")));
		setHandlePosCentered(math::Vec(6.f, 36.f - 3.f), math::Vec(6.f, 3.f));
	}
};
struct ZenonSlider46 : app::SvgSlider {
	ZenonSlider46() {
		setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/ZenonSlider46.svg")));
		setHandleSvg(Svg::load(asset::plugin(pluginInstance, "res/ZenonSliderHandle.svg")));
		setHandlePosCentered(math::Vec(6.f, 46.f - 3.f), math::Vec(6.f, 3.f));
	}
};

// Shows the name of the selected destination. Works without a module (browser preview).
struct DestDisplay : widget::Widget {
	Zenon* module = nullptr;
	int track = 0;
	NVGcolor color;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			int d = track == 0 ? zenon::DEST_PITCH : zenon::DEST_VEL;
			if (module)
				d = zenon::destFrom(module->params[Zenon::DEST_PARAMS + track].getValue());
			std::shared_ptr<window::Font> font = APP->window->uiFont;
			if (font && font->handle >= 0) {
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, 6.5f);
				nvgFillColor(args.vg, color);
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f + 0.5f, zenon::destName(d), NULL);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

struct ZenonWidget : ModuleWidget {
	ZenonWidget(Zenon* module) {
		setModule(module);
		// Panel converted from Philipp's Illustrator file (Panel-Entwuerfe/Zenon-Layout-Entwurf-v6.svg).
		// Positions (px, 1 HP = 15 px) are taken from that file, see docs/modules/Zenon-Panel-Bauplan.md.
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Zenon.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Transport
		const float TY = 37.f;
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(26, TY), module, Zenon::TEMPO_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(60, TY), module, Zenon::TEMPO_CV_INPUT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(
			Vec(98, TY), module, Zenon::RUN_PARAM, Zenon::RUN_LIGHT));
		addInput(createInputCentered<PJ301MPort>(Vec(126, TY), module, Zenon::RUN_INPUT));
		addParam(createParamCentered<VCVButton>(Vec(164, TY), module, Zenon::ADV_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(192, TY), module, Zenon::CLOCK_INPUT));
		addParam(createParamCentered<VCVButton>(Vec(230, TY), module, Zenon::TRIG_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(258, TY), module, Zenon::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(296, TY), module, Zenon::RESET_INPUT));

		// Tracks
		const float CY[2] = {92.f, 155.f};
		const NVGcolor COL[2] = {nvgRGB(0xff, 0x8a, 0x3d), nvgRGB(0x3f, 0xd0, 0xb8)};
		for (int t = 0; t < 2; t++) {
			float cy = CY[t];
			addParam(createParamCentered<ZenonSnapKnob>(Vec(46, cy), module, Zenon::DEST_PARAMS + t));
			DestDisplay* disp = createWidget<DestDisplay>(Vec(30, cy + 19));
			disp->box.size = Vec(32, 9);
			disp->module = module;
			disp->track = t;
			disp->color = COL[t];
			addChild(disp);
			addParam(createParamCentered<ZenonSnapTrimpot>(Vec(82, cy), module, Zenon::LENGTH_PARAMS + t));
			addInput(createInputCentered<PJ301MPort>(Vec(112, cy), module, Zenon::LENGTH_CV_INPUTS + t));
			addParam(createParamCentered<Trimpot>(Vec(146, cy), module, Zenon::AMOUNT_PARAMS + t));
			for (int i = 0; i < 8; i++) {
				float x = 178 + i * 34;
				addParam(createParamCentered<RoundSmallBlackKnob>(Vec(x, cy), module, Zenon::STEP_PARAMS + t * 8 + i));
				if (t == 0)
					addChild(createLightCentered<SmallLight<ZenonOrangeLight>>(Vec(x, cy + 19), module, Zenon::STEP_LIGHTS + i));
				else
					addChild(createLightCentered<SmallLight<ZenonTealLight>>(Vec(x, cy + 19), module, Zenon::STEP_LIGHTS + 8 + i));
			}
			if (t == 0)
				addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<ZenonOrangeLight>>>(
					Vec(486, cy), module, Zenon::RND_PARAMS + t, Zenon::RND_LIGHTS + t));
			else
				addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<ZenonTealLight>>>(
					Vec(486, cy), module, Zenon::RND_PARAMS + t, Zenon::RND_LIGHTS + t));
			addInput(createInputCentered<PJ301MPort>(Vec(514, cy), module, Zenon::RND_INPUTS + t));
			addOutput(createOutputCentered<PJ301MPort>(Vec(450, cy), module, Zenon::EOC_OUTPUTS + t));
		}

		// Voice: channel strips
		const float RK = 226.5f, RS = 256.5f, RJ = 284.5f;
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(32, RK), module, Zenon::OSC1_FREQ_PARAM));
		addParam(createParamCentered<ZenonSwitch>(Vec(32, RS), module, Zenon::OSC1_WAVE_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(32, RJ), module, Zenon::PITCH1_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(92, RK), module, Zenon::OSC2_FREQ_PARAM));
		addParam(createParamCentered<ZenonSwitch>(Vec(92, RS), module, Zenon::OSC2_WAVE_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(92, RJ), module, Zenon::PITCH2_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(152, RK), module, Zenon::FM_PARAM));
		addParam(createParamCentered<ZenonSwitch>(Vec(152, RS), module, Zenon::SYNC_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(152, RJ), module, Zenon::FM_CV_INPUT));
		addParam(createParamCentered<ZenonSlider36>(Vec(198, 228.5f), module, Zenon::MIX_OSC1_PARAM));
		addParam(createParamCentered<ZenonSlider36>(Vec(216, 228.5f), module, Zenon::MIX_OSC2_PARAM));
		addParam(createParamCentered<ZenonSlider36>(Vec(234, 228.5f), module, Zenon::MIX_NOISE_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(248, RJ), module, Zenon::EXT_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(292, RK), module, Zenon::CUTOFF_PARAM));
		addParam(createParamCentered<Trimpot>(Vec(328, 224.5f), module, Zenon::RES_PARAM));
		addParam(createParamCentered<ZenonSwitch>(Vec(326, 260.5f), module, Zenon::FILTER_MODE_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(292, RJ), module, Zenon::CUTOFF_CV_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(380, RK), module, Zenon::LEVEL_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(380, RJ), module, Zenon::LEVEL_CV_INPUT));
		// Decay block
		addParam(createParamCentered<ZenonSlider46>(Vec(436, 233.5f), module, Zenon::PITCH_DEC_PARAM));
		addParam(createParamCentered<ZenonSlider46>(Vec(474, 233.5f), module, Zenon::FILTER_DEC_PARAM));
		addParam(createParamCentered<ZenonSlider46>(Vec(512, 233.5f), module, Zenon::AMP_DEC_PARAM));
		addParam(createParamCentered<Trimpot>(Vec(436, 282.5f), module, Zenon::PITCH_AMT_PARAM));
		addParam(createParamCentered<Trimpot>(Vec(474, 282.5f), module, Zenon::FILTER_AMT_PARAM));

		// Outputs
		const int outIds[8] = {Zenon::OSC1_OUTPUT, Zenon::OSC2_OUTPUT, Zenon::PITCH_ENV_OUTPUT, Zenon::FILTER_ENV_OUTPUT,
			Zenon::AMP_ENV_OUTPUT, Zenon::TRACK_OUTPUTS + 0, Zenon::TRACK_OUTPUTS + 1, Zenon::TRIG_OUTPUT};
		for (int i = 0; i < 8; i++)
			addOutput(createOutputCentered<PJ301MPort>(Vec(26 + i * 40, 334), module, outIds[i]));
		addOutput(createOutputCentered<PJ301MPort>(Vec(500, 335), module, Zenon::AUDIO_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Zenon* module = dynamic_cast<Zenon*>(this->module);
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("RUN input", {"Trigger (toggles)", "Gate (high = run)"},
			[=]() { return (size_t) (module->runInputGate ? 1 : 0); },
			[=](size_t i) { module->runInputGate = (i == 1); }));
	}
};


Model* modelZenon = createModel<Zenon, ZenonWidget>("Zenon");
