#include "audiostream.h"
#include <atomic>
#include "oslib/i18n.h"
#include <chrono>
#include <thread>

class NullAudioBackend : public AudioBackend
{
	using the_clock = std::chrono::high_resolution_clock;

public:
	NullAudioBackend()
		: AudioBackend("null", Tnop("No Audio")) {}

	bool init() override
	{
		last_time = the_clock::time_point();
		return true;
	}

	u32 push(const void* frame, u32 samples, bool wait) override
	{
		// MAPLECAST_DC_AUDIT bypasses wall-clock pacing — the audit needs
		// deterministic SH4 forward execution, and sleep_for(now-last_time)
		// is the prime suspect for the 448-cycle LIVE-vs-REDO drift.
		static const bool _bypassPacing = std::getenv("MAPLECAST_DC_AUDIT") != nullptr
			|| std::getenv("MAPLECAST_BYPASS_AUDIO_PACING") != nullptr;
		// A2 run-ahead paces itself at the tick end (this sample-based sleep is confused by the
		// per-tick rewind un-producing the hidden leg's audio -> 85fps). Skip it while armed.
		extern std::atomic<bool> mc_runaheadArmed;
		if (_bypassPacing || mc_runaheadArmed.load(std::memory_order_relaxed))
			return 1;

		if (wait && last_time.time_since_epoch() != the_clock::duration::zero())
		{
			auto fduration = std::chrono::nanoseconds(1'000'000'000LL * samples / 44100);
			auto duration = fduration - (the_clock::now() - last_time);
			if (duration > std::chrono::nanoseconds::zero())
				std::this_thread::sleep_for(duration);
			if (duration < -std::chrono::milliseconds(67))
				// if ~4 frames ahead, reset time (fast forward detection)
				last_time = the_clock::now();
			else
				last_time += fduration;
		}
		else {
			last_time = the_clock::now();
		}
		return 1;
	}

	bool initRecord(u32 sampling_freq) override
	{
		return true;
	}

	u32 record(void *buffer, u32 samples) override
	{
		memset(buffer, 0, samples * 2);
		return samples;
	}

	std::string getName() const override {
		return i18n::Ts(name);
	}

private:
	the_clock::time_point last_time;
};
static NullAudioBackend nullBackend;
