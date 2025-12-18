/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/**
*/
class NewProjectAudioProcessorEditor  : public juce::AudioProcessorEditor
                   , private juce::Timer
{
public:
    NewProjectAudioProcessorEditor (NewProjectAudioProcessor&);
    ~NewProjectAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    NewProjectAudioProcessor& audioProcessor;

  void timerCallback() override;

  juce::Label statusLabel;
  juce::TextButton recordButton { "Record" };
  juce::TextButton clearButton { "Clear Buffer" };

  juce::Label recordLengthMsLabel;
  juce::Label fadeInLabel;
  juce::Label fadeOutLabel;
  juce::Label mixLabel;
  juce::Label trimLabel;

  juce::Slider recordLengthMsSlider;
  juce::Slider fadeInSlider;
  juce::Slider fadeOutSlider;
  juce::Slider mixSlider;
  juce::Slider trimSlider;

  using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

  std::unique_ptr<SliderAttachment> recordLengthAttachment;
  std::unique_ptr<SliderAttachment> fadeInAttachment;
  std::unique_ptr<SliderAttachment> fadeOutAttachment;
  std::unique_ptr<SliderAttachment> mixAttachment;
  std::unique_ptr<SliderAttachment> trimAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NewProjectAudioProcessorEditor)
};
