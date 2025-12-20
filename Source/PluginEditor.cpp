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

  configureLabel (irWaveformLabel, "IR Waveform");
  irWaveformLabel.setColour (juce::Label::textColourId, juce::Colours::white);
  addAndMakeVisible (irWaveformLabel);

  configureLabel (irWaveformChannelLabel, "View IR Ch");
  irWaveformChannelLabel.setColour (juce::Label::textColourId, juce::Colours::white);
  addAndMakeVisible (irWaveformChannelLabel);

  irWaveformChannelSlider.setSliderStyle (juce::Slider::IncDecButtons);
  irWaveformChannelSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 20);
  irWaveformChannelSlider.setRange (1.0, 1.0, 1.0);
  irWaveformChannelSlider.setNumDecimalPlacesToDisplay (0);
  irWaveformChannelSlider.setValue (1.0, juce::dontSendNotification);
  addAndMakeVisible (irWaveformChannelSlider);

  addAndMakeVisible (irWaveform);

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

  // Ensure the displayed waveform responds immediately to fade edits and channel changes.
  fadeInSlider.onValueChange = [this] { updateWaveformDisplay(); };
  fadeOutSlider.onValueChange = [this] { updateWaveformDisplay(); };
  irWaveformChannelSlider.onValueChange = [this] { updateWaveformDisplay(); };

  setSize (520, 520);
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

  {
    auto wfHeader = r.removeFromTop (24);
    irWaveformLabel.setBounds (wfHeader.removeFromLeft (120));
    irWaveformChannelSlider.setBounds (wfHeader.removeFromRight (110));
    irWaveformChannelLabel.setBounds (wfHeader.removeFromRight (90));
  }

  irWaveform.setBounds (r.removeFromTop (120));

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

  updateWaveformDisplay();
}

void NewProjectAudioProcessorEditor::applyFadesToWaveformForDisplay (std::vector<float>& samples,
                                                                    double fadeInPct,
                                                                    double fadeOutPct)
{
  const int numSamples = (int) samples.size();
  if (numSamples <= 0)
    return;

  const int fadeInSamples = (int) std::llround ((double) numSamples * fadeInPct * 0.01);
  const int fadeOutSamples = (int) std::llround ((double) numSamples * fadeOutPct * 0.01);

  if (fadeInSamples > 1)
  {
    const int n = juce::jmin (fadeInSamples, numSamples);
    for (int i = 0; i < n; ++i)
    {
      const double t = (double) i / (double) (n - 1);
      const float g = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t));
      samples[(size_t) i] *= g;
    }
  }

  if (fadeOutSamples > 1)
  {
    const int n = juce::jmin (fadeOutSamples, numSamples);
    for (int i = 0; i < n; ++i)
    {
      const double t = (double) i / (double) (n - 1);
      const float g = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t));
      samples[(size_t) (numSamples - 1 - i)] *= g;
    }
  }
}

void NewProjectAudioProcessorEditor::updateWaveformDisplay()
{
  // Waveform selector clamps to the IR buffer channel count.
  const int irCh = audioProcessor.getIRChannelCount();
  const int maxIRCh = juce::jmax (1, irCh);
  irWaveformChannelSlider.setEnabled (irCh > 0);

  if ((int) std::llround (irWaveformChannelSlider.getMaximum()) != maxIRCh)
    irWaveformChannelSlider.setRange (1.0, (double) maxIRCh, 1.0);

  const int selectedOneBased = juce::jlimit (1, maxIRCh, (int) std::llround (irWaveformChannelSlider.getValue()));
  if ((int) std::llround (irWaveformChannelSlider.getValue()) != selectedOneBased)
    irWaveformChannelSlider.setValue ((double) selectedOneBased, juce::dontSendNotification);

  if (irCh > 0)
  {
    std::vector<float> samples;
    double sr = 0.0;
    if (audioProcessor.copyIRChannelSnapshot (selectedOneBased - 1, samples, sr))
    {
      float rawPeak = 0.0f;
      for (auto v : samples)
        rawPeak = juce::jmax (rawPeak, std::abs (v));
      if (rawPeak < 1.0e-6f)
        rawPeak = 1.0f;

      applyFadesToWaveformForDisplay (samples, fadeInSlider.getValue(), fadeOutSlider.getValue());
      irWaveform.setSamples (std::move (samples), rawPeak);
    }
    else
      irWaveform.clear();
  }
  else
  {
    irWaveform.clear();
  }
}
