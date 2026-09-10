#pragma once

#include "Support.h"

namespace live
{
/** The one effect the engine does not already have.

    Everything else the mixer offers — EQ, compression and limiting, delay, chorus,
    reverb — is a built-in Tracktion plugin. Saturation is a drive into a soft clip
    with a wet/dry blend and make-up gain, which is enough to warm a bus without
    pulling in a whole plugin library for it.
*/
class SaturationPlugin final : public te::Plugin
{
public:
    static const char* getPluginName() { return NEEDS_TRANS ("Saturation"); }
    static const char* xmlTypeName;

    explicit SaturationPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        auto* undo = getUndoManager();
        driveValue.referTo (state, "drive", undo, 2.0f);
        mixValue.referTo (state, "mix", undo, 1.0f);
        outputValue.referTo (state, "output", undo, 0.0f);

        driveParam = addParam ("drive", TRANS ("Drive"), { 1.0f, 24.0f });
        mixParam = addParam ("mix", TRANS ("Mix"), { 0.0f, 1.0f });
        outputParam = addParam ("output", TRANS ("Output"), { -24.0f, 12.0f });

        driveParam->attachToCurrentValue (driveValue);
        mixParam->attachToCurrentValue (mixValue);
        outputParam->attachToCurrentValue (outputValue);
    }

    ~SaturationPlugin() override
    {
        notifyListenersOfDeletion();
        driveParam->detachFromCurrentValue();
        mixParam->detachFromCurrentValue();
        outputParam->detachFromCurrentValue();
    }

    String getName() const override                  { return getPluginName(); }
    String getPluginType() override                  { return xmlTypeName; }
    String getSelectableDescription() override       { return getName(); }

    void initialise (const te::PluginInitialisationInfo&) override {}
    void deinitialise() override {}

    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }

    void applyToBuffer (const te::PluginRenderContext& context) override
    {
        if (! isEnabled() || context.destBuffer == nullptr)
            return;

        const auto drive = std::max (1.0f, driveValue.get());
        const auto mix = jlimit (0.0f, 1.0f, mixValue.get());
        const auto output = Decibels::decibelsToGain (outputValue.get());
        // Dividing by tanh(drive) keeps the level roughly steady as drive comes up,
        // so the control changes the tone rather than just the volume.
        const auto normalise = 1.0f / std::tanh (drive);

        for (int channel = 0; channel < context.destBuffer->getNumChannels(); ++channel)
        {
            auto* samples = context.destBuffer->getWritePointer (channel, context.bufferStartSample);

            for (int i = 0; i < context.bufferNumSamples; ++i)
            {
                const auto dry = samples[i];
                const auto wet = std::tanh (drive * dry) * normalise;
                samples[i] = (dry + (wet - dry) * mix) * output;
            }
        }
    }

    void restorePluginStateFromValueTree (const ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, driveValue, mixValue, outputValue);

        for (auto* parameter : getAutomatableParameters())
            parameter->updateFromAttachedValue();
    }

    CachedValue<float> driveValue, mixValue, outputValue;
    te::AutomatableParameter::Ptr driveParam, mixParam, outputParam;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SaturationPlugin)
};

inline const char* SaturationPlugin::xmlTypeName = "coComposeSaturation";
}
