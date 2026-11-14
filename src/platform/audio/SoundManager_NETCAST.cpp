#include "net/minecraft/src/SoundManager.h"
#include "platform/Log.h"

#if defined(NO_SOUND)


void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

SoundManager::SoundManager()
    : soundPoolSounds(), soundPoolStreaming(), soundPoolMusic(), soundVolume(0),
      options(nullptr), rand(), ticksBeforeMusic(0)
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings) { options = gamesettings; loaded = false; }
void SoundManager::onSoundOptionsChanged() {}
void SoundManager::closeMinecraft() { loaded = false; }
void SoundManager::addSound(const jstring &s, const std::string &file) { soundPoolSounds.addSound(s, file); }
void SoundManager::addStreaming(const jstring &s, const std::string &file) { soundPoolStreaming.addSound(s, file); }
void SoundManager::addMusic(const jstring &s, const std::string &file) { soundPoolMusic.addSound(s, file); }
void SoundManager::playRandomMusicIfReady() {}
bool SoundManager::playMusicFileNow(const std::string &) { return false; }
void SoundManager::setListenerPosition(EntityLiving *, float) {}
void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}
void SoundManager::playSound(const jstring &, float, float, float, float, float) {}
void SoundManager::playSoundFX(const jstring &, float, float) {}
void SoundManager::tryToSetLibraryAndCodecs()
{
    if (loaded) return;

    if (netcast::configInt("sound", 1) == 0)
    {
        netcast::log("sound disabled by netcast.cfg (sound=0)");
        return;
    }
    const int want = netcast::configInt("audio_backend", 0);
    bool ok = false;
    if (want == 0 || want == 1)
        ok = openDirect();
    if (!ok && (want == 0 || want == 2))
        ok = openMedia();
    if (!ok)
    {
        netcast::log("audio: no output could be opened; sound disabled");
        return;
    }
    s_deviceOpen = true;
    netcast::log("audio: output ready (%s, device rate %d Hz, mixer %d Hz)",
                 s_backend == Backend::Direct ? "LGNC_DIRECTAUDIO" : "LGNC_MEDIA", s_devRate, SAMPLE_RATE);
    loaded = true;
}

void SoundManager::onSoundOptionsChanged() {}
void SoundManager::closeMinecraft() { loaded = false; }
void SoundManager::addSound(const jstring &s, const std::string &file) { soundPoolSounds.addSound(s, file); }
void SoundManager::addStreaming(const jstring &s, const std::string &file) { soundPoolStreaming.addSound(s, file); }
void SoundManager::addMusic(const jstring &s, const std::string &file) { soundPoolMusic.addSound(s, file); }
void SoundManager::playRandomMusicIfReady() {}
bool SoundManager::playMusicFileNow(const std::string &) { return false; }
void SoundManager::setListenerPosition(EntityLiving *, float) {}
void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}
void SoundManager::playSound(const jstring &, float, float, float, float, float) {}
void SoundManager::playSoundFX(const jstring &, float, float) {}
void SoundManager::tryToSetLibraryAndCodecs() {}


#else

#include "net/minecraft/src/SoundManager.h"

#include <cmath>
#include <iostream>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#include <time.h>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <thread>
#include <chrono>

#include <lgncopenapi/lgnc.h>
#include "netcast/NetcastPlatform.h"

#include "pc/external/stb_vorbis.h"
#include "platform/Resources.h"
#include "platform/audio/VorbisAssetOpen.h"
#include "pc/audio/PcMusicStream.h"

#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/MathHelper.h"
#include "net/minecraft/src/SoundPoolEntry.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/audio/AudioSpatialization.h"
#include "ps2/audio/Ps2AdpcmStreamDecoder.h"

// ─── Internal audio mixer ────────────────────────────────────────────────────

static constexpr int SAMPLE_RATE   = 44100;
static constexpr int CHANNELS      = 2;      // stereo
static constexpr int MAX_SOUNDS    = 32;     // concurrent sound slots

enum class AudioBus
{
    Sound,
    Music,
    Streaming
};

// Decoded audio: interleaved stereo floats, or mono floats at their own rate (the PS2
// asset set is 22.05 kHz mono; the mixer resamples while mixing).
struct PcmData
{
    std::vector<float> samples;
    bool mono = false;
    int  rate = 44100;
};
using PcmPtr = std::shared_ptr<const PcmData>;

