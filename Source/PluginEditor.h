#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "WasapiOutput.h"

class WASAPIStreamerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                           public juce::Timer
{
public:
    WASAPIStreamerAudioProcessorEditor(WASAPIStreamerAudioProcessor&);
    ~WASAPIStreamerAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    void updateButtonState();

    WASAPIStreamerAudioProcessor& audioProcessor;

    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::ComboBox deviceSelector;

    juce::Label bufferLabel;
    juce::Slider bufferSlider;

    juce::Label volumeLabel;
    juce::Slider volumeSlider;

    juce::TextButton toggleButton;

    std::vector<WasapiOutput::DeviceInfo> deviceList;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WASAPIStreamerAudioProcessorEditor)
};