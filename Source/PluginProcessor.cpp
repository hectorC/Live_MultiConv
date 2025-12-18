/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ParamIDs
{
    static constexpr auto recordLengthMs = "recordLengthMs";
    static constexpr auto fadeInPct = "fadeInPct";
    static constexpr auto fadeOutPct = "fadeOutPct";
    static constexpr auto mix = "mix";
    static constexpr auto trimDb = "trimDb";
}

static int msToSamples (double sampleRate, float ms)
{
    return juce::jmax (1, (int) std::llround (sampleRate * (double) ms / 1000.0));
}

//==============================================================================
NewProjectAudioProcessor::NewProjectAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
    , apvts (*this, nullptr, "PARAMS", createParameterLayout())
    , rebuildThread (*this)
{
    apvts.addParameterListener (ParamIDs::fadeInPct, this);
    apvts.addParameterListener (ParamIDs::fadeOutPct, this);
}

NewProjectAudioProcessor::~NewProjectAudioProcessor()
{
    apvts.removeParameterListener (ParamIDs::fadeInPct, this);
    apvts.removeParameterListener (ParamIDs::fadeOutPct, this);
}

juce::AudioProcessorValueTreeState::ParameterLayout NewProjectAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::recordLengthMs,
        "Record Length (ms)",
        juce::NormalisableRange<float> (100.0f, 2000.0f, 1.0f, 0.5f),
        500.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::fadeInPct,
        "Fade In (%)",
        juce::NormalisableRange<float> (0.0f, 50.0f, 0.1f),
        0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::fadeOutPct,
        "Fade Out (%)",
        juce::NormalisableRange<float> (0.0f, 50.0f, 0.1f),
        0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::mix,
        "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        1.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::trimDb,
        "Output Trim (dB)",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
        0.0f));

    return { params.begin(), params.end() };
}

void NewProjectAudioProcessor::ConvolutionBank::prepare (const juce::dsp::ProcessSpec& perSpec, int numChannels)
{
    spec = perSpec;
    convolvers.clear();
    convolvers.reserve ((size_t) numChannels);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto conv = std::make_unique<juce::dsp::Convolution>();
        conv->prepare (spec);
        convolvers.push_back (std::move (conv));
    }
}

void NewProjectAudioProcessor::ConvolutionBank::reset()
{
    for (auto& conv : convolvers)
        conv->reset();
}

