#pragma once
#include "initialisation.h"
#include "Filter.h"
#include "DrumVoice.h"
#include "stmlib/dsp/filter.h"

class NoteMapper;

class Toms : public DrumVoice {
public:
	enum Engine : uint8_t {classic = 0, bass, engineCount};

	void Play(const uint8_t voice, const uint32_t noteOffset, const uint32_t noteRange, const float velocity, const float pitch);
	void Play(const uint8_t voice, const uint32_t index);
	void CalcOutput();
	void UpdateFilter();
	uint32_t SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex);
	void StoreConfig(uint8_t* buff, const uint32_t len);
	uint32_t ConfigSize();
	uint8_t EngineCount() override { return engineCount; }
	uint8_t GetEngine() override { return engine; }
	void SetEngine(const uint8_t e) override;

	void KickPlayed(const float sixteenthSamples, const float velocity);	// Bass engine: auto bass notes after each kick

	NoteMapper* noteMapper;

private:
	void CalcToms();
	float CalcBass();
	void BassNote(const float ratio, const float velocity);

	enum class Phase {Off, Ramp, Sine} phase;
	static constexpr uint8_t partialCount = 2;

	float position[partialCount];
	float currentLevel;
	float velocityScale;

	float sineInc[partialCount];
	float sineLevel[partialCount];
	float pitchScale;						// Note index allows different frequency notes

	// Bass engine (modulove): saw/pulse + sub sine -> 2 x SVF low pass (24 dB) with envelope -> drive
	Engine engine = classic;
	float oscPhase = 0.0f, subPhase = 0.0f, oscInc = 0.001f;
	float ampEnv = 0.0f, ampPeak = 0.0f, ampCoef = 0.999f;
	bool ampAttack = false;
	float filterEnv = 0.0f, filterCoef = 0.99f, envDepth = 3.0f;
	float noteRatio = 1.0f;					// Last chromatic note (auto bass notes use it too)
	int32_t followCountdown[3] = {-1, -1, -1};
	float followVelocity = 0.0f;
	stmlib::Svf lp1, lp2;
	uint32_t quietCount = 0;
	bool idle = true;

	struct Config {
		float decaySpeed[partialCount] = {0.9995f, 0.9993f};	// Volume decay speed
		float rampInc = 0.2f;									// Initial wave steep ramp
		float baseFreq = 90.0f;									// Initial frequency of sine wave (60 Hz upstream, but upstream played partial 0 at 1.588x)
		float sineFreqScale[partialCount] = {1.0f, 1.588f};		// Relative frequencies of partials
		float sineInitLevel[partialCount] = {1.0f, 0.3f};		// Initial volume level (2nd partial adds tonal body)
		float sineSlowDownRate = 0.99995f;						// Rate of decrease of sine frequency

		// modulove: bass engine (MIDI only: drum map notes, chromatic channel, or auto notes after each kick)
		float engine = 0.0f;					// 0 toms, 1 bass
		float bassPitch = 41.2f;				// Hz at chromatic note 60 and for drum map / auto notes (E1)
		float bassWave = 0.25f;					// 0 saw ... 1 square
		float bassSub = 0.35f;					// Sub sine one octave down
		float bassCutoff = 160.0f;				// Filter base cutoff (Hz)
		float bassEnv = 3.0f;					// Filter envelope depth (octaves)
		float bassEnvDecay = 55.0f;				// Filter envelope decay (ms)
		float bassDecay = 130.0f;				// Amplitude decay to -60 dB (ms): psy rolling bass ~100 - 150
		float bassReso = 0.3f;					// Resonance 0 - 1
		float bassDrive = 0.35f;				// Saturation 0 - 1
		float bassFollow = 0.0f;				// Auto notes after each kick: 0 off, 1 offbeat (K-B-), 2 rolling (KBBB), 3 KBB-, 4 K-BB, 5 KB-B
		float bassFollowVel = 0.85f;			// Auto note velocity (x kick velocity)
		float bassAccent = 0.5f;				// Velocity -> filter envelope
		float bassLevel = 0.9f;					// Output level (toms have no level pot)
	} config;

};
