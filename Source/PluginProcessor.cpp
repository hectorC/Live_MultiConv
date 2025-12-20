/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::AudioBuffer<float> resampleBuffer (const juce::AudioBuffer<float>& src,
                                            double srcSampleRate,
                                            double dstSampleRate,
                                            int maxDstSamples)
    {
        if (dstSampleRate <= 0.0 || srcSampleRate <= 0.0 || src.getNumSamples() <= 0)
            return src;

        if (std::abs (dstSampleRate - srcSampleRate) < 1.0e-9)
            return src;

        const double ratio = srcSampleRate / dstSampleRate;
        const int srcSamples = src.getNumSamples();
        const int dstSamplesUnclamped = (int) std::llround ((double) srcSamples * dstSampleRate / srcSampleRate);
        const int dstSamples = juce::jlimit (1, juce::jmax (1, maxDstSamples), dstSamplesUnclamped);

        const int numChannels = src.getNumChannels();
        juce::AudioBuffer<float> dst (numChannels, dstSamples);

        // Pad input a little so the interpolator can safely read past the end.
        juce::AudioBuffer<float> padded (numChannels, srcSamples + 8);
        padded.clear();
        for (int ch = 0; ch < numChannels; ++ch)
            padded.copyFrom (ch, 0, src, ch, 0, srcSamples);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.reset();
            interp.process (ratio, padded.getReadPointer (ch), dst.getWritePointer (ch), dstSamples);
        }

        return dst;
    }
}

namespace ParamIDs
{
    static constexpr auto recordLengthMs = "recordLengthMs";
    static constexpr auto processChannels = "processChannels";
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
    apvts.addParameterListener (ParamIDs::processChannels, this);
    apvts.addParameterListener (ParamIDs::fadeInPct, this);
    apvts.addParameterListener (ParamIDs::fadeOutPct, this);
}

NewProjectAudioProcessor::~NewProjectAudioProcessor()
{
    apvts.removeParameterListener (ParamIDs::processChannels, this);
    apvts.removeParameterListener (ParamIDs::fadeInPct, this);
    apvts.removeParameterListener (ParamIDs::fadeOutPct, this);
}

juce::AudioProcessorValueTreeState::ParameterLayout NewProjectAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        ParamIDs::processChannels,
        "Process Channels",
        1,
        16,
        16));

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
    currentIOChannels.store (0, std::memory_order_release);
    convolutionBankChannels.store (0, std::memory_order_release);

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
    currentIOChannels.store (juce::jlimit (0, 16, lastKnownNumChannels), std::memory_order_release);

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

    const int ioChannels = juce::jmin (16, juce::jmin (totalNumInputChannels, totalNumOutputChannels));
    currentIOChannels.store (juce::jlimit (0, 16, ioChannels), std::memory_order_release);

    const int requestedCh = (int) std::llround (apvts.getRawParameterValue (ParamIDs::processChannels)->load());
    requestedProcessChannels.store (juce::jlimit (1, 16, requestedCh), std::memory_order_release);
    const int processCh = juce::jlimit (1, juce::jmax (1, ioChannels), requestedCh);

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
    if (ioChannels != lastKnownNumChannels)
    {
        lastKnownNumChannels = ioChannels;
        resetForLayoutOrClear (true);
    }

    if (recordRequested.exchange (false, std::memory_order_acq_rel))
    {
        const float ms = apvts.getRawParameterValue (ParamIDs::recordLengthMs)->load();
        const int recordSamples = juce::jmin (irAllocatedSamples, msToSamples (getSampleRate(), ms));
        startRecording (processCh, recordSamples);
    }

    // Swap in a freshly rebuilt convolver bank if available.
    trySwapInPendingBank();

    // Always capture dry input for mixing/recording.
    for (int ch = 0; ch < ioChannels; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    const auto state = getRunState();

    // Recording: fill IR buffer and pass-through.
    if (state == RunState::recording)
    {
        const int targetLen = irLengthSamples.load (std::memory_order_acquire);
        const int remaining = targetLen - irWritePos;
        const int toCopy = juce::jlimit (0, remaining, numSamples);

        for (int ch = 0; ch < processCh; ++ch)
            irOriginalBuffers[irWriteBufferIndex].copyFrom (ch, irWritePos, dryBuffer, ch, 0, toCopy);

        irWritePos += toCopy;

        if (irWritePos >= targetLen)
            finishRecordingAndRequestRebuild();

        // pass-through (ignore mix when IR is empty/recording)
        for (int ch = 0; ch < ioChannels; ++ch)
            buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
    }
    else if (state == RunState::convolving && activeBank != nullptr)
    {
        const int convCh = juce::jlimit (0,
                                         ioChannels,
                                         juce::jmin (processCh, (int) activeBank->convolvers.size()));

        // Wet path in-place.
        for (int ch = 0; ch < convCh; ++ch)
            activeBank->processChannelReplacing (buffer, ch, 0, numSamples);

        // Pass-through remaining channels.
        for (int ch = convCh; ch < ioChannels; ++ch)
            buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);

        if (fadingOutBank != nullptr && crossfadeRemainingSamples > 0)
        {
            for (int ch = 0; ch < convCh; ++ch)
                oldWetBuffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
            for (int ch = 0; ch < convCh; ++ch)
                fadingOutBank->processChannelReplacing (oldWetBuffer, ch, 0, numSamples);

            const int fadeSamplesThisBlock = juce::jmin (crossfadeRemainingSamples, numSamples);
            const int fadeStart = crossfadeTotalSamples - crossfadeRemainingSamples;

            for (int ch = 0; ch < convCh; ++ch)
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
            for (int ch = 0; ch < convCh; ++ch)
                buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
        }
        else if (mix < 0.9999f)
        {
            for (int ch = 0; ch < convCh; ++ch)
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
        for (int ch = 0; ch < ioChannels; ++ch)
            buffer.copyFrom (ch, 0, dryBuffer, ch, 0, numSamples);
        if (! irHasContent.load (std::memory_order_acquire))
            runStateAtomic.store ((int) RunState::passThroughEmpty, std::memory_order_release);
    }

    const float trimDb = apvts.getRawParameterValue (ParamIDs::trimDb)->load();
    buffer.applyGain (juce::Decibels::decibelsToGain (trimDb));
}

void NewProjectAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    if (parameterID == ParamIDs::processChannels)
    {
        requestedProcessChannels.store (juce::jlimit (1, 16, (int) std::llround (newValue)), std::memory_order_release);
        clearRequested.store (true, std::memory_order_release);
        runStateAtomic.store ((int) RunState::passThroughEmpty, std::memory_order_release);
        return;
    }

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
    setLastIRFilename ({});
}

void NewProjectAudioProcessor::setLastIRFilename (juce::String fileNameOnly)
{
    const juce::ScopedLock lock (lastIRFilenameLock);
    lastIRFilename = std::move (fileNameOnly);
}

juce::String NewProjectAudioProcessor::getLastIRFilename() const
{
    const juce::ScopedLock lock (lastIRFilenameLock);
    return lastIRFilename;
}

void NewProjectAudioProcessor::syncLastIRFilenameFromState()
{
    const auto v = apvts.state.getProperty ("irLastFilename");
    setLastIRFilename (v.toString());
}

bool NewProjectAudioProcessor::saveIRToWavFile (const juce::File& file, juce::String& errorMessage) const
{
    if (! irHasContent.load (std::memory_order_acquire))
    {
        errorMessage = "IR buffer is empty.";
        return false;
    }

    const int numChannels = irNumChannels.load (std::memory_order_acquire);
    const int numSamples = irLengthSamples.load (std::memory_order_acquire);
    const int readIndex = irReadBufferIndex.load (std::memory_order_acquire);

    if (numChannels <= 0 || numSamples <= 0)
    {
        errorMessage = "IR buffer is empty.";
        return false;
    }

    auto outFile = file;
    if (outFile.getFileExtension().isEmpty())
        outFile = outFile.withFileExtension ("wav");

    auto stream = std::make_unique<juce::FileOutputStream> (outFile);
    if (! stream->openedOk())
    {
        errorMessage = "Could not open file for writing.";
        return false;
    }

    // Copy into a local buffer first (avoids holding references while writing to disk).
    juce::AudioBuffer<float> temp (numChannels, numSamples);
    const auto& src = irOriginalBuffers[juce::jlimit (0, 1, readIndex)];
    for (int ch = 0; ch < numChannels; ++ch)
        temp.copyFrom (ch, 0, src, ch, 0, numSamples);

    // Requirement: project sample rate and 32-bit.
    double sr = getSampleRate();
    if (sr <= 0.0)
        sr = perChannelSpec.sampleRate;
    if (sr <= 0.0)
        sr = irRecordedSampleRate.load (std::memory_order_acquire);
    if (sr <= 0.0)
        sr = 44100.0;

    juce::WavAudioFormat wav;
    juce::StringPairArray metadata;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.release(),
                                                                           sr,
                                                                           (unsigned int) numChannels,
                                                                           32,
                                                                           metadata,
                                                                           0));
    if (writer == nullptr)
    {
        errorMessage = "Could not create WAV writer.";
        return false;
    }

    if (! writer->writeFromAudioSampleBuffer (temp, 0, numSamples))
    {
        errorMessage = "Failed writing audio data.";
        return false;
    }

    return true;
}

