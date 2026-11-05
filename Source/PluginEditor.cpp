#include "PluginProcessor.h"
#include "PluginEditor.h"

WASAPIStreamerAudioProcessorEditor::WASAPIStreamerAudioProcessorEditor(WASAPIStreamerAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // Title
    addAndMakeVisible(titleLabel);
    titleLabel.setText("WASAPI Streamer", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);

    // Device Dropdown
    addAndMakeVisible(deviceSelector);
    deviceSelector.setTextWhenNoChoicesAvailable("No Windows devices found");
    deviceSelector.setTextWhenNothingSelected("Select Output Device...");

    deviceList = WasapiOutput::getAvailableDevices();
    std::wstring currentDevId = audioProcessor.wasapiOutput.getActiveDeviceId();
    int selectedItemIndex = 0;

    for (size_t i = 0; i < deviceList.size(); ++i)
    {
        deviceSelector.addItem(deviceList[i].name, (int)(i + 1));
        if (!currentDevId.empty() && deviceList[i].id == currentDevId)
            selectedItemIndex = (int)(i + 1);
    }

    if (selectedItemIndex > 0)
        deviceSelector.setSelectedId(selectedItemIndex, juce::dontSendNotification);

    // Buffer / Latency Slider (10ms to 3,000ms)
    addAndMakeVisible(bufferLabel);
    bufferLabel.setText("Buffer / Latency:", juce::dontSendNotification);
    bufferLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    bufferLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible(bufferSlider);
    bufferSlider.setRange(10.0, 3000.0, 5.0);
    bufferSlider.setValue((double)audioProcessor.wasapiOutput.getTargetLatencyMs(), juce::dontSendNotification);
    bufferSlider.setTextValueSuffix(" ms");
    bufferSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    bufferSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 20);

    bufferSlider.onValueChange = [this]()
    {
        audioProcessor.wasapiOutput.setTargetLatencyMs((int)bufferSlider.getValue());
    };

    // Output Volume Slider (-60 dB to +6 dB)
    addAndMakeVisible(volumeLabel);
    volumeLabel.setText("Output Volume:", juce::dontSendNotification);
    volumeLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    volumeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible(volumeSlider);
    volumeSlider.setRange(-60.0, 6.0, 0.5);

    float currentGain = audioProcessor.wasapiOutput.getVolumeGain();
    float currentDb = (currentGain <= 0.0001f) ? -60.0f : juce::Decibels::gainToDecibels(currentGain);
    volumeSlider.setValue(currentDb, juce::dontSendNotification);
    volumeSlider.setTextValueSuffix(" dB");
    volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 20);

    volumeSlider.onValueChange = [this]()
    {
        float dB = (float)volumeSlider.getValue();
        float linearGain = (dB <= -59.5f) ? 0.0f : juce::Decibels::decibelsToGain(dB);
        audioProcessor.wasapiOutput.setVolumeGain(linearGain);
    };

    // START / STOP Toggle Button
    addAndMakeVisible(toggleButton);
    toggleButton.onClick = [this]()
    {
        if (audioProcessor.wasapiOutput.isRunning())
        {
            audioProcessor.streamShouldBeActive.store(false);
            audioProcessor.wasapiOutput.stop();
        }
        else
        {
            int selectedIndex = deviceSelector.getSelectedId() - 1;
            if (selectedIndex >= 0 && selectedIndex < (int)deviceList.size())
            {
                audioProcessor.streamShouldBeActive.store(true);
                audioProcessor.wasapiOutput.start(deviceList[selectedIndex].id);
            }
        }
        updateButtonState();
    };

    deviceSelector.onChange = [this]()
    {
        int selectedIndex = deviceSelector.getSelectedId() - 1;
        if (selectedIndex >= 0 && selectedIndex < (int)deviceList.size())
        {
            audioProcessor.wasapiOutput.setSelectedDeviceId(deviceList[selectedIndex].id);
            if (audioProcessor.wasapiOutput.isRunning() || audioProcessor.streamShouldBeActive.load())
            {
                audioProcessor.streamShouldBeActive.store(true);
                audioProcessor.wasapiOutput.start(deviceList[selectedIndex].id);
            }
        }
        updateButtonState();
    };

    // Status / Tip Label
    addAndMakeVisible(statusLabel);
    statusLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    statusLabel.setJustificationType(juce::Justification::centred);

    updateButtonState();

    // Heartbeat timer: check state 5 times per second
    startTimerHz(5);

    setSize(400, 275);
}

WASAPIStreamerAudioProcessorEditor::~WASAPIStreamerAudioProcessorEditor()
{
    stopTimer();
}

void WASAPIStreamerAudioProcessorEditor::timerCallback()
{
    updateButtonState();
}

void WASAPIStreamerAudioProcessorEditor::updateButtonState()
{
    if (audioProcessor.wasapiOutput.isRunning())
    {
        if (toggleButton.getButtonText() != "STOP STREAM")
        {
            toggleButton.setButtonText("STOP STREAM");
            toggleButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffed4245));
            toggleButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }

        int selectedIndex = deviceSelector.getSelectedId() - 1;
        juce::String devName = (selectedIndex >= 0 && selectedIndex < (int)deviceList.size())
            ? deviceList[selectedIndex].name : "Windows Audio";
        statusLabel.setText("Active: Streaming to " + devName, juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff57f287));
    }
    else
    {
        if (toggleButton.getButtonText() != "START STREAM")
        {
            toggleButton.setButtonText("START STREAM");
            toggleButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff57f287));
            toggleButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
        }

        statusLabel.setText("Tip: Select an unused output (monitor or spare jack)\nto avoid hearing a delayed echo.", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    }
}

void WASAPIStreamerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff2f3136));
}

void WASAPIStreamerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(15);

    titleLabel.setBounds(area.removeFromTop(24));
    area.removeFromTop(10);

    deviceSelector.setBounds(area.removeFromTop(30));
    area.removeFromTop(12);

    auto bufferRow = area.removeFromTop(24);
    bufferLabel.setBounds(bufferRow.removeFromLeft(110));
    bufferSlider.setBounds(bufferRow);
    area.removeFromTop(8);

    auto volumeRow = area.removeFromTop(24);
    volumeLabel.setBounds(volumeRow.removeFromLeft(110));
    volumeSlider.setBounds(volumeRow);
    area.removeFromTop(14);

    toggleButton.setBounds(area.removeFromTop(32));
    area.removeFromTop(10);

    statusLabel.setBounds(area.removeFromTop(36));
}