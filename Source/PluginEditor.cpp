#include "PluginProcessor.h"
#include "PluginEditor.h"

WASAPIStreamerAudioProcessorEditor::WASAPIStreamerAudioProcessorEditor(WASAPIStreamerAudioProcessor& p)
    : AudioProcessorEditor(&p),
    audioProcessor(p),
    githubLink("GitHub Repository", juce::URL("https://github.com/stephmouse/WASAPI-Streamer"))
{
    // Header: Version (left), Title (center), About button (right)
    addAndMakeVisible(versionLabel);
    versionLabel.setText("v" JucePlugin_VersionString, juce::dontSendNotification);
    versionLabel.setFont(juce::FontOptions(11.5f, juce::Font::plain));
    versionLabel.setJustificationType(juce::Justification::centredLeft);
    versionLabel.setColour(juce::Label::textColourId, juce::Colours::grey);

    addAndMakeVisible(titleLabel);
    titleLabel.setText(JucePlugin_Name, juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);

    addAndMakeVisible(aboutButton);
    aboutButton.setButtonText("About");
    aboutButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff4f545c));
    aboutButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    aboutButton.onClick = [this]()
        {
            setAboutVisible(!showingAbout);
        };

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

    // About View (hidden initially)
    addChildComponent(aboutInfoLabel);
    juce::String aboutText;
    aboutText << JucePlugin_Name << " v" << JucePlugin_VersionString << "\n"
        << "By " << JucePlugin_Manufacturer << "\n\n"
        << "Routes ASIO DAW audio to Windows WASAPI Shared Mode\n"
        << "for Discord, OBS, and desktop capture.\n\n"
        << "License: AGPL-3.0 (Built with JUCE)\n"
        << "Developed with AI assistance.";
    aboutInfoLabel.setText(aboutText, juce::dontSendNotification);
    aboutInfoLabel.setFont(juce::FontOptions(12.5f, juce::Font::plain));
    aboutInfoLabel.setJustificationType(juce::Justification::centred);
    aboutInfoLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addChildComponent(githubLink);
    githubLink.setFont(juce::FontOptions(13.0f, juce::Font::bold), false);
    githubLink.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xff7289da));

    updateButtonState();
    startTimerHz(5);

    setSize(400, 275);
}

WASAPIStreamerAudioProcessorEditor::~WASAPIStreamerAudioProcessorEditor()
{
    stopTimer();
}

void WASAPIStreamerAudioProcessorEditor::setAboutVisible(bool shouldShow)
{
    showingAbout = shouldShow;
    aboutButton.setButtonText(showingAbout ? "Back" : "About");

    deviceSelector.setVisible(!showingAbout);
    bufferLabel.setVisible(!showingAbout);
    bufferSlider.setVisible(!showingAbout);
    volumeLabel.setVisible(!showingAbout);
    volumeSlider.setVisible(!showingAbout);
    toggleButton.setVisible(!showingAbout);
    statusLabel.setVisible(!showingAbout);

    aboutInfoLabel.setVisible(showingAbout);
    githubLink.setVisible(showingAbout);
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

    auto headerRow = area.removeFromTop(24);
    versionLabel.setBounds(headerRow.removeFromLeft(60));
    aboutButton.setBounds(headerRow.removeFromRight(60));
    titleLabel.setBounds(headerRow);

    area.removeFromTop(10);

    // Main controls layout
    auto mainArea = area;
    deviceSelector.setBounds(mainArea.removeFromTop(30));
    mainArea.removeFromTop(12);

    auto bufferRow = mainArea.removeFromTop(24);
    bufferLabel.setBounds(bufferRow.removeFromLeft(110));
    bufferSlider.setBounds(bufferRow);
    mainArea.removeFromTop(8);

    auto volumeRow = mainArea.removeFromTop(24);
    volumeLabel.setBounds(volumeRow.removeFromLeft(110));
    volumeSlider.setBounds(volumeRow);
    mainArea.removeFromTop(14);

    toggleButton.setBounds(mainArea.removeFromTop(32));
    mainArea.removeFromTop(10);

    statusLabel.setBounds(mainArea.removeFromTop(36));

    // About screen layout (shares the same body area)
    auto aboutArea = area;
    aboutInfoLabel.setBounds(aboutArea.removeFromTop(165));
    aboutArea.removeFromTop(6);
    githubLink.setBounds(aboutArea.removeFromTop(24).withSizeKeepingCentre(160, 24));
}