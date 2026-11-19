// Parzen FX - drive, distortion and bit crusher, in series (SIM) or switched by trigger (SEQ).
// Signal contract: docs/modules/Parzen-Signalvertrag.md (project). Panel positions: ParzenLayout.hpp (generated).
#include "plugin.hpp"
#include "ParzenLayout.hpp"
#include "dsp/ParzenDsp.hpp"
#include <atomic>

namespace {

const int OS = 4;          // oversampling factor for DRIVE and DIST
const int OS_Q = 8;        // resampler quality (taps per phase)
const int SCOPE_COLS = 216;
const float FADE_SECONDS = 0.005f;
const float LEVEL_SMOOTH_SECONDS = 0.01f;
const float SCOPE_WINDOWS[] = {0.002f, 0.010f, 0.050f};
const char* const LANE_NAMES[] = {"DRIVE", "DIST", "CRUSH"};

// Oversampled nonlinear lane with dry/wet mix inside the oversampled domain, so dry and wet
// have the same latency and partial MIX settings do not comb-filter.
struct OversampledLane {
	dsp::Upsampler<OS, OS_Q> up;
	dsp::Decimator<OS, OS_Q> down;
	parzen::DcBlocker dc;
	void reset() {
		up.reset();
		down.reset();
		dc.reset();
	}
	template <typename F>
	float process(float x, float mix, bool dcBlock, F shape) {
		float buf[OS];
		up.process(x, buf);
		for (int i = 0; i < OS; i++) {
			float wet = shape(buf[i]);
			if (dcBlock)
				wet = dc.process(wet);
			buf[i] += mix * (wet - buf[i]);
		}
		return down.process(buf);
	}
};

// Displays the crusher hold frequency in Hz (depends on the engine sample rate).
struct RateQuantity : ParamQuantity {
	float getDisplayValue() override {
		float sr = APP->engine->getSampleRate();
		return parzen::crushHoldFreq(getValue(), sr);
	}
	void setDisplayValue(float hz) override {
		float sr = APP->engine->getSampleRate();
		float lo = std::min(200.f, sr);
		if (!(sr > lo)) {
			setValue(0.f);
			return;
		}
		hz = clamp(hz, lo, sr);
		setValue(std::log(hz / sr) / std::log(lo / sr));
	}
};

} // namespace

struct ParzenFX : Module {
	enum ParamId {
		DRIVE_PARAM,
		DRIVE_BIAS_PARAM,
		DRIVE_MIX_PARAM,
		DIST_GAIN_PARAM,
		DIST_SHAPE_PARAM,
		DIST_MIX_PARAM,
		CRUSH_BITS_PARAM,
		CRUSH_RATE_PARAM,
		CRUSH_MIX_PARAM,
		IN_LEVEL_PARAM,
		MODE_PARAM,
		OUT_LEVEL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		DRIVE_CV_INPUT,
		DIST_CV_INPUT,
		CRUSH_CV_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		TRIG_INPUT,
		RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		DRIVE_LIGHT,
		DIST_LIGHT,
		CRUSH_LIGHT,
		LIGHTS_LEN
	};

	// per channel (L, R)
	OversampledLane drive[2], distSim[2], distSeq[2];
	parzen::Crusher crushSim[2], crushSeq[2];

	parzen::ConfigFader fader;
	int seqLane = 0;           // 0..2, active lane in SEQ
	bool started = false;
	dsp::SchmittTrigger trigIn, resetIn;
	float inGain = 1.f, outGain = 1.f;
	float lastSampleRate = 0.f;

	// scope (L channel): audio thread writes, display reads the published frame
	parzen::ScopeCapture<SCOPE_COLS> scope;
	std::atomic<int> scopeFront{-1};
	std::atomic<int> dispConfig{0};
	int scopeWindow = 1;        // index into SCOPE_WINDOWS, saved with the patch
	int appliedScopeWindow = -1;

