// Punck host renderer (modulove): compiles the real voice code (Kick, Snare, HiHat, master bus dynamics) on the PC
// and renders WAV previews + level statistics. Build: see tools/hostsim/build_hostsim.sh
//
//   render <outdir>
//
// The master bus below mirrors VoiceManager::Output (keep in sync). The main reverb is not simulated.
#include "initialisation.h"
#include "VoiceManager.h"
#include "Dynamics.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <new>

volatile uint32_t SysTickVal = 1000;
volatile uint16_t ADC_array[ADC1_BUFFER_LENGTH + ADC2_BUFFER_LENGTH];
static HostRng rngInstance;
HostRng* RNG = &rngInstance;
VoiceManager voiceManager;

static uint32_t rngState = 0x12345678;
uint32_t HostRandom()
{
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return rngState;
}

// ------------------------------------------------------------------------------------------------
struct Wav {
	std::vector<float> data;						// interleaved stereo
	void Add(const float l, const float r) { data.push_back(l); data.push_back(r); }
	bool Write(const std::string& path) const
	{
		FILE* f = fopen(path.c_str(), "wb");
		if (!f) return false;
		const uint32_t bytes = static_cast<uint32_t>(data.size() * 4), rate = systemSampleRate, byteRate = rate * 8;
		const uint16_t fmtFloat = 3, chans = 2, align = 8, bits = 32;
		const uint32_t riff = 36 + bytes, fmtLen = 16;
		fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
		fwrite(&fmtFloat, 2, 1, f); fwrite(&chans, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
		fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
		fwrite(data.data(), 4, data.size(), f);
		fclose(f);
		return true;
	}
};

struct Stats {
	double sumSq = 0.0; float peak = 0.0f; uint64_t n = 0; uint64_t nan = 0;
	void Add(const float x) { if (!std::isfinite(x)) { ++nan; return; } sumSq += double(x) * x; peak = std::max(peak, std::abs(x)); ++n; }
	float RmsDb() const { return 10.0f * std::log10(std::max(sumSq / std::max<uint64_t>(n, 1), 1e-12)); }
	float PeakDb() const { return 20.0f * std::log10(std::max(peak, 1e-6f)); }
};

static void Pots(const float kickAttack, const float kickDecay, const float hatDecay)
{
	for (auto& a : ADC_array) a = 32768;			// all levels at noon (gain 1.0), other pots centred
	ADC_array[ADC_KickAttack] = static_cast<uint16_t>(kickAttack * 65535.0f);
	ADC_array[ADC_KickDecay] = static_cast<uint16_t>(kickDecay * 65535.0f);
	ADC_array[ADC_HiHatDecay] = static_cast<uint16_t>(hatDecay * 65535.0f);
}

// ------------------------------------------------------------------------------------------------
// Master bus: same maths as VoiceManager::Output / StoreMixConfig
struct MixConfig {
	float compOn = 1.0f, compThreshold = -12.0f, compRatio = 3.0f, compAttack = 15.0f, compRelease = 120.0f, compKnee = 6.0f,
		  compMakeup = 3.0f, compMix = 1.0f, compScHpf = 90.0f, compKick = 0.0f, clipMode = 1.0f, clipDrive = 2.0f, outTrim = 0.0f,
		  inputTrim = 0.0f;
};

struct MasterBus {
	MixConfig m;
	dyn::Compressor comp;
	dyn::Clipper clip[2];
	float clipGain = 1.0f, outGain = 1.0f, inGain = 1.0f;
	float grMax = 0.0f; double grSum = 0.0; uint64_t n = 0, clipped = 0;

