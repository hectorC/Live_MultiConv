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
  void updateWaveformDisplay();
  static void applyFadesToWaveformForDisplay (std::vector<float>& samples,
                                             double fadeInPct,
                                             double fadeOutPct);

  juce::Label statusLabel;
  juce::Label convChannelsLabel;

  class IRWaveformComponent final : public juce::Component
  {
  public:
    void setSamples (std::vector<float> newSamples)
    {
      samples = std::move (newSamples);
      displayPeak = 0.0f;
      repaint();
    }

    void setSamples (std::vector<float> newSamples, float peakForDisplay)
    {
      samples = std::move (newSamples);
      displayPeak = peakForDisplay;
      repaint();
    }

    void clear()
    {
      samples.clear();
      displayPeak = 0.0f;
      repaint();
    }

    void paint (juce::Graphics& g) override
    {
      g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
      g.setColour (juce::Colours::white.withAlpha (0.2f));
      g.drawRect (getLocalBounds());

      if (samples.empty())
      {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("(IR empty)", getLocalBounds().reduced (6), juce::Justification::centred);
        return;
      }

      const auto area = getLocalBounds().reduced (6);
      const int w = juce::jmax (1, area.getWidth());
      const int h = juce::jmax (1, area.getHeight());
      const float midY = (float) area.getCentreY();
      const float halfH = 0.5f * (float) h;

      float peak = displayPeak;
      if (peak <= 0.0f)
      {
        for (auto v : samples)
          peak = juce::jmax (peak, std::abs (v));
      }
      if (peak < 1.0e-6f)
        peak = 1.0f;

      g.setColour (juce::Colours::white.withAlpha (0.8f));

      const int n = (int) samples.size();
      for (int x = 0; x < w; ++x)
      {
        const int i0 = (int) std::floor ((double) x * (double) n / (double) w);
        const int i1 = (int) std::floor ((double) (x + 1) * (double) n / (double) w);
        const int a = juce::jlimit (0, n - 1, i0);
        const int b = juce::jlimit (a + 1, n, i1);

        float mn = 1.0e9f, mx = -1.0e9f;
        for (int i = a; i < b; ++i)
        {
          const float v = samples[(size_t) i];
          mn = juce::jmin (mn, v);
          mx = juce::jmax (mx, v);
        }

        const float y0 = midY - (mx / peak) * halfH;
        const float y1 = midY - (mn / peak) * halfH;
        const float px = (float) (area.getX() + x);
        g.drawLine (px, y0, px, y1);
      }
    }

  private:
    std::vector<float> samples;
    float displayPeak = 0.0f;
  };

  juce::Label irWaveformLabel;
  juce::Label irWaveformChannelLabel;
  juce::Slider irWaveformChannelSlider;
  IRWaveformComponent irWaveform;

  juce::Label processChannelsLabel;
  juce::Slider processChannelsSlider;
  juce::TextButton recordButton { "Record" };
  juce::TextButton clearButton { "Clear Buffer" };
  juce::TextButton loadButton { "Load" };
  juce::TextButton saveButton { "Save" };

  std::unique_ptr<juce::FileChooser> loadChooser;
  std::unique_ptr<juce::FileChooser> saveChooser;

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

  std::unique_ptr<SliderAttachment> processChannelsAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NewProjectAudioProcessorEditor)
};
