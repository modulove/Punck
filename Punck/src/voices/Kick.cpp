#include <Kick.h>
#include <cmath>
#include <cstring>
#include "Filter.h"
#include "VoiceManager.h"
#include "Dynamics.h"
#include "stmlib/utils/random.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"

// ------------------------------------------------------------------------------------------------
// Rumble processor
// ------------------------------------------------------------------------------------------------
constexpr uint32_t Rumble::lineLen[4];

void Rumble::Init()
{
	clearPos = 0;														// Process clears the lines before running
	for (auto& p : pos) {
		p = 0;
	}
	phase = 0;
	preLp = heldL = heldR = smoothL = smoothR = 0.0f;
	hpStateL = hpStateR = hpLastL = hpLastR = 0.0f;
	lpL.Reset();									// keep the coefficients: Init() would set Q = 100 (screaming resonance)
	lpR.Reset();
}


void Rumble::Configure(const float fb, const float cutoffHz, const float drive, const float hpHz, const float w)
{
	feedback = std::clamp(fb, 0.0f, 0.97f);
	gain = 1.0f + 8.0f * std::clamp(drive, 0.0f, 1.0f);
	norm = 1.0f / (1.0f + 1.5f * std::clamp(drive, 0.0f, 1.0f));
	width = std::clamp(w, 0.0f, 1.0f);
	hpCoef = dyn::ExpNeg(6.2831853f * std::clamp(hpHz, 5.0f, 300.0f) / rate);
	const float f = std::clamp(cutoffHz, 20.0f, 1500.0f) / rate;
	lpL.set_f_q<stmlib::FREQUENCY_FAST>(f, 0.8f);
	lpR.set_f_q<stmlib::FREQUENCY_FAST>(f, 0.8f);
}


void Rumble::Process(const float in, float& outL, float& outR)
{
	constexpr uint32_t total = sizeof(line) / sizeof(float);
	if (clearPos < total) {												// 64 values per sample: cleared in ~0.5 ms
		float* l = &line[0][0];
		const uint32_t end = std::min(clearPos + 64, total);
		while (clearPos < end) {
			l[clearPos++] = 0.0f;
		}
		outL = outR = 0.0f;
		return;
	}
	preLp += 0.075f * (in - preLp);										// ~600 Hz anti-alias before decimation

	if (++phase >= decimation) {
		phase = 0;
		float o[4];
		for (uint32_t i = 0; i < 4; ++i) {
			o[i] = line[i][pos[i]];
		}
		// Energy preserving 4x4 Hadamard mix in the feedback path
		const float h0 = 0.5f * (o[0] + o[1] + o[2] + o[3]);
		const float h1 = 0.5f * (o[0] - o[1] + o[2] - o[3]);
		const float h2 = 0.5f * (o[0] + o[1] - o[2] - o[3]);
		const float h3 = 0.5f * (o[0] - o[1] - o[2] + o[3]);
		const float x = 0.5f * preLp;
		line[0][pos[0]] = x + feedback * h0;
		line[1][pos[1]] = x + feedback * h1;
		line[2][pos[2]] = x + feedback * h2;
		line[3][pos[3]] = x + feedback * h3;
		for (uint32_t i = 0; i < 4; ++i) {
			if (++pos[i] >= lineLen[i]) {
				pos[i] = 0;
			}
		}

		// Mid = all lines (same as the mono version), side = an orthogonal combination
		const float mid = h0, side = width * h2;
		float l = stmlib::SoftClip((mid + side) * gain) * norm;			// saturation adds harmonics the low pass shapes
		float r = stmlib::SoftClip((mid - side) * gain) * norm;
		l = lpL.Process<stmlib::FILTER_MODE_LOW_PASS>(l);
		r = lpR.Process<stmlib::FILTER_MODE_LOW_PASS>(r);
		hpStateL = hpCoef * (hpStateL + l - hpLastL);					// one-pole high pass keeps the sub tidy
		hpStateR = hpCoef * (hpStateR + r - hpLastR);
		hpLastL = l;
		hpLastR = r;
		heldL = hpStateL;
		heldR = hpStateR;
	}
	smoothL += 0.04f * (heldL - smoothL);								// smooth the 6 kHz steps
	smoothR += 0.04f * (heldR - smoothR);
	outL = smoothL;
	outR = smoothR;
}


