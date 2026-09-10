#pragma once

#include "Model.h"

namespace live
{
/** Recording and rendering.

    Arming a channel points every enabled input device at its track. Recording then
    goes through the transport as usual, and whatever lands on the track is folded
    back into the model as a pattern the arrangement plays, so a take is edited like
    anything else rather than living outside the model.
*/
class Recorder
{
public:
    explicit Recorder (Model& m) : model (m) {}

    //==============================================================================
    /** Points the enabled inputs at a channel's track, or clears them. MIDI inputs go
        to any channel; audio inputs only to one that can hold a wave clip. */
    bool arm (const String& channelID, bool shouldArm)
    {
        auto channel = model.channelFor (channelID);
        auto* track = model.trackFor (channelID);
        if (! channel.isValid() || track == nullptr)
            return false;

        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction (shouldArm ? "Arm channel" : "Disarm channel");
        channel.setProperty (ids::arm, shouldArm, &undo);

        for (auto* instance : model.edit.getAllInputDevices())
        {
            if (! instance->getInputDevice().isEnabled())
                continue;

            if (shouldArm)
            {
                if (! instance->setTarget (track->itemID, true, &undo).has_value())
                    continue;

                instance->setRecordingEnabled (track->itemID, true);
            }
            else
            {
                instance->setRecordingEnabled (track->itemID, false);
                [[maybe_unused]] const auto removed = instance->removeTarget (track->itemID, &undo);
            }
        }

        return true;
    }

    bool isArmed (const String& channelID) const
    {
        return static_cast<bool> (model.channelFor (channelID).getProperty (ids::arm, false));
    }

    StringArray armedChannels() const
    {
        StringArray armed;
        for (auto channel : model.channels())
            if (static_cast<bool> (channel.getProperty (ids::arm, false)))
                armed.add (Model::uidOf (channel));
        return armed;
    }

    /** Counts in with the click before dropping into record, which is what makes an
        unaccompanied first take playable. */
    void setCountIn (int bars) { countInBars = jlimit (0, 4, bars); }
    int getCountIn() const { return countInBars; }

    bool startRecording()
    {
        if (armedChannels().isEmpty())
            return false;

        auto& transport = model.edit.getTransport();
        const auto wasClicking = model.edit.clickTrackEnabled.get();

        if (countInBars > 0)
        {
            model.edit.clickTrackEnabled = true;
            model.edit.setCountInMode (te::Edit::CountIn::oneBar);
        }
        else
        {
            model.edit.setCountInMode (te::Edit::CountIn::none);
        }

        clickWasEnabled = wasClicking;
        transport.record (false);
        return true;
    }

    /** Stops, then turns whatever was recorded into pattern notes or an audio clip so
        the arrangement owns the take. */
    String stopRecording()
    {
        auto& transport = model.edit.getTransport();
        if (! transport.isRecording())
            return {};

        transport.stop (false, false);
        model.edit.clickTrackEnabled = clickWasEnabled;

        return keepTakes();
    }

    /** Folds whatever is sitting on the armed channels into the model. Stopping a
        recording does this; it is also how a check can exercise the fold-back without
        a MIDI keyboard plugged in. */
    String keepTakes()
    {
        int captured = 0;
        for (const auto& channelID : armedChannels())
            captured += adoptTake (channelID);

        model.render();
        return captured == 0 ? "Nothing was recorded"
                             : "Recorded " + String (captured) + (captured == 1 ? " take" : " takes");
    }

    /** Puts a clip on an armed channel's track the way a recording leaves one: no
        model tag, so the fold-back has to claim it. Diagnostic only. */
    bool simulateTake (const String& channelID, double startBeat, double lengthBeats,
                       const Array<int>& pitches)
    {
        auto* track = model.trackFor (channelID);
        if (track == nullptr || pitches.isEmpty())
            return false;

        auto& tempo = model.edit.tempoSequence;
        const te::TimeRange range { tempo.toTime (te::BeatPosition::fromBeats (startBeat)),
                                    tempo.toTime (te::BeatPosition::fromBeats (startBeat + lengthBeats)) };

        auto* clip = track->insertMIDIClip ("Recording", range, nullptr).get();
        if (clip == nullptr)
            return false;

        auto& sequence = clip->getSequence();
        const auto step = lengthBeats / pitches.size();

        for (int i = 0; i < pitches.size(); ++i)
            sequence.addNote (pitches[i], te::BeatPosition::fromBeats (i * step),
                              te::BeatDuration::fromBeats (step * 0.8), 100, 0, nullptr);

        return true;
    }

private:
    /** A recorded clip arrives on the engine track untagged. Move its notes into a new
        pattern placed where the take happened; leave a wave take as an audio clip. */
    int adoptTake (const String& channelID)
    {
        auto* track = model.trackFor (channelID);
        auto channel = model.channelFor (channelID);
        if (track == nullptr || ! channel.isValid())
            return 0;

        auto& undo = model.edit.getUndoManager();
        auto lane = firstLane (&undo);
        int captured = 0;

        const auto clips = track->getClips();
        for (auto* base : clips)
        {
            if (base->state.hasProperty (ids::clipInstance) || base->state.hasProperty (ids::clipAudio))
                continue;

            auto& tempo = model.edit.tempoSequence;
            const auto range = base->getPosition().time;
            const auto startBeat = tempo.toBeats (range.getStart()).inBeats();
            const auto lengthBeats = std::max (0.25, tempo.toBeats (range.getEnd()).inBeats() - startBeat);

            if (auto* midi = dynamic_cast<te::MidiClip*> (base))
            {
                undo.beginNewTransaction ("Keep recorded take");
                auto pattern = model.addPattern ("Take " + String (model.patterns().getNumChildren() + 1),
                                                 lengthBeats, &undo);
                auto sequence = model.sequenceFor (pattern, channelID, &undo);

                for (auto* note : midi->getSequence().getNotes())
                    model.addNote (sequence, note->getNoteNumber(), note->getStartBeat().inBeats(),
                                   std::max (0.03125, note->getLengthBeats().inBeats()),
                                   note->getVelocity(), &undo);

                auto instance = model.addInstance (Model::uidOf (lane), Model::uidOf (pattern), startBeat, &undo);
                instance.setProperty (ids::length, lengthBeats, &undo);
                base->removeFromParent();
                ++captured;
            }
            else if (auto* wave = dynamic_cast<te::WaveAudioClip*> (base))
            {
                const auto source = wave->getSourceFileReference().getFile();
                if (! source.existsAsFile())
                    continue;

                undo.beginNewTransaction ("Keep recorded take");
                auto clip = model.addAudioClip (Model::uidOf (lane), source, startBeat, lengthBeats, &undo);
                clip.setProperty (ids::name, "Take " + source.getFileNameWithoutExtension(), &undo);
                base->removeFromParent();
                ++captured;
            }
        }

        return captured;
    }

    ValueTree firstLane (UndoManager* undo)
    {
        if (model.lanes().getNumChildren() > 0)
            return model.lanes().getChild (0);
        return model.addLane ("Takes", undo);
    }

    Model& model;
    int countInBars = 1;
    bool clickWasEnabled = false;
};

//==============================================================================
/** Writes the arrangement, or a range of it, to WAV — as one mix or as one file per
    channel.

    Rendering takes the playback graph over and waits for the audio devices to stop,
    which cannot be done from the message thread without deadlocking it. So a render
    runs on its own thread and the result is collected on the next UI tick.
*/
class Exporter final : private juce::Thread
{
public:
    explicit Exporter (Model& m) : juce::Thread ("CoCompose render"), model (m) {}

    ~Exporter() override { stopThread (30000); }

    struct Result
    {
        StringArray files;
        String message;
    };

    bool isBusy() const { return isThreadRunning(); }

    bool startMix (const File& destination, te::TimeRange range)
    {
        if (isBusy() || range.getLength().inSeconds() <= 0.0)
            return false;

        target = destination.hasFileExtension ("wav") ? destination : destination.withFileExtension ("wav");
        renderRange = range;
        stems = false;
        startThread();
        return true;
    }

    bool startStems (const File& folder, te::TimeRange range)
    {
        if (isBusy() || range.getLength().inSeconds() <= 0.0)
            return false;

        target = folder;
        renderRange = range;
        stems = true;
        startThread();
        return true;
    }

    /** The result of the last finished render, once. */
    std::optional<Result> takeResult()
    {
        const juce::ScopedLock lock (resultLock);

        if (! finished)
            return {};

        finished = false;
        return result;
    }

    /** The whole arrangement, or at least a bar of it. */
    te::TimeRange arrangementRange() const
    {
        auto& tempo = model.edit.tempoSequence;
        double end = 0.0;
        for (auto clip : model.instances())
            end = std::max (end, static_cast<double> (clip[ids::start]) + static_cast<double> (clip[ids::length]));

        return { te::TimePosition(), tempo.toTime (te::BeatPosition::fromBeats (std::max (4.0, end))) };
    }

private:
    void run() override
    {
        Result outcome;

        if (stems)
        {
            if (! target.createDirectory().wasOk())
            {
                outcome.message = "Could not create " + target.getFullPathName();
            }
            else
            {
                for (auto channel : model.channels())
                {
                    auto* track = model.trackFor (Model::uidOf (channel));
                    if (track == nullptr || threadShouldExit())
                        continue;

                    // A channel plays into its insert and onwards, so a stem has to carry
                    // that chain with it or the render comes out silent.
                    juce::BigInteger tracks;
                    addTrack (tracks, track);
                    addOutputChain (tracks, static_cast<int> (channel[ids::insert]));

                    auto file = target.getChildFile (File::createLegalFileName (channel[ids::name].toString()) + ".wav");
                    if (render (file, tracks))
                        outcome.files.add (file.getFullPathName());
                }

                outcome.message = outcome.files.isEmpty() ? "Nothing to render"
                                                          : "Rendered " + String (outcome.files.size()) + " stems";
            }
        }
        else
        {
            // An empty set renders nothing in this overload, so name every track.
            juce::BigInteger tracks;
            const auto allTracks = te::getAllTracks (model.edit);
            for (int i = 0; i < allTracks.size(); ++i)
                tracks.setBit (i);

            if (render (target, tracks))
            {
                outcome.files.add (target.getFullPathName());
                outcome.message = "Rendered " + target.getFileName();
            }
            else
            {
                outcome.message = "Could not write " + target.getFileName();
            }
        }

        const juce::ScopedLock lock (resultLock);
        result = outcome;
        finished = true;
    }

    void addTrack (juce::BigInteger& tracks, te::Track* track) const
    {
        const auto index = te::getAllTracks (model.edit).indexOf (track);
        if (index >= 0)
            tracks.setBit (index);
    }

    /** Follows an insert's routing to the master, marking every bus on the way. */
    void addOutputChain (juce::BigInteger& tracks, int slot) const
    {
        auto insert = model.insertForSlot (slot);
        StringArray seen;

        while (insert.isValid() && ! seen.contains (Model::uidOf (insert)))
        {
            seen.add (Model::uidOf (insert));
            addTrack (tracks, model.trackFor (Model::insertTrackID (Model::uidOf (insert))));

            const auto destination = insert.getProperty (ids::output, masterInsert).toString();
            if (destination == masterInsert)
                break;

            insert = model.insertFor (destination);
        }
    }

    bool render (const File& file, const juce::BigInteger& tracks)
    {
        file.deleteFile();
        return te::Renderer::renderToFile ("Render", file, model.edit, renderRange, tracks,
                                           true, true, {}, false);
    }

    Model& model;
    File target;
    te::TimeRange renderRange;
    bool stems = false, finished = false;
    Result result;
    juce::CriticalSection resultLock;
};
}