bool NewProjectAudioProcessor::loadIRFromAudioFile (const juce::File& file, juce::String& errorMessage)
{
    if (! file.existsAsFile())
    {
        errorMessage = "File does not exist.";
        return false;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        errorMessage = "Unsupported audio file.";
        return false;
    }

    const int fileChannels = (int) reader->numChannels;
    const int numChannels = juce::jlimit (1, 16, fileChannels);

    // Target: current project sample rate.
    double hostSR = getSampleRate();
    if (hostSR <= 0.0)
        hostSR = perChannelSpec.sampleRate;
    if (hostSR <= 0.0)
        hostSR = reader->sampleRate;

    const int maxIRSamples = (irAllocatedSamples > 0) ? irAllocatedSamples : msToSamples (hostSR, 2000.0f);

    const juce::int64 fileLen = reader->lengthInSamples;
    if (fileLen <= 0)
    {
        errorMessage = "Audio file is empty.";
        return false;
    }

    // Clamp file length to our maximum supported IR duration (2s) at the file's sample rate.
    const int maxSourceSamples = msToSamples (reader->sampleRate, 2000.0f);
    const int sourceSamples = (int) juce::jlimit<juce::int64> (1, (juce::int64) maxSourceSamples, fileLen);

    juce::AudioBuffer<float> fileBuf (numChannels, sourceSamples);
    fileBuf.clear();

    if (! reader->read (&fileBuf, 0, sourceSamples, 0, true, true))
    {
        errorMessage = "Failed reading audio file.";
        return false;
    }

    auto loaded = resampleBuffer (fileBuf, reader->sampleRate, hostSR, maxIRSamples);

    // Ensure allocations exist even if load happens before prepareToPlay().
    if (irAllocatedChannels != 16 || irAllocatedSamples != maxIRSamples)
    {
        irAllocatedChannels = 16;
        irAllocatedSamples = maxIRSamples;
        for (auto& b : irOriginalBuffers)
            b.setSize (irAllocatedChannels, irAllocatedSamples, true, true, true);
    }

    const int finalSamples = juce::jlimit (1, irAllocatedSamples, loaded.getNumSamples());

    // Publish the loaded IR safely.
    irGeneration.fetch_add (1, std::memory_order_acq_rel);
    irHasContent.store (false, std::memory_order_release);

    const int currentRead = irReadBufferIndex.load (std::memory_order_acquire);
    const int stagingIndex = 1 - juce::jlimit (0, 1, currentRead);

    // Copy into staging buffer.
    for (int ch = 0; ch < numChannels; ++ch)
        irOriginalBuffers[stagingIndex].copyFrom (ch, 0, loaded, ch, 0, finalSamples);
    for (int ch = numChannels; ch < 16; ++ch)
        irOriginalBuffers[stagingIndex].clear (ch, 0, finalSamples);

    irNumChannels.store (numChannels, std::memory_order_release);
    irLengthSamples.store (finalSamples, std::memory_order_release);
    irRecordedSampleRate.store (hostSR, std::memory_order_release);

    irReadBufferIndex.store (stagingIndex, std::memory_order_release);
    irWriteBufferIndex = 1 - stagingIndex;

    {
        const juce::SpinLock::ScopedLockType lock (pendingBankLock);
        pendingBank.reset();
    }
    activeBank.reset();
    fadingOutBank.reset();
    convolutionBankChannels.store (0, std::memory_order_release);
    crossfadeRemainingSamples = 0;

    // Safety: start dry, then user can blend back in.
    if (auto* mixParam = apvts.getParameter (ParamIDs::mix))
        mixParam->setValueNotifyingHost (0.0f);

    irHasContent.store (true, std::memory_order_release);
    runStateAtomic.store ((int) RunState::convolving, std::memory_order_release);
    requestRebuild();

    return true;
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
    convolutionBankChannels.store (0, std::memory_order_release);

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
    convolutionBankChannels.store ((int) activeBank->convolvers.size(), std::memory_order_release);
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
    state.setProperty ("irLastFilename", getLastIRFilename(), nullptr);
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
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            syncLastIRFilenameFromState();
        }
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
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            syncLastIRFilenameFromState();
        }
    }

    const bool hasIR = mi.readByte() != 0;
    if (! hasIR)
    {
        resetForLayoutOrClear (true);
        setLastIRFilename ({});
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

    // Stage the restored IR into the write buffer, then publish by swapping the read index.
    const int targetIndex = juce::jlimit (0, 1, irWriteBufferIndex);

    // Copy planar float data into the staging buffer.
    auto* rawPtr = (const float*) raw.getData();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        irOriginalBuffers[targetIndex].copyFrom (ch,
                                                 0,
                                                 rawPtr + ((size_t) ch * (size_t) numSamples),
                                                 numSamples);
    }

    irRecordedSampleRate.store (irSR, std::memory_order_release);
    irNumChannels.store (numChannels, std::memory_order_release);
    irLengthSamples.store (numSamples, std::memory_order_release);
    runStateAtomic.store ((int) RunState::convolving, std::memory_order_release);

    irReadBufferIndex.store (targetIndex, std::memory_order_release);
    irWriteBufferIndex = 1 - targetIndex;
    irHasContent.store (true, std::memory_order_release);

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
