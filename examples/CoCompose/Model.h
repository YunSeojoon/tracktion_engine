#pragma once

#include "Support.h"

namespace live
{
/** Pattern-centric project model.

    FL-style composition needs five things the engine does not distinguish on its own:
    a Channel (an instrument), a Pattern (notes for any number of channels), a Playlist
    lane, a Clip instance (one placement of a pattern) and a Mixer insert. They live in
    a COCOMPOSE tree inside the Edit, so they are saved, undone and synced with the rest
    of the session.

    Engine MIDI clips are derived from that model, never authored directly: one clip per
    (instance, channel with notes) pair, tagged with both ids. Editing a pattern therefore
    updates every placement of it, and duplicating a pattern makes one placement
    independent. Mixer inserts are modelled here but not yet routed; that is M5.
*/
namespace ids
{
    const Identifier COCOMPOSE ("COCOMPOSE"), CHANNELS ("CHANNELS"), CHANNEL ("CHANNEL"),
        PATTERNS ("PATTERNS"), PATTERN ("PATTERN"), SEQUENCE ("SEQUENCE"), NOTE ("NOTE"),
        PLAYLIST ("PLAYLIST"), LANES ("LANES"), LANE ("LANE"), CLIPS ("CLIPS"), INSTANCE ("INSTANCE"),
        MIXER ("MIXER"), INSERT ("INSERT");

    const Identifier uid ("id"), name ("name"), schema ("schema"), channel ("channel"),
        pattern ("pattern"), lane ("lane"), start ("start"), length ("length"), pitch ("pitch"),
        velocity ("velocity"), gainDb ("gainDb"), pan ("pan"), mute ("mute"), solo ("solo"),
        insert ("insert"), index ("index");

    // Written onto engine clips so a derived clip can be matched back to the model.
    const Identifier clipInstance ("coComposeInstance"), clipChannel ("coComposeChannel");
}

constexpr int modelSchema = 2;
constexpr double defaultPatternBeats = 16.0;

class Model  : private ValueTree::Listener
{
public:
    explicit Model (te::Edit& e) : edit (e)
    {
        state = edit.state.getChildWithName (ids::COCOMPOSE);
        const bool isNew = ! state.isValid();

        if (isNew)
        {
            state = ValueTree (ids::COCOMPOSE);
            state.setProperty (ids::schema, modelSchema, nullptr);
            for (auto child : { ids::CHANNELS, ids::PATTERNS, ids::PLAYLIST, ids::MIXER })
                state.appendChild (ValueTree (child), nullptr);
            for (auto child : { ids::LANES, ids::CLIPS })
                state.getChildWithName (ids::PLAYLIST).appendChild (ValueTree (child), nullptr);
            edit.state.appendChild (state, nullptr);
        }

        require (static_cast<int> (state.getProperty (ids::schema, 0)) == modelSchema,
                 "Unsupported project model schema");

        if (isNew)
            migrateExistingClips();

        state.addListener (this);
        dirty = true;
    }

    ~Model() override { state.removeListener (this); }

    te::Edit& edit;
    ValueTree state;

    ValueTree channels() const { return state.getChildWithName (ids::CHANNELS); }
    ValueTree patterns() const { return state.getChildWithName (ids::PATTERNS); }
    ValueTree playlist() const { return state.getChildWithName (ids::PLAYLIST); }
    ValueTree lanes()    const { return playlist().getChildWithName (ids::LANES); }
    ValueTree instances() const { return playlist().getChildWithName (ids::CLIPS); }
    ValueTree mixer()    const { return state.getChildWithName (ids::MIXER); }

    static String uidOf (ValueTree v) { return v[ids::uid].toString(); }

    static ValueTree withID (ValueTree parent, const Identifier& type, const String& key)
    {
        for (auto child : parent)
            if (child.hasType (type) && uidOf (child) == key)
                return child;
        return {};
    }

    ValueTree channelFor  (const String& key) const { return withID (channels(), ids::CHANNEL, key); }
    ValueTree patternFor  (const String& key) const { return withID (patterns(), ids::PATTERN, key); }
    ValueTree laneFor     (const String& key) const { return withID (lanes(), ids::LANE, key); }
    ValueTree instanceFor (const String& key) const { return withID (instances(), ids::INSTANCE, key); }
    ValueTree insertFor   (const String& key) const { return withID (mixer(), ids::INSERT, key); }

    static ValueTree findSequence (ValueTree pattern, const String& channelID)
    {
        for (auto child : pattern)
            if (child.hasType (ids::SEQUENCE) && child[ids::channel].toString() == channelID)
                return child;
        return {};
    }

    /** The notes of one channel inside one pattern, created on demand. */
    ValueTree sequenceFor (ValueTree pattern, const String& channelID, UndoManager* undo)
    {
        if (auto existing = findSequence (pattern, channelID); existing.isValid())
            return existing;

        ValueTree sequence (ids::SEQUENCE);
        sequence.setProperty (ids::channel, channelID, nullptr);
        pattern.appendChild (sequence, undo);
        return sequence;
    }

    //==============================================================================
    ValueTree addChannel (const String& channelName, UndoManager* undo)
    {
        const auto slot = nextInsertIndex();

        ValueTree channel (ids::CHANNEL);
        channel.setProperty (ids::uid, Uuid().toString(), nullptr);
        channel.setProperty (ids::name, channelName, nullptr);
        channel.setProperty (ids::gainDb, -12.0, nullptr);
        channel.setProperty (ids::pan, 0.0, nullptr);
        channel.setProperty (ids::mute, false, nullptr);
        channel.setProperty (ids::solo, false, nullptr);
        channel.setProperty (ids::insert, slot, nullptr);
        channels().appendChild (channel, undo);
        addInsert (channelName, slot, undo);
        return channel;
    }

    ValueTree addInsert (const String& insertName, int slot, UndoManager* undo)
    {
        for (auto child : mixer())
            if (static_cast<int> (child[ids::index]) == slot)
                return child;

        ValueTree insert (ids::INSERT);
        insert.setProperty (ids::uid, Uuid().toString(), nullptr);
        insert.setProperty (ids::name, insertName, nullptr);
        insert.setProperty (ids::index, slot, nullptr);
        insert.setProperty (ids::gainDb, 0.0, nullptr);
        insert.setProperty (ids::pan, 0.0, nullptr);
        insert.setProperty (ids::mute, false, nullptr);
        mixer().appendChild (insert, undo);
        return insert;
    }

    ValueTree addPattern (const String& patternName, double lengthBeats, UndoManager* undo)
    {
        ValueTree pattern (ids::PATTERN);
        pattern.setProperty (ids::uid, Uuid().toString(), nullptr);
        pattern.setProperty (ids::name, patternName, nullptr);
        pattern.setProperty (ids::length, lengthBeats, nullptr);
        patterns().appendChild (pattern, undo);
        return pattern;
    }

    ValueTree addLane (const String& laneName, UndoManager* undo)
    {
        ValueTree lane (ids::LANE);
        lane.setProperty (ids::uid, Uuid().toString(), nullptr);
        lane.setProperty (ids::name, laneName, nullptr);
        lane.setProperty (ids::mute, false, nullptr);
        lanes().appendChild (lane, undo);
        return lane;
    }

    ValueTree addInstance (const String& laneID, const String& patternID, double startBeat, UndoManager* undo)
    {
        auto pattern = patternFor (patternID);
        require (pattern.isValid(), "Unknown pattern: " + patternID);
        require (laneFor (laneID).isValid(), "Unknown playlist lane: " + laneID);

        ValueTree instance (ids::INSTANCE);
        instance.setProperty (ids::uid, Uuid().toString(), nullptr);
        instance.setProperty (ids::lane, laneID, nullptr);
        instance.setProperty (ids::pattern, patternID, nullptr);
        instance.setProperty (ids::start, startBeat, nullptr);
        instance.setProperty (ids::length, static_cast<double> (pattern[ids::length]), nullptr);
        instances().appendChild (instance, undo);
        return instance;
    }

    /** Gives one placement its own copy of the pattern, so editing it no longer changes
        the other placements. FL calls this "Make unique". */
    ValueTree makeUnique (ValueTree instance, UndoManager* undo)
    {
        auto original = patternFor (instance[ids::pattern].toString());
        require (original.isValid(), "Instance references an unknown pattern");

        auto copy = original.createCopy();
        copy.setProperty (ids::uid, Uuid().toString(), nullptr);
        copy.setProperty (ids::name, uniquePatternName (original[ids::name].toString()), nullptr);
        for (auto sequence : copy)
            for (auto note : sequence)
                note.setProperty (ids::uid, Uuid().toString(), nullptr);

        patterns().appendChild (copy, undo);
        instance.setProperty (ids::pattern, uidOf (copy), undo);
        return copy;
    }

    ValueTree addNote (ValueTree sequence, int notePitch, double startBeat, double lengthBeats,
                       int noteVelocity, UndoManager* undo)
    {
        ValueTree note (ids::NOTE);
        note.setProperty (ids::uid, Uuid().toString(), nullptr);
        note.setProperty (ids::pitch, notePitch, nullptr);
        note.setProperty (ids::velocity, noteVelocity, nullptr);
        note.setProperty (ids::start, startBeat, nullptr);
        note.setProperty (ids::length, lengthBeats, nullptr);
        sequence.appendChild (note, undo);
        return note;
    }

    //==============================================================================
    /** Rebuilds the engine from the model when anything changed. Derived clips are not
        themselves undoable: undo restores the model and the next call re-derives the
        engine from it, so one edit stays one undo. */
    bool renderIfNeeded()
    {
        if (! dirty)
            return false;

        dirty = false;
        syncChannels();
        syncClips();
        return true;
    }

    void render() { dirty = true; renderIfNeeded(); }

    te::AudioTrack* trackFor (const String& channelID) const
    {
        for (auto* track : te::getAudioTracks (edit))
            if (stableID (track->state) == channelID)
                return track;
        return nullptr;
    }

private:
    bool dirty = false;

    void valueTreePropertyChanged (ValueTree&, const Identifier&) override { dirty = true; }
    void valueTreeChildAdded (ValueTree&, ValueTree&) override             { dirty = true; }
    void valueTreeChildRemoved (ValueTree&, ValueTree&, int) override      { dirty = true; }
    void valueTreeChildOrderChanged (ValueTree&, int, int) override        { dirty = true; }
    void valueTreeParentChanged (ValueTree&) override                      { dirty = true; }

    int nextInsertIndex() const
    {
        int highest = 0;
        for (auto child : mixer())
            highest = std::max (highest, static_cast<int> (child[ids::index]));
        return highest + 1;
    }

    String uniquePatternName (const String& base) const
    {
        for (int suffix = 2; ; ++suffix)
        {
            const auto candidate = base + " " + String (suffix);
            bool taken = false;
            for (auto child : patterns())
                taken = taken || child[ids::name].toString() == candidate;
            if (! taken)
                return candidate;
        }
    }

    te::TimePosition atBeat (double beat) const
    {
        return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat));
    }

    /** Every channel owns one engine audio track, in the channel's order. */
    void syncChannels()
    {
        for (auto channel : channels())
        {
            const auto channelID = uidOf (channel);
            auto* track = trackFor (channelID);

            if (track == nullptr)
            {
                const auto count = te::getAudioTracks (edit).size();
                edit.ensureNumberOfAudioTracks (count + 1);
                track = te::getAudioTracks (edit)[count];
                require (track != nullptr, "Cannot create channel track");
                track->state.setProperty ("coComposeId", channelID, nullptr);

                if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
                    track->pluginList.insertPlugin (*synth, 0, nullptr);
            }

            track->setName (channel[ids::name].toString());
            track->setMute (static_cast<bool> (channel[ids::mute]));
            track->setSolo (static_cast<bool> (channel[ids::solo]));

            if (auto* volume = track->getVolumePlugin())
            {
                volume->setVolumeDb (static_cast<float> (channel[ids::gainDb]));
                volume->setPan (static_cast<float> (channel[ids::pan]));
            }
        }

        std::set<String> wanted;
        for (auto channel : channels())
            wanted.insert (uidOf (channel));

        for (auto* track : te::getAudioTracks (edit))
            if (wanted.find (stableID (track->state)) == wanted.end())
                edit.deleteTrack (track);

        te::AudioTrack* previous = nullptr;
        int position = 0;
        for (auto channel : channels())
        {
            auto* track = trackFor (uidOf (channel));
            if (track != nullptr && te::getAudioTracks (edit)[position] != track)
                edit.moveTrack (track, te::TrackInsertPoint (nullptr, previous));
            ++position;
            previous = track;
        }
    }

    /** One derived MIDI clip per (instance, channel that plays in its pattern). */
    void syncClips()
    {
        std::set<String> wanted;

        for (auto instance : instances())
        {
            auto pattern = patternFor (instance[ids::pattern].toString());
            if (! pattern.isValid())
                continue;

            const auto instanceID = uidOf (instance);
            const auto startBeat = static_cast<double> (instance[ids::start]);
            const auto lengthBeats = std::max (0.001, static_cast<double> (instance[ids::length]));

            for (auto sequence : pattern)
            {
                if (! sequence.hasType (ids::SEQUENCE) || sequence.getNumChildren() == 0)
                    continue;

                const auto channelID = sequence[ids::channel].toString();
                auto* track = trackFor (channelID);
                if (track == nullptr)
                    continue;

                wanted.insert (instanceID + "/" + channelID);
                syncClip (*track, instanceID, channelID, sequence, startBeat, lengthBeats);
            }
        }

        for (auto* track : te::getAudioTracks (edit))
        {
            const auto existingClips = track->getClips();
            for (auto* existing : existingClips)
                if (existing->state.hasProperty (ids::clipInstance))
                {
                    const auto key = existing->state[ids::clipInstance].toString() + "/"
                                   + existing->state[ids::clipChannel].toString();
                    if (wanted.find (key) == wanted.end())
                        existing->removeFromParent();
                }
        }
    }

    void syncClip (te::AudioTrack& track, const String& instanceID, const String& channelID,
                   ValueTree sequence, double startBeat, double lengthBeats)
    {
        te::MidiClip* clip = nullptr;
        for (auto* existing : track.getClips())
            if (existing->state[ids::clipInstance].toString() == instanceID
                 && existing->state[ids::clipChannel].toString() == channelID)
                clip = dynamic_cast<te::MidiClip*> (existing);

        const auto range = te::TimeRange (atBeat (startBeat), atBeat (startBeat + lengthBeats));

        if (clip == nullptr)
        {
            clip = track.insertMIDIClip (track.getName(), range, nullptr).get();
            require (clip != nullptr, "Cannot create pattern clip");
            clip->state.setProperty (ids::clipInstance, instanceID, nullptr);
            clip->state.setProperty (ids::clipChannel, channelID, nullptr);
        }

        if (clip->getPosition().time != range)
            clip->setPosition ({ range, clip->getPosition().offset });

        auto& notes = clip->getSequence();
        std::set<String> keep;

        for (auto note : sequence)
        {
            if (! note.hasType (ids::NOTE))
                continue;

            const auto noteID = uidOf (note);
            keep.insert (noteID);

            const auto position = te::BeatPosition::fromBeats (static_cast<double> (note[ids::start]));
            const auto duration = te::BeatDuration::fromBeats (std::max (0.001, static_cast<double> (note[ids::length])));
            const auto notePitch = jlimit (0, 127, static_cast<int> (note[ids::pitch]));
            const auto noteVelocity = jlimit (1, 127, static_cast<int> (note[ids::velocity]));

            te::MidiNote* engineNote = nullptr;
            for (auto* candidate : notes.getNotes())
                if (candidate->state["coComposeId"].toString() == noteID)
                {
                    engineNote = candidate;
                    break;
                }

            if (engineNote == nullptr)
            {
                engineNote = notes.addNote (notePitch, position, duration, noteVelocity, 0, nullptr);
                require (engineNote != nullptr, "Cannot create pattern note");
                engineNote->state.setProperty ("coComposeId", noteID, nullptr);
            }
            else
            {
                if (engineNote->getNoteNumber() != notePitch) engineNote->setNoteNumber (notePitch, nullptr);
                if (engineNote->getVelocity() != noteVelocity) engineNote->setVelocity (noteVelocity, nullptr);
                if (engineNote->getStartBeat() != position || engineNote->getLengthBeats() != duration)
                    engineNote->setStartAndLength (position, duration, nullptr);
            }
        }

        const auto existingNotes = notes.getNotes();
        for (auto* existing : existingNotes)
            if (keep.find (existing->state["coComposeId"].toString()) == keep.end())
                notes.removeNote (*existing, nullptr);
    }

    /** Converts a session written by the pre-pattern build: every track becomes a channel
        with its own playlist lane, and every MIDI clip becomes a pattern placed once. The
        existing clips are adopted rather than recreated, so the arrangement and the plugin
        state survive the conversion untouched. */
    void migrateExistingClips()
    {
        int slot = 0;

        for (auto* track : te::getAudioTracks (edit))
        {
            const auto channelID = stableID (track->state);
            auto* volume = track->getVolumePlugin();

            ValueTree channel (ids::CHANNEL);
            channel.setProperty (ids::uid, channelID, nullptr);
            channel.setProperty (ids::name, track->getName(), nullptr);
            channel.setProperty (ids::gainDb, volume != nullptr ? volume->getVolumeDb() : -12.0f, nullptr);
            channel.setProperty (ids::pan, volume != nullptr ? volume->getPan() : 0.0f, nullptr);
            channel.setProperty (ids::mute, track->isMuted (false), nullptr);
            channel.setProperty (ids::solo, track->isSolo (false), nullptr);
            channel.setProperty (ids::insert, ++slot, nullptr);
            channels().appendChild (channel, nullptr);
            addInsert (track->getName(), slot, nullptr);

            // Keep the ids an external tool already knows, matching upgradeToSchema2().
            auto lane = addLane (track->getName(), nullptr);
            lane.setProperty (ids::uid, channelID + "-lane", nullptr);

            for (auto* base : track->getClips())
            {
                auto* clip = dynamic_cast<te::MidiClip*> (base);
                if (clip == nullptr)
                    continue;

                const auto range = clip->getPosition().time;
                const auto startBeat = edit.tempoSequence.toBeats (range.getStart()).inBeats();
                const auto lengthBeats = std::max (0.001, edit.tempoSequence.toBeats (range.getEnd()).inBeats() - startBeat);

                const auto clipID = stableID (clip->state);
                auto pattern = addPattern (clip->getName(), lengthBeats, nullptr);
                pattern.setProperty (ids::uid, clipID, nullptr);
                auto sequence = sequenceFor (pattern, channelID, nullptr);

                for (auto* note : clip->getSequence().getNotes())
                {
                    ValueTree modelNote (ids::NOTE);
                    modelNote.setProperty (ids::uid, stableID (note->state), nullptr);
                    modelNote.setProperty (ids::pitch, note->getNoteNumber(), nullptr);
                    modelNote.setProperty (ids::velocity, note->getVelocity(), nullptr);
                    modelNote.setProperty (ids::start, note->getStartBeat().inBeats(), nullptr);
                    modelNote.setProperty (ids::length, note->getLengthBeats().inBeats(), nullptr);
                    sequence.appendChild (modelNote, nullptr);
                }

                auto instance = addInstance (uidOf (lane), clipID, startBeat, nullptr);
                instance.setProperty (ids::uid, clipID + "-placement", nullptr);
                instance.setProperty (ids::length, lengthBeats, nullptr);

                // Adopt the clip that is already playing instead of rebuilding it.
                clip->state.setProperty (ids::clipInstance, uidOf (instance), nullptr);
                clip->state.setProperty (ids::clipChannel, channelID, nullptr);
            }
        }
    }
};
}