struct AudioChannel
{
    PcmPtr pcm;                // interleaved L/R samples, normalised [-1,1] (shared with the cache)
    double pos     = 0.0;      // position in frames (fractional: pitch is applied while mixing)
    bool   active  = false;
    bool   looping = false;
    float  volume  = 1.0f;
    float  pitch   = 1.0f;   // playback speed, applied by linear resampling in mixFrames
    AudioBus bus   = AudioBus::Sound;
};

// LGNC_DIRECTAUDIO takes S16 stereo PCM. Writes never block and fail while the
// decoder's ~1.5 s buffer is full; CheckBuffer() reports the bytes queued.
static std::thread        s_thread;
static std::atomic<bool>  s_threadRun(false);
static bool               s_deviceOpen = false;
static constexpr int      MIX_FRAMES = 1024;
static AudioChannel      s_channels[MAX_SOUNDS];
static std::mutex        s_mutex;
static float             s_soundVolume   = 1.0f;
static float             s_musicVolume   = 1.0f;
static int               s_musicChannel  = -1; // index into s_channels, or -1
static int               s_streamingChannel = -1;
static AudioListenerState s_listener;

// Decode an OGG file to float stereo PCM at SAMPLE_RATE (linear resample when the
// file uses another rate).
static std::vector<float> decodeOgg(const std::string &path, float pitch)
{
    (void)pitch;
    int channels = 0, sampleRate = 0;
    short *pcmShort = nullptr;
    int samples = platformDecodeVorbis(path, &channels, &sampleRate, &pcmShort);
    if (samples <= 0 || pcmShort == nullptr || channels < 1)
    {
        if (pcmShort) free(pcmShort);
        return {};
    }
    auto at = [&](int frame, int c) -> float {
        return pcmShort[(size_t)frame * channels + (channels > 1 ? c : 0)] / 32768.0f;
    };
    std::vector<float> out;
    if (sampleRate <= 0 || sampleRate == SAMPLE_RATE)
    {
        out.reserve((size_t)samples * 2);
        for (int i = 0; i < samples; i++)
        {
            out.push_back(at(i, 0));
            out.push_back(at(i, 1));
        }
    }
    else
    {
        const size_t dst = (size_t)((double)samples * SAMPLE_RATE / sampleRate);
        out.reserve(dst * 2);
        for (size_t i = 0; i < dst; ++i)
        {
            const double pos = (double)i * sampleRate / SAMPLE_RATE;
            int i0 = (int)pos;
            int i1 = i0 + 1 < samples ? i0 + 1 : i0;
            const float t = (float)(pos - i0);
            out.push_back(at(i0, 0) * (1 - t) + at(i1, 0) * t);
            out.push_back(at(i0, 1) * (1 - t) + at(i1, 1) * t);
        }
    }
    free(pcmShort);
    return out;
}

// Decode a WAV file (PCM 8/16-bit, mono/stereo) to float stereo at SAMPLE_RATE.
static std::vector<float> decodeWav(const std::string &path)
{
    unsigned int fileBytes = 0;
    unsigned char *fileData = PlatformResources::loadFile(path, &fileBytes);
    if (fileData == nullptr)
        return {};
    std::vector<float> out;
    auto rd16 = [&](size_t o) { return (unsigned)(fileData[o] | (fileData[o + 1] << 8)); };
    auto rd32 = [&](size_t o) { return (unsigned)rd16(o) | ((unsigned)rd16(o + 2) << 16); };
    if (fileBytes >= 44 && std::memcmp(fileData, "RIFF", 4) == 0 && std::memcmp(fileData + 8, "WAVE", 4) == 0)
    {
        unsigned fmtTag = 0, ch = 0, rate = 0, bits = 0;
        size_t dataOff = 0, dataLen = 0;
        for (size_t p = 12; p + 8 <= fileBytes;)
        {
            const unsigned len = rd32(p + 4);
            if (std::memcmp(fileData + p, "fmt ", 4) == 0 && p + 8 + 16 <= fileBytes)
            {
                fmtTag = rd16(p + 8);
                ch = rd16(p + 10);
                rate = rd32(p + 12);
                bits = rd16(p + 22);
            }
            else if (std::memcmp(fileData + p, "data", 4) == 0)
            {
                dataOff = p + 8;
                dataLen = std::min<size_t>(len, fileBytes - dataOff);
                break;
            }
            p += 8 + len + (len & 1);
        }
        if (fmtTag == 1 && (ch == 1 || ch == 2) && (bits == 8 || bits == 16) && rate > 0 && dataLen > 0)
        {
            const size_t bytesPerFrame = ch * (bits / 8);
            const size_t srcFrames = dataLen / bytesPerFrame;
            auto sample = [&](size_t frame, unsigned c) -> float {
                const unsigned char *q = fileData + dataOff + frame * bytesPerFrame + c * (bits / 8);
                return bits == 16 ? (int16_t)(q[0] | (q[1] << 8)) / 32768.0f : ((int)q[0] - 128) / 128.0f;
            };
            const size_t dstFrames = (size_t)((double)srcFrames * SAMPLE_RATE / rate);
            out.reserve(dstFrames * 2);
            for (size_t i = 0; i < dstFrames; ++i)
            {
                const double pos = (double)i * rate / SAMPLE_RATE;
                size_t i0 = (size_t)pos;
                size_t i1 = std::min(i0 + 1, srcFrames - 1);
                const float t = (float)(pos - (double)i0);
                const float l = sample(i0, 0) * (1 - t) + sample(i1, 0) * t;
                const float r = ch == 2 ? sample(i0, 1) * (1 - t) + sample(i1, 1) * t : l;
                out.push_back(l);
                out.push_back(r);
            }
        }
    }
    std::free(fileData);
    return out;
}

