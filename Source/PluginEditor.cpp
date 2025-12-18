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
  addAndMakeVisible (statusLabel);

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

  setSize (520, 280);
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
  g.drawText ("Live MultiConv", 12, 8, getWidth() - 24, 20, juce::Justification::centredLeft);
}

void NewProjectAudioProcessorEditor::resized()
{
  auto r = getLocalBounds().reduced (12);
  auto top = r.removeFromTop (32);

  statusLabel.setBounds (top.removeFromLeft (200));
  clearButton.setBounds (top.removeFromRight (120));
  recordButton.setBounds (top.removeFromRight (100).reduced (0, 2));

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
  placeRow (fadeInLabel, fadeInSlider);
  placeRow (fadeOutLabel, fadeOutSlider);
  placeRow (mixLabel, mixSlider);
  placeRow (trimLabel, trimSlider);
}

void NewProjectAudioProcessorEditor::timerCallback()
{
  statusLabel.setText ("Status: " + audioProcessor.getStatusText(), juce::dontSendNotification);
}