	ParzenFX() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DRIVE_PARAM, 0.f, 36.f, 12.f, "Drive", " dB");
		configParam(DRIVE_BIAS_PARAM, -1.f, 1.f, 0.f, "Drive bias");
		configParam(DRIVE_MIX_PARAM, 0.f, 1.f, 1.f, "Drive mix", "%", 0.f, 100.f);
		configParam(DIST_GAIN_PARAM, 0.f, 40.f, 12.f, "Distortion gain", " dB");
		configParam(DIST_SHAPE_PARAM, 0.f, 1.f, 0.f, "Distortion shape (clip to fold)", "%", 0.f, 100.f);
		configParam(DIST_MIX_PARAM, 0.f, 1.f, 1.f, "Distortion mix", "%", 0.f, 100.f);
		configParam(CRUSH_BITS_PARAM, 1.f, 16.f, 16.f, "Crush bits", " bit");
		configParam<RateQuantity>(CRUSH_RATE_PARAM, 0.f, 1.f, 0.f, "Crush sample rate", " Hz");
		configParam(CRUSH_MIX_PARAM, 0.f, 1.f, 1.f, "Crush mix", "%", 0.f, 100.f);
		configParam(IN_LEVEL_PARAM, -60.f, 12.f, 0.f, "Input level", " dB");
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Mode", {"SIM: all lanes in series", "SEQ: one lane, switched by trigger"});
		configParam(OUT_LEVEL_PARAM, -60.f, 6.f, 0.f, "Output level", " dB");

		configInput(DRIVE_CV_INPUT, "Drive CV (1 V = 10 %)");
		configInput(DIST_CV_INPUT, "Distortion gain CV (1 V = 10 %)");
		configInput(CRUSH_CV_INPUT, "Crush bits CV (1 V = 10 %)");
		configInput(IN_L_INPUT, "Left audio");
		configInput(IN_R_INPUT, "Right audio (normalled to left)");
		configInput(TRIG_INPUT, "Next lane (SEQ)");
		configInput(RESET_INPUT, "Reset to DRIVE (SEQ)");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		configLight(DRIVE_LIGHT, "Drive active");
		configLight(DIST_LIGHT, "Distortion active");
		configLight(CRUSH_LIGHT, "Crush active");

		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		resetDsp();
	}

	void resetDsp() {
		for (int c = 0; c < 2; c++) {
			drive[c].reset();
			distSim[c].reset();
			distSeq[c].reset();
			crushSim[c].reset();
			crushSeq[c].reset();
		}
		scope.reset();
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		seqLane = 0;
		scopeWindow = 1;
		started = false;
		resetDsp();
	}

	int targetConfig() {
		return params[MODE_PARAM].getValue() > 0.5f ? 1 + seqLane : 0;
	}

	void process(const ProcessArgs& args) override {
		const float sr = args.sampleRate;
		if (sr != lastSampleRate) {
			lastSampleRate = sr;
			for (int c = 0; c < 2; c++)
				drive[c].dc.setCutoff(5.f, sr * OS);
			appliedScopeWindow = -1;
		}
		if (appliedScopeWindow != scopeWindow) {
			appliedScopeWindow = clamp(scopeWindow, 0, 2);
			scope.setWindow(SCOPE_WINDOWS[appliedScopeWindow], sr);
			scope.reset();
		}

		// sequence
		bool seq = params[MODE_PARAM].getValue() > 0.5f;
		if (trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f) && seq)
			seqLane = (seqLane + 1) % 3;
		if (resetIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f) && seq)
			seqLane = 0;
		int target = targetConfig();
		if (!started) {
			fader.reset(target);
			started = true;
		}
		else if (target != fader.to) {
			fader.start(target, FADE_SECONDS, sr);
		}

		// parameters (+ CV, 1 V = 10 % of the range)
		float driveDb = parzen::withCv(params[DRIVE_PARAM].getValue(), inputs[DRIVE_CV_INPUT].getVoltage(), 0.f, 36.f);
		float distDb = parzen::withCv(params[DIST_GAIN_PARAM].getValue(), inputs[DIST_CV_INPUT].getVoltage(), 0.f, 40.f);
		float bits = parzen::withCv(params[CRUSH_BITS_PARAM].getValue(), inputs[CRUSH_CV_INPUT].getVoltage(), 1.f, 16.f);
		float driveGain = parzen::dbToGain(driveDb);
		float distGain = parzen::dbToGain(distDb);
		float bias = params[DRIVE_BIAS_PARAM].getValue();
		float shape = params[DIST_SHAPE_PARAM].getValue();
		float rate = params[CRUSH_RATE_PARAM].getValue();
		float mixDrive = params[DRIVE_MIX_PARAM].getValue();
		float mixDist = params[DIST_MIX_PARAM].getValue();
		float mixCrush = params[CRUSH_MIX_PARAM].getValue();

		// smoothed levels
		float a = clamp(args.sampleTime / LEVEL_SMOOTH_SECONDS, 0.f, 1.f);
		inGain += a * (parzen::levelGain(params[IN_LEVEL_PARAM].getValue()) - inGain);
		outGain += a * (parzen::levelGain(params[OUT_LEVEL_PARAM].getValue()) - outGain);

		auto driveFn = [=](float v) { return parzen::driveShape(v, driveGain, bias); };
		auto distFn = [=](float v) { return parzen::distShape(v, distGain, shape); };

		// inputs: R normalled to L; 5 V = 1.0 internally
		float xin[2];
		xin[0] = inputs[IN_L_INPUT].getVoltage();
		xin[1] = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : xin[0];

		float y[2];
		bool bad = false;
		for (int c = 0; c < 2; c++) {
			float x = xin[c];
			if (!std::isfinite(x))
				x = 0.f;
			x = x / 5.f * inGain;
			// all four configurations run continuously, so a crossfade never meets a cold state
			float cfg[4];
			float d = drive[c].process(x, mixDrive, true, driveFn);                // SIM step 1 and SEQ lane DRIVE
			float t = distSim[c].process(d, mixDist, false, distFn);              // SIM step 2
			float cs = crushSim[c].process(t, bits, rate, sr);                    // SIM step 3
			cfg[0] = t + mixCrush * (cs - t);
			cfg[1] = d;
			cfg[2] = distSeq[c].process(x, mixDist, false, distFn);
			float cq = crushSeq[c].process(x, bits, rate, sr);
			cfg[3] = x + mixCrush * (cq - x);
			float out = fader.mix(cfg) * outGain * 5.f;
			if (!std::isfinite(out)) {
				bad = true;
				out = 0.f;
			}
			y[c] = out;
			if (c == 0)
				xin[0] = x * 5.f;   // input after IN LEVEL, in volts, for the scope
		}
		fader.advance();
		if (bad)
			resetDsp();

		outputs[OUT_L_OUTPUT].setVoltage(y[0]);
		outputs[OUT_R_OUTPUT].setVoltage(y[1]);

		// scope: input L (after IN LEVEL) as reference, both outputs
		if (scope.process(xin[0], y[0], y[1]))
			scopeFront.store(scope.front);
		dispConfig.store(fader.to);

		// lamps: SEQ = active lane, SIM = brightness follows the lane's MIX
		float wSim = fader.weight(0);
		const float mixes[3] = {mixDrive, mixDist, mixCrush};
		for (int i = 0; i < 3; i++) {
			float b = wSim * mixes[i] + fader.weight(1 + i);
			lights[DRIVE_LIGHT + i].setBrightnessSmooth(b, args.sampleTime);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "scopeWindow", json_integer(scopeWindow));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "scopeWindow");
		scopeWindow = (j && json_is_integer(j)) ? clamp((int) json_integer_value(j), 0, 2) : 1;
	}
};

