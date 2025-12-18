/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
*/
class NewProjectAudioProcessor  : public juce::AudioProcessor
                , private juce::AudioProcessorValueTreeState::Listener
{
public:
    //==============================================================================
    NewProjectAudioProcessor();
    ~NewProjectAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    void triggerOneShotRecord();
    void clearIR();
    juce::String getStatusText() const;

    int getIRChannelCount() const
    {
      return irHasContent.load (std::memory_order_acquire)
               ? irNumChannels.load (std::memory_order_acquire)
               : 0;
    }

    int getConvolutionBufferChannelCount() const
    {
      const auto bankCh = convolutionBankChannels.load (std::memory_order_acquire);
      if (bankCh <= 0)
        return 0;

      const auto ioCh = currentIOChannels.load (std::memory_order_acquire);
      return juce::jlimit (0, 16, juce::jmin (bankCh, ioCh));
    }

    int getCurrentIOChannelCount() const
    {
      return juce::jlimit (0, 16, currentIOChannels.load (std::memory_order_acquire));
    }

    enum class RunState
    {
      passThroughEmpty = 0,
      recording = 1,
      convolving = 2,
    };

    RunState getRunState() const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void parameterChanged (const juce::String& parameterID, float newValue) override;

    void resetForLayoutOrClear (bool keepParameters);
    void startRecording (int numChannels, int recordLengthSamples);
    void finishRecordingAndRequestRebuild();

    void requestRebuild();
    void trySwapInPendingBank();

    struct ConvolutionBank
    {
      void prepare (const juce::dsp::ProcessSpec& perChannelSpec, int numChannels);
      void reset();
      void processChannelReplacing (juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples);

      juce::dsp::ProcessSpec spec { 44100.0, 512u, 1u };
      std::vector<std::unique_ptr<juce::dsp::Convolution>> convolvers;
    };

    class IRRebuildThread final : public juce::Thread
    {
    public:
      explicit IRRebuildThread (NewProjectAudioProcessor& owner);
      ~IRRebuildThread() override;

      void request (uint64_t generation);
      void run() override;

    private:
      NewProjectAudioProcessor& processor;
      juce::WaitableEvent wakeEvent;
      std::atomic<bool> hasPendingRequest { false };
      std::atomic<uint64_t> pendingGeneration { 0 };
    };

    // Parameters
    juce::AudioProcessorValueTreeState apvts;

    // State
    std::atomic<int> runStateAtomic { static_cast<int> (RunState::passThroughEmpty) };
    std::atomic<bool> recordRequested { false };
    std::atomic<bool> clearRequested { false };
    std::atomic<bool> rebuildRequested { false };
    std::atomic<uint64_t> irGeneration { 1 };

    std::atomic<double> irRecordedSampleRate { 44100.0 };

    std::atomic<juce::uint32> lastIRShapeParamChangeMs { 0 };

    // Recording buffers (double-buffered to avoid audio-thread writes racing background reads)
    juce::AudioBuffer<float> irOriginalBuffers[2];
    int irAllocatedChannels = 0;
    int irAllocatedSamples = 0;
    int irWriteBufferIndex = 0;
    std::atomic<int> irReadBufferIndex { 1 };
    std::atomic<int> irNumChannels { 0 };
    std::atomic<int> irLengthSamples { 0 };
    int irWritePos = 0;
    std::atomic<bool> irHasContent { false };

    int lastKnownNumChannels = 0;

    // UI-visible counters (avoid racing shared_ptr access from message thread)
    std::atomic<int> currentIOChannels { 0 };
    std::atomic<int> convolutionBankChannels { 0 };

    // Processing buffers
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> oldWetBuffer;

    // Convolution banks
    juce::dsp::ProcessSpec perChannelSpec { 44100.0, 512u, 1u };
    std::shared_ptr<ConvolutionBank> activeBank;
    std::shared_ptr<ConvolutionBank> fadingOutBank;
    int crossfadeRemainingSamples = 0;
    int crossfadeTotalSamples = 0;

    juce::SpinLock pendingBankLock;
    std::shared_ptr<ConvolutionBank> pendingBank;

    IRRebuildThread rebuildThread;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NewProjectAudioProcessor)
};
