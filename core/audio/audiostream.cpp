#include "audiostream.h"
#include "cfg/option.h"
#include "emulator.h"
#include "network/maplecast_audio.h"

static void registerForEvents();

struct SoundFrame { s16 l; s16 r; };

static SoundFrame Buffer[SAMPLE_COUNT];
static u32 writePtr;  // next sample index

static AudioBackend *currentBackend;
std::vector<AudioBackend *> *AudioBackend::backends;

static bool audio_recording_started;
static bool eight_khz;

AudioBackend *AudioBackend::getBackend(const std::string& slug)
{
	if (backends == nullptr)
		return nullptr;
	if (slug == "auto")
	{
		// Prefer sdl2 if available and avoid the null driver
		AudioBackend *sdlBackend = nullptr;
		AudioBackend *autoBackend = nullptr;
		for (auto backend : *backends)
		{
			if (backend->slug == "sdl2")
				sdlBackend = backend;
			if (backend->slug != "null" && autoBackend == nullptr)
				autoBackend = backend;
		}
		if (sdlBackend != nullptr)
			autoBackend = sdlBackend;
		if (autoBackend == nullptr)
			autoBackend = backends->front();
		INFO_LOG(AUDIO, "Auto-selected audio backend \"%s\" (%s).", autoBackend->slug.c_str(), autoBackend->getName().c_str());

		return autoBackend;
	}
	for (auto backend : *backends)
	{
		if (backend->slug == slug)
			return backend;
	}
	WARN_LOG(AUDIO, "WARNING: Audio backend \"%s\" not found!", slug.c_str());
	return nullptr;
}

void WriteSample(s16 r, s16 l)
{
	// Stream raw audio to remote players (before local volume scaling)
	maplecast_audio::pushSample(l, r);

	Buffer[writePtr].r = r * config::AudioVolume.dbPower();
	Buffer[writePtr].l = l * config::AudioVolume.dbPower();

	if (++writePtr == SAMPLE_COUNT)
	{
		if (currentBackend != nullptr)
			currentBackend->push(Buffer, SAMPLE_COUNT, config::LimitFPS);
		writePtr = 0;
	}
}

// External audio push — see audiostream.h for rationale. On the MapleCast
// mirror client build there is no local SH4 / AICA running, so WriteSample()
// above is never called and `Buffer` / `writePtr` are dormant. We push the
// incoming PCM straight to whichever backend was selected by InitAudio(),
// and the backend handles its own locking.
//
// IMPORTANT: we pass wait=true. Here's why:
//   - The caller (maplecast_audio_client's recv thread) is a dedicated
//     thread with nothing else to do. Blocking until the backend has
//     room is fine — we're not on a hot path.
//   - With wait=false, the PulseAudio backend's push() loop *silently
//     drops* samples that don't fit in the current pa_stream_writable_size()
//     window. See audiobackend_pulseaudio.cpp:138-149 — the inner loop
//     breaks out instead of copying the remainder to an internal ring.
//     Dropping 3/4 of every ~11.6 ms PCM packet sounds like static.
//   - SDL2's backend with wait=true blocks on a read_wait event until
//     the ring has room. Also fine on a dedicated thread.
//   - If the backend is broken (sink disappeared, pipe closed), wait=true
//     could hang — but so could wait=false in a tight retry loop. Neither
//     is ideal, but for a mirror client watching a remote game, a stall
//     is acceptable.
u32 PushExternalAudio(const void *data, u32 frames)
{
	if (currentBackend == nullptr) return 0;
	return currentBackend->push(data, frames, true);
}

void InitAudio()
{
	registerForEvents();
	TermAudio();

	std::string slug = config::AudioBackend;
#ifdef MAPLECAST_HEADLESS_BUILD
	// Headless build: force the null audio backend. NullAudioBackend::push()
	// blocks the calling thread to wall-clock 44.1 kHz — the same pacing
	// that a real ALSA/Pulse/SDL backend provides on a desktop build.
	// This keeps the SH4 thread at real-time 60 fps. Without this, the
	// emulator runs as fast as CPU allows and AICA sample output drifts.
	slug = "null";
#endif
	currentBackend = AudioBackend::getBackend(slug);
	if (currentBackend == nullptr && slug != "auto")
	{
		slug = "auto";
		currentBackend = AudioBackend::getBackend(slug);
	}
	if (currentBackend != nullptr)
	{
		printf("[audiostream] selected audio backend: %s (%s)\n",
			currentBackend->slug.c_str(), currentBackend->getName().c_str());
		INFO_LOG(AUDIO, "Initializing audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->getName().c_str());
		if (!currentBackend->init())
		{
			currentBackend = nullptr;
			if (slug != "auto")
			{
				WARN_LOG(AUDIO, "Audio driver %s failed to initialize. Defaulting to 'auto'", slug.c_str());
				slug = "auto";
				currentBackend = AudioBackend::getBackend(slug);
				if (!currentBackend->init())
					currentBackend = nullptr;
			}
		}
	}

	if (currentBackend == nullptr)
	{
		WARN_LOG(AUDIO, "Running without audio!");
#ifdef MAPLECAST_HEADLESS_BUILD
		printf("[audiostream] WARNING: headless null backend failed to select — no wall-clock pacing, audio streaming will drift!\n");
#endif
		return;
	}
#ifdef MAPLECAST_HEADLESS_BUILD
	printf("[audiostream] using backend '%s' — wall-clock paced at 44.1 kHz\n", currentBackend->slug.c_str());
#endif

	if (audio_recording_started)
	{
		// Restart recording
		audio_recording_started = false;
		StartAudioRecording(eight_khz);
	}
}

void TermAudio()
{
	if (currentBackend == nullptr)
		return;

	// Save recording state before stopping
	bool rec_started = audio_recording_started;
	StopAudioRecording();
	audio_recording_started = rec_started;
	currentBackend->term();
	INFO_LOG(AUDIO, "Terminating audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->getName().c_str());
	currentBackend = nullptr;
}

void StartAudioRecording(bool eight_khz)
{
	::eight_khz = eight_khz;
	if (currentBackend != nullptr)
		audio_recording_started = currentBackend->initRecord(eight_khz ? 8000 : 11025);
	else
		// might be called between TermAudio/InitAudio
		audio_recording_started = true;
}

u32 RecordAudio(void *buffer, u32 samples)
{
	if (!audio_recording_started || currentBackend == nullptr)
		return 0;
	return currentBackend->record(buffer, samples);
}

void StopAudioRecording()
{
	// might be called between TermAudio/InitAudio
	if (audio_recording_started && currentBackend != nullptr)
		currentBackend->termRecord();
	audio_recording_started = false;
}

static void registerForEvents()
{
	static bool done;
	if (done)
		return;
	done = true;
	// Empty the audio buffer when loading a state or terminating the game
	const auto& callback = [](Event, void *) {
		writePtr = 0;
	};
	EventManager::listen(Event::Terminate, callback);
	EventManager::listen(Event::LoadState, callback);
}