void NewProjectAudioProcessor::ConvolutionBank::processChannelReplacing (juce::AudioBuffer<float>& buffer,
                                                                        int channel,
                                                                        int startSample,
                                                                        int numSamples)
{
    if (channel < 0 || channel >= (int) convolvers.size())
        return;

    juce::dsp::AudioBlock<float> block (buffer);
    auto channelBlock = block.getSingleChannelBlock ((size_t) channel).getSubBlock ((size_t) startSample, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> ctx (channelBlock);
    convolvers[(size_t) channel]->process (ctx);
}

NewProjectAudioProcessor::IRRebuildThread::IRRebuildThread (NewProjectAudioProcessor& owner)
    : juce::Thread ("IRRebuild"), processor (owner)
{
    startThread();
}

NewProjectAudioProcessor::IRRebuildThread::~IRRebuildThread()
{
    signalThreadShouldExit();
    wakeEvent.signal();
    stopThread (2000);
}

void NewProjectAudioProcessor::IRRebuildThread::request (uint64_t generation)
{
    pendingGeneration.store (generation, std::memory_order_release);
    hasPendingRequest.store (true, std::memory_order_release);
    wakeEvent.signal();
}

void NewProjectAudioProcessor::IRRebuildThread::run()
{
    while (! threadShouldExit())
    {
        wakeEvent.wait (250);
        if (threadShouldExit())
            break;

        if (! hasPendingRequest.exchange (false, std::memory_order_acq_rel))
            continue;

        const auto requestGen = pendingGeneration.load (std::memory_order_acquire);
        const auto currentGen = processor.irGeneration.load (std::memory_order_acquire);
        if (requestGen != currentGen)
            continue;

        if (! processor.irHasContent.load (std::memory_order_acquire))
            continue;

        const int numChannels = processor.irNumChannels.load (std::memory_order_acquire);
        const int numSamples = processor.irLengthSamples.load (std::memory_order_acquire);

        if (numSamples <= 0 || numChannels <= 0)
            continue;

        const auto fadeInPct = processor.apvts.getRawParameterValue (ParamIDs::fadeInPct)->load();
        const auto fadeOutPct = processor.apvts.getRawParameterValue (ParamIDs::fadeOutPct)->load();
        const auto doNormalize = false; // hardcoded off for safety

        const int readIndex = processor.irReadBufferIndex.load (std::memory_order_acquire);
        const auto& irSource = processor.irOriginalBuffers[juce::jlimit (0, 1, readIndex)];

        auto newBank = std::make_shared<ConvolutionBank>();
        newBank->prepare (processor.perChannelSpec, numChannels);

        const int fadeInSamples = juce::jlimit (0, numSamples, (int) std::llround ((double) numSamples * (double) fadeInPct / 100.0));
        const int fadeOutSamples = juce::jlimit (0, numSamples, (int) std::llround ((double) numSamples * (double) fadeOutPct / 100.0));

        // Build shaped IR + compute peak for normalization.
        float peak = 0.0f;
        juce::AudioBuffer<float> shaped (numChannels, numSamples);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            shaped.copyFrom (ch, 0, irSource, ch, 0, numSamples);

            // Raised-cosine fades reduce high-frequency artifacts vs linear ramps.
            if (fadeInSamples > 1)
            {
                auto* w = shaped.getWritePointer (ch);
                for (int i = 0; i < fadeInSamples; ++i)
                {
                    const double t = (double) i / (double) (fadeInSamples - 1);
                    const float g = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t));
                    w[i] *= g;
                }
            }
            else if (fadeInSamples == 1)
            {
                shaped.setSample (ch, 0, 0.0f);
            }

            if (fadeOutSamples > 1)
            {
                auto* w = shaped.getWritePointer (ch);
                for (int i = 0; i < fadeOutSamples; ++i)
                {
                    const double t = (double) i / (double) (fadeOutSamples - 1);
                    const float g = (float) (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * t));
                    const int idx = numSamples - 1 - i;
                    if (idx >= 0)
                        w[idx] *= g;
                }
            }
            else if (fadeOutSamples == 1)
            {
                shaped.setSample (ch, numSamples - 1, 0.0f);
            }

            peak = juce::jmax (peak, shaped.getMagnitude (ch, 0, numSamples));
        }

        if (doNormalize)
        {
            constexpr float targetPeak = 0.98f;
            if (peak > 1.0e-6f)
            {
                const float desiredGain = targetPeak / peak;
                const float maxGain = juce::Decibels::decibelsToGain (24.0f);
                const float gain = juce::jmin (desiredGain, maxGain);
                shaped.applyGain (gain);
            }
        }

        // Load per-channel mono IRs.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::AudioBuffer<float> mono (1, numSamples);
            mono.copyFrom (0, 0, shaped, ch, 0, numSamples);

            // Use partitioned convolution internally; JUCE handles partitioning.
            newBank->convolvers[(size_t) ch]->loadImpulseResponse (std::move (mono),
                                                                  processor.irRecordedSampleRate.load (std::memory_order_acquire),
                                                                  juce::dsp::Convolution::Stereo::no,
                                                                  juce::dsp::Convolution::Trim::no,
                                                                  juce::dsp::Convolution::Normalise::no);
        }

        // Discard if state changed while building.
        if (processor.irGeneration.load (std::memory_order_acquire) != requestGen)
            continue;

        const juce::SpinLock::ScopedLockType lock (processor.pendingBankLock);
        processor.pendingBank = std::move (newBank);
    }
}

//==============================================================================
const juce::String NewProjectAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NewProjectAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool NewProjectAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool NewProjectAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double NewProjectAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NewProjectAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int NewProjectAudioProcessor::getCurrentProgram()
{
    return 0;
}

void NewProjectAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String NewProjectAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void NewProjectAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void NewProjectAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    perChannelSpec.sampleRate = sampleRate;
    perChannelSpec.maximumBlockSize = (juce::uint32) juce::jmax (1, samplesPerBlock);
    perChannelSpec.numChannels = 1;

    const bool hasRestoredIR = irHasContent.load (std::memory_order_acquire)
                              && irNumChannels.load (std::memory_order_acquire) > 0
                              && irLengthSamples.load (std::memory_order_acquire) > 0;

    if (! hasRestoredIR)
        irRecordedSampleRate.store (sampleRate, std::memory_order_release);

    const int maxChannels = 16;
    const int maxIRSamples = msToSamples (sampleRate, 2000.0f);

    irAllocatedChannels = maxChannels;
    irAllocatedSamples = maxIRSamples;
    for (auto& b : irOriginalBuffers)
    {
        b.setSize (irAllocatedChannels, irAllocatedSamples,
                   hasRestoredIR /*keepExistingContent*/,
                   true          /*clearExtraSpace*/,
                   true          /*avoidRealloc*/);

        if (! hasRestoredIR)
            b.clear();
    }

    if (! hasRestoredIR)
    {
        irWriteBufferIndex = 0;
        irReadBufferIndex.store (1, std::memory_order_release);
    }

    lastKnownNumChannels = 0;

    dryBuffer.setSize (maxChannels, samplesPerBlock, false, false, true);
    oldWetBuffer.setSize (maxChannels, samplesPerBlock, false, false, true);
    dryBuffer.clear();
    oldWetBuffer.clear();

    crossfadeTotalSamples = juce::jmax (1, (int) std::llround (sampleRate * 0.05));
    crossfadeRemainingSamples = 0;

    // Don't clear restored state here. Hosts (including REAPER) may restore state before/after
    // prepareToPlay(), so we keep any saved IR and just ensure the DSP is in a consistent state.
    {
        const juce::SpinLock::ScopedLockType lock (pendingBankLock);
        pendingBank.reset();
    }
    fadingOutBank.reset();
    activeBank.reset();

    lastKnownNumChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels());

    if (irHasContent.load (std::memory_order_acquire)
        && irNumChannels.load (std::memory_order_acquire) > 0
        && irLengthSamples.load (std::memory_order_acquire) > 0)
    {
        runStateAtomic.store ((int) RunState::convolving, std::memory_order_release);
        requestRebuild();
    }
    else
    {
        runStateAtomic.store ((int) RunState::passThroughEmpty, std::memory_order_release);
    }
}

void NewProjectAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool NewProjectAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    const int inCh = mainIn.size();
    const int outCh = mainOut.size();

    if (inCh < 1 || inCh > 16 || outCh < 1 || outCh > 16)
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (inCh != outCh)
        return false;
   #endif

    return true;
  #endif
}
#endif

void NewProjectAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const int numChannels = juce::jmin (16, totalNumInputChannels);
    const int numSamples = buffer.getNumSamples();

    if (numSamples > dryBuffer.getNumSamples())
    {
        // Host changed block size without calling prepareToPlay(). Resize as a fallback.
        dryBuffer.setSize (16, numSamples, false, false, true);
        oldWetBuffer.setSize (16, numSamples, false, false, true);
    }

    // Consume UI actions
    if (clearRequested.exchange (false, std::memory_order_acq_rel))
        resetForLayoutOrClear (false);

    // Reset if host changed channel count.
    if (numChannels != lastKnownNumChannels || totalNumOutputChannels != lastKnownNumChannels)
    {
        lastKnownNumChannels = juce::jmin (numChannels, totalNumOutputChannels);
        resetForLayoutOrClear (true);
    }

    if (recordRequested.exchange (false, std::memory_order_acq_rel))
    {
        const float ms = apvts.getRawParameterValue (ParamIDs::recordLengthMs)->load();
        const int recordSamples = juce::jmin (irAllocatedSamples, msToSamples (getSampleRate(), ms));
        startRecording (numChannels, recordSamples);
    }

    // Swap in a freshly rebuilt convolver bank if available.
    trySwapInPendingBank();

    // Always capture dry input for mixing/recording.
    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    const auto state = getRunState();

    // Recording: fill IR buffer and pass-through.
    if (state == RunState::recording)
    {
        const int targetLen = irLengthSamples.load (std::memory_order_acquire);
        const int remaining = targetLen - irWritePos;
        const int toCopy = juce::jlimit (0, remaining, numSamples);

        for (int ch = 0; ch < numChannels; ++ch)
            irOriginalBuffers[irWriteBufferIndex].copyFrom (ch, irWritePos, dryBuffer, ch, 0, toCopy);

        irWritePos += toCopy;

        if (irWritePos >= targetLen)
            finishRecordingAndRequestRebuild();

        // pass-through (ignore mix when IR is empty/recording)
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
    }
    else if (state == RunState::convolving && activeBank != nullptr)
    {
        // Wet path in-place.
        for (int ch = 0; ch < numChannels; ++ch)
            activeBank->processChannelReplacing (buffer, ch, 0, numSamples);

        if (fadingOutBank != nullptr && crossfadeRemainingSamples > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                oldWetBuffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
            for (int ch = 0; ch < numChannels; ++ch)
                fadingOutBank->processChannelReplacing (oldWetBuffer, ch, 0, numSamples);

            const int fadeSamplesThisBlock = juce::jmin (crossfadeRemainingSamples, numSamples);
            const int fadeStart = crossfadeTotalSamples - crossfadeRemainingSamples;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* wetNew = buffer.getWritePointer (ch);
                auto* wetOld = oldWetBuffer.getReadPointer (ch);

                for (int i = 0; i < fadeSamplesThisBlock; ++i)
                {
                    const float a = (float) (fadeStart + i) / (float) crossfadeTotalSamples;
                    wetNew[i] = wetOld[i] * (1.0f - a) + wetNew[i] * a;
                }
            }

            crossfadeRemainingSamples -= fadeSamplesThisBlock;
            if (crossfadeRemainingSamples <= 0)
                fadingOutBank.reset();
        }

        const float mix = apvts.getRawParameterValue (ParamIDs::mix)->load();
        if (mix <= 0.0001f)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
        }
        else if (mix < 0.9999f)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                auto* dry = dryBuffer.getReadPointer (ch);
                juce::FloatVectorOperations::multiply (wet, mix, numSamples);
                juce::FloatVectorOperations::addWithMultiply (wet, dry, 1.0f - mix, numSamples);
            }
        }
    }
    else
    {
        // If we have IR content but no active convolver yet (e.g. just loaded from saved state),
        // keep passing through but make sure a rebuild is requested.
        if (state == RunState::convolving && irHasContent.load (std::memory_order_acquire) && activeBank == nullptr)
            requestRebuild();

        // Pass-through
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
        if (! irHasContent.load (std::memory_order_acquire))
            runStateAtomic.store ((int) RunState::passThroughEmpty, std::memory_order_release);
    }

    const float trimDb = apvts.getRawParameterValue (ParamIDs::trimDb)->load();
    buffer.applyGain (juce::Decibels::decibelsToGain (trimDb));
}

void NewProjectAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused (newValue);

    if (parameterID == ParamIDs::fadeInPct || parameterID == ParamIDs::fadeOutPct)
    {
        lastIRShapeParamChangeMs.store (juce::Time::getMillisecondCounter(), std::memory_order_release);
        rebuildRequested.store (true, std::memory_order_release);
    }
}

void NewProjectAudioProcessor::triggerOneShotRecord()
{
    recordRequested.store (true, std::memory_order_release);
}

void NewProjectAudioProcessor::clearIR()
{
    clearRequested.store (true, std::memory_order_release);
}

NewProjectAudioProcessor::RunState NewProjectAudioProcessor::getRunState() const
{
    return static_cast<RunState> (runStateAtomic.load (std::memory_order_acquire));
}

juce::String NewProjectAudioProcessor::getStatusText() const
{
    switch (getRunState())
    {
        case RunState::recording:        return "Recording";
        case RunState::convolving:       return "Convolving";
        case RunState::passThroughEmpty: return "Pass-through";
        default:                         return "Pass-through";
    }
}

void NewProjectAudioProcessor::resetForLayoutOrClear (bool keepParameters)
{
    juce::ignoreUnused (keepParameters);

    irGeneration.fetch_add (1, std::memory_order_acq_rel);
    irHasContent.store (false, std::memory_order_release);
    irNumChannels.store (0, std::memory_order_release);
    irLengthSamples.store (0, std::memory_order_release);
    irWritePos = 0;

    {
        const juce::SpinLock::ScopedLockType lock (pendingBankLock);
        pendingBank.reset();
    }

    fadingOutBank.reset();
    activeBank.reset();
    crossfadeRemainingSamples = 0;

    runStateAtomic.store ((int) RunState::passThroughEmpty, std::memory_order_release);
}

