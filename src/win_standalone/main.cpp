/*
 *  main.cpp
 *
 *  Copyright (c) 2026
 *
 *  This file is part of amsynth.
 */

#include "core/Configuration.h"
#include "core/filesystem.h"
#include "core/gui/MainComponent.h"
#include "core/synth/Preset.h"
#include "core/synth/Synthesizer.h"

#include "juce_audio_devices/juce_audio_devices.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "juce_audio_utils/juce_audio_utils.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "amsynth"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "???"
#endif

namespace {

class StandaloneSynth final : public juce::AudioIODeviceCallback,
                              public juce::MidiInputCallback
{
public:
    StandaloneSynth()
    {
        auto &config = Configuration::get();

        Preset::setIgnoredParameterNames(config.ignored_parameters);

        synthesizer_.setSampleRate(config.sample_rate);
        synthesizer_.setMaxNumVoices(config.polyphony);
        synthesizer_.setMidiChannel((unsigned char) config.midi_channel);
        synthesizer_.setPitchBendRangeSemitones(config.pitch_bend_range);
        synthesizer_.loadBank(config.current_bank_file.c_str());

        if (config.current_tuning_file != "default") {
            synthesizer_.loadTuningScale(config.current_tuning_file.c_str());
        }
    }

    Synthesizer &getSynthesizer() { return synthesizer_; }

    void audioDeviceAboutToStart(juce::AudioIODevice *device) override
    {
        if (device == nullptr) {
            return;
        }

        synthesizer_.setSampleRate((int) device->getCurrentSampleRate());
    }

    void audioDeviceStopped() override
    {
        tempLeft_.clear();
        tempRight_.clear();
    }

    void audioDeviceIOCallbackWithContext(const float **inputChannelData,
                                          int numInputChannels,
                                          float **outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext &context) override
    {
        juce::ignoreUnused(inputChannelData, numInputChannels, context);

        for (int channel = 0; channel < numOutputChannels; ++channel) {
            if (outputChannelData[channel] != nullptr) {
                juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);
            }
        }

        if (numOutputChannels <= 0 || numSamples <= 0) {
            return;
        }

        std::vector<juce::MidiMessage> pendingMessages;
        {
            const juce::ScopedLock lock(midiLock_);
            pendingMessages.swap(midiQueue_);
        }

        std::vector<std::vector<unsigned char>> rawMessages;
        std::vector<amsynth_midi_event_t> midiIn;
        rawMessages.reserve(pendingMessages.size());
        midiIn.reserve(pendingMessages.size());

        for (const auto &message : pendingMessages) {
            rawMessages.emplace_back(message.getRawData(), message.getRawData() + message.getRawDataSize());
            midiIn.push_back({0u, (unsigned) rawMessages.back().size(), rawMessages.back().data()});
        }

        std::vector<amsynth_midi_cc_t> midiOut;

        tempLeft_.resize((size_t) numSamples);
        tempRight_.resize((size_t) numSamples);

        synthesizer_.process((unsigned) numSamples, midiIn, midiOut, tempLeft_.data(), tempRight_.data());

        if (outputChannelData[0] != nullptr) {
            std::memcpy(outputChannelData[0], tempLeft_.data(), (size_t) numSamples * sizeof(float));
        }

        if (numOutputChannels > 1 && outputChannelData[1] != nullptr) {
            std::memcpy(outputChannelData[1], tempRight_.data(), (size_t) numSamples * sizeof(float));
        }

        if (numOutputChannels == 1 && outputChannelData[0] != nullptr) {
            for (int i = 0; i < numSamples; ++i) {
                outputChannelData[0][i] = 0.5f * (tempLeft_[(size_t) i] + tempRight_[(size_t) i]);
            }
        }
    }