// ------------------------------------------------------------------ widgets

struct ParzenKnobBig : app::SvgKnob {
	ParzenKnobBig() {
		minAngle = -0.83f * M_PI;
		maxAngle = 0.83f * M_PI;
		shadow->opacity = 0.f;
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/ParzenKnobBig.svg")));
	}
};

struct ParzenKnobSmall : app::SvgKnob {
	ParzenKnobSmall() {
		minAngle = -0.83f * M_PI;
		maxAngle = 0.83f * M_PI;
		shadow->opacity = 0.f;
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/ParzenKnobSmall.svg")));
	}
};

struct ParzenToggle : app::SvgSwitch {
	ParzenToggle() {
		shadow->opacity = 0.f;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/ParzenToggle_0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/ParzenToggle_1.svg")));
	}
};

// Lens of the jewel lamp (the bezel ring is part of the panel).
struct ParzenLamp : RedLight {
	ParzenLamp() {
		box.size = math::Vec(6.f, 6.f);
	}
};

// Scope window: dark screen, OUT L and OUT R drawn in the colour of the cable plugged into each
// output (fallback colours without a cable), input L as a faint reference. Drawn on the light layer
// so it stays lit when the room is dimmed. Works without a module (browser preview shows an example).
struct ParzenScope : widget::Widget {
	ParzenFX* module = NULL;
	app::ModuleWidget* moduleWidget = NULL;
	std::string fontPath = asset::system("res/fonts/ShareTechMono-Regular.ttf");

