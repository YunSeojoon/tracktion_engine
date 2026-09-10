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
        int revision = 0;
        bool complete = false;
    };

    bool isBusy() const { return isThreadRunning(); }

    bool startMix (const File& destination, te::TimeRange range, int revision)
    {
        if (! begin (range, revision))
            return false;

        target = destination.hasFileExtension ("wav") ? destination : destination.withFileExtension ("wav");
        stems = false;
        startThread();
        return true;
    }

    bool startStems (const File& folder, te::TimeRange range, int revision)
    {
        if (! begin (range, revision))
            return false;

        target = folder;
        stems = true;
        startThread();
        return true;
    }

    /** The result of the last finished render, once. Called on the message thread,
        which is also where the rendering Edit is let go. */
    std::optional<Result> takeResult()
    {
        Result outcome;

        {
            const juce::ScopedLock lock (resultLock);

            if (! finished)
                return {};

            finished = false;
            outcome = result;
        }

        waitForThreadToExit (-1);
        renderEdit.reset();
        return outcome;
    }

    /** Takes a copy of the project as it is right now. The render runs from that copy
        in an Edit of its own, so an edit made while it is running changes the next
        render rather than corrupting this one. */
    bool begin (te::TimeRange range, int revision)
    {
        if (isBusy() || range.getLength().inSeconds() <= 0.0)
            return false;

        renderRange = range;
        startedAtRevision = revision;

        // The copy is opened here, on the message thread, because that is where an Edit
        // is built and taken down. The worker only runs the render over it.
        renderEdit = te::loadEditFromState (model.edit.engine, model.edit.state.createCopy(),
                                            te::Edit::EditRole::forRendering);
        if (renderEdit == nullptr)
            return false;

        // The channels are what a stem is per, and the copy has no model wrapper, so
        // the plan is made here where the live model is safe to read.
        plannedStems.clear();
        StringArray taken;

        for (auto channel : model.channels())
        {
            auto* track = model.trackFor (Model::uidOf (channel));
            if (track == nullptr)
                continue;

            plannedStems.add ({ te::getAllTracks (model.edit).indexOf (track),
                                chainFor (static_cast<int> (channel[ids::insert])),
                                uniqueStemName (channel, taken) });
        }

        return true;
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
        outcome.revision = startedAtRevision;

        auto* rendering = renderEdit.get();

        if (rendering == nullptr)
        {
            outcome.message = "Could not prepare the project for rendering";
            publish (outcome);
            return;
        }

        if (stems)
        {
            if (! target.createDirectory().wasOk())
            {
                outcome.message = "Could not create " + target.getFullPathName();
            }
            else
            {
                int failed = 0;
                String firstProblem;

                for (const auto& stem : plannedStems)
                {
                    if (threadShouldExit())
                        break;

                    juce::BigInteger tracks;
                    for (auto index : stem.trackIndexes)
                        tracks.setBit (index);
                    tracks.setBit (stem.channelTrackIndex);

                    String problem;
                    if (render (*rendering, target.getChildFile (stem.fileName), tracks, problem))
                    {
                        outcome.files.add (target.getChildFile (stem.fileName).getFullPathName());
                    }
                    else
                    {
                        ++failed;
                        if (firstProblem.isEmpty())
                            firstProblem = problem;
                    }
                }

                outcome.complete = failed == 0 && ! threadShouldExit();
                outcome.message = outcome.files.isEmpty()
                                    ? (plannedStems.isEmpty() ? "Nothing to render"
                                                              : "Could not write any stems")
                                    : "Rendered " + String (outcome.files.size())
                                        + (outcome.files.size() == 1 ? " stem" : " stems")
                                        + (failed > 0 ? "; " + String (failed) + " failed" : "");

                if (failed > 0 && firstProblem.isNotEmpty())
                    outcome.message += " (" + firstProblem + ")";
            }
        }
        else
        {
            // An empty set renders nothing in this overload, so name every track.
            juce::BigInteger tracks;
            const auto allTracks = te::getAllTracks (*rendering);
            for (int i = 0; i < allTracks.size(); ++i)
                tracks.setBit (i);

            String problem;
            if (render (*rendering, target, tracks, problem))
            {
                outcome.files.add (target.getFullPathName());
                outcome.message = "Rendered " + target.getFileName();
                outcome.complete = true;
            }
            else
            {
                outcome.message = problem.isEmpty() ? "Could not write " + target.getFileName()
                                                    : "Could not write " + target.getFileName() + ": " + problem;
            }
        }

        publish (outcome);
    }

    void publish (const Result& outcome)
    {
        const juce::ScopedLock lock (resultLock);
        result = outcome;
        finished = true;
    }

    /** Everything the channel can be heard through: its insert, whatever that insert
        is routed to, and every insert it sends to, each with its own routing. A stem
        without them is missing the part of the sound that comes back from a bus.

        Wet buses are shared, so two stems can both carry the same return and their sum
        is not the mix. That is what a stem of a channel means here. */
    Array<int> chainFor (int slot) const
    {
        Array<int> indexes;
        StringArray seen;
        Array<ValueTree> queue;

        if (auto start = model.insertForSlot (slot); start.isValid())
            queue.add (start);

        while (! queue.isEmpty())
        {
            auto insert = queue.removeAndReturn (0);
            const auto insertID = Model::uidOf (insert);

            if (seen.contains (insertID))
                continue;

            seen.add (insertID);

            const auto index = te::getAllTracks (model.edit)
                                   .indexOf (model.trackFor (Model::insertTrackID (insertID)));
            if (index >= 0)
                indexes.add (index);

            const auto destination = insert.getProperty (ids::output, masterInsert).toString();
            if (destination != masterInsert)
                if (auto next = model.insertFor (destination); next.isValid())
                    queue.add (next);

            for (auto child : insert)
                if (child.hasType (ids::SEND))
                    if (auto sendTarget = model.insertFor (child[ids::target].toString()); sendTarget.isValid())
                        queue.add (sendTarget);
        }

        return indexes;
    }

    /** A file name per channel that cannot collide, whatever the channels are called. */
    static String uniqueStemName (ValueTree channel, StringArray& taken)
    {
        auto base = File::createLegalFileName (channel[ids::name].toString()).trim();
        if (base.isEmpty())
            base = "Channel";

        auto name = base;
        for (int suffix = 2; taken.contains (name.toLowerCase()); ++suffix)
            name = base + " " + String (suffix);

        taken.add (name.toLowerCase());
        return name + ".wav";
    }

    /** Renders beside the destination and only replaces it once there is a whole file
        to replace it with, so a failed export never costs the last good one.

        Two different things can go wrong and they are not the same. A render that did
        not finish leaves nothing worth keeping. A render that finished but could not
        take the destination's place is a whole file that cost real time, so it is kept
        and named rather than thrown away with the failure. */
    bool render (te::Edit& source, const File& file, const juce::BigInteger& tracks, String& problem)
    {
        if (file.isDirectory())
        {
            problem = file.getFileName() + " is a folder";
            return false;
        }

        auto working = file.getSiblingFile (file.getFileNameWithoutExtension()
                                              + "-rendering-" + Uuid().toString().substring (0, 8) + ".wav");
        working.deleteFile();

        const auto rendered = te::Renderer::renderToFile ("Render", working, source, renderRange, tracks,
                                                          true, true, {}, false);

        if (! rendered || ! working.existsAsFile() || working.getSize() == 0)
        {
            working.deleteFile();
            problem = "could not render " + file.getFileName();
            return false;
        }

        // replaceFileIn goes through ReplaceFile, which leaves the existing file where
        // it is when it cannot do the swap. moveFileTo deletes the destination first
        // and only then moves, so anything that goes wrong in between - or a process
        // that dies there - costs the last good render.
        if (! working.replaceFileIn (file))
        {
            problem = "could not replace " + file.getFileName()
                        + "; the new render is kept as " + working.getFileName();
            return false;
        }

        return true;
    }

    struct PlannedStem
    {
        int channelTrackIndex = -1;
        Array<int> trackIndexes;
        String fileName;
    };

    Model& model;
    File target;
    std::unique_ptr<te::Edit> renderEdit;
    Array<PlannedStem> plannedStems;
    te::TimeRange renderRange;
    int startedAtRevision = 0;
    bool stems = false, finished = false;
    Result result;
    juce::CriticalSection resultLock;
};
}