// ------------------------------------------------------------------------------------------------
// Kick body (rumble engine)
// ------------------------------------------------------------------------------------------------
void KickBody::Init()
{
	phase = penv = amp = fb1 = fb2 = clickEnv = knockPhase = residual = last = 0.0f;
	hold = 0;
	noiseBp.Init();
}


void KickBody::Trigger(const Params& np, const float accent)
{
	residual = last;									// previous hit fades out from its last value: no click
	p = np;
	phase = 0.0f;										// sine starts at the zero crossing (the transient gives the click)
	penv = 1.0f;
	amp = accent;
	hold = p.holdSamples;
	clickEnv = p.click * accent;
	knockPhase = 0.0f;
	fb1 = fb2 = 0.0f;
	noiseBp.set_f_q<stmlib::FREQUENCY_FAST>(p.clickNoiseF, 1.2f);
}


float KickBody::Process()
{
	if (amp < 1e-5f && clickEnv < 1e-5f && std::abs(residual) < 1e-5f) {
		last = 0.0f;
		return 0.0f;
	}

	// Exponential pitch sweep down to f0
	penv *= p.sweepCoef;
	const float f = std::min(p.f0 * dyn::FastExp2(p.sweepOct * penv), 0.25f);
	phase += f;
	if (phase >= 1.0f) {
		phase -= 1.0f;
	}

	// Self FM (two sample average keeps it stable), fading with the amplitude so the tail gets purer
	const float fb = p.color * 0.35f * (fb1 + fb2) * std::min(amp * 1.4f, 1.0f);
	const float s = plaits::Sine(phase + fb + 2.0f);
	fb2 = fb1;
	fb1 = s;

	// Amplitude: plateau (sustain) then exponential decay
	if (hold > 0) {
		--hold;
		amp *= p.holdCoef;
	} else {
		amp *= p.decayCoef;
	}
	float out = s * amp;

	// Transient synth: pitched knock falling with its envelope + band-passed noise
	if (clickEnv > 1e-5f) {
		knockPhase += p.clickF * (1.0f + 1.5f * clickEnv);
		if (knockPhase >= 1.0f) {
			knockPhase -= 1.0f;
		}
		const float noise = noiseBp.Process<stmlib::FILTER_MODE_BAND_PASS_NORMALIZED>(2.0f * stmlib::Random::GetFloat() - 1.0f);
		out += clickEnv * (0.6f * plaits::Sine(knockPhase) + 1.5f * noise);
		clickEnv *= p.clickCoef;
	}

	residual *= 0.97f;									// ~1.5 ms crossfade from the previous hit
	out += residual;
	last = out;
	return out;
}


// ------------------------------------------------------------------------------------------------
// Kick
// ------------------------------------------------------------------------------------------------
static inline float Saturate(const float x, const float drive)
{
	if (drive <= 0.0f) {
		return x;
	}
	const float g = 1.0f + 6.0f * drive;
	return stmlib::SoftClip(x * g) / (1.0f + 2.0f * drive);
}


void Kick::Play(const uint8_t voice, const uint32_t noteOffset, const uint32_t noteRange, const float velocity, const float pitch)
{
	playing = true;
	velocityScale = velocity * voiceManager.LevelGain(VoiceManager::potKick);
	// Key & scale: the hit's fundamental snaps to the key (notePitch then carries the tuned ratio for every engine)
	const float baseHz = (engine == classic) ? config.initSlowSinFreq : config.pitch;
	notePitch = voiceManager.Tune(VoiceManager::kick, baseHz * pitch) / baseHz;
	accent = std::clamp(velocity, 0.0f, 1.0f);
	trigger = true;

	// Decay pot with the random zone at the top of its travel
	const float pot = static_cast<float>(ADC_array[ADC_KickDecay]) / 65536.0f;
	hitInZone = config.randomZone > 0.0f && pot >= 1.0f - std::clamp(config.randomZone, 0.0f, 0.5f);
	hitDecay = RandomZone(pot, config.randomZone, config.randomLongMin, config.randomLongMax, config.randomBase,
			stmlib::Random::GetFloat(), stmlib::Random::GetFloat());

	switch (engine) {
	case classic:
		phase = Phase::Ramp1;
		position = 0.0f;
		currentLevel = 0.0f;
		slowSinInc = FreqToInc(config.initSlowSinFreq * notePitch);
		slowSinLevel = 1.0f;
		break;

	case fm:
		fmPhaseC = 0.25f;				// start on the sine peak: click
		fmPhaseM = 0.0f;
		fmAmp = 1.0f;
		fmPitchEnv = 1.0f;
		fmIndexEnv = 1.0f;
		// Coefficients once per hit (Attack/Decay pots are read at the hit)
		fmPitchCoef = dyn::ExpNeg(1.0f / (std::max(config.fmPitchDecay, 1.0f) * 48.0f));
		fmIndexCoef = dyn::ExpNeg(1.0f / ((4.0f + 40.0f * knobA) * 48.0f));
		fmAmpCoef = dyn::DecayCoef(80.0f * dyn::FastExp2(4.9658f * hitDecay));		// 80 ms .. 2.5 s
		fmIndex = 5.0f * knobA;
		break;

	case rumble: {
		body.Trigger(BodyParams(hitDecay, 1.0f, 1.0f), accent);
		noiseEnv = std::max(noiseEnv, accent * config.rumbleNoise);

		// Ghost taps on the next three 16ths (Groove style): a copy of the kick into the rumble, plus a sub pulse
		const float sixteenth = voiceManager.SixteenthSamples();
		const float levels[3] = {config.tap2, config.tap3, config.tap4};
		for (uint32_t i = 0; i < 3; ++i) {
			tapCountdown[i] = (sixteenth > 0.0f && levels[i] > 0.0f) ? static_cast<int32_t>(sixteenth * (i + 1)) : -1;
		}
		break;
	}

	default:
		break;
	}

	idle = false;
	quietCount = 0;
	voiceManager.KickTriggered(velocity);		// Sidechain envelope (used by the rumble; other voices only if enabled)
}


void Kick::Play(const uint8_t voice, const uint32_t index)
{
	// Called when button is pressed
	Play(0, 0, 0, 1.0f, 1.0f);
}


void Kick::CalcClassic()
{
	switch (phase) {
	case Phase::Ramp1:
		currentLevel = currentLevel + (config.ramp1Inc * (1.0f - currentLevel));
		if (currentLevel > 0.6f) {
			phase = Phase::Ramp2;
		}
		break;

	case Phase::Ramp2:
		currentLevel = currentLevel + (config.ramp2Inc * (1.0f - currentLevel));
		if (currentLevel > 0.82f) {
			currentLevel = 0.78f;					// discontinuity sharply falls at first
			phase = Phase::Ramp3;
		}
		break;

	case Phase::Ramp3:
		currentLevel = currentLevel + (config.ramp3Inc * (1.0f - currentLevel));
		if (currentLevel > 0.93f) {
			position = 2.0f;
			phase = Phase::FastSine;
		}
		break;

	case Phase::FastSine:
		position += fastSinInc * notePitch;
		currentLevel = std::sin(position);
		if (position >= 1.5f * pi) {
			phase = Phase::SlowSine;
		}
		break;

	case Phase::SlowSine:
	{
		slowSinInc *= config.sineSlowDownRate;			// Sine wave slowly decreases in frequency
		position += slowSinInc;							// Set current poition in sine wave
		const float decaySpeed = 0.9994f + 0.00055f * DecayParam();
		slowSinLevel = slowSinLevel * decaySpeed;
		currentLevel = std::sin(position) * slowSinLevel;

		if (slowSinLevel <= 0.00001f) {
			playing = false;
			phase = Phase::Off;
		}
		break;
	}

	default:
		break;
	}

	if (phase != Phase::Off) {
		noteMapper->pwmLed.Level(slowSinLevel);
		outputLevel[0] = velocityScale * filter.CalcFilter(currentLevel, left);
		outputLevel[1] = outputLevel[0];
	}
}


