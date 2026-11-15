#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>

#if defined(min)
#undef min
#endif
#if defined(max)
#undef max
#endif

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

class WasapiOutput
{
public:
    struct DeviceInfo
    {
        std::wstring id;
        juce::String name;
    };

    WasapiOutput()
        : fifoBuffer(2, 192000), abstractFifo(192000)
    {
        hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~WasapiOutput()
    {
        stop();
        if (hStopEvent)
        {
            CloseHandle(hStopEvent);
            hStopEvent = nullptr;
        }
    }

    void setVolumeGain(float gain)
    {
        volumeGain.store(gain, std::memory_order_relaxed);
    }

    float getVolumeGain() const
    {
        return volumeGain.load(std::memory_order_relaxed);
    }

    void setTargetLatencyMs(int ms)
    {
        targetLatencyMs.store(ms, std::memory_order_relaxed);
        needsRebuffer.store(true, std::memory_order_release);
    }

    int getTargetLatencyMs() const
    {
        return targetLatencyMs.load(std::memory_order_relaxed);
    }

    void setSelectedDeviceId(const std::wstring& id)
    {
        std::lock_guard<std::mutex> lock(deviceMutex);
        selectedDeviceId = id;
    }

    std::wstring getActiveDeviceId() const
    {
        std::lock_guard<std::mutex> lock(deviceMutex);
        return selectedDeviceId;
    }

    static std::vector<DeviceInfo> getAvailableDevices()
    {
        std::vector<DeviceInfo> devices;
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needsUninit = SUCCEEDED(hrCom);

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr) && enumerator)
        {
            ComPtr<IMMDeviceCollection> collection;
            hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
            if (SUCCEEDED(hr) && collection)
            {
                UINT count = 0;
                collection->GetCount(&count);

                for (UINT i = 0; i < count; ++i)
                {
                    ComPtr<IMMDevice> device;
                    if (SUCCEEDED(collection->Item(i, &device)) && device)
                    {
                        LPWSTR deviceId = nullptr;
                        device->GetId(&deviceId);

                        ComPtr<IPropertyStore> props;
                        juce::String friendlyName = "Unknown Device";
                        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props)
                        {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)))
                            {
                                if (varName.pwszVal)
                                    friendlyName = juce::String(varName.pwszVal);
                            }
                            PropVariantClear(&varName);
                        }

                        if (deviceId != nullptr)
                        {
                            devices.push_back({ std::wstring(deviceId), friendlyName });
                            CoTaskMemFree(deviceId);
                        }
                    }
                }
            }
        }

        if (needsUninit)
            CoUninitialize();

        return devices;
    }

    void pushAudio(const juce::AudioBuffer<float>& inputBuffer, double sampleRate)
    {
        // Wait-free guard so stop()/start() never races abstractFifo.reset()
        struct ProducerGuard
        {
            std::atomic<int>& count;
            explicit ProducerGuard(std::atomic<int>& c) : count(c) { count.fetch_add(1, std::memory_order_acq_rel); }
            ~ProducerGuard() { count.fetch_sub(1, std::memory_order_release); }
        } guard(activeProducers);

        if (!acceptingAudio.load(std::memory_order_acquire) || !isRunning())
            return;

        hostSampleRate.store(sampleRate, std::memory_order_relaxed);

        const int numSamples = inputBuffer.getNumSamples();
        const int inChannels = (std::min)(2, inputBuffer.getNumChannels());

        if (numSamples <= 0 || inChannels <= 0 || abstractFifo.getFreeSpace() < numSamples)
            return;

        int start1, size1, start2, size2;
        abstractFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
        {
            for (int ch = 0; ch < inChannels; ++ch)
                fifoBuffer.copyFrom(ch, start1, inputBuffer.getReadPointer(ch, 0), size1);
            if (inChannels == 1)
                fifoBuffer.copyFrom(1, start1, inputBuffer.getReadPointer(0, 0), size1);
        }

        if (size2 > 0)
        {
            for (int ch = 0; ch < inChannels; ++ch)
                fifoBuffer.copyFrom(ch, start2, inputBuffer.getReadPointer(ch, size1), size2);
            if (inChannels == 1)
                fifoBuffer.copyFrom(1, start2, inputBuffer.getReadPointer(0, size1), size2);
        }

        abstractFifo.finishedWrite(size1 + size2);
    }

    void start(const std::wstring& targetDeviceId)
    {
        stop();

        setSelectedDeviceId(targetDeviceId);
        if (hStopEvent)
            ResetEvent(hStopEvent);

        shouldStop.store(false, std::memory_order_relaxed);
        needsRebuffer.store(true, std::memory_order_relaxed);
        abstractFifo.reset();
        threadActive.store(true, std::memory_order_release);
        acceptingAudio.store(true, std::memory_order_release);
        workerThread = std::thread(&WasapiOutput::renderLoop, this);
    }

    void stop()
    {
        acceptingAudio.store(false, std::memory_order_release);
        shouldStop.store(true, std::memory_order_release);

        if (hStopEvent)
            SetEvent(hStopEvent);

        if (workerThread.joinable())
            workerThread.join();

        threadActive.store(false, std::memory_order_release);

        // Spin briefly until any in-flight DAW audio callback finishes pushAudio()
        while (activeProducers.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        abstractFifo.reset();
    }

    bool isRunning() const
    {
        return threadActive.load(std::memory_order_acquire);
    }

private:
    enum class SampleFormat { Float32, Int16, Int24, Int32, Unsupported };

    mutable std::mutex deviceMutex;
    std::wstring selectedDeviceId;

    HANDLE hStopEvent{ nullptr };
    std::thread workerThread;
    std::atomic<bool> shouldStop{ true };
    std::atomic<bool> threadActive{ false };
    std::atomic<bool> acceptingAudio{ false };
    std::atomic<int>  activeProducers{ 0 };
    std::atomic<bool> needsRebuffer{ true };
    std::atomic<double> hostSampleRate{ 44100.0 };
    std::atomic<int> targetLatencyMs{ 50 };
    std::atomic<float> volumeGain{ 1.0f };

    juce::AudioBuffer<float> fifoBuffer;
    juce::AbstractFifo abstractFifo;

    static SampleFormat detectSampleFormat(const WAVEFORMATEX* fmt)
    {
        if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32)
            return SampleFormat::Float32;

        if (fmt->wFormatTag == WAVE_FORMAT_PCM)
        {
            if (fmt->wBitsPerSample == 16) return SampleFormat::Int16;
            if (fmt->wBitsPerSample == 24) return SampleFormat::Int24;
            if (fmt->wBitsPerSample == 32) return SampleFormat::Int32;
        }
        else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && fmt->wBitsPerSample == 32)
                return SampleFormat::Float32;
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            {
                if (fmt->wBitsPerSample == 16) return SampleFormat::Int16;
                if (fmt->wBitsPerSample == 24) return SampleFormat::Int24;
                if (fmt->wBitsPerSample == 32) return SampleFormat::Int32;
            }
        }
        return SampleFormat::Unsupported;
    }

    void renderLoop()
    {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needsUninit = SUCCEEDED(hrCom);

        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioRenderClient> renderClient;
        HANDLE hAudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX* mixFormat = nullptr;

        auto cleanup = [&]()
            {
                if (audioClient) audioClient->Stop();
                if (mixFormat) { CoTaskMemFree(mixFormat); mixFormat = nullptr; }
                if (hAudioEvent) CloseHandle(hAudioEvent);
                if (hTask) AvRevertMmThreadCharacteristics(hTask);
                if (needsUninit) CoUninitialize();
                threadActive.store(false, std::memory_order_release);
            };

        if (!hAudioEvent || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
            cleanup();
            return;
        }

        std::wstring devId = getActiveDeviceId();
        HRESULT hr = devId.empty()
            ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)
            : enumerator->GetDevice(devId.c_str(), &device);

        if (FAILED(hr) || !device) { cleanup(); return; }

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient)))
        {
            cleanup();
            return;
        }

        if (FAILED(audioClient->GetMixFormat(&mixFormat)) || !mixFormat)
        {
            cleanup();
            return;
        }

        const SampleFormat sampleFmt = detectSampleFormat(mixFormat);
        if (sampleFmt == SampleFormat::Unsupported)
        {
            cleanup();
            return;
        }

        const DWORD wasapiSampleRate = mixFormat->nSamplesPerSec;
        const WORD wasapiChannels = mixFormat->nChannels;

        // Per Microsoft docs, shared-mode event-driven streams pass 0 for minimum engine buffer;
        // fall back to 30ms if a driver rejects 0.
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0,
            0,
            mixFormat,
            nullptr
        );

        if (FAILED(hr))
        {
            // Re-activate cleanly in case Initialize left client partially configured
            audioClient.Reset();
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient)))
            {
                cleanup();
                return;
            }
            hr = audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                300000, // 30ms fallback
                0,
                mixFormat,
                nullptr
            );
            if (FAILED(hr))
            {
                cleanup();
                return;
            }
        }

        if (FAILED(audioClient->SetEventHandle(hAudioEvent)) ||
            FAILED(audioClient->GetService(IID_PPV_ARGS(&renderClient))) || !renderClient)
        {
            cleanup();
            return;
        }

        UINT32 bufferFrameCount = 0;
        audioClient->GetBufferSize(&bufferFrameCount);

        // Pre-fill endpoint buffer with silence before Start() per WASAPI best practices
        {
            BYTE* pInitData = nullptr;
            if (SUCCEEDED(renderClient->GetBuffer(bufferFrameCount, &pInitData)))
            {
                std::memset(pInitData, 0, bufferFrameCount * mixFormat->nBlockAlign);
                renderClient->ReleaseBuffer(bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }

        if (FAILED(audioClient->Start()))
        {
            cleanup();
            return;
        }

        std::vector<float> pullBufferL(8192, 0.0f);
        std::vector<float> pullBufferR(8192, 0.0f);

        double resamplePhase = 0.0;
        float prevSampleL = 0.0f, currSampleL = 0.0f;
        float prevSampleR = 0.0f, currSampleR = 0.0f;

        HANDLE waitHandles[2] = { hStopEvent, hAudioEvent };
        DWORD numWaitHandles = hStopEvent ? 2 : 1;
        HANDLE* waitPtr = hStopEvent ? waitHandles : &hAudioEvent;

        while (!shouldStop.load(std::memory_order_relaxed))
        {
            DWORD waitResult = WaitForMultipleObjects(numWaitHandles, waitPtr, FALSE, 100);
            if (shouldStop.load(std::memory_order_relaxed)) break;
            if (hStopEvent && waitResult == WAIT_OBJECT_0) break;
            if (waitResult == WAIT_TIMEOUT) continue;
            if (waitResult != WAIT_OBJECT_0 + (hStopEvent ? 1 : 0)) break;

            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding)))
                break;

            UINT32 framesNeeded = bufferFrameCount - padding;
            if (framesNeeded == 0) continue;

            BYTE* pData = nullptr;
            if (FAILED(renderClient->GetBuffer(framesNeeded, &pData)))
                break;

            double currentHostRate = hostSampleRate.load(std::memory_order_relaxed);
            double baseRatio = currentHostRate / (double)wasapiSampleRate;
            int latMs = targetLatencyMs.load(std::memory_order_relaxed);
            int requestedTarget = (int)((currentHostRate * latMs) / 1000.0);

            // Ensure rebuffer target is never smaller than one WASAPI callback block
            int minSafeTarget = (int)std::ceil(framesNeeded * baseRatio) + 64;
            int targetBacklog = (std::max)(requestedTarget, minSafeTarget);

            // Pre-buffering
            if (needsRebuffer.load(std::memory_order_acquire))
            {
                if (abstractFifo.getNumReady() < targetBacklog)
                {
                    std::memset(pData, 0, framesNeeded * mixFormat->nBlockAlign);
                    renderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                    continue;
                }
                needsRebuffer.store(false, std::memory_order_release);
                resamplePhase = 0.0;
                prevSampleL = currSampleL = 0.0f;
                prevSampleR = currSampleR = 0.0f;
            }

            int availableInFifo = abstractFifo.getNumReady();

            // Safety cap if buffer grew massively (> 500ms past target)
            int maxAllowedBacklog = targetBacklog + (int)(currentHostRate * 0.50);
            if (availableInFifo > maxAllowedBacklog)
            {
                int excess = availableInFifo - targetBacklog;
                int s1, c1, s2, c2;
                abstractFifo.prepareToRead(excess, s1, c1, s2, c2);
                abstractFifo.finishedRead(c1 + c2);
                availableInFifo = abstractFifo.getNumReady();
            }

            // Clock Drift Lock: subtle +/- 0.1% adjustment
            double error = (double)(availableInFifo - targetBacklog);
            double driftNudge = std::clamp((error / (double)(std::max)(1, targetBacklog)) * 0.005, -0.001, 0.001);
            double ratio = baseRatio * (1.0 + driftNudge);

            int maxToPeek = (int)std::ceil(resamplePhase + (double)framesNeeded * ratio) + 2;
            int samplesToPeek = (std::min)(availableInFifo, maxToPeek);

            if (pullBufferL.size() < (size_t)samplesToPeek)
            {
                pullBufferL.resize(samplesToPeek);
                pullBufferR.resize(samplesToPeek);
            }

            // Peek without advancing FIFO read head yet
            if (samplesToPeek > 0)
            {
                int s1, c1, s2, c2;
                abstractFifo.prepareToRead(samplesToPeek, s1, c1, s2, c2);

                if (c1 > 0)
                {
                    std::copy_n(fifoBuffer.getReadPointer(0, s1), c1, pullBufferL.data());
                    std::copy_n(fifoBuffer.getReadPointer(1, s1), c1, pullBufferR.data());
                }
                if (c2 > 0)
                {
                    std::copy_n(fifoBuffer.getReadPointer(0, s2), c2, pullBufferL.data() + c1);
                    std::copy_n(fifoBuffer.getReadPointer(1, s2), c2, pullBufferR.data() + c1);
                }
            }

            float currentGain = volumeGain.load(std::memory_order_relaxed);
            int readIdx = 0;
            bool underflowed = false;

            auto writeSample = [&](UINT32 frameIdx, float sL, float sR)
                {
                    if (sampleFmt == SampleFormat::Float32)
                    {
                        float* out = reinterpret_cast<float*>(pData);
                        if (wasapiChannels >= 2)
                        {
                            out[frameIdx * wasapiChannels + 0] = sL;
                            out[frameIdx * wasapiChannels + 1] = sR;
                            for (WORD ch = 2; ch < wasapiChannels; ++ch)
                                out[frameIdx * wasapiChannels + ch] = 0.0f;
                        }
                        else
                        {
                            out[frameIdx] = (sL + sR) * 0.5f;
                        }
                    }
                    else if (sampleFmt == SampleFormat::Int16)
                    {
                        int16_t* out = reinterpret_cast<int16_t*>(pData);
                        int16_t shL = static_cast<int16_t>(std::lrint(sL * 32767.0f));
                        int16_t shR = static_cast<int16_t>(std::lrint(sR * 32767.0f));
                        if (wasapiChannels >= 2)
                        {
                            out[frameIdx * wasapiChannels + 0] = shL;
                            out[frameIdx * wasapiChannels + 1] = shR;
                            for (WORD ch = 2; ch < wasapiChannels; ++ch)
                                out[frameIdx * wasapiChannels + ch] = 0;
                        }
                        else
                        {
                            out[frameIdx] = static_cast<int16_t>((shL + shR) / 2);
                        }
                    }
                    else if (sampleFmt == SampleFormat::Int32)
                    {
                        int32_t* out = reinterpret_cast<int32_t*>(pData);
                        int32_t iL = static_cast<int32_t>(std::llround((double)sL * 2147483647.0));
                        int32_t iR = static_cast<int32_t>(std::llround((double)sR * 2147483647.0));
                        if (wasapiChannels >= 2)
                        {
                            out[frameIdx * wasapiChannels + 0] = iL;
                            out[frameIdx * wasapiChannels + 1] = iR;
                            for (WORD ch = 2; ch < wasapiChannels; ++ch)
                                out[frameIdx * wasapiChannels + ch] = 0;
                        }
                        else
                        {
                            out[frameIdx] = static_cast<int32_t>(((int64_t)iL + (int64_t)iR) / 2);
                        }
                    }
                    else if (sampleFmt == SampleFormat::Int24)
                    {
                        BYTE* out = pData + (size_t)frameIdx * wasapiChannels * 3;
                        int32_t iL = static_cast<int32_t>(std::llround((double)sL * 8388607.0));
                        int32_t iR = static_cast<int32_t>(std::llround((double)sR * 8388607.0));
                        auto put24 = [](BYTE* dst, int32_t v)
                            {
                                dst[0] = static_cast<BYTE>(v & 0xFF);
                                dst[1] = static_cast<BYTE>((v >> 8) & 0xFF);
                                dst[2] = static_cast<BYTE>((v >> 16) & 0xFF);
                            };
                        if (wasapiChannels >= 2)
                        {
                            put24(out, iL);
                            put24(out + 3, iR);
                            for (WORD ch = 2; ch < wasapiChannels; ++ch)
                                put24(out + (size_t)ch * 3, 0);
                        }
                        else
                        {
                            put24(out, (iL + iR) / 2);
                        }
                    }
                };

            for (UINT32 i = 0; i < framesNeeded; ++i)
            {
                while (resamplePhase >= 1.0)
                {
                    prevSampleL = currSampleL;
                    prevSampleR = currSampleR;

                    if (readIdx < samplesToPeek)
                    {
                        currSampleL = pullBufferL[readIdx];
                        currSampleR = pullBufferR[readIdx];
                        ++readIdx;
                    }
                    else
                    {
                        currSampleL = 0.0f;
                        currSampleR = 0.0f;
                        underflowed = true;
                    }

                    resamplePhase -= 1.0;
                }

                float frac = (float)resamplePhase;
                float sL = (prevSampleL + (currSampleL - prevSampleL) * frac) * currentGain;
                float sR = (prevSampleR + (currSampleR - prevSampleR) * frac) * currentGain;

                sL = std::clamp(sL, -1.0f, 1.0f);
                sR = std::clamp(sR, -1.0f, 1.0f);

                writeSample(i, sL, sR);
                resamplePhase += ratio;
            }

            // Advance FIFO by the EXACT number of samples the loop actually consumed
            if (readIdx > 0)
                abstractFifo.finishedRead(readIdx);

            if (underflowed)
                needsRebuffer.store(true, std::memory_order_release);

            renderClient->ReleaseBuffer(framesNeeded, 0);
        }

        cleanup();
    }
};