	void Configure()
	{
		comp.Configure(systemSampleRate, m.compThreshold, m.compRatio, m.compKnee, m.compAttack, m.compRelease, m.compMakeup, m.compScHpf);
		clipGain = std::pow(10.0f, m.clipDrive / 20.0f);
		outGain = std::pow(10.0f, m.outTrim / 20.0f);
		inGain = std::pow(10.0f, m.inputTrim / 20.0f);
	}
	static float FastTanh(const float x)
	{
		const float x2 = x * x;
		const float a = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
		const float b = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
		return a / b;
	}
	void Process(const float kick[2], const float drums[2], float out[2])
	{
		const bool kickThru = m.compKick > 0.5f;
		float bus[2];
		const float k[2] = {kick[0] * inGain, kick[1] * inGain};
		for (int c = 0; c < 2; ++c) bus[c] = inGain * drums[c] + (kickThru ? k[c] : 0.0f);
		if (m.compOn > 0.5f) {
			const float g = comp.Gain(bus[0], bus[1]);
			const float busGain = (1.0f - m.compMix) + m.compMix * g;
			bus[0] *= busGain; bus[1] *= busGain;
			grMax = std::max(grMax, comp.ReductionDb()); grSum += comp.ReductionDb();
		}
		++n;
		for (int c = 0; c < 2; ++c) {
			const float x = bus[c] + (kickThru ? 0.0f : k[c]);
			if (m.clipMode < 0.5f) {
				out[c] = FastTanh(x);
				if (std::abs(x) > 1.0f) ++clipped;
			} else {
				const float xd = x * clipGain;
				if (std::abs(xd) > 1.0f) ++clipped;
				out[c] = clip[c].Process(xd, static_cast<uint32_t>(m.clipMode + 0.5f)) * outGain;
			}
		}
	}
};

// ------------------------------------------------------------------------------------------------
struct Rig {
	Kick kick; Snare snare; HiHat hat; Toms toms;
	NoteMapper nmK, nmS, nmH, nmT;
	Rig()
	{
		kick.noteMapper = &nmK; snare.noteMapper = &nmS; hat.noteMapper = &nmH; toms.noteMapper = &nmT;
		kick.StoreConfig(nullptr, 0); snare.StoreConfig(nullptr, 0); hat.StoreConfig(nullptr, 0); toms.StoreConfig(nullptr, 0);
		voiceManager.bassVoice = &toms;
	}
	template<class V> static void Set(V& v, const uint32_t index, const float value)
	{
		uint8_t* buf; const uint32_t len = v.SerialiseConfig(&buf, 0);
		std::vector<uint8_t> copy(buf, buf + len);
		memcpy(copy.data() + 4 * index, &value, 4);
		v.StoreConfig(copy.data(), len);
	}
	void SetKick(const uint32_t index, const float value)		// set a kick config float by index (order of Kick::Config)
	{
		uint8_t* buf; const uint32_t len = kick.SerialiseConfig(&buf, 0);
		std::vector<uint8_t> copy(buf, buf + len);
		memcpy(copy.data() + 4 * index, &value, 4);
		kick.StoreConfig(copy.data(), len);
	}
};

static void Tick(Rig& r, float kickOut[2], float drums[2])
{
	voiceManager.UpdateDuck();
	static uint32_t mainLoop = 0;
	if ((++mainLoop & 31) == 0) {				// the module's main loop (IdleTasks) recalculates the filter coefficients
		r.kick.UpdateFilter(); r.snare.UpdateFilter(); r.hat.UpdateFilter(); r.toms.UpdateFilter();
	}
	r.kick.CalcOutput(); r.snare.CalcOutput(); r.hat.CalcOutput(); r.toms.CalcOutput();
	kickOut[0] = r.kick.outputLevel[0]; kickOut[1] = r.kick.outputLevel[1];
	drums[0] = r.snare.outputLevel[0] + r.hat.outputLevel[0] + r.toms.outputLevel[0];
	drums[1] = r.snare.outputLevel[1] + r.hat.outputLevel[1] + r.toms.outputLevel[1];
}


// Snare engines side by side (4 hits each), the modal engine with several structure / brightness settings
static void RenderSnares(const std::string& dir)
{
	struct Case { const char* name; uint32_t engine; float structure, filterPot; };
	const Case cases[] = {{"analog 808", 1, 0, 0.5f}, {"synthetic 909", 2, 0, 0.5f}, {"modal drum (struct 0.15)", 4, 0.15f, 0.5f},
		{"modal drum dark (filter 0.2)", 4, 0.15f, 0.2f}, {"modal harmonic (struct 0.3)", 4, 0.3f, 0.6f}, {"modal metal (struct 0.75)", 4, 0.75f, 0.7f}};
	const float beat = 60.0f / 124.0f * systemSampleRate;
	Wav wav;
	printf("\nSnare engines (Level pot at noon):\n");
	for (const auto& c : cases) {
		Rig r;
		r.snare.SetEngine(c.engine);
		Rig::Set(r.snare, 17, c.structure);
		Pots(0.5f, 0.5f, 0.3f);
		ADC_array[ADC_SnareFilter] = static_cast<uint16_t>(c.filterPot * 65535.0f);
		Stats st;
		auto t0 = std::chrono::steady_clock::now();
		for (uint32_t s = 0; s < 4 * static_cast<uint32_t>(beat); ++s) {
			if (s % static_cast<uint32_t>(beat) == 0) r.snare.Play(0, 0, 2, 1.0f, 1.0f);
			r.snare.CalcOutput();
			wav.Add(r.snare.outputLevel[0], r.snare.outputLevel[1]); st.Add(r.snare.outputLevel[0]);
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (4 * beat);
		printf("  %-30s peak %+5.1f dBFS  rms %+5.1f dBFS  nan %llu  host %.3f us/sample\n", c.name, st.PeakDb(), st.RmsDb(),
				(unsigned long long)st.nan, us);
		for (uint32_t s = 0; s < systemSampleRate / 2; ++s) wav.Add(0.0f, 0.0f);
	}
	wav.Write(dir + "/snare_engines.wav");
}


// Bass engine: psytrance rolling bass (KBBB after each kick) and an offbeat techno bass, through the master bus
static void RenderBass(const std::string& dir, const char* name, const float bpm, const float follow, const uint32_t kickEngine,
		const float kickDecay, const float bassPitch, const float bassDecay)
{
	Rig r;
	r.kick.SetEngine(kickEngine);
	r.toms.SetEngine(1);
	Rig::Set(r.toms, 19, follow);
	Rig::Set(r.toms, 10, bassPitch);
	Rig::Set(r.toms, 16, bassDecay);
	r.hat.SetEngine(1);
	Pots(0.3f, kickDecay, 0.25f);
	const float sixteenth = 60.0f / bpm * systemSampleRate / 4.0f;
	voiceManager.sixteenth = sixteenth;
	MasterBus bus; bus.Configure();
	Wav wav; Stats st, bassSt;
	auto t0 = std::chrono::steady_clock::now();
	double bassUs = 0.0;
	for (uint32_t step = 0; step < 16 * 8; ++step) {
		if (step % 4 == 0) r.kick.Play(0, 0, 0, 1.0f, 1.0f);
		if (step % 4 == 2) r.hat.Play(0, 2, 3, 0.6f, 1.0f);
		for (uint32_t s = 0; s < static_cast<uint32_t>(sixteenth); ++s) {
			float k[2], d[2], o[2];
			Tick(r, k, d);
			bus.Process(k, d, o);
			wav.Add(o[0], o[1]); st.Add(o[0]); bassSt.Add(r.toms.outputLevel[0]);
		}
	}
	(void)t0; (void)bassUs;
	wav.Write(dir + "/" + name + ".wav");
	printf("  %-22s mix peak %+5.1f rms %+5.1f dBFS | bass alone peak %+5.1f rms %+5.1f dBFS  GR max %.1f  over-clip %.2f %%  nan %llu\n",
			name, st.PeakDb(), st.RmsDb(), bassSt.PeakDb(), bassSt.RmsDb(), bus.grMax, 100.0 * bus.clipped / std::max<uint64_t>(2 * bus.n, 1),
			(unsigned long long)(st.nan + bassSt.nan));
}

// Kick engines one after the other (4 hits each at 124 BPM), then the rumble groove
static void RenderKickEngines(const std::string& dir)
{
	const char* names[] = {"classic", "deep", "punch", "fm", "rumble (Attack 0 = body only)", "rumble groove (Attack 0.8)"};
	const float bpm = 124.0f, beat = 60.0f / bpm * systemSampleRate;
	voiceManager.sixteenth = beat / 4.0f;
	Wav wav;
	printf("\nKick engines (Level pot at noon, Decay 0.5):\n");
	for (uint32_t e = 0; e < 6; ++e) {
		Rig r;
		r.kick.SetEngine(std::min(e, 4u));
		Pots(e == 5 ? 0.8f : (e == 4 ? 0.0f : 0.5f), 0.5f, 0.3f);
		const uint32_t beats = e == 5 ? 16 : 4;
		Stats st, quarter[4];
		auto t0 = std::chrono::steady_clock::now();
		const uint32_t beatLen = static_cast<uint32_t>(beat);
		for (uint32_t s = 0; s < beats * beatLen; ++s) {
			if (s % beatLen == 0) r.kick.Play(0, 0, 0, 1.0f, 1.0f);
			float k[2], d[2];
			Tick(r, k, d);
			wav.Add(k[0], k[1]); st.Add(k[0]); st.Add(k[1]);
			if (s >= beatLen) quarter[(s % beatLen) * 4 / beatLen].Add(0.5f * (k[0] + k[1]));
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (beats * beat);
		printf("  %-32s peak %+5.1f dBFS  rms %+5.1f dBFS  nan %llu  host %.3f us/sample\n", names[e], st.PeakDb(), st.RmsDb(),
				(unsigned long long)st.nan, us);
		printf("  %-32s rms per 16th of the beat: %+5.1f %+5.1f %+5.1f %+5.1f dBFS\n", "", quarter[0].RmsDb(), quarter[1].RmsDb(),
				quarter[2].RmsDb(), quarter[3].RmsDb());
		for (uint32_t s = 0; s < systemSampleRate / 2; ++s) wav.Add(0.0f, 0.0f);
	}
	wav.Write(dir + "/kick_engines.wav");
}

// Level of the rumble parts per 16th (for calibrating the defaults)
static void RumbleProfile(const char* name, const float attack, const int index = -1, const float value = 0.0f,
		const int index2 = -1, const float value2 = 0.0f)
{
	const float bpm = 124.0f, beat = 60.0f / bpm * systemSampleRate;
	voiceManager.sixteenth = beat / 4.0f;
	Rig r;
	r.kick.SetEngine(4);
	if (index >= 0) r.SetKick(index, value);
	if (index2 >= 0) r.SetKick(index2, value2);
	Pots(attack, 0.5f, 0.3f);
	Stats q[4];
	const uint32_t beatLen = static_cast<uint32_t>(beat);
	for (uint32_t s = 0; s < 16 * beatLen; ++s) {
		if (s % beatLen == 0) r.kick.Play(0, 0, 0, 1.0f, 1.0f);
		float k[2], d[2];
		Tick(r, k, d);
		if (s >= 2 * beatLen) q[(s % beatLen) * 4 / beatLen].Add(0.5f * (k[0] + k[1]));
	}
	printf("  %-34s %+5.1f %+5.1f %+5.1f %+5.1f dBFS\n", name, q[0].RmsDb(), q[1].RmsDb(), q[2].RmsDb(), q[3].RmsDb());
}


// Deep / dub techno groove through the old output stage and through the new master bus
static void RenderGroove(const std::string& dir, const std::string& name, MixConfig cfg, const float kickLevel)  // kickLevel: all level pots
{
	Rig r;
	r.kick.SetEngine(4);											// rumble
	r.snare.SetEngine(3);											// rim
	r.hat.SetEngine(1);												// 808 hats
	Pots(0.6f, 0.45f, 0.3f);
	for (auto pot : {ADC_KickLevel, ADC_SnareLevel, ADC_HiHatLevel}) {		// level pots: 1.0 = noon, 2.0 = full
		ADC_array[pot] = static_cast<uint16_t>(std::min(kickLevel * 32768.0f, 65535.0f));
	}
	MasterBus bus; bus.m = cfg; bus.Configure();
	const float bpm = 124.0f, sixteenth = 60.0f / bpm * systemSampleRate / 4.0f;
	voiceManager.sixteenth = sixteenth;
	const char* hats = "3.6.3.6.3.6.3.6.";					// closed hats (velocity / 9)
	const char* open = "..7...7...7...7.";
	const char* rim  = "......5.......4.";
	Wav wav; Stats st;
	const uint32_t steps = 16 * 8;
	for (uint32_t step = 0; step < steps; ++step) {
		const uint32_t i = step % 16;
		if (i % 4 == 0) r.kick.Play(0, 0, 0, 1.0f, 1.0f);
		if (hats[i] != '.') r.hat.Play(0, 0, 3, (hats[i] - '0') / 9.0f, 1.0f);
		if (open[i] != '.') r.hat.Play(0, 2, 3, (open[i] - '0') / 9.0f, 1.0f);
		if (rim[i] != '.') r.snare.Play(0, 0, 2, (rim[i] - '0') / 9.0f, 1.0f);
		for (uint32_t s = 0; s < static_cast<uint32_t>(sixteenth); ++s) {
			float k[2], d[2], o[2];
			Tick(r, k, d);
			bus.Process(k, d, o);
			wav.Add(o[0], o[1]); st.Add(o[0]); st.Add(o[1]);
		}
	}
	wav.Write(dir + "/" + name + ".wav");
	printf("  %-22s peak %+5.1f dBFS  rms %+5.1f dBFS  crest %4.1f dB  GR max %4.1f avg %4.1f dB  over-clip %5.2f %%  nan %llu\n",
			name.c_str(), st.PeakDb(), st.RmsDb(), st.PeakDb() - st.RmsDb(), bus.grMax, bus.n ? bus.grSum / bus.n : 0.0,
			100.0 * bus.clipped / std::max<uint64_t>(2 * bus.n, 1), (unsigned long long)st.nan);
}

// Hats and kick Decay pot incl. the random zone: length per hit (time until -40 dB below the hit's peak)
template<class F> static void ZoneStats(const char* label, F setup, const float pot, const bool kick, const uint32_t engine,
		const uint32_t offset, Wav* wav)
{
	Rig r;
	if (kick) r.kick.SetEngine(engine); else r.hat.SetEngine(engine);
	setup(r);
	Pots(0.3f, kick ? pot : 0.5f, kick ? 0.3f : pot);
	const uint32_t hitLen = systemSampleRate * (kick ? 3 : 2) / 2;
	const uint32_t hits = 48;
	uint32_t longHits = 0; double sumMs = 0.0, maxMs = 0.0, minMs = 1e9;
	for (uint32_t h = 0; h < hits; ++h) {
		if (kick) r.kick.Play(0, 0, 0, 1.0f, 1.0f); else r.hat.Play(0, offset, 3, 1.0f, 1.0f);
		std::vector<float> env(hitLen);
		float pk = 0.0f;
		for (uint32_t s = 0; s < hitLen; ++s) {
			float k[2], d[2];
			Tick(r, k, d);
			const float v = kick ? k[0] : d[0];
			env[s] = std::abs(v); pk = std::max(pk, env[s]);
			if (wav && h < 12) wav->Add(v, v);
		}
		uint32_t last = 0;
		for (uint32_t s = 0; s < hitLen; ++s) if (env[s] > pk * 0.01f) last = s;
		const double ms = 1000.0 * last / systemSampleRate;
		sumMs += ms; maxMs = std::max(maxMs, ms); minMs = std::min(minMs, ms);
		if (ms > (kick ? 700.0 : 250.0)) ++longHits;
	}
	printf("  %-26s pot %4.2f: length avg %6.0f ms  min %5.0f  max %5.0f   long hits %2u / %u\n", label, pot, sumMs / hits, minMs, maxMs, longHits, hits);
}


// Long run: the module's current setup (kick deep, snare 808, classic hats, toms) plays a 124 BPM groove for hours
// through the master bus. Every kick hit is compared cycle by cycle with the first one (any drift with run time
// shows up as cents), hat hits by level; NaN / level per 10 minutes. Tests for state that degrades over time.
static void LongRun(const std::string& dir, const double hours)
{
	// GARBAGE=0xNN: build the voices on memory filled with that byte (0xFF = every float NaN) to find state that is
	// used before it is set. The module's voices sit in zeroed .bss, so the default is zero-filled memory.
	alignas(Rig) static unsigned char rigMem[sizeof(Rig)];
	const char* g = getenv("GARBAGE");
	memset(rigMem, g ? static_cast<int>(strtol(g, nullptr, 0)) : 0, sizeof(rigMem));
	Rig& r = *new (rigMem) Rig();
	r.kick.SetEngine(1); r.snare.SetEngine(1); r.hat.SetEngine(0); r.toms.SetEngine(0);
	Pots(0.851f, 0.511f, 0.817f);
	ADC_array[ADC_SnareTuning] = static_cast<uint16_t>(0.523f * 65535.0f);
	ADC_array[ADC_SnareFilter] = static_cast<uint16_t>(0.884f * 65535.0f);
	MixConfig cfg; cfg.outTrim = 0.7f; cfg.inputTrim = -5.0f;
	MasterBus bus; bus.m = cfg; bus.Configure();
	const float sixteenth = 60.0f / 124.0f * systemSampleRate / 4.0f;
	voiceManager.sixteenth = sixteenth;
	const uint32_t sixteenthN = static_cast<uint32_t>(sixteenth);
	const uint64_t steps = static_cast<uint64_t>(hours * 3600.0 * systemSampleRate / sixteenthN);

	auto crossings = [](const std::vector<float>& x) {
		std::vector<double> xs;
		for (size_t i = 1; i < x.size() && xs.size() < 24; ++i) {
			if (x[i - 1] < 0.0f && x[i] >= 0.0f) xs.push_back(i - 1 + x[i - 1] / (x[i - 1] - x[i]));
		}
		return xs;
	};
	std::vector<double> refKick; double refHat = 0.0;
	double kickDevMax = 0.0, hatDevMax = 0.0, intervalDevMax = 0.0;
	uint64_t nan = 0; float peak = 0.0f; double sum = 0.0; uint64_t n = 0;
	uint64_t vNan[4] = {}; int64_t vFirst[4] = {-1, -1, -1, -1}; uint32_t vFirstStep[4] = {};
	const char* vName[4] = {"kick", "snare", "hats", "toms"};
	std::vector<float> kickBuf, hatBuf;
	Wav startWav, endWav;
	const uint64_t samplesPer10Min = static_cast<uint64_t>((hours < 0.2 ? 30.0 : 600.0) * systemSampleRate);
	uint64_t sampleCount = 0;
	printf("\nLong run %.1f h (kick deep, snare 808, hats classic, toms; pots as on the module):\n", hours);
	auto t0 = std::chrono::steady_clock::now();
	for (uint64_t step = 0; step < steps; ++step) {
		const uint32_t i = step % 16;
		const bool kickHit = (i % 4 == 0), hatHit = (i % 2 == 0);
		if (kickHit) { r.kick.Play(0, 0, 0, 1.0f, 1.0f); kickBuf.clear(); }
		if (hatHit) { r.hat.Play(0, 0, 3, 0.7f, 1.0f); hatBuf.clear(); }
		if (i == 4 || i == 12) r.snare.Play(0, 0, 2, 0.8f, 1.0f);
		if (i == 14 && (step / 16) % 4 == 3) r.toms.Play(0, 0, 1, 0.8f, 1.0f);
		for (uint32_t s = 0; s < sixteenthN; ++s, ++sampleCount) {
			float k[2], d[2], o[2];
			Tick(r, k, d);
			bus.Process(k, d, o);
			const float vOut[4] = {r.kick.outputLevel[0], r.snare.outputLevel[0], r.hat.outputLevel[0], r.toms.outputLevel[0]};
			for (int v = 0; v < 4; ++v) {
				if (!std::isfinite(vOut[v])) {
					if (vFirst[v] < 0) { vFirst[v] = sampleCount; vFirstStep[v] = i; }
					++vNan[v];
				}
			}
			if (kickBuf.size() < 4 * sixteenthN) kickBuf.push_back(k[0]);
			if (hatBuf.size() < sixteenthN) hatBuf.push_back(r.hat.outputLevel[0]);
			if (!std::isfinite(o[0]) || !std::isfinite(o[1])) ++nan;
			peak = std::max(peak, std::max(std::abs(o[0]), std::abs(o[1])));
			sum += o[0] * o[0]; ++n;
			if (step < 16 * 8) startWav.Add(o[0], o[1]);
			if (step >= steps - 16 * 8) endWav.Add(o[0], o[1]);
			if ((sampleCount + 1) % samplesPer10Min == 0) {
				printf("  %5.0f min  peak %+5.1f dBFS  rms %+5.1f dBFS  NaN %llu  kick vs first hit: timing %.3f smp, period %.3f cents  hats: %.2f dB\n",
						(sampleCount + 1) / (60.0 * systemSampleRate), 20 * std::log10(std::max(peak, 1e-9f)), 10 * std::log10(std::max(sum / n, 1e-18)),
						(unsigned long long)nan, kickDevMax, intervalDevMax, hatDevMax);
				for (int v = 0; v < 4; ++v) {
					if (vNan[v]) printf("      %s: %llu NaN samples, first at %.3f s (16th step %u)\n", vName[v], (unsigned long long)vNan[v],
							vFirst[v] / static_cast<double>(systemSampleRate), vFirstStep[v]);
				}
				fflush(stdout);
				peak = 0.0f; sum = 0.0; n = 0;
			}
		}
		if (i % 4 == 3) {																// kick buffer complete (4 sixteenths)
			const auto xs = crossings(kickBuf);
			if (refKick.empty()) refKick = xs;
			for (size_t c = 0; c < std::min(xs.size(), refKick.size()); ++c) {
				kickDevMax = std::max(kickDevMax, std::abs(xs[c] - refKick[c]));		// zero crossing timing error in samples
				if (c > 0) {
					const double p0 = refKick[c] - refKick[c - 1], p1 = xs[c] - xs[c - 1];
					intervalDevMax = std::max(intervalDevMax, std::abs(1200.0 * std::log2(p1 / p0)));
				}
			}
		}
		if (i % 2 == 1) {
			double e = 0.0; for (float v : hatBuf) e += v * v;
			const double db = 10.0 * std::log10(std::max(e, 1e-20));
			if (refHat == 0.0) refHat = db;
			hatDevMax = std::max(hatDevMax, std::abs(db - refHat));
		}
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	printf("  done in %.0f s host time; first and last 8 bars: longrun_start.wav / longrun_end.wav\n", secs);
	startWav.Write(dir + "/longrun_start.wav");
	endWav.Write(dir + "/longrun_end.wav");
}


// Classic snare with a long noise decay hit on the top of its MIDI range (note offset 1 of 2): the per-sample decay
// factor must stay below 1 (stress test 2026-11-16: decay + sustainRate went above 1 and the level grew to inf)
static void SnareSustainTest()
{
	alignas(Rig) static unsigned char mem[sizeof(Rig)];
	Rig& r = *new (mem) Rig;
	r.snare.SetEngine(0);
	ADC_array[ADC_SnareLevel] = 32768;									// Level pot at noon
	ADC_array[ADC_SnareTuning] = 32768;
	ADC_array[ADC_SnareFilter] = 49000;
	uint8_t* buff = nullptr;
	const uint32_t size = r.snare.SerialiseConfig(&buff, 0);
	float* cfg = reinterpret_cast<float*>(buff);
	cfg[1] = std::exp(std::log(0.001f) / (48000.0f * 0.412f));		// noiseDecay 412 ms (parameter set 13)
	cfg[4] = std::exp(std::log(0.001f) / (48000.0f * 0.054f));		// partialDecay 54 ms
	cfg[0] = 1.69f;													// noiseInitLevel
	r.snare.StoreConfig(buff, size);
	float peak = 0.0f, tailPeak = 0.0f;
	for (uint32_t s = 0; s < 48000 * 6; ++s) {
		if (s < 48000 * 2 && s % 4800 == 0) {
			r.snare.Play(0, 1, 2, 1.0f, 1.0f);						// MIDI note 38: offset 1 of the snare's 2 notes
		}
		r.snare.CalcOutput();
		const float a = std::fabs(r.snare.outputLevel[0]);
		peak = std::isfinite(a) ? std::max(peak, a) : INFINITY;
		if (s > 48000 * 5) {
			tailPeak = std::isfinite(a) ? std::max(tailPeak, a) : INFINITY;
		}
	}
	printf("Classic snare, noise decay 412 ms, note 38 hits for 2 s: peak %.2f (%+.1f dB), level 3 s after the last hit %+.1f dB\n",
			peak, 20.0f * std::log10(std::max(peak, 1e-9f)), 20.0f * std::log10(std::max(tailPeak, 1e-9f)));
}


// String engine (Mutable Plaits string): snare engine 6 and toms engine 3 'pluck'. Run separately ('strings') so
// the reference renders of the other engines keep their random number sequence (bit-identical comparison).
// Snare config indices: engine 11, pitch 12, decay 14, stringStructure 23, stringStrings 24, stringLevel 25.
// Toms: engine 9, bassFollow 19, pluckPitch 23, pluckBrightness 24, pluckDecay 25, pluckStructure 26, pluckStrings 27.
static float Rnd01() { return (HostRandom() & 0xFFFF) / 65535.0f; }

static void StringTests(const std::string& dir)
{
	const float beat = 60.0f / 124.0f * systemSampleRate;
	alignas(Rig) static unsigned char mem[sizeof(Rig)];

	// 1. Snare: 808 reference, then the string at several settings (4 hits each, Level pot at noon)
	struct Case { const char* name; uint32_t engine; float structure, filterPot, decay, strings; };
	const Case cases[] = {
		{"808 snare (reference)", 1, 0.25f, 0.5f, 0.35f, 3}, {"string clean", 5, 0.25f, 0.5f, 0.35f, 3},
		{"string dark (filter 0.2)", 5, 0.25f, 0.2f, 0.35f, 3}, {"string bright (filter 0.9)", 5, 0.25f, 0.9f, 0.35f, 3},
		{"string long (decay 0.7)", 5, 0.25f, 0.5f, 0.7f, 3}, {"string buzz (struct 0.05)", 5, 0.05f, 0.6f, 0.5f, 3},
		{"string metallic (struct 0.6)", 5, 0.6f, 0.6f, 0.5f, 3}, {"string bell (struct 0.95)", 5, 0.95f, 0.7f, 0.6f, 3}};
	Wav wav;
	printf("\nSnare string engine (Level pot at noon, 4 hits at 124 BPM, 200 Hz):\n");
	for (const auto& c : cases) {
		Rig& r = *new (mem) Rig;
		r.snare.SetEngine(c.engine);
		Rig::Set(r.snare, 14, c.decay);
		Rig::Set(r.snare, 23, c.structure);
		Rig::Set(r.snare, 24, c.strings);
		Pots(0.5f, 0.5f, 0.3f);
		ADC_array[ADC_SnareFilter] = static_cast<uint16_t>(c.filterPot * 65535.0f);
		Stats st;
		auto t0 = std::chrono::steady_clock::now();
		for (uint32_t s = 0; s < 4 * static_cast<uint32_t>(beat); ++s) {
			if (s % static_cast<uint32_t>(beat) == 0) r.snare.Play(0, 0, 2, 1.0f, 1.0f);
			r.snare.CalcOutput();
			wav.Add(r.snare.outputLevel[0], r.snare.outputLevel[1]); st.Add(r.snare.outputLevel[0]);
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (4 * beat);
		// decay of a single hit: time until the level stays below -60 dB re its peak
		uint32_t len = 0;
		{
			Rig& q = *new (mem) Rig;
			q.snare.SetEngine(c.engine); Rig::Set(q.snare, 14, c.decay); Rig::Set(q.snare, 23, c.structure);
			q.snare.Play(0, 0, 2, 1.0f, 1.0f);
			float pk = 0.0f;
			for (uint32_t s = 0; s < systemSampleRate * 20; ++s) {
				q.snare.CalcOutput();
				const float a = std::fabs(q.snare.outputLevel[0]);
				pk = std::max(pk, a);
				if (a > pk * 0.001f) len = s;
			}
		}
		printf("  %-30s peak %+5.1f dBFS  rms %+5.1f dBFS  -60 dB after %6.0f ms  nan %llu  host %.3f us/sample\n", c.name,
				st.PeakDb(), st.RmsDb(), 1000.0f * len / systemSampleRate, (unsigned long long)st.nan, us);
		for (uint32_t s = 0; s < systemSampleRate / 2; ++s) wav.Add(0.0f, 0.0f);
	}
	wav.Write(dir + "/string_snare.wav");

	// 2. Snare roll: 16ths through a minor pentatonic, 3 strings (tails overlap)
	{
		Rig& r = *new (mem) Rig;
		r.snare.SetEngine(5);
		Rig::Set(r.snare, 14, 0.55f);
		Pots(0.5f, 0.5f, 0.3f);
		ADC_array[ADC_SnareFilter] = static_cast<uint16_t>(0.55f * 65535.0f);
		const int notes[] = {0, 3, 5, 7, 10, 12, 10, 7, 5, 3, 0, -2, 0, 3, 7, 12};
		Wav roll; Stats st;
		for (uint32_t step = 0; step < 32; ++step) {
			r.snare.Play(0, 0, 0, step % 4 == 0 ? 1.0f : 0.7f, std::pow(2.0f, notes[step % 16] / 12.0f));
			for (uint32_t s = 0; s < static_cast<uint32_t>(beat / 4); ++s) {
				r.snare.CalcOutput();
				roll.Add(r.snare.outputLevel[0], r.snare.outputLevel[1]); st.Add(r.snare.outputLevel[0]);
			}
		}
		for (uint32_t s = 0; s < systemSampleRate; ++s) { r.snare.CalcOutput(); roll.Add(r.snare.outputLevel[0], r.snare.outputLevel[1]); }
		roll.Write(dir + "/string_snare_roll.wav");
		printf("  %-30s peak %+5.1f dBFS  rms %+5.1f dBFS  nan %llu\n", "roll (16ths, pentatonic)", st.PeakDb(), st.RmsDb(), (unsigned long long)st.nan);
	}

	// 3. Toms pluck: melody alone, then dub offbeat stabs after a deep kick through the master bus
	printf("\nToms pluck engine (C3 at note 60):\n");
	{
		Rig& r = *new (mem) Rig;
		r.toms.SetEngine(2);
		Pots(0.5f, 0.5f, 0.3f);
		const int notes[] = {0, 0, 12, 0, 7, 0, 10, 12, 3, 0, 15, 0, 7, 10, 12, 19};
		Wav mel; Stats st;
		auto t0 = std::chrono::steady_clock::now();
		uint32_t n = 0;
		for (uint32_t step = 0; step < 32; ++step) {
			r.toms.Play(0, 0, 0, step % 2 == 0 ? 0.9f : 0.6f, std::pow(2.0f, (notes[step % 16] - 12) / 12.0f));
			for (uint32_t s = 0; s < static_cast<uint32_t>(beat / 4); ++s, ++n) {
				r.toms.CalcOutput();
				mel.Add(r.toms.outputLevel[0], r.toms.outputLevel[1]); st.Add(r.toms.outputLevel[0]);
			}
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
		for (uint32_t s = 0; s < systemSampleRate; ++s) { r.toms.CalcOutput(); mel.Add(r.toms.outputLevel[0], r.toms.outputLevel[1]); }
		mel.Write(dir + "/pluck_melody.wav");
		printf("  %-30s peak %+5.1f dBFS  rms %+5.1f dBFS  nan %llu  host %.3f us/sample\n", "melody (16ths)", st.PeakDb(), st.RmsDb(),
				(unsigned long long)st.nan, us);
	}
	const std::pair<const char*, float> dubs[] = {{"pluck_dub_offbeat_124", 0.25f}, {"pluck_dub_metallic_124", 0.55f}};
	for (const auto& v : dubs) {
		Rig& r = *new (mem) Rig;
		r.kick.SetEngine(1);
		r.toms.SetEngine(2);
		Rig::Set(r.toms, 19, 1.0f);									// follow: offbeat after every kick
		Rig::Set(r.toms, 25, 0.7f);									// longer ring
		Rig::Set(r.toms, 26, v.second);
		r.hat.SetEngine(1);
		Pots(0.3f, 0.45f, 0.25f);
		const float sixteenth = beat / 4.0f;
		voiceManager.sixteenth = sixteenth;
		MasterBus bus; bus.Configure();
		Wav wav2; Stats st, pl;
		const int chord[] = {0, 0, 3, -2};								// note per bar (C, C, Eb, Bb)
		for (uint32_t step = 0; step < 16 * 8; ++step) {
			if (step % 16 == 0) r.toms.Play(0, 0, 0, 0.35f, std::pow(2.0f, chord[(step / 16) % 4] / 12.0f));	// sets the note (soft)
			if (step % 4 == 0) r.kick.Play(0, 0, 0, 1.0f, 1.0f);
			if (step % 4 == 2) r.hat.Play(0, 2, 3, 0.5f, 1.0f);
			for (uint32_t s = 0; s < static_cast<uint32_t>(sixteenth); ++s) {
				float k[2], d[2], o[2];
				Tick(r, k, d);
				bus.Process(k, d, o);
				wav2.Add(o[0], o[1]); st.Add(o[0]); pl.Add(r.toms.outputLevel[0]);
			}
		}
		wav2.Write(dir + "/" + v.first + ".wav");
		printf("  %-30s mix peak %+5.1f rms %+5.1f dBFS | pluck alone peak %+5.1f rms %+5.1f dBFS  nan %llu\n", v.first,
				st.PeakDb(), st.RmsDb(), pl.PeakDb(), pl.RmsDb(), (unsigned long long)(st.nan + pl.nan));
	}

	// Decay time (single hit, accent 0.8) vs damping and brightness, pluck at 65 / 200 / 800 Hz
	printf("\nString T60 (s) vs damping (rows) at brightness 0.1 / 0.5 / 1.0 and 65 / 200 / 800 Hz:\n");
	for (const float damp : {0.0f, 0.2f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f}) {
		printf("  damping %.2f:", damp);
		for (const float br : {0.1f, 0.5f, 1.0f}) {
			for (const float hz : {65.0f, 200.0f, 800.0f}) {
				Rig& q = *new (mem) Rig;
				q.toms.SetEngine(2);
				Rig::Set(q.toms, 23, hz); Rig::Set(q.toms, 24, br); Rig::Set(q.toms, 25, damp); Rig::Set(q.toms, 27, 1.0f);
				q.toms.Play(0, 0, 0, 0.8f, 1.0f);
				float pk = 0.0f; uint32_t len = 0;
				for (uint32_t s = 0; s < systemSampleRate * 30; ++s) {
					q.toms.CalcOutput();
					const float a = std::fabs(q.toms.outputLevel[0]);
					pk = std::max(pk, a);
					if (a > pk * 0.001f) len = s;
				}
				printf(" %5.1f", float(len) / systemSampleRate);
			}
			printf(" |");
		}
		printf("\n");
	}

	// 4. Robustness: random hits with random settings (incl. floods), and a same-pitch flood at maximum ring
	printf("\nString robustness:\n");
	for (uint32_t voice = 0; voice < 2; ++voice) {
		Rig& r = *new (mem) Rig;
		Pots(0.5f, 0.5f, 0.3f);
		if (voice == 0) r.snare.SetEngine(5); else r.toms.SetEngine(2);
		Stats st;
		uint32_t next = 0, hits = 0;
		for (uint32_t s = 0; s < systemSampleRate * 120; ++s) {
			if (s == next) {
				const uint32_t gap = Rnd01() < 0.2f ? 1 + HostRandom() % 100 : 1 + HostRandom() % 24000;		// 20 % floods
				next = s + gap;
				++hits;
				const float pitch = std::pow(2.0f, (Rnd01() - 0.5f) * 10.0f);							// +-5 octaves
				const float vel = Rnd01();
				if (hits % 50 == 1) {
					if (voice == 0) {
						Rig::Set(r.snare, 14, Rnd01()); Rig::Set(r.snare, 23, Rnd01()); Rig::Set(r.snare, 24, 1.0f + std::floor(Rnd01() * 3.0f));
						ADC_array[ADC_SnareFilter] = static_cast<uint16_t>(Rnd01() * 65535.0f);
						ADC_array[ADC_SnareTuning] = static_cast<uint16_t>(Rnd01() * 65535.0f);
					} else {
						Rig::Set(r.toms, 24, Rnd01()); Rig::Set(r.toms, 25, Rnd01()); Rig::Set(r.toms, 26, Rnd01());
						Rig::Set(r.toms, 27, 1.0f + std::floor(Rnd01() * 3.0f));
					}
				}
				if (voice == 0) r.snare.Play(0, HostRandom() % 2, 2, vel, pitch); else r.toms.Play(0, 0, 0, vel, pitch);
			}
			float o;
			if (voice == 0) { r.snare.CalcOutput(); o = r.snare.outputLevel[0]; } else { r.toms.CalcOutput(); o = r.toms.outputLevel[0]; }
			st.Add(o);
		}
		printf("  %-6s random: %u hits in 120 s, peak %+5.1f dBFS, rms %+5.1f dBFS, nan %llu\n", voice == 0 ? "snare" : "pluck", hits,
				st.PeakDb(), st.RmsDb(), (unsigned long long)st.nan);
	}
	for (const float pitchHz : {50.0f, 200.0f, 1000.0f}) {
		Rig& r = *new (mem) Rig;
		Pots(0.5f, 0.5f, 0.3f);
		r.toms.SetEngine(2);
		Rig::Set(r.toms, 23, pitchHz); Rig::Set(r.toms, 24, 1.0f); Rig::Set(r.toms, 25, 0.94f); Rig::Set(r.toms, 26, 0.25f);
		Stats st, tail;
		for (uint32_t s = 0; s < systemSampleRate * 8; ++s) {
			if (s < systemSampleRate * 4 && s % 96 == 0) r.toms.Play(0, 0, 0, 1.0f, 1.0f);			// 500 hits/s, same pitch
			r.toms.CalcOutput();
			st.Add(r.toms.outputLevel[0]);
			if (s > systemSampleRate * 7) tail.Add(r.toms.outputLevel[0]);
		}
		printf("  flood 500/s at %4.0f Hz, max ring: peak %+5.1f dBFS, rms %+5.1f dBFS, 3 s after: rms %+5.1f dBFS, nan %llu\n", pitchHz,
				st.PeakDb(), st.RmsDb(), tail.RmsDb(), (unsigned long long)st.nan);
	}
}

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : ".";
	if (argc > 2 && std::string(argv[2]) == "snaresustain") {
		SnareSustainTest();
		return 0;
	}
	if (argc > 2 && std::string(argv[2]) == "strings") {
		StringTests(dir);
		return 0;
	}
	if (argc > 3 && std::string(argv[2]) == "longrun") {
		LongRun(dir, atof(argv[3]));
		return 0;
	}
	{
		Rig r;
		printf("Config sizes (bytes): kick %u snare %u hats %u toms %u\n", (unsigned)r.kick.ConfigSize(), (unsigned)r.snare.ConfigSize(),
				(unsigned)r.hat.ConfigSize(), (unsigned)r.toms.ConfigSize());
	}
	{
		// Key & scale: what the voices' default pitches snap to
		static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		static char bufs[4][16];
		static int bi = 0;
		auto noteName = [](float hz) {
			char* buf = bufs[bi++ % 4];
			const int n = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0)));
			snprintf(buf, 16, "%s%d", names[(n + 120) % 12], n / 12 - 1);
			return buf;
		};
		const uint32_t modes[VoiceManager::count] = {2, 3, 0, 0, 0, 3, 0};
		printf("Key & scale check (default tune modes: kick root+5th, snare scale, toms/bass scale):\n");
		for (int32_t root : {5, 9, 0}) {
			for (uint32_t scale : {0u, 1u, 2u}) {
				voiceManager.SetKey(root, scale, modes);
				const float k = voiceManager.Tune(VoiceManager::kick, 49.0f), b = voiceManager.Tune(VoiceManager::toms, 41.2f);
				const float s = voiceManager.Tune(VoiceManager::snare, 200.0f);
				printf("  %-2s %-9s kick 49 Hz -> %5.1f Hz %-4s  bass 41.2 Hz -> %5.1f Hz %-4s  snare 200 Hz -> %5.1f Hz %s\n", names[root],
						scale == 0 ? "minor" : scale == 1 ? "dorian" : "phrygian", k, noteName(k), b, noteName(b), s, noteName(s));
			}
		}
		// Tuning pot sweep in F minor (snare mode scale): every semitone of the pot range lands on a scale note
		voiceManager.SetKey(5, 0, modes);
		printf("  F minor, snare Tuning pot 100 .. 400 Hz in semitones ->");
		for (int st = 0; st <= 24; st += 2) printf(" %s", noteName(voiceManager.Tune(VoiceManager::snare, 100.0f * std::pow(2.0f, st / 12.0f))));
		printf("\n");
		voiceManager.SetKey(-1, 0, modes);
	}
	RenderKickEngines(dir);
	RenderSnares(dir);

	printf("\nHats 808 closed (note 42) / open (note 44), Decay pot incl. random zone (top 10 %%):\n");
	auto none = [](Rig&) {};
	Wav zoneWav;
	for (float pot : {0.0f, 0.5f, 0.89f, 0.91f, 0.95f, 1.0f}) ZoneStats("808 closed", none, pot, false, 1, 0, pot == 1.0f ? &zoneWav : nullptr);
	for (float pot : {0.0f, 0.5f, 0.89f}) ZoneStats("808 open", none, pot, false, 1, 2, nullptr);
	for (float pot : {0.0f, 0.5f, 0.89f}) ZoneStats("classic closed", none, pot, false, 0, 0, nullptr);
	printf("\nKick (deep) Decay pot incl. random zone:\n");
	for (float pot : {0.3f, 0.89f, 0.91f, 0.95f, 1.0f}) ZoneStats("deep kick", none, pot, true, 1, 0, pot == 1.0f ? &zoneWav : nullptr);
	zoneWav.Write(dir + "/random_zone_full_cw.wav");

	printf("\nBass engine (toms slot) through the master bus:\n");
	RenderBass(dir, "bass_psy_kbbb_142", 142.0f, 2.0f, 2, 0.15f, 46.25f, 110.0f);		// punch kick, F#1, rolling
	RenderBass(dir, "bass_offbeat_124", 124.0f, 1.0f, 1, 0.45f, 43.65f, 180.0f);		// deep kick, F1, offbeat

	printf("\nRumble engine, rms per 16th (kick on the 1st):\n");
	RumbleProfile("Attack 0.8 defaults", 0.8f);
	RumbleProfile("Attack 1.0 defaults", 1.0f);
	RumbleProfile("Attack 0.8 no noise", 0.8f, 30, 0.0f);
	RumbleProfile("Attack 0.8 no taps", 0.8f, 19, 0.0f, 20, 0.0f);
	RumbleProfile("Attack 0.8 noise 1.0", 0.8f, 30, 1.0f);

	printf("\nGroove (rumble kick, rim, 808 hats, 124 BPM, all Level pots at noon):\n");
	MixConfig legacy; legacy.compOn = 0.0f; legacy.clipMode = 0.0f;
	RenderGroove(dir, "groove_1_original", legacy, 1.0f);
	MixConfig def;
	RenderGroove(dir, "groove_2_master", def, 1.0f);
	MixConfig loud = def; loud.clipDrive = 5.0f; loud.compThreshold = -16.0f;
	RenderGroove(dir, "groove_3_master_loud", loud, 1.0f);
	MixConfig pump = def; pump.compKick = 1.0f; pump.compScHpf = 40.0f;
	RenderGroove(dir, "groove_4_kick_pumps", pump, 1.0f);
	MixConfig warm = def; warm.clipMode = 3.0f;
	RenderGroove(dir, "groove_5_master_warm", warm, 1.0f);
	printf("\nHot pots (all levels at max = +6 dB), clean clipper, input trim 0 / -6 dB:\n");
	RenderGroove(dir, "groove_6_hot_pots", def, 2.0f);
	MixConfig staged = def; staged.inputTrim = -6.0f;
	RenderGroove(dir, "groove_7_hot_pots_trim-6", staged, 2.0f);
	return 0;
}