float Kick::CalcFm()
{
	// 2-operator FM kick: sine carrier with exponential pitch drop, modulator index envelope = attack grit
	const float f0 = config.pitch * notePitch / systemSampleRate;
	const float f = std::min(f0 * (1.0f + config.fmPitchEnv * fmPitchEnv), 0.2f);
	fmPitchEnv *= fmPitchCoef;
	fmIndexEnv *= fmIndexCoef;
	fmAmp *= fmAmpCoef;

	fmPhaseM += f * config.fmRatio;
	fmPhaseM -= static_cast<float>(static_cast<int32_t>(fmPhaseM));
	fmPhaseC += f;
	fmPhaseC -= static_cast<float>(static_cast<int32_t>(fmPhaseC));

	const float index = fmIndex * fmIndexEnv;
	const float mod = plaits::Sine(fmPhaseM) * index * 0.159155f;					// radians -> cycles
	const float out = plaits::Sine(fmPhaseC + mod + 4.0f) * fmAmp;
	return Saturate(out, config.drive);
}


void Kick::CalcOutput()
{
	// Smooth the pots (they are also read for engines that use them as continuous parameters)
	knobA += 0.002f * (static_cast<float>(ADC_array[ADC_KickAttack]) / 65536.0f - knobA);
	knobB += 0.002f * (static_cast<float>(ADC_array[ADC_KickDecay]) / 65536.0f - knobB);

	if (engine == classic) {
		CalcClassic();
		return;
	}

	if (idle && !trigger) {
		outputLevel[0] = outputLevel[1] = 0.0f;
		ledEnv = 0.0f;
		noteMapper->pwmLed.Level(0.0f);
		return;
	}

	const float level = voiceManager.LevelGain(VoiceManager::potKick);
	const float f0 = std::min(config.pitch * notePitch / systemSampleRate, 0.1f);
	float out = 0.0f;

	switch (engine) {
	case deep: {
		// Plaits bass drum mapping: one 'harmonics' macro = attack FM -> self FM -> drive
		const float h = knobA;
		const float attackFm = std::min(h * 4.0f, 1.0f);
		const float selfFm = std::clamp(h * 4.0f - 1.0f, 0.0f, 1.0f);
		const float drive = std::max(h * 2.0f - 1.0f, 0.0f) * std::max(1.0f - 16.0f * f0, 0.0f);
		analogBd.Render(false, trigger, accent, f0, config.tone, DecayParam(), attackFm, selfFm, &out, 1);
		out = Saturate(out, drive);
		break;
	}

	case punch: {
		const float h = knobA;
		const float d = DecayParam();
		syntheticBd.Render(false, trigger, accent, f0, config.tone, d, 0.4f - 0.25f * d * d,
				std::min(h * 2.0f, 1.0f), std::max(h * 2.0f - 1.0f, 0.0f), &out, 1);
		break;
	}

	case fm:
		out = CalcFm();
		break;

	case rumble: {
		float l, r;
		CalcRumble(l, r);
		trigger = false;
		outputLevel[0] = level * l;
		outputLevel[1] = level * r;
		ledEnv = std::max(body.Level(), ledEnv * 0.9997f);
		noteMapper->pwmLed.Level(std::min(ledEnv, 1.0f));
		const bool tapsPending = tapCountdown[0] > 0 || tapCountdown[1] > 0 || tapCountdown[2] > 0;
		CheckIdle(std::abs(l) + std::abs(r), tapsPending);
		return;
	}

	default:
		break;
	}
	trigger = false;

	outputLevel[0] = outputLevel[1] = level * out;
	ledEnv = std::max(std::abs(out), ledEnv * 0.9997f);
	noteMapper->pwmLed.Level(std::min(ledEnv, 1.0f));
	CheckIdle(std::abs(out), false);
}


float Kick::DecayParam() const
{
	// Randomised hits keep their length; otherwise follow the (smoothed) pot, rescaled below the random zone
	if (hitInZone) {
		return hitDecay;
	}
	const float zone = std::clamp(config.randomZone, 0.0f, 0.5f);
	return std::min(knobB / (1.0f - zone), 1.0f);
}


void Kick::CheckIdle(const float magnitude, const bool busy)
{
	if (magnitude < 2e-5f && !busy) {
		if (++quietCount > quietLimit) {
			idle = true;
		}
	} else {
		quietCount = 0;
	}
}


KickBody::Params Kick::BodyParams(const float lengthKnob, const float lengthScale, const float clickScale)
{
	// Called on every hit and ghost tap inside the audio interrupt: config dependent parts are precomputed in
	// ConfigureRumble, the rest uses cheap maths (library exp/pow here caused audio underruns)
	KickBody::Params bp;
	bp.f0 = std::min(config.pitch * notePitch / systemSampleRate, 0.1f);
	bp.sweepOct = bodySweepOct;
	bp.sweepCoef = bodySweepCoef;

	const float lengthMs = lengthScale * 80.0f * dyn::FastExp2(4.9658f * lengthKnob);	// Decay pot: 80 ms .. 2.5 s
	const float holdMs = std::clamp(config.sustain, 0.0f, 1.0f) * 0.5f * lengthMs;
	bp.holdSamples = static_cast<uint32_t>(holdMs * 48.0f);
	bp.holdCoef = bp.holdSamples > 0 ? dyn::ExpNeg(0.3567f / bp.holdSamples) : 1.0f;	// -3 dB across the plateau
	bp.decayCoef = dyn::DecayCoef(std::max(lengthMs - holdMs, 20.0f));

	bp.color = std::clamp(config.color, 0.0f, 1.0f);
	bp.click = std::clamp(config.click, 0.0f, 2.0f) * clickScale;
	bp.clickCoef = bodyClickCoef;
	bp.clickF = std::min(bp.f0 * bodyClickRatio, 0.08f);							// knock 3x .. 12x the base pitch
	bp.clickNoiseF = bodyNoiseF;
	return bp;
}


float Kick::BodyFx(const float x)
{
	const float d = std::clamp(config.drive, 0.0f, 1.0f);
	if (d <= 0.0f) {
		return x;
	}
	const float g = 1.0f + 6.0f * d;
	switch (static_cast<uint32_t>(config.fxMode)) {
	case 1:																		// hard clip
		return std::clamp(x * g, -1.0f, 1.0f) / (1.0f + 2.0f * d);
	case 2:																		// wavefold: sin(pi/2 * g * x)
		return plaits::Sine(std::clamp(0.25f * g * x, -7.0f, 7.0f) + 8.0f) * 0.8f;
	default:																	// soft clip
		return stmlib::SoftClip(x * g) / (1.0f + 2.0f * d);
	}
}


void Kick::ConfigureRumble()
{
	rumbleFx.Configure(config.rumbleFeedback, config.rumbleCutoff, config.rumbleDrive, config.rumbleHp, config.rumbleWidth);
	const float tapMs = 50.0f + 400.0f * std::clamp(config.tapLength, 0.0f, 1.0f);
	noiseCoef = dyn::DecayCoef(300.0f + 3.0f * tapMs);							// noise lingers: a bed under the rumble
	subDecayCoef = dyn::DecayCoef(tapMs * 1.5f);

	// Body parameters that only depend on the config (BodyParams runs on every hit)
	const float curve = std::clamp(config.curve, 0.0f, 1.0f);
	bodySweepOct = std::clamp(config.sweep, 0.0f, 60.0f) / 12.0f * (0.35f + 0.65f * curve);
	const float tauMs = 45.0f * dyn::FastExp2(-2.737f * curve);					// 45 ms round ... 7 ms snap (0.15^curve)
	bodySweepCoef = dyn::ExpNeg(1.0f / (tauMs * 48.0f));
	bodyClickCoef = dyn::ExpNeg(1.0f / (std::clamp(config.clickDecay, 0.3f, 100.0f) * 48.0f));
	const float tone = std::clamp(config.tone, 0.0f, 1.0f);
	bodyClickRatio = 3.0f + 9.0f * tone;
	bodyNoiseF = 500.0f * dyn::FastExp2(3.585f * tone) / systemSampleRate;	// noise band 500 Hz .. 6 kHz (12^tone)
}


void Kick::CalcRumble(float& outL, float& outR)
{
	// Ghost taps: a copy of the kick, noise burst and sub pulse on each tap
	for (uint32_t i = 0; i < 3; ++i) {
		int32_t& t = tapCountdown[i];
		if (t > 0 && --t == 0) {
			t = -1;
			const float tapLevel = accent * (i == 0 ? config.tap2 : i == 1 ? config.tap3 : config.tap4);
			ghost.Trigger(BodyParams(hitDecay, std::clamp(config.tapLength, 0.05f, 1.0f), 0.4f), tapLevel);
			noiseEnv = std::max(noiseEnv, tapLevel * config.rumbleNoise);
			subPeak = tapLevel;
			subAttack = true;
		}
	}

	const float main = BodyFx(body.Process());
	const float ghostOut = ghost.Process();

	// Noise texture for the space
	float noise = 0.0f;
	if (noiseEnv > 1e-5f) {
		// Low passed (~300 Hz) so it lands in the 140 Hz space; scaled so 0.2 is a bed and 1.0 a wall of noise
		noiseLp += 0.04f * ((2.0f * stmlib::Random::GetFloat() - 1.0f) - noiseLp);
		noise = 28.0f * noiseLp * noiseEnv;
		noiseEnv *= noiseCoef;
	}

	// Drive before the reverb, then the stereo rumble space
	const float grit = std::clamp(config.rumbleGrit, 0.0f, 1.0f);
	const float send = stmlib::SoftClip((main + ghostOut) * (1.0f + 8.0f * grit)) / (1.0f + 2.0f * grit) + noise;
	float rumL, rumR;
	rumbleFx.Process(send, rumL, rumR);

	// Sub pulse in tune with the kick (3 ms attack, decays with the tap length)
	if (subAttack) {
		subEnv += 0.007f * (subPeak - subEnv);
		if (subEnv >= 0.98f * subPeak) {
			subAttack = false;
		}
	} else {
		subEnv *= subDecayCoef;
	}
	float sub = 0.0f;
	if (subEnv > 1e-5f) {
		subPhase += std::min(config.pitch * notePitch / systemSampleRate, 0.1f);
		if (subPhase >= 1.0f) {
			subPhase -= 1.0f;
		}
		sub = 2.0f * plaits::Sine(subPhase) * subEnv * config.subLevel;
	}

	// Attack pot = groove amount (rumble, sub pulses, audible ghost taps), all ducked by the main kick.
	// rumbleScale: Rumble lvl 1.4 at Attack 0.8 sits ~10 dB under the kick (measured with tools/hostsim)
	constexpr float rumbleScale = 0.5f;
	const float g = knobA * (1.0f - config.rumbleDuck * voiceManager.duckLevel);
	const float centre = main + g * (sub + config.tapDirect * ghostOut);
	outL = centre + g * rumbleScale * config.rumbleLevel * rumL;
	outR = centre + g * rumbleScale * config.rumbleLevel * rumR;
}


void Kick::SetEngine(const uint8_t e)
{
	engine = static_cast<Engine>(e < engineCount ? e : 0);
	config.engine = engine;
	// Only initialise the new engine's state: this runs inside the audio interrupt
	switch (engine) {
	case deep:		analogBd.Init(); break;
	case punch:		syntheticBd.Init(); break;
	case rumble:	body.Init(); ghost.Init(); rumbleFx.Init(); ConfigureRumble(); break;
	default:		break;
	}
	for (auto& t : tapCountdown) {
		t = -1;
	}
	noiseEnv = subEnv = 0.0f;
	subAttack = false;
	idle = false;
	quietCount = 0;
	phase = Phase::Off;
	playing = (engine != classic);				// sample based engines run continuously
	trigger = false;
	outputLevel[0] = outputLevel[1] = 0.0f;
}


void Kick::UpdateFilter()
{
	filter.Update(false);
}


uint32_t Kick::SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex)
{
	config.engine = engine;
	*buff = reinterpret_cast<uint8_t*>(&config);
	return sizeof(config);
}


FLASH_CODE void Kick::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff != nullptr && len <= sizeof(config)) {
		memcpy(&config, buff, len);
	}

	// Apply any settings that are constant until configuration changes
	fastSinInc = FreqToInc(config.fastSinFreq);
	ConfigureRumble();
	const uint8_t e = static_cast<uint8_t>(std::clamp(config.engine, 0.0f, static_cast<float>(engineCount - 1)));
	if (buff == nullptr || e != engine) {
		SetEngine(e);
	}
}

uint32_t Kick::ConfigSize()
{
	return sizeof(config);
}
