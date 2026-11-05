#include "PluginProcessor.h"
#include "PluginEditor.h"

WASAPIStreamerAudioProcessor::WASAPIStreamerAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
#endif
{
}

WASAPIStreamerAudioProcessor::~WASAPIStreamerAudioProcessor()
{
    wasapiOutput.stop();
}

const juce::String WASAPIStreamerAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool WASAPIStreamerAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool WASAPIStreamerAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool WASAPIStreamerAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double WASAPIStreamerAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int WASAPIStreamerAudioProcessor::getNumPrograms()
{
    return 1;
}

int WASAPIStreamerAudioProcessor::getCurrentProgram()
{
    return 0;
}

void WASAPIStreamerAudioProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String WASAPIStreamerAudioProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void WASAPIStreamerAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void WASAPIStreamerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);

    // Auto-resume when returning from offline exports during an active session
    if (streamShouldBeActive.load() && !isNonRealtime())
    {
        auto devId = wasapiOutput.getActiveDeviceId();
        if (!devId.empty() && !wasapiOutput.isRunning())
            wasapiOutput.start(devId);
    }
}

void WASAPIStreamerAudioProcessor::releaseResources()
{
    wasapiOutput.stop();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool WASAPIStreamerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
#endif
}
#endif

void WASAPIStreamerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Only push audio during real-time playback (skip during offline bounces/exports)
    if (!isNonRealtime())
    {
        wasapiOutput.pushAudio(buffer, getSampleRate());
    }
}

bool WASAPIStreamerAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* WASAPIStreamerAudioProcessor::createEditor()
{
    return new WASAPIStreamerAudioProcessorEditor(*this);
}

void WASAPIStreamerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::XmlElement xml("WASAPIStreamerState");

    xml.setAttribute("deviceId", juce::String(wasapiOutput.getActiveDeviceId().c_str()));
    xml.setAttribute("latencyMs", wasapiOutput.getTargetLatencyMs());
    xml.setAttribute("volumeGain", (double)wasapiOutput.getVolumeGain());

    copyXmlToBinary(xml, destData);
}

void WASAPIStreamerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName("WASAPIStreamerState"))
    {
        juce::String devIdStr = xmlState->getStringAttribute("deviceId", "");
        int latMs = xmlState->getIntAttribute("latencyMs", 50);
        float gain = (float)xmlState->getDoubleAttribute("volumeGain", 1.0);

        wasapiOutput.setTargetLatencyMs(latMs);
        wasapiOutput.setVolumeGain(gain);
        wasapiOutput.setSelectedDeviceId(devIdStr.toWideCharPointer());

        // Always default to stopped when loading a saved project
        streamShouldBeActive.store(false);
        wasapiOutput.stop();
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WASAPIStreamerAudioProcessor();
}