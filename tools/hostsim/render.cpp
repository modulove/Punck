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
		  compMakeup = 3.0f, compMix = 1.0f, compScHpf = 90.0f, compKick = 0.0f, clipMode = 1.0f, clipDrive = 2.0f, outTrim = 0.0f;
};

struct MasterBus {
	MixConfig m;
	dyn::Compressor comp;
	dyn::Clipper clip[2];
	float clipGain = 1.0f, outGain = 1.0f;
	float grMax = 0.0f; double grSum = 0.0; uint64_t n = 0, clipped = 0;

	void Configure()
	{
		comp.Configure(systemSampleRate, m.compThreshold, m.compRatio, m.compKnee, m.compAttack, m.compRelease, m.compMakeup, m.compScHpf);
		clipGain = std::pow(10.0f, m.clipDrive / 20.0f);
		outGain = std::pow(10.0f, m.outTrim / 20.0f);
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
		for (int c = 0; c < 2; ++c) bus[c] = drums[c] + (kickThru ? kick[c] : 0.0f);
		if (m.compOn > 0.5f) {
			const float g = comp.Gain(bus[0], bus[1]);
			const float busGain = (1.0f - m.compMix) + m.compMix * g;
			bus[0] *= busGain; bus[1] *= busGain;
			grMax = std::max(grMax, comp.ReductionDb()); grSum += comp.ReductionDb();
		}
		++n;
		for (int c = 0; c < 2; ++c) {
			const float x = bus[c] + (kickThru ? 0.0f : kick[c]);
			if (m.clipMode < 0.5f) {
				out[c] = FastTanh(x);
				if (std::abs(x) > 1.0f) ++clipped;
			} else {
				const float xd = x * clipGain;
				if (std::abs(xd) > 1.0f) ++clipped;
				out[c] = clip[c].Process(xd, m.clipMode > 1.5f) * outGain;
			}
		}
	}
};

// ------------------------------------------------------------------------------------------------
struct Rig {
	Kick kick; Snare snare; HiHat hat;
	NoteMapper nmK, nmS, nmH;
	Rig()
	{
		kick.noteMapper = &nmK; snare.noteMapper = &nmS; hat.noteMapper = &nmH;
		kick.StoreConfig(nullptr, 0); snare.StoreConfig(nullptr, 0); hat.StoreConfig(nullptr, 0);
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
	r.kick.CalcOutput(); r.snare.CalcOutput(); r.hat.CalcOutput();
	kickOut[0] = r.kick.outputLevel[0]; kickOut[1] = r.kick.outputLevel[1];
	drums[0] = r.snare.outputLevel[0] + r.hat.outputLevel[0];
	drums[1] = r.snare.outputLevel[1] + r.hat.outputLevel[1];
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
static void RenderGroove(const std::string& dir, const std::string& name, MixConfig cfg, const float kickLevel)
{
	Rig r;
	r.kick.SetEngine(4);											// rumble
	r.snare.SetEngine(3);											// rim
	r.hat.SetEngine(1);												// 808 hats
	Pots(0.6f, 0.45f, 0.3f);
	ADC_array[ADC_KickLevel] = static_cast<uint16_t>(kickLevel * 32768.0f);
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

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : ".";
	RenderKickEngines(dir);

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
	return 0;
}