void NewProjectAudioProcessor::startRecording (int numChannels, int recordLengthSamples)
{
    irGeneration.fetch_add (1, std::memory_order_acq_rel);

    const int clampedChannels = juce::jlimit (1, 16, numChannels);
    const int clampedLen = juce::jlimit (1, irAllocatedSamples, recordLengthSamples);

    irHasContent.store (false, std::memory_order_release);
    irNumChannels.store (clampedChannels, std::memory_order_release);
    irLengthSamples.store (clampedLen, std::memory_order_release);
    irWritePos = 0;

    for (int ch = 0; ch < clampedChannels; ++ch)
        irOriginalBuffers[irWriteBufferIndex].clear (ch, 0, clampedLen);

    // Clear existing convolution; we'll rebuild after recording.
    {
        const juce::SpinLock::ScopedLockType lock (pendingBankLock);
        pendingBank.reset();
    }
    activeBank.reset();
    fadingOutBank.reset();
    crossfadeRemainingSamples = 0;

    runStateAtomic.store ((int) RunState::recording, std::memory_order_release);
}

void NewProjectAudioProcessor::finishRecordingAndRequestRebuild()
{
    // Publish the freshly recorded buffer for the rebuild thread to read.
    irReadBufferIndex.store (irWriteBufferIndex, std::memory_order_release);
    irWriteBufferIndex = 1 - irWriteBufferIndex;

    irRecordedSampleRate.store (getSampleRate(), std::memory_order_release);

    irHasContent.store (true, std::memory_order_release);

    // Safety: immediately drop to fully dry before convolution starts.
    if (auto* mixParam = apvts.getParameter (ParamIDs::mix))
        mixParam->setValueNotifyingHost (0.0f);

    runStateAtomic.store ((int) RunState::convolving, std::memory_order_release);
    requestRebuild();
}

void NewProjectAudioProcessor::requestRebuild()
{
    const auto gen = irGeneration.load (std::memory_order_acquire);
    rebuildThread.request (gen);
}

void NewProjectAudioProcessor::trySwapInPendingBank()
{
    if (rebuildRequested.load (std::memory_order_acquire) && irHasContent.load (std::memory_order_acquire))
    {
        // Debounce IR rebuilds while the user is dragging fade sliders.
        constexpr juce::uint32 debounceMs = 80;
        const auto now = juce::Time::getMillisecondCounter();
        const auto last = lastIRShapeParamChangeMs.load (std::memory_order_acquire);
        if ((juce::uint32) (now - last) >= debounceMs)
        {
            rebuildRequested.store (false, std::memory_order_release);
            requestRebuild();
        }
    }

    if (! pendingBankLock.tryEnter())
        return;

    std::shared_ptr<ConvolutionBank> newBank;
    newBank.swap (pendingBank);
    pendingBankLock.exit();

    if (newBank == nullptr)
        return;

    if (activeBank != nullptr)
    {
        fadingOutBank = activeBank;
        crossfadeRemainingSamples = crossfadeTotalSamples;
    }

    activeBank = std::move (newBank);
}

//==============================================================================
bool NewProjectAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* NewProjectAudioProcessor::createEditor()
{
    return new NewProjectAudioProcessorEditor (*this);
}

//==============================================================================
void NewProjectAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Custom binary format:
    // [u32 magic 'LMCV'][u32 version]
    // [u32 xmlSize][xml bytes]
    // [u8 hasIR]
    //   if hasIR: [i32 ch][i32 samples][f64 irSampleRate][u32 gzipSize][gzip bytes]

    constexpr juce::uint32 magic = 0x56434D4C; // 'LMCV' little-endian
    constexpr juce::uint32 version = 1;

    auto state = apvts.copyState();
    auto xml = state.createXml();
    const auto xmlString = (xml != nullptr ? xml->toString() : juce::String());
    const auto xmlBytes = xmlString.toRawUTF8();
    const auto xmlSize = (juce::uint32) std::strlen (xmlBytes);

    juce::MemoryOutputStream mo (destData, false);
    mo.writeInt ((int) magic);
    mo.writeInt ((int) version);
    mo.writeInt ((int) xmlSize);
    mo.write (xmlBytes, xmlSize);

    // Only persist the last completed IR (not an in-progress recording)
    const bool hasIR = irHasContent.load (std::memory_order_acquire) && getRunState() == RunState::convolving;
    mo.writeByte (hasIR ? 1 : 0);

    if (! hasIR)
        return;

    const auto startGen = irGeneration.load (std::memory_order_acquire);
    const int readIndex = irReadBufferIndex.load (std::memory_order_acquire);
    const int numChannels = irNumChannels.load (std::memory_order_acquire);
    const int numSamples = irLengthSamples.load (std::memory_order_acquire);
    const double irSR = irRecordedSampleRate.load (std::memory_order_acquire);

    if (numChannels <= 0 || numSamples <= 0)
    {
        mo.writeByte (0);
        return;
    }

    const auto& irSource = irOriginalBuffers[juce::jlimit (0, 1, readIndex)];

    // Serialize planar floats then gzip-compress.
    juce::MemoryBlock raw;
    {
        juce::MemoryOutputStream rawOut (raw, false);
        for (int ch = 0; ch < numChannels; ++ch)
            rawOut.write (irSource.getReadPointer (ch), (size_t) numSamples * sizeof (float));
    }

    // If state changed while we were copying, avoid writing a partial/incorrect IR.
    if (irGeneration.load (std::memory_order_acquire) != startGen)
    {
        mo.writeByte (0);
        return;
    }

    juce::MemoryBlock gz;
    {
        juce::MemoryOutputStream gzOut (gz, false);
        juce::GZIPCompressorOutputStream gzip (gzOut, 9);
        gzip.write (raw.getData(), raw.getSize());
    }

    mo.writeInt (numChannels);
    mo.writeInt (numSamples);
    mo.writeDouble (irSR);
    mo.writeInt ((int) gz.getSize());
    mo.write (gz.getData(), gz.getSize());
}

void NewProjectAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    constexpr juce::uint32 magic = 0x56434D4C; // 'LMCV'
    constexpr juce::uint32 version = 1;

    juce::MemoryInputStream mi (data, (size_t) sizeInBytes, false);
    const auto maybeMagic = (juce::uint32) mi.readInt();

    if (maybeMagic != magic)
    {
        // Backwards compatibility with JUCE's XML binary state.
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        return;
    }

    const auto fileVersion = (juce::uint32) mi.readInt();
    if (fileVersion != version)
        return;

    const auto xmlSize = (juce::uint32) mi.readInt();
    if (xmlSize > 0 && xmlSize < (juce::uint32) sizeInBytes)
    {
        juce::MemoryBlock xmlBlock;
        xmlBlock.setSize (xmlSize);
        mi.read (xmlBlock.getData(), xmlSize);
        const juce::String xmlString ((const char*) xmlBlock.getData(), (int) xmlSize);
        if (auto xml = juce::XmlDocument::parse (xmlString))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    const bool hasIR = mi.readByte() != 0;
    if (! hasIR)
    {
        resetForLayoutOrClear (true);
        return;
    }

    const int numChannels = mi.readInt();
    const int numSamples = mi.readInt();
    const double irSR = mi.readDouble();
    const auto gzSize = (juce::uint32) mi.readInt();

    if (numChannels <= 0 || numChannels > 16 || numSamples <= 0 || gzSize == 0)
        return;

    juce::MemoryBlock gz;
    gz.setSize (gzSize);
    mi.read (gz.getData(), gzSize);

    // Decompress into raw float data
    juce::MemoryInputStream gzIn (gz.getData(), gz.getSize(), false);
    juce::GZIPDecompressorInputStream unzip (gzIn);

    juce::MemoryBlock raw;
    {
        juce::MemoryOutputStream rawOut (raw, false);
        rawOut.writeFromInputStream (unzip, -1);
    }

    const size_t expectedRawBytes = (size_t) numChannels * (size_t) numSamples * sizeof (float);
    if (raw.getSize() < expectedRawBytes)
        return;

    // Reset runtime DSP state but keep parameters.
    resetForLayoutOrClear (true);

    // Ensure buffers exist even if prepareToPlay hasn't run yet.
    irAllocatedChannels = 16;
    irAllocatedSamples = juce::jmax (irAllocatedSamples, numSamples);
    for (auto& b : irOriginalBuffers)
        b.setSize (irAllocatedChannels, irAllocatedSamples, false, false, true);

    irWriteBufferIndex = 1;
    irReadBufferIndex.store (0, std::memory_order_release);
    irRecordedSampleRate.store (irSR, std::memory_order_release);

    irNumChannels.store (numChannels, std::memory_order_release);
    irLengthSamples.store (numSamples, std::memory_order_release);
    irHasContent.store (true, std::memory_order_release);
    runStateAtomic.store ((int) RunState::convolving, std::memory_order_release);

    // Copy planar float data into the read buffer.
    auto* rawPtr = (const float*) raw.getData();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        irOriginalBuffers[0].copyFrom (ch, 0, rawPtr + ((size_t) ch * (size_t) numSamples), numSamples);
    }

    // Trigger a rebuild using a new generation.
    const auto newGen = irGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    rebuildThread.request (newGen);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NewProjectAudioProcessor();
}