	void drawTrace(NVGcontext* vg, const float* mn, const float* mx, int n, NVGcolor col, float width) {
		const float h = box.size.y, mid = h / 2.f, scale = (h / 2.f - 1.f) / 10.f;   // +-10 V over the height
		auto py = [&](float v) { return clamp(mid - v * scale, 0.5f, h - 0.5f); };
		float dx = box.size.x / (float) n;
		nvgBeginPath(vg);
		for (int i = 0; i < n; i++) {
			float x = (i + 0.5f) * dx;
			float c = py(0.5f * (mn[i] + mx[i]));
			if (i == 0) nvgMoveTo(vg, x, c);
			else nvgLineTo(vg, x, c);
		}
		for (int i = 0; i < n; i++) {
			if (mx[i] - mn[i] > 0.05f) {
				float x = (i + 0.5f) * dx;
				nvgMoveTo(vg, x, py(mx[i]));
				nvgLineTo(vg, x, py(mn[i]));
			}
		}
		nvgStrokeColor(vg, col);
		nvgStrokeWidth(vg, width);
		nvgLineJoin(vg, NVG_ROUND);
		nvgStroke(vg);
	}

	// Colour of the top cable on an output, or the fallback if nothing is plugged in.
	NVGcolor cableColor(int outputId, NVGcolor fallback) {
		if (!moduleWidget || !APP->scene || !APP->scene->rack)
			return fallback;
		app::PortWidget* pw = moduleWidget->getOutput(outputId);
		if (!pw)
			return fallback;
		app::CableWidget* cw = APP->scene->rack->getTopCable(pw);
		return cw ? cw->color : fallback;
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		nvgSave(vg);
		nvgScissor(vg, 0, 0, w, h);
		// window
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 2.f);
		nvgFillColor(vg, nvgRGB(0x12, 0x12, 0x10));
		nvgFill(vg);
		// grid: centre line and +-5 V
		const float scale = (h / 2.f - 1.f) / 10.f;
		for (int k = -1; k <= 1; k++) {
			float yy = h / 2.f - k * 5.f * scale;
			nvgBeginPath(vg);
			nvgMoveTo(vg, 0, yy);
			nvgLineTo(vg, w, yy);
			nvgStrokeColor(vg, k == 0 ? nvgRGBA(0xff, 0xff, 0xff, 0x30) : nvgRGBA(0xff, 0xff, 0xff, 0x14));
			nvgStrokeWidth(vg, 0.6f);
			nvgStroke(vg);
		}

		static float pInMin[SCOPE_COLS], pInMax[SCOPE_COLS], pOutMin[SCOPE_COLS], pOutMax[SCOPE_COLS], pOut2Min[SCOPE_COLS], pOut2Max[SCOPE_COLS];
		const float *inMin, *inMax, *outMin, *outMax, *out2Min, *out2Max;
		int cfg = 0;
		int front = module ? module->scopeFront.load() : -1;
		if (module && front >= 0) {
			const auto& f = module->scope.frames[front];
			inMin = f.inMin;
			inMax = f.inMax;
			outMin = f.outMin;
			outMax = f.outMax;
			out2Min = f.out2Min;
			out2Max = f.out2Max;
			cfg = module->dispConfig.load();
		}
		else {
			// static example: sine and its clipped version
			for (int i = 0; i < SCOPE_COLS; i++) {
				float v = 6.f * std::sin(i / (float) SCOPE_COLS * 3.f * 2.f * M_PI);
				pInMin[i] = pInMax[i] = v;
				pOutMin[i] = pOutMax[i] = clamp(v * 1.6f, -5.f, 5.f);
				pOut2Min[i] = pOut2Max[i] = std::round(v * 0.5f) * 1.6f;
			}
			inMin = pInMin;
			inMax = pInMax;
			outMin = pOutMin;
			outMax = pOutMax;
			out2Min = pOut2Min;
			out2Max = pOut2Max;
		}
		NVGcolor colL = cableColor(ParzenFX::OUT_L_OUTPUT, nvgRGB(0xe8, 0xc7, 0x7a));
		NVGcolor colR = cableColor(ParzenFX::OUT_R_OUTPUT, nvgRGB(0x8f, 0xc1, 0xd6));
		drawTrace(vg, inMin, inMax, SCOPE_COLS, nvgRGBA(0xff, 0xff, 0xff, 0x40), 0.8f);
		drawTrace(vg, out2Min, out2Max, SCOPE_COLS, nvgTransRGBAf(colR, 0.9f), 1.2f);
		drawTrace(vg, outMin, outMax, SCOPE_COLS, nvgTransRGBAf(colL, 0.9f), 1.2f);

