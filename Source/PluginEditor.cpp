/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
  void configureSlider (juce::Slider& s)
  {
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 20);
  }

  void configureLabel (juce::Label& l, const juce::String& text)
  {
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centredLeft);
  }
}

//==============================================================================
NewProjectAudioProcessorEditor::NewProjectAudioProcessorEditor (NewProjectAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
  statusLabel.setJustificationType (juce::Justification::centredLeft);
  statusLabel.setText ("Pass-through", juce::dontSendNotification);
  statusLabel.setColour (juce::Label::textColourId, juce::Colours::white);
  addAndMakeVisible (statusLabel);

  convChannelsLabel.setJustificationType (juce::Justification::centredLeft);
  convChannelsLabel.setText ("Channels: 0", juce::dontSendNotification);
  convChannelsLabel.setColour (juce::Label::textColourId, juce::Colours::white);
  addAndMakeVisible (convChannelsLabel);

  // Number-box style (stepper) rather than a casual slider.
  processChannelsSlider.setSliderStyle (juce::Slider::IncDecButtons);
  processChannelsSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 20);
  processChannelsSlider.setRange (1.0, 16.0, 1.0);
  processChannelsSlider.setNumDecimalPlacesToDisplay (0);
  processChannelsSlider.setName ("Process Channels");
  addAndMakeVisible (processChannelsSlider);
  configureLabel (processChannelsLabel, "Process Channels");
  addAndMakeVisible (processChannelsLabel);

  recordButton.setClickingTogglesState (false);
  recordButton.onClick = [this]
  {
    audioProcessor.triggerOneShotRecord();
  };
  addAndMakeVisible (recordButton);

  clearButton.onClick = [this]
  {
    audioProcessor.clearIR();
  };
  addAndMakeVisible (clearButton);

  configureSlider (recordLengthMsSlider);
  recordLengthMsSlider.setName ("Record (ms)");
  addAndMakeVisible (recordLengthMsSlider);
  configureLabel (recordLengthMsLabel, "Record Length (ms)");
  addAndMakeVisible (recordLengthMsLabel);

  configureSlider (fadeInSlider);
  fadeInSlider.setName ("Fade In (%)");
  addAndMakeVisible (fadeInSlider);
  configureLabel (fadeInLabel, "Fade In (%)");
  addAndMakeVisible (fadeInLabel);

  configureSlider (fadeOutSlider);
  fadeOutSlider.setName ("Fade Out (%)");
  addAndMakeVisible (fadeOutSlider);
  configureLabel (fadeOutLabel, "Fade Out (%)");
  addAndMakeVisible (fadeOutLabel);

  configureSlider (mixSlider);
  mixSlider.setName ("Mix");
  addAndMakeVisible (mixSlider);
  configureLabel (mixLabel, "Wet / Dry");
  addAndMakeVisible (mixLabel);

  configureSlider (trimSlider);
  trimSlider.setName ("Trim (dB)");
  addAndMakeVisible (trimSlider);
  configureLabel (trimLabel, "Output Trim (dB)");
  addAndMakeVisible (trimLabel);

  auto& apvts = audioProcessor.getAPVTS();
  recordLengthAttachment = std::make_unique<SliderAttachment> (apvts, "recordLengthMs", recordLengthMsSlider);
  fadeInAttachment = std::make_unique<SliderAttachment> (apvts, "fadeInPct", fadeInSlider);
  fadeOutAttachment = std::make_unique<SliderAttachment> (apvts, "fadeOutPct", fadeOutSlider);
  mixAttachment = std::make_unique<SliderAttachment> (apvts, "mix", mixSlider);
  trimAttachment = std::make_unique<SliderAttachment> (apvts, "trimDb", trimSlider);
  processChannelsAttachment = std::make_unique<SliderAttachment> (apvts, "processChannels", processChannelsSlider);

  setSize (520, 380);
  startTimerHz (15);
}

NewProjectAudioProcessorEditor::~NewProjectAudioProcessorEditor()
{
}

//==============================================================================
void NewProjectAudioProcessorEditor::paint (juce::Graphics& g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

  g.setColour (juce::Colours::white);
  g.setFont (juce::FontOptions (14.0f));
  g.drawText ("Live MultiConv", 12, 8, getWidth() - 24, 18, juce::Justification::centredLeft);
}

void NewProjectAudioProcessorEditor::resized()
{
  auto r = getLocalBounds().reduced (12);
  auto top = r.removeFromTop (76);

  auto buttonsArea = top.removeFromRight (230);
  clearButton.setBounds (buttonsArea.removeFromRight (120));
  recordButton.setBounds (buttonsArea.removeFromRight (100).reduced (0, 2));

  auto leftArea = top;
  leftArea.removeFromTop (22); // reserved for the title drawn in paint()
  statusLabel.setBounds (leftArea.removeFromTop (20));
  convChannelsLabel.setBounds (leftArea.removeFromTop (20));

  r.removeFromTop (10);

  const int labelW = 150;
  const int rowH = 34;
  const int rowGap = 8;

  auto placeRow = [&r, labelW, rowH, rowGap] (juce::Label& label, juce::Slider& slider)
  {
    auto row = r.removeFromTop (rowH);
    label.setBounds (row.removeFromLeft (labelW));
    slider.setBounds (row);
    r.removeFromTop (rowGap);
  };

  placeRow (recordLengthMsLabel, recordLengthMsSlider);
  placeRow (processChannelsLabel, processChannelsSlider);
  placeRow (fadeInLabel, fadeInSlider);
  placeRow (fadeOutLabel, fadeOutSlider);
  placeRow (mixLabel, mixSlider);
  placeRow (trimLabel, trimSlider);
}

void NewProjectAudioProcessorEditor::timerCallback()
{
  statusLabel.setText ("Status: " + audioProcessor.getStatusText(), juce::dontSendNotification);

  const auto ioCh = audioProcessor.getCurrentIOChannelCount();
  const auto maxCh = juce::jmax (1, juce::jmin (16, ioCh));

  if ((int) std::llround (processChannelsSlider.getMaximum()) != maxCh)
      processChannelsSlider.setRange (1.0, (double) maxCh, 1.0);

  if ((int) std::llround (processChannelsSlider.getValue()) > maxCh)
      processChannelsSlider.setValue ((double) maxCh, juce::sendNotificationSync);

  const auto reqCh = audioProcessor.getRequestedProcessChannelCount();
  const auto bankCh = audioProcessor.getConvolutionBufferChannelCount();
  const auto effectiveCh = juce::jmax (0, juce::jmin (ioCh, juce::jmin (reqCh, bankCh)));
  convChannelsLabel.setText ("Channels: " + juce::String (effectiveCh), juce::dontSendNotification);
}