// ─── PS2 asset formats (assets.pak) ──────────────────────────────────────────
// *.pcm : raw signed 16-bit little-endian, 22.05 kHz, mono (music, records)
// *.adp : 16-byte header + 16-byte SPU ADPCM blocks (28 samples each), mono one-shots.
//         Files written with the "APCM" header carry channels/rate/length; planar
//         stereo (all left blocks, then all right blocks) is supported for those.
static int s_adpLogCount = 0;

static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int rateFromPitchWord(unsigned w)
{
    static const int rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    for (int r : rates)
    {
        const long expect = (long)((long long)r * 4096 / 48000);
        if (std::labs((long)w - expect) <= 2)
            return r;
    }
    return 0;
}

static PcmPtr decodeRawPcm(const std::string &path)
{
    unsigned int bytes = 0;
    unsigned char *data = PlatformResources::loadFile(path, &bytes);
    if (data == nullptr || bytes < 4)
    {
        std::free(data);
        return nullptr;
    }
    auto pd = std::make_shared<PcmData>();
    pd->mono = true;
    pd->rate = 22050;
    const size_t n = bytes / 2;
    pd->samples.resize(n);
    for (size_t i = 0; i < n; ++i)
        pd->samples[i] = (int16_t)(data[i * 2] | (data[i * 2 + 1] << 8)) / 32768.0f;
    std::free(data);
    return pd;
}

static PcmPtr decodeAdp(const std::string &path)
{
    unsigned int bytes = 0;
    unsigned char *data = PlatformResources::loadFile(path, &bytes);
    if (data == nullptr || bytes < 32)
    {
        std::free(data);
        return nullptr;
    }
    Ps2AdpcmStream::Header h;
    const bool apcm = Ps2AdpcmStream::parseHeader(data, bytes, h);
    int rate = 22050, channels = 1;
    unsigned wanted = 0xffffffffu; // frames to decode (APCM files know their length)
    if (apcm)
    {
        rate = h.sampleRate;
        channels = h.channels;
        wanted = h.samplesPerChannel;
    }
    else
    {
        for (int i = 0; i < 4; ++i)
        {
            const int r = rateFromPitchWord(le32(data + i * 4));
            if (r != 0) { rate = r; break; }
        }
    }
    if (s_adpLogCount < 3)
    {
        ++s_adpLogCount;
        netcast::log("audio: adp %s: %u bytes, header %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x -> %d Hz, %d ch%s",
                     path.c_str(), bytes, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
                     data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15],
                     rate, channels, apcm ? " (APCM)" : "");
    }

    const unsigned totalBlocks = (bytes - 16) / 16;
    auto pd = std::make_shared<PcmData>();
    pd->rate = rate;
    pd->mono = channels == 1;
    short blockPcm[28];
    if (channels == 2)
    {
        const unsigned perCh = (wanted + 27) / 28;
        if (perCh == 0 || perCh * 2 > totalBlocks)
        {
            std::free(data);
            return nullptr;
        }
        Ps2AdpcmStream::ChannelState ls, rs;
        short lp[28], rp[28];
        for (unsigned b = 0; b < perCh; ++b)
        {
            if (!Ps2AdpcmStream::decodeBlock(data + 16 + b * 16, ls, lp) ||
                !Ps2AdpcmStream::decodeBlock(data + 16 + (perCh + b) * 16, rs, rp))
                break;
            for (int i = 0; i < 28 && pd->samples.size() / 2 < wanted; ++i)
            {
                pd->samples.push_back(lp[i] / 32768.0f);
                pd->samples.push_back(rp[i] / 32768.0f);
            }
        }
    }
    else
    {
        Ps2AdpcmStream::ChannelState st;
        for (unsigned b = 0; b < totalBlocks && pd->samples.size() < wanted; ++b)
        {
            const unsigned char *blk = data + 16 + b * 16;
            if (!Ps2AdpcmStream::decodeBlock(blk, st, blockPcm))
                break;
            for (int i = 0; i < 28 && pd->samples.size() < wanted; ++i)
                pd->samples.push_back(blockPcm[i] / 32768.0f);
            if (!apcm && (blk[1] & 0x01))
                break; // SPU "end" flag
        }
    }
    std::free(data);
    if (pd->samples.size() < 4)
        return nullptr;
    return pd;
}

// Decoded sounds are cached: decoding an OGG on the game thread for every footstep
// or block break was a large part of the frame time on this CPU.
static std::unordered_map<std::string, PcmPtr> s_cache;
static size_t s_cacheFloats = 0;
static std::mutex s_cacheMutex;
static constexpr size_t CACHE_MAX_FLOATS   = 8u * 1024u * 1024u; // 32 MB
static constexpr size_t CACHE_ENTRY_MAX    = 2u * 1024u * 1024u; // longer sounds (records) are not cached
static int s_soundLogCount = 0;

static PcmPtr loadSound(const std::string &path, float pitch = 1.0f)
{
    (void)pitch;
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        auto it = s_cache.find(path);
        if (it != s_cache.end())
            return it->second;
    }
    PcmPtr ptr;
    if (audioPathHasExtension(path, ".adp"))
        ptr = decodeAdp(path);
    else if (audioPathHasExtension(path, ".pcm"))
        ptr = decodeRawPcm(path);
    else
    {
        std::vector<float> data;
        if (audioPathHasExtension(path, ".ogg"))
            data = decodeOgg(path, 1.0f);
        else if (audioPathHasExtension(path, ".wav"))
            data = decodeWav(path);
        if (!data.empty())
        {
            auto pd = std::make_shared<PcmData>();
            pd->samples = std::move(data);
            ptr = pd;
        }
    }
    if (!ptr || ptr->samples.empty())
    {
        if (s_soundLogCount < 40)
        {
            ++s_soundLogCount;
            netcast::log("sound: could not decode %s", path.c_str());
        }
        return nullptr;
    }
    if (ptr->samples.size() <= CACHE_ENTRY_MAX)
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        if (s_cacheFloats + ptr->samples.size() > CACHE_MAX_FLOATS)
        {
            s_cache.clear(); // simple: start over when full
            s_cacheFloats = 0;
        }
        s_cache[path] = ptr;
        s_cacheFloats += ptr->samples.size();
    }
    return ptr;
}

// Play decoded PCM on the next free channel; returns channel index or -1
static int playPCM(PcmPtr pcm, float volume, bool looping, AudioBus bus, float pitch = 1.0f)
{
    if (!pcm || pcm->samples.empty()) return -1;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (int i = 0; i < MAX_SOUNDS; i++)
    {
        if (!s_channels[i].active)
        {
            s_channels[i].pcm     = std::move(pcm);
            s_channels[i].pos     = 0.0;
            s_channels[i].active  = true;
            s_channels[i].looping = looping;
            s_channels[i].volume  = volume;
            s_channels[i].pitch   = pitch;
            s_channels[i].bus     = bus;
            return i;
        }
    }
    return -1;
}

static void stopChannel(int idx, AudioBus expectedBus)
{
    if (idx < 0 || idx >= MAX_SOUNDS) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_channels[idx].active || s_channels[idx].bus != expectedBus)
        return;
    s_channels[idx].active = false;
    s_channels[idx].pcm.reset();
    s_channels[idx].pos = 0.0;
}

// Long tracks (*.pcm, 8-15 MB) are read on a helper thread so starting music or a
// record never stalls the game thread on the USB drive.
static std::atomic<unsigned> s_loadGen(0);
static std::atomic<bool>     s_asyncBusy(false);
static void startAsync(const std::string &path, float volume, AudioBus bus, float pitch, int *slot)
{
    if (s_asyncBusy.exchange(true))
        return;
    const unsigned gen = s_loadGen.load();
    std::thread([=]() {
        PcmPtr pcm = loadSound(path);
        if (pcm && gen == s_loadGen.load())
        {
            const int ch = playPCM(pcm, volume, false, bus, pitch);
            if (slot) *slot = ch;
            netcast::log("audio: started %s (%.0f s)", path.c_str(),
                         (double)(pcm->mono ? pcm->samples.size() : pcm->samples.size() / 2) / (pcm->rate > 0 ? pcm->rate : 44100));
        }
        s_asyncBusy.store(false);
    }).detach();
}

// Mix `frames` stereo frames of all active channels into `out` (float, zeroed here).
static void mixFrames(float *out, int frames)
{
    std::memset(out, 0, sizeof(float) * (size_t)frames * CHANNELS);

    float musicVolume = 1.0f;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        musicVolume = s_musicVolume;
        for (auto &ch : s_channels)
        {
            if (!ch.active) continue;
            float busVolume = ch.bus == AudioBus::Music ? s_musicVolume : s_soundVolume;
            float vol = ch.volume * busVolume;

            const PcmData &pd = *ch.pcm;
            const std::vector<float> &pcm = pd.samples;
            const bool mono = pd.mono;
            const size_t total = mono ? pcm.size() : pcm.size() / 2; // frames
            const double rateScale = pd.rate > 0 ? (double)pd.rate / SAMPLE_RATE : 1.0;
            const double step = (ch.pitch < 0.25f ? 0.25 : (ch.pitch > 4.0f ? 4.0 : (double)ch.pitch)) * rateScale;
            for (int f = 0; f < frames; f++)
            {
                if (ch.pos >= (double)(total - 1) || total < 2)
                {
                    if (ch.looping && total >= 2)
                        ch.pos = 0.0;
                    else
                    {
                        ch.active = false;
                        ch.pcm.reset();
                        break;
                    }
                }
                const size_t i0 = (size_t)ch.pos;
                const float t = (float)(ch.pos - (double)i0);
                const size_t i1 = i0 + 1 < total ? i0 + 1 : i0;
                float l, r;
                if (mono)
                {
                    l = r = pcm[i0] * (1.0f - t) + pcm[i1] * t;
                }
                else
                {
                    l = pcm[i0 * 2] * (1.0f - t) + pcm[i1 * 2] * t;
                    r = pcm[i0 * 2 + 1] * (1.0f - t) + pcm[i1 * 2 + 1] * t;
                }
                out[f * 2 + 0] += l * vol;
                out[f * 2 + 1] += r * vol;
                ch.pos += step;
            }
        }
    }

    PcMusicStream::mix(out, frames, musicVolume);
}

// ─── Output device ───────────────────────────────────────────────────────────
// The mixer always runs at SAMPLE_RATE (44.1 kHz float stereo). The TV is opened in
// this order (netcast.cfg audio_backend: 0 auto, 1 direct only, 2 media only):
//   1. LGNC_DIRECTAUDIO  (what SDL's nc4 driver uses for a registered app): 44.1k,
//      48k, then 22.05k S16 stereo; fed by a thread that tops the queue up.
//   2. LGNC_MEDIA channel 1 (PCM through the TV's GStreamer pipeline): fed from the
//      NEED_DATA callback in 4 KiB chunks.
// A device rate other than 44.1k is bridged by a linear resampler.

enum class Backend { None, Direct, Media };
static Backend            s_backend = Backend::None;
static int                s_devRate = SAMPLE_RATE;
static std::atomic<unsigned> s_mediaCallbacks(0);
static std::atomic<unsigned> s_playFail(0);
static std::atomic<unsigned long> s_framesOut(0);

// Produce `frames` device-rate S16 stereo frames.
static void renderDevice(short *out, int frames)
{
    static std::vector<float> mix, scratch;
    int srcFrames = frames;
    if (s_devRate != SAMPLE_RATE)
        srcFrames = (int)((long long)frames * SAMPLE_RATE / s_devRate) + 2;
    if ((int)mix.size() < srcFrames * CHANNELS)
        mix.resize((size_t)srcFrames * CHANNELS);
    mixFrames(mix.data(), srcFrames);
    for (int i = 0; i < frames; ++i)
    {
        float l, r;
        if (s_devRate == SAMPLE_RATE)
        {
            l = mix[(size_t)i * 2];
            r = mix[(size_t)i * 2 + 1];
        }
        else
        {
            const double pos = (double)i * (srcFrames - 2) / frames;
            const int i0 = (int)pos;
            const float t = (float)(pos - i0);
            l = mix[(size_t)i0 * 2] * (1 - t) + mix[(size_t)(i0 + 1) * 2] * t;
            r = mix[(size_t)i0 * 2 + 1] * (1 - t) + mix[(size_t)(i0 + 1) * 2 + 1] * t;
        }
        l = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        r = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
        out[i * 2] = (short)(l * 32767.0f);
        out[i * 2 + 1] = (short)(r * 32767.0f);
    }
    s_framesOut += (unsigned long)frames;
}

static void noteAudioCpu()
{
    struct timespec cts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cts);
    netcast::g_statAudioCpu = (double)cts.tv_sec + (double)cts.tv_nsec * 1e-9;
}

// LGNC_DIRECTAUDIO feeder thread.
static void audioThread()
{
    const int frames = MIX_FRAMES;
    std::vector<short> pcm((size_t)frames * CHANNELS);
    const int bufBytes = frames * CHANNELS * (int)sizeof(short);
    // Every call is a round trip to the TV's audio service. It buffers ~1.5 s, so
    // keep ~6 chunks queued and look again every 20 ms.
    const auto period = std::chrono::milliseconds(20);
    unsigned reported = 0;

    while (s_threadRun.load())
    {
        noteAudioCpu();
        int queued = 0;
        const int cb = LGNC_DIRECTAUDIO_CheckBuffer(&queued);
        if (cb == LGNC_OK && queued >= bufBytes * 6)
        {
            std::this_thread::sleep_for(period);
            continue;
        }
        renderDevice(pcm.data(), frames);
        bool ok = false;
        for (int tries = 0; tries < 60 && s_threadRun.load(); ++tries)
        {
            if (LGNC_DIRECTAUDIO_Play(pcm.data(), (unsigned)bufBytes) == LGNC_OK)
            {
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!ok && reported < 5)
        {
            ++reported;
            netcast::log("audio: LGNC_DIRECTAUDIO_Play kept failing (queued=%d, check=%d)", queued, cb);
        }
    }
}

// LGNC_MEDIA callback (GLib main loop thread of the TV's media service).
static void mediaFeed()
{
    static short pcm[1024 * CHANNELS];
    renderDevice(pcm, 1024);
    if (LGNC_MEDIA_PlayBuffer(LGNC_CHANNEL_PCM_1, pcm, sizeof(pcm)) != LGNC_OK)
        ++s_playFail;
}

static void mediaCallback(LGNC_CHANNEL channel, LGNC_CB_MSG msg)
{
    const unsigned n = ++s_mediaCallbacks;
    if (n <= 6)
        netcast::log("audio: media callback #%u channel=%d msg=0x%x", n, (int)channel, (unsigned)msg);
    if (channel == LGNC_CHANNEL_PCM_1 && (msg == LGNC_CB_MSG_NEED_DATA || msg == LGNC_CB_MSG_END))
    {
        noteAudioCpu();
        mediaFeed();
    }
}

static int directRateEnum(int hz) { return (int)LGNC_ADEC_SAMPLING_FREQ_OF(hz); }

static bool openDirect()
{
    static const int rates[] = { 44100, 48000, 22050 };
    for (int hz : rates)
    {
        LGNC_ADEC_DATA_INFO_T info;
        std::memset(&info, 0, sizeof(info));
        info.codec = LGNC_ADEC_FMT_PCM;
        info.AChannel = LGNC_ADEC_CH_INDEX_MAIN;
        info.samplingFreq = (LGNC_ADEC_SAMPLING_FREQ_T)directRateEnum(hz);
        info.numberOfChannel = CHANNELS;
        info.bitPerSample = 16;
        const int r = LGNC_DIRECTAUDIO_Open(&info);
        netcast::log("audio: LGNC_DIRECTAUDIO_Open(%d Hz, stereo, 16 bit) = %d", hz, r);
        if (r == LGNC_OK)
        {
            s_devRate = hz;
            s_backend = Backend::Direct;
            s_threadRun = true;
            s_thread = std::thread(audioThread);
            return true;
        }
    }
    return false;
}

static bool openMedia()
{
    static const struct { int hz; LGNC_MEDIA_PCM_RATE_T e; } rates[] = {
        { 44100, LGNC_MEDIA_PCM_RATE_44100 }, { 48000, LGNC_MEDIA_PCM_RATE_48000 } };
    for (const auto &rt : rates)
    {
        LGNC_MEDIA_PCM_INFO_T info;
        std::memset(&info, 0, sizeof(info));
        info.format = 1;            // signed 16-bit little endian
        info.rate = rt.e;
        info.stereo = 1;
        info.minPercent = 30;
        s_devRate = rt.hz;
        const int r = LGNC_MEDIA_Initialize(mediaCallback, &info);
        netcast::log("audio: LGNC_MEDIA_Initialize(%d Hz, stereo, S16) = %d", rt.hz, r);
        if (r == LGNC_OK)
        {
            s_backend = Backend::Media;
            // Prime the pipeline; further chunks are requested by NEED_DATA.
            for (int i = 0; i < 6; ++i)
                mediaFeed();
            return true;
        }
    }
    return false;
}

static void closeDevice()
{
    if (s_threadRun.exchange(false) && s_thread.joinable())
        s_thread.join();
    if (s_backend == Backend::Direct)
        LGNC_DIRECTAUDIO_Close();
    else if (s_backend == Backend::Media)
    {
        LGNC_MEDIA_Stop(LGNC_CHANNEL_PCM_1);
        LGNC_MEDIA_Finalize();
    }
    s_backend = Backend::None;
}

// ─── SoundManager static members ─────────────────────────────────────────────

void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

// ─── SoundManager implementation ─────────────────────────────────────────────

SoundManager::SoundManager() :
    soundPoolSounds(),
    soundPoolStreaming(),
    soundPoolMusic(),
    soundVolume(0),
    options(nullptr),
    rand(),
    ticksBeforeMusic(rand.nextInt(12000))
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings)
{
    soundPoolStreaming.motionX = false;
    options = gamesettings;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_soundVolume = gamesettings ? gamesettings->soundVolume : 1.0f;
        s_musicVolume = gamesettings ? gamesettings->musicVolume : 1.0f;
    }

    netcast::log("audio: loadSoundSettings sound=%.2f music=%.2f", gamesettings ? gamesettings->soundVolume : -1.0f,
                 gamesettings ? gamesettings->musicVolume : -1.0f);
    if (!loaded && (gamesettings == nullptr || gamesettings->soundVolume != 0.0f || gamesettings->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();
}

void SoundManager::tryToSetLibraryAndCodecs()
{
    if (loaded) return;

    if (netcast::configInt("sound", 1) == 0)
    {
        netcast::log("sound disabled by netcast.cfg (sound=0)");
        return;
    }
    LGNC_ADEC_DATA_INFO_T info;
    std::memset(&info, 0, sizeof(info));
    info.codec = LGNC_ADEC_FMT_PCM;
    info.AChannel = LGNC_ADEC_CH_INDEX_MAIN;
    info.samplingFreq = LGNC_ADEC_SAMPLING_FREQ_44_1KHZ;
    info.numberOfChannel = CHANNELS;
    info.bitPerSample = 16;
    if (LGNC_DIRECTAUDIO_Open(&info) != LGNC_OK)
    {
        netcast::log("LGNC_DIRECTAUDIO_Open failed; sound disabled");
        return;
    }
    s_deviceOpen = true;
    netcast::log("audio: LGNC_DIRECTAUDIO opened (44.1 kHz, 16-bit stereo)");
    s_threadRun = true;
    s_thread = std::thread(audioThread);
    loaded = true;
}

void SoundManager::onSoundOptionsChanged()
{
    if (!loaded && options && (options->soundVolume != 0.0f || options->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();

    if (!loaded) return;

    float musicVolume = options ? options->musicVolume : 1.0f;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_soundVolume = options ? options->soundVolume : 1.0f;
        s_musicVolume = musicVolume;
    }

    // Match Java: setting Music to zero stops only ambient background music.
    // Records/streaming remain on the Sound bus.
    if (musicVolume == 0.0f)
    {
        ++s_loadGen;
        PcMusicStream::stop();
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
    }
}

void SoundManager::closeMinecraft()
{
    ++s_loadGen;
    PcMusicStream::stop();
    if (s_deviceOpen)
    {
        closeDevice();
        s_deviceOpen = false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &ch : s_channels)
        {
            ch.active = false;
            ch.looping = false;
            ch.pos = 0.0;
            ch.pcm.reset();
        }
        s_musicChannel = -1;
        s_streamingChannel = -1;
    }

    loaded = false;
}

void SoundManager::addSound(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".wav") ||
        audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm"))
        soundPoolSounds.addSound(s, file);
}

void SoundManager::addStreaming(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".wav") ||
        audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm"))
        soundPoolStreaming.addSound(s, file);
}

void SoundManager::addMusic(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".wav") ||
        audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm"))
        soundPoolMusic.addSound(s, file);
}

bool SoundManager::playMusicFileNow(const std::string &file)
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return false;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_streamingChannel >= 0 && s_channels[s_streamingChannel].active
            && s_channels[s_streamingChannel].bus == AudioBus::Streaming)
            return false;
    }

    if (PcMusicStream::active())
        return false;

    if (audioPathHasExtension(file, ".pcm"))
    {
        if (s_asyncBusy.load())
            return false;
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        startAsync(file, 1.0f, AudioBus::Music, 1.0f, &s_musicChannel);
    }
    else if (audioPathHasExtension(file, ".ogg"))
    {
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        if (!PcMusicStream::start(file))
            return false;
    }
    else
    {
        PcmPtr pcm = loadSound(file);
        if (!pcm)
            return false;
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        const int channel = playPCM(pcm, 1.0f, false, AudioBus::Music);
        if (channel < 0)
            return false;
        s_musicChannel = channel;
    }

    ticksBeforeMusic = rand.nextInt(12000) + 12000;
    return true;
}

void SoundManager::playRandomMusicIfReady()
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return;

    bool channelMusicPlaying = false;
    bool streamingPlaying = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        channelMusicPlaying = s_musicChannel >= 0 && s_channels[s_musicChannel].active
            && s_channels[s_musicChannel].bus == AudioBus::Music;
        streamingPlaying = s_streamingChannel >= 0 && s_channels[s_streamingChannel].active
            && s_channels[s_streamingChannel].bus == AudioBus::Streaming;
    }

    if (PcMusicStream::active() || channelMusicPlaying || streamingPlaying || s_asyncBusy.load())
        return;

    if (ticksBeforeMusic > 0)
    {
        ticksBeforeMusic--;
        return;
    }

    SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
    ticksBeforeMusic = rand.nextInt(12000) + 12000;
    if (entry == nullptr)
        return;

    if (audioPathHasExtension(entry->soundUrl, ".pcm"))
    {
        s_musicChannel = -1;
        startAsync(entry->soundUrl, 1.0f, AudioBus::Music, 1.0f, &s_musicChannel);
        return;
    }
    if (audioPathHasExtension(entry->soundUrl, ".ogg"))
    {
        PcMusicStream::start(entry->soundUrl);
        s_musicChannel = -1;
        return;
    }

    s_musicChannel = playPCM(loadSound(entry->soundUrl), 1.0f, false, AudioBus::Music);
}

void SoundManager::setListenerPosition(EntityLiving *entityliving, float f)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;
    updateAudioListener(s_listener, entityliving, f);
}

void SoundManager::playStreaming(const jstring &s, float f, float f1, float f2, float f3, float f4)
{
    if (!loaded || !options || (options->soundVolume == 0.0f && !s.empty())) return;

    // Java uses a distinct source named "streaming".
    if (s_streamingChannel >= 0)
        stopChannel(s_streamingChannel, AudioBus::Streaming);
    s_streamingChannel = -1;

    if (s.empty()) return;

    SoundPoolEntry *entry = soundPoolStreaming.getRandomSoundFromSoundPool(s);
    if (entry != nullptr && f3 > 0.0f)
    {
        const float attenuation = audioStreamingAttenuation(s_listener, f, f1, f2);
        if (attenuation <= 0.0f)
            return;

        // A record/stream temporarily replaces ambient background music.
        PcMusicStream::stop();
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        ticksBeforeMusic = rand.nextInt(12000) + 12000;

        ++s_loadGen; // cancel a music track that is still loading
        if (audioPathHasExtension(entry->soundUrl, ".pcm"))
        {
            s_asyncBusy.store(false);
            startAsync(entry->soundUrl, 0.5f * attenuation, AudioBus::Streaming, f4, &s_streamingChannel);
        }
        else
            s_streamingChannel = playPCM(loadSound(entry->soundUrl, f4), 0.5f * attenuation, false, AudioBus::Streaming, f4);
    }
}

void SoundManager::playSound(const jstring &s, float f, float f1, float f2, float f3, float f4)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry == nullptr && s_soundLogCount < 40)
    {
        ++s_soundLogCount;
        netcast::log("sound: no sound registered for '%s' (are the sound files in resources/?)", s.c_str());
    }
    if (entry != nullptr && f3 > 0.0f)
    {
        const float attenuation = audioSpatialAttenuation(s_listener, f, f1, f2, f3);
        if (attenuation <= 0.0f)
            return;

        const float vol = std::min(f3, 1.0f) * attenuation;
        if (s_soundLogCount < 40)
        {
            ++s_soundLogCount;
            netcast::log("sound: %s vol=%.2f pitch=%.2f", entry->soundUrl.c_str(), vol, f4);
        }
        playPCM(loadSound(entry->soundUrl, f4), vol, false, AudioBus::Sound, f4);
    }
}

void SoundManager::playSoundFX(const jstring &s, float f, float f1)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry != nullptr)
    {
        float vol = (f > 1.0f ? 1.0f : f) * 0.25f;
        playPCM(loadSound(entry->soundUrl, f1), vol, false, AudioBus::Sound, f1);
    }
}

#endif