		// mode / lane text
		std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
		if (font && font->handle >= 0) {
			std::string text = (cfg == 0) ? "SIM" : std::string("SEQ \xc2\xb7 ") + LANE_NAMES[clamp(cfg - 1, 0, 2)];
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, 8.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, nvgRGB(0xc9, 0xbe, 0xa0));
			nvgText(vg, 4.f, 3.f, text.c_str(), NULL);
		}
		nvgResetScissor(vg);
		nvgRestore(vg);
		Widget::drawLayer(args, layer);
	}
};

struct ParzenFXWidget : ModuleWidget {
	ParzenFXWidget(ParzenFX* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ParzenFX.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		ParzenScope* scope = createWidget<ParzenScope>(parzenlayout::SCOPE_POS);
		scope->box.size = parzenlayout::SCOPE_SIZE;
		scope->module = module;
		scope->moduleWidget = this;
		addChild(scope);

		using namespace parzenlayout;
		addChild(createLightCentered<ParzenLamp>(DRIVE_LIGHT, module, ParzenFX::DRIVE_LIGHT));
		addChild(createLightCentered<ParzenLamp>(DIST_LIGHT, module, ParzenFX::DIST_LIGHT));
		addChild(createLightCentered<ParzenLamp>(CRUSH_LIGHT, module, ParzenFX::CRUSH_LIGHT));

		addParam(createParamCentered<ParzenKnobBig>(DRIVE_PARAM, module, ParzenFX::DRIVE_PARAM));
		addParam(createParamCentered<ParzenKnobSmall>(DRIVE_BIAS_PARAM, module, ParzenFX::DRIVE_BIAS_PARAM));
		addParam(createParamCentered<ParzenKnobSmall>(DRIVE_MIX_PARAM, module, ParzenFX::DRIVE_MIX_PARAM));
		addInput(createInputCentered<PJ301MPort>(DRIVE_CV_INPUT, module, ParzenFX::DRIVE_CV_INPUT));

		addParam(createParamCentered<ParzenKnobBig>(DIST_GAIN_PARAM, module, ParzenFX::DIST_GAIN_PARAM));
		addParam(createParamCentered<ParzenKnobSmall>(DIST_SHAPE_PARAM, module, ParzenFX::DIST_SHAPE_PARAM));
		addParam(createParamCentered<ParzenKnobSmall>(DIST_MIX_PARAM, module, ParzenFX::DIST_MIX_PARAM));
		addInput(createInputCentered<PJ301MPort>(DIST_CV_INPUT, module, ParzenFX::DIST_CV_INPUT));

		addParam(createParamCentered<ParzenKnobBig>(CRUSH_BITS_PARAM, module, ParzenFX::CRUSH_BITS_PARAM));
		addParam(createParamCentered<ParzenKnobSmall>(CRUSH_RATE_PARAM, module, ParzenFX::CRUSH_RATE_PARAM));
		addParam(createParamCentered<ParzenKnobSmall>(CRUSH_MIX_PARAM, module, ParzenFX::CRUSH_MIX_PARAM));
		addInput(createInputCentered<PJ301MPort>(CRUSH_CV_INPUT, module, ParzenFX::CRUSH_CV_INPUT));

		addParam(createParamCentered<ParzenKnobSmall>(IN_LEVEL_PARAM, module, ParzenFX::IN_LEVEL_PARAM));
		addInput(createInputCentered<PJ301MPort>(IN_L_INPUT, module, ParzenFX::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(IN_R_INPUT, module, ParzenFX::IN_R_INPUT));
		addParam(createParamCentered<ParzenToggle>(MODE_PARAM, module, ParzenFX::MODE_PARAM));
		addInput(createInputCentered<PJ301MPort>(TRIG_INPUT, module, ParzenFX::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(RESET_INPUT, module, ParzenFX::RESET_INPUT));
		addParam(createParamCentered<ParzenKnobSmall>(OUT_LEVEL_PARAM, module, ParzenFX::OUT_LEVEL_PARAM));
		addOutput(createOutputCentered<PJ301MPort>(OUT_L_OUTPUT, module, ParzenFX::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(OUT_R_OUTPUT, module, ParzenFX::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		ParzenFX* module = getModule<ParzenFX>();
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Scope time window", {"2 ms", "10 ms", "50 ms"}, &module->scopeWindow));
	}
};

Model* modelParzenFX = createModel<ParzenFX, ParzenFXWidget>("ParzenFX");