    void handleIncomingMidiMessage(juce::MidiInput *source, const juce::MidiMessage &message) override
    {
        juce::ignoreUnused(source);
        const juce::ScopedLock lock(midiLock_);
        midiQueue_.push_back(message);
    }

private:
    Synthesizer synthesizer_;
    juce::CriticalSection midiLock_;
    std::vector<juce::MidiMessage> midiQueue_;
    std::vector<float> tempLeft_;
    std::vector<float> tempRight_;
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow(StandaloneSynth &standaloneSynth, juce::AudioDeviceManager &deviceManager)
        : juce::DocumentWindow(PACKAGE_NAME,
                               juce::Colours::lightgrey,
                               juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          standaloneSynth_(standaloneSynth),
          deviceManager_(deviceManager)
    {
        auto *mainComponent = new MainComponent(standaloneSynth_.getSynthesizer().getPresetController(),
                                                standaloneSynth_.getSynthesizer().getMidiController());
        mainComponent->isPlugin = false;
        mainComponent->openSettings = [this] { showAudioMidiSettings(); };

        for (const auto &it : standaloneSynth_.getSynthesizer().getProperties()) {
            mainComponent->propertyChanged(it.first.c_str(), it.second.c_str());
        }

        mainComponent->sendProperty = [this](const char *name, const char *value) {
            standaloneSynth_.getSynthesizer().setProperty(name, value);

            auto &config = Configuration::get();
            if (name == std::string(PROP_NAME(max_polyphony))) {
                config.polyphony = std::stoi(value);
            }
            if (name == std::string(PROP_NAME(midi_channel))) {
                config.midi_channel = std::stoi(value);
            }
            if (name == std::string(PROP_NAME(pitch_bend_range))) {
                config.pitch_bend_range = std::stoi(value);
            }
            if (name == std::string(PROP_NAME(tuning_scl_file))) {
                config.current_tuning_file = (value != nullptr && *value != '\0') ? value : "default";
            }
            config.save();
        };

        setContentOwned(mainComponent, true);
        centreWithSize(getWidth(), getHeight());
        setResizable(false, false);
        setUsingNativeTitleBar(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    void showAudioMidiSettings()
    {
        if (settingsWindow_ != nullptr) {
            settingsWindow_->toFront(true);
            return;
        }

        auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(deviceManager_, 0, 2, 0, 2, true, false, true, false);
        selector->setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff2b2b2b));
        selector->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1f1f1f));
        selector->setColour(juce::TextEditor::textColourId, juce::Colours::white);
        selector->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4f6f8f));
        selector->setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        selector->setColour(juce::Label::textColourId, juce::Colours::white);
        selector->setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff2b2b2b));
        selector->setColour(juce::ListBox::textColourId, juce::Colours::white);
        selector->setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1f1f1f));
        selector->setColour(juce::ComboBox::textColourId, juce::Colours::white);
        selector->setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff5a5a5a));
        selector->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3a3a));
        selector->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        selector->setColour(juce::ToggleButton::textColourId, juce::Colours::white);
        selector->setSize(520, 460);

        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Audio/MIDI Settings";
        options.dialogBackgroundColour = juce::Colour(0xff2b2b2b);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.componentToCentreAround = this;
        options.content.setOwned(selector.release());

        settingsWindow_ = options.launchAsync();
        settingsWindow_->enterModalState(true, juce::ModalCallbackFunction::create([this](int) {
            settingsWindow_ = nullptr;
        }), true);
    }

    StandaloneSynth &standaloneSynth_;
    juce::AudioDeviceManager &deviceManager_;
    juce::DialogWindow *settingsWindow_ {nullptr};
};

class Application final : public juce::JUCEApplication, public juce::ChangeListener
{
public:
    const juce::String getApplicationName() override { return PACKAGE_NAME; }
    const juce::String getApplicationVersion() override { return PACKAGE_VERSION; }

    void initialise(const juce::String &commandLine) override
    {
        juce::ignoreUnused(commandLine);

        auto savedState = loadAudioDeviceState();
        auto error = deviceManager_.initialise(0, 2, savedState.get(), true);
        if (error.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   PACKAGE_NAME,
                                                   error,
                                                   "OK",
                                                   nullptr,
                                                   juce::ModalCallbackFunction::create([this](int) { quit(); }));
            return;
        }

        deviceManager_.addChangeListener(this);
        deviceManager_.addAudioCallback(&standaloneSynth_);

        auto midiDevices = juce::MidiInput::getAvailableDevices();
        for (const auto &device : midiDevices) {
            if (savedState == nullptr) {
                deviceManager_.setMidiInputDeviceEnabled(device.identifier, true);
            }
            deviceManager_.addMidiInputDeviceCallback(device.identifier, &standaloneSynth_);
        }

        mainWindow_ = std::make_unique<MainWindow>(standaloneSynth_, deviceManager_);
        mainWindow_->setVisible(true);
    }

    void shutdown() override
    {
        saveAudioDeviceState();

        for (const auto &device : juce::MidiInput::getAvailableDevices()) {
            deviceManager_.removeMidiInputDeviceCallback(device.identifier, &standaloneSynth_);
        }

        deviceManager_.removeAudioCallback(&standaloneSynth_);
        deviceManager_.removeChangeListener(this);
        mainWindow_.reset();
    }

    void changeListenerCallback(juce::ChangeBroadcaster *source) override
    {
        if (source == &deviceManager_) {
            saveAudioDeviceState();
        }
    }

private:
    static juce::File getAudioDeviceStateFile()
    {
        return juce::File(juce::String(filesystem::get().config) + ".audio.xml");
    }

    static std::unique_ptr<juce::XmlElement> loadAudioDeviceState()
    {
        auto stateFile = getAudioDeviceStateFile();
        if (!stateFile.existsAsFile()) {
            return nullptr;
        }

        return std::unique_ptr<juce::XmlElement>(juce::XmlDocument::parse(stateFile));
    }

    static void saveAudioDeviceState(juce::AudioDeviceManager &deviceManager)
    {
        auto stateFile = getAudioDeviceStateFile();
        if (auto state = deviceManager.createStateXml()) {
            state->writeTo(stateFile);
        }
    }

    void saveAudioDeviceState()
    {
        saveAudioDeviceState(deviceManager_);
    }

    StandaloneSynth standaloneSynth_;
    juce::AudioDeviceManager deviceManager_;
    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace

START_JUCE_APPLICATION(Application)
