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
    }

    ~WasapiOutput()
    {
        stop();
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
        if (!isRunning())
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
        shouldStop.store(false, std::memory_order_relaxed);
        needsRebuffer.store(true, std::memory_order_relaxed);
        abstractFifo.reset();
        threadActive.store(true, std::memory_order_release);
        workerThread = std::thread(&WasapiOutput::renderLoop, this);
    }

    void stop()
    {
        shouldStop.store(true, std::memory_order_release);
        if (workerThread.joinable())
            workerThread.join();
        threadActive.store(false, std::memory_order_release);
        abstractFifo.reset();
    }

    bool isRunning() const
    {
        return threadActive.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex deviceMutex;
    std::wstring selectedDeviceId;

    std::thread workerThread;
    std::atomic<bool> shouldStop{ true };
    std::atomic<bool> threadActive{ false };
    std::atomic<bool> needsRebuffer{ true };
    std::atomic<double> hostSampleRate{ 44100.0 };
    std::atomic<int> targetLatencyMs{ 50 };
    std::atomic<float> volumeGain{ 1.0f };

    juce::AudioBuffer<float> fifoBuffer;
    juce::AbstractFifo abstractFifo;

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
        HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        auto cleanup = [&]()
        {
            if (audioClient) audioClient->Stop();
            if (hEvent) CloseHandle(hEvent);
            if (hTask) AvRevertMmThreadCharacteristics(hTask);
            if (needsUninit) CoUninitialize();
            threadActive.store(false, std::memory_order_release);
        };

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
            cleanup();
            return;
        }

        std::wstring devId = getActiveDeviceId();
        HRESULT hr = S_OK;
        if (devId.empty())
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        else
            hr = enumerator->GetDevice(devId.c_str(), &device);

        if (FAILED(hr) || !device) { cleanup(); return; }

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient)))
        {
            cleanup();
            return;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        if (FAILED(audioClient->GetMixFormat(&mixFormat)) || !mixFormat)
        {
            cleanup();
            return;
        }

        const DWORD wasapiSampleRate = mixFormat->nSamplesPerSec;
        const WORD wasapiChannels = mixFormat->nChannels;

        bool isFloat = false;
        if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            isFloat = true;
        else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            auto* pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
            if (pExt->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                isFloat = true;
        }

        REFERENCE_TIME requestedDuration = 1000000; // 100ms
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            requestedDuration,
            0,
            mixFormat,
            nullptr
        );

        if (FAILED(hr))
        {
            CoTaskMemFree(mixFormat);
            cleanup();
            return;
        }

        audioClient->SetEventHandle(hEvent);
        if (FAILED(audioClient->GetService(IID_PPV_ARGS(&renderClient))) || !renderClient)
        {
            CoTaskMemFree(mixFormat);
            cleanup();
            return;
        }

        UINT32 bufferFrameCount = 0;
        audioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(audioClient->Start()))
        {
            CoTaskMemFree(mixFormat);
            cleanup();
            return;
        }

        std::vector<float> pullBufferL(8192, 0.0f);
        std::vector<float> pullBufferR(8192, 0.0f);

        // Continuous resampler state across callbacks
        double resamplePhase = 0.0;
        float prevSampleL = 0.0f, currSampleL = 0.0f;
        float prevSampleR = 0.0f, currSampleR = 0.0f;

        while (!shouldStop.load(std::memory_order_relaxed))
        {
            DWORD waitResult = WaitForSingleObject(hEvent, 100);
            if (shouldStop.load(std::memory_order_relaxed)) break;
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding)))
                break; // Device invalidated / disconnected

            UINT32 framesNeeded = bufferFrameCount - padding;
            if (framesNeeded == 0) continue;

            BYTE* pData = nullptr;
            if (FAILED(renderClient->GetBuffer(framesNeeded, &pData)))
                break;

            double currentHostRate = hostSampleRate.load(std::memory_order_relaxed);
            int latMs = targetLatencyMs.load(std::memory_order_relaxed);
            int targetBacklog = (int)((currentHostRate * latMs) / 1000.0);

            int availableInFifo = abstractFifo.getNumReady();

            // Clock Drift Lock: subtle +/- 0.1% adjustment to lock the two hardware clocks
            double baseRatio = currentHostRate / (double)wasapiSampleRate;
            double error = (double)(availableInFifo - targetBacklog);
            double driftNudge = std::clamp((error / (double)(std::max)(1, targetBacklog)) * 0.005, -0.001, 0.001);
            double ratio = baseRatio * (1.0 + driftNudge);

            // Exact number of input samples the sliding window will advance through
            int exactSamplesNeeded = (int)std::floor(resamplePhase + ((double)framesNeeded - 1.0) * ratio);
            if (exactSamplesNeeded < 0) exactSamplesNeeded = 0;

            // AUTO-RECOVERY: If the buffer ran dry, automatically re-buffer instead of crackling!
            if (availableInFifo < exactSamplesNeeded && !needsRebuffer.load(std::memory_order_relaxed))
            {
                needsRebuffer.store(true, std::memory_order_release);
            }

            // Pre-buffering
            if (needsRebuffer.load(std::memory_order_acquire))
            {
                if (abstractFifo.getNumReady() < targetBacklog)
                {
                    std::memset(pData, 0, framesNeeded * mixFormat->nBlockAlign);
                    renderClient->ReleaseBuffer(framesNeeded, 0);
                    continue;
                }
                needsRebuffer.store(false, std::memory_order_release);
                resamplePhase = 0.0;
            }

            // Refresh available count and recalculate exact read
            availableInFifo = abstractFifo.getNumReady();

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

            exactSamplesNeeded = (int)std::floor(resamplePhase + ((double)framesNeeded - 1.0) * ratio);
            if (exactSamplesNeeded < 0) exactSamplesNeeded = 0;

            int framesToRead = (std::min)(exactSamplesNeeded, availableInFifo);

            if (pullBufferL.size() < (size_t)framesToRead)
            {
                pullBufferL.resize(framesToRead);
                pullBufferR.resize(framesToRead);
            }

            if (framesToRead > 0)
            {
                int s1, c1, s2, c2;
                abstractFifo.prepareToRead(framesToRead, s1, c1, s2, c2);

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

                abstractFifo.finishedRead(c1 + c2);
            }

            float currentGain = volumeGain.load(std::memory_order_relaxed);
            float* floatOutput = reinterpret_cast<float*>(pData);
            short* shortOutput = reinterpret_cast<short*>(pData);

            int readIdx = 0;

            for (UINT32 i = 0; i < framesNeeded; ++i)
            {
                while (resamplePhase >= 1.0)
                {
                    prevSampleL = currSampleL;
                    prevSampleR = currSampleR;

                    if (readIdx < framesToRead)
                    {
                        currSampleL = pullBufferL[readIdx];
                        currSampleR = pullBufferR[readIdx];
                        ++readIdx;
                    }
                    else
                    {
                        currSampleL = 0.0f;
                        currSampleR = 0.0f;
                    }

                    resamplePhase -= 1.0;
                }

                float frac = (float)resamplePhase;
                float sL = (prevSampleL + (currSampleL - prevSampleL) * frac) * currentGain;
                float sR = (prevSampleR + (currSampleR - prevSampleR) * frac) * currentGain;

                sL = std::clamp(sL, -1.0f, 1.0f);
                sR = std::clamp(sR, -1.0f, 1.0f);

                if (isFloat)
                {
                    if (wasapiChannels >= 2)
                    {
                        floatOutput[i * wasapiChannels + 0] = sL;
                        floatOutput[i * wasapiChannels + 1] = sR;
                        for (WORD ch = 2; ch < wasapiChannels; ++ch)
                            floatOutput[i * wasapiChannels + ch] = 0.0f;
                    }
                    else
                    {
                        floatOutput[i] = (sL + sR) * 0.5f;
                    }
                }
                else
                {
                    short shL = static_cast<short>(sL * 32767.0f);
                    short shR = static_cast<short>(sR * 32767.0f);

                    if (wasapiChannels >= 2)
                    {
                        shortOutput[i * wasapiChannels + 0] = shL;
                        shortOutput[i * wasapiChannels + 1] = shR;
                        for (WORD ch = 2; ch < wasapiChannels; ++ch)
                            shortOutput[i * wasapiChannels + ch] = 0;
                    }
                    else
                    {
                        shortOutput[i] = static_cast<short>((shL + shR) / 2);
                    }
                }

                resamplePhase += ratio;
            }

            renderClient->ReleaseBuffer(framesNeeded, 0);
        }

        CoTaskMemFree(mixFormat);
        cleanup();
    }
};