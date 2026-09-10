#pragma once

#include "Support.h"
#include "Saturation.h"

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
        MIXER ("MIXER"), INSERT ("INSERT"), AUDIO ("AUDIO"), EFFECT ("EFFECT"), SEND ("SEND");

    const Identifier uid ("id"), name ("name"), schema ("schema"), channel ("channel"),
        pattern ("pattern"), lane ("lane"), start ("start"), length ("length"), pitch ("pitch"),
        velocity ("velocity"), gainDb ("gainDb"), pan ("pan"), mute ("mute"), solo ("solo"),
        insert ("insert"), index ("index"),
        instrument ("instrument"), sample ("sample"), stepPitch ("stepPitch"), stepLength ("stepLength"),
        offset ("offset"), file ("file"), fadeIn ("fadeIn"), fadeOut ("fadeOut"), speed ("speed"),
        type ("type"), bypass ("bypass"), wet ("wet"), target ("target"), level ("level"),
        output ("output");

    // Written onto engine clips so a derived clip can be matched back to the model.
    const Identifier clipInstance ("coComposeInstance"), clipChannel ("coComposeChannel"),
        clipAudio ("coComposeAudio");

    // Written onto engine plugins so a derived effect can be matched back to the model.
    const Identifier pluginEffect ("coComposeEffect");
}

constexpr int modelSchema = 2;
constexpr double defaultPatternBeats = 4.0;   // One bar, sixteen steps, as FL does

/** A step in the Channel Rack grid is a sixteenth note. */
constexpr double stepBeats = 0.25;

// Channel instruments are named by these, or by a scanned plugin's identifier string.
const String builtInSynth ("4osc"), builtInSampler ("sampler");

/** The mixer slot everything reaches in the end. */
const String masterInsert ("master");

/** The six effects the mixer offers, mapped to the engine plugin that provides each.
    Only saturation had to be written; the rest are Tracktion's own. */
inline const std::pair<const char*, const char*>& effectTypes (int index)
{
    static const std::pair<const char*, const char*> types[] = {
        { "eq",         "4bandEq" },
        { "limiter",    "compressor" },
        { "saturation", "coComposeSaturation" },
        { "delay",      "delay" },
        { "chorus",     "chorus" },
        { "reverb",     "reverb" },
    };
    return types[jlimit (0, 5, index)];
}

constexpr int numEffectTypes = 6;

inline String enginePluginFor (const String& effectType)
{
    for (int i = 0; i < numEffectTypes; ++i)
        if (effectType == effectTypes (i).first)
            return effectTypes (i).second;
    return {};
}

namespace layoutIds
{
    const Identifier LAYOUT ("COCOMPOSELAYOUT");
    const Identifier visible ("visible"), sizes ("sizes"), selectedChannel ("selectedChannel"),
        selectedPattern ("selectedPattern"), selectedLane ("selectedLane"), patternMode ("patternMode");
}

//==============================================================================
/** What the panels agree is currently being worked on. Stored in the Edit so it
    survives a reopen; not undoable, because selecting is not an edit. */
class Selection
{
public:
    explicit Selection (ValueTree layoutState) : state (layoutState) {}

    String channel() const { return state[layoutIds::selectedChannel].toString(); }
    String pattern() const { return state[layoutIds::selectedPattern].toString(); }
    String lane()    const { return state[layoutIds::selectedLane].toString(); }

    void setChannel (const String& value) { state.setProperty (layoutIds::selectedChannel, value, nullptr); }
    void setPattern (const String& value) { state.setProperty (layoutIds::selectedPattern, value, nullptr); }
    void setLane (const String& value)    { state.setProperty (layoutIds::selectedLane, value, nullptr); }

    ValueTree state;
};

//==============================================================================
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
    ValueTree audioClipFor (const String& key) const { return withID (instances(), ids::AUDIO, key); }

    /** A placement of either kind: a pattern instance or an audio clip. */
    ValueTree placementFor (const String& key) const
    {
        for (auto child : instances())
            if (uidOf (child) == key)
                return child;
        return {};
    }

    /** Audio lives on the playlist lane, not on an instrument channel, so a lane that
        holds audio owns an engine track of its own. */
    static String laneTrackID (const String& laneID) { return "lane:" + laneID; }

    /** A mixer insert is a bus track: everything assigned to it plays through its
        effect chain and its fader before reaching wherever it is routed. */
    static String insertTrackID (const String& insertID) { return "insert:" + insertID; }

    ValueTree insertForSlot (int slot) const
    {
        for (auto child : mixer())
            if (static_cast<int> (child[ids::index]) == slot)
                return child;
        return {};
    }

    bool laneHasAudio (const String& laneID) const
    {
        for (auto clip : instances())
            if (clip.hasType (ids::AUDIO) && clip[ids::lane].toString() == laneID)
                return true;
        return false;
    }

    ValueTree addAudioClip (const String& laneID, const File& source, double startBeat,
                            double lengthBeats, UndoManager* undo)
    {
        require (laneFor (laneID).isValid(), "Unknown playlist lane: " + laneID);

        ValueTree clip (ids::AUDIO);
        clip.setProperty (ids::uid, Uuid().toString(), nullptr);
        clip.setProperty (ids::name, source.getFileNameWithoutExtension(), nullptr);
        clip.setProperty (ids::lane, laneID, nullptr);
        clip.setProperty (ids::file, source.getFullPathName(), nullptr);
        clip.setProperty (ids::start, startBeat, nullptr);
        clip.setProperty (ids::length, std::max (0.01, lengthBeats), nullptr);
        clip.setProperty (ids::offset, 0.0, nullptr);
        clip.setProperty (ids::gainDb, 0.0, nullptr);
        clip.setProperty (ids::fadeIn, 0.0, nullptr);
        clip.setProperty (ids::fadeOut, 0.0, nullptr);
        clip.setProperty (ids::speed, 1.0, nullptr);
        instances().appendChild (clip, undo);
        return clip;
    }

    /** How long a file is, in beats at the current tempo. */
    double fileLengthInBeats (const File& source) const
    {
        te::AudioFile audio (edit.engine, source);
        const auto seconds = audio.getLength();
        if (seconds <= 0.0)
            return 4.0;

        auto& tempo = edit.tempoSequence;
        return tempo.toBeats (te::TimePosition::fromSeconds (seconds)).inBeats();
    }

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
        channel.setProperty (ids::instrument, builtInSynth, nullptr);
        channel.setProperty (ids::sample, "", nullptr);
        channel.setProperty (ids::stepPitch, 60, nullptr);
        channel.setProperty (ids::stepLength, stepBeats, nullptr);
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
        insert.setProperty (ids::output, masterInsert, nullptr);
        mixer().appendChild (insert, undo);
        return insert;
    }

    /** Adds one of the six effects to an insert's chain, at the end. */
    ValueTree addEffect (ValueTree insert, const String& effectType, UndoManager* undo)
    {
        require (enginePluginFor (effectType).isNotEmpty(), "Unknown effect: " + effectType);

        ValueTree effect (ids::EFFECT);
        effect.setProperty (ids::uid, Uuid().toString(), nullptr);
        effect.setProperty (ids::type, effectType, nullptr);
        effect.setProperty (ids::bypass, false, nullptr);
        effect.setProperty (ids::wet, 1.0, nullptr);
        insert.appendChild (effect, undo);
        return effect;
    }

    /** Feeds some of an insert's signal to another insert, which is how a reverb bus
        is fed without moving the dry sound off the channel. */
    ValueTree addSend (ValueTree insert, const String& targetInsertID, double sendLevel, UndoManager* undo)
    {
        require (insertFor (targetInsertID).isValid(), "Unknown mixer insert: " + targetInsertID);
        require (uidOf (insert) != targetInsertID, "An insert cannot send to itself");

        for (auto child : insert)
            if (child.hasType (ids::SEND) && child[ids::target].toString() == targetInsertID)
            {
                child.setProperty (ids::level, sendLevel, undo);
                return child;
            }

        ValueTree send (ids::SEND);
        send.setProperty (ids::uid, Uuid().toString(), nullptr);
        send.setProperty (ids::target, targetInsertID, nullptr);
        send.setProperty (ids::level, sendLevel, nullptr);
        insert.appendChild (send, undo);
        return send;
    }

    /** Refuses a routing that would send a signal back into itself. */
    bool wouldFeedBack (const String& fromInsertID, const String& toInsertID) const
    {
        StringArray seen;
        auto current = toInsertID;

        while (current.isNotEmpty() && current != masterInsert && ! seen.contains (current))
        {
            if (current == fromInsertID)
                return true;

            seen.add (current);
            auto insert = insertFor (current);
            current = insert.isValid() ? insert.getProperty (ids::output, masterInsert).toString() : String();
        }

        return false;
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
        instance.setProperty (ids::offset, 0.0, nullptr);
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

    /** Where the next placement on a lane should start: after everything already there. */
    double laneEndBeat (const String& laneID) const
    {
        double end = 0.0;
        for (auto instance : instances())
            if (instance[ids::lane].toString() == laneID)
                end = std::max (end, static_cast<double> (instance[ids::start])
                                      + static_cast<double> (instance[ids::length]));
        return end;
    }

    te::AudioTrack* trackFor (const String& channelID) const
    {
        for (auto* track : te::getAudioTracks (edit))
            if (stableID (track->state) == channelID)
                return track;
        return nullptr;
    }

    /** The channel's instrument: the first plugin that is not the track's own volume
        or metering. */
    static te::Plugin* instrumentOf (te::AudioTrack& track)
    {
        for (auto* plugin : track.pluginList)
            if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) == nullptr
                 && dynamic_cast<te::LevelMeterPlugin*> (plugin) == nullptr
                 && ! plugin->state.hasProperty (ids::pluginEffect))
                return plugin;
        return nullptr;
    }

    /** What `instrument` would have to say for this plugin to be the right one. */
    static String kindOf (te::Plugin* plugin)
    {
        if (auto* external = dynamic_cast<te::ExternalPlugin*> (plugin))
            return external->getIdentifierString();
        if (dynamic_cast<te::SamplerPlugin*> (plugin) != nullptr)
            return builtInSampler;
        if (plugin != nullptr)
            return builtInSynth;
        return {};
    }

    /** Instruments the Channel Rack can offer: the two built in, plus every scanned
        plugin that reports itself as an instrument. */
    Array<std::pair<String, String>> availableInstruments() const
    {
        Array<std::pair<String, String>> result;
        result.add ({ builtInSynth, "4OSC (built in)" });
        result.add ({ builtInSampler, "Sampler (built in)" });
        for (const auto& type : edit.engine.getPluginManager().knownPluginList.getTypes())
            if (type.isInstrument)
                result.add ({ type.createIdentifierString(), type.name });
        return result;
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

    /** Puts the instrument the channel asks for at the head of the track, replacing
        whatever is there only when it is actually a different one. */
    void syncInstrument (te::AudioTrack& track, ValueTree channel)
    {
        auto wanted = channel[ids::instrument].toString();
        if (wanted.isEmpty())
        {
            // A channel created from a document that predates instrument selection.
            wanted = builtInSynth;
            channel.setProperty (ids::instrument, wanted, nullptr);
        }

        auto* existing = instrumentOf (track);

        if (kindOf (existing) != wanted)
        {
            te::Plugin::Ptr replacement;

            if (wanted == builtInSynth)
                replacement = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
            else if (wanted == builtInSampler)
                replacement = edit.getPluginCache().createNewPlugin (te::SamplerPlugin::xmlTypeName, {});
            else if (auto description = edit.engine.getPluginManager().knownPluginList
                                            .getTypeForIdentifierString (wanted))
                replacement = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *description);

            // A plugin that cannot be created leaves the channel silent rather than
            // wrong, and the model keeps the request so a later scan can satisfy it.
            if (replacement != nullptr)
            {
                if (existing != nullptr)
                    existing->deleteFromParent();
                track.pluginList.insertPlugin (*replacement, 0, nullptr);
                existing = replacement.get();
            }
        }

        if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (existing))
            syncSampler (*sampler, channel);
    }

    /** One sound across the whole keyboard, rooted at the channel's step pitch, which
        is what a drum or one-shot channel needs. */
    void syncSampler (te::SamplerPlugin& sampler, ValueTree channel)
    {
        const auto wanted = channel[ids::sample].toString();
        const auto root = jlimit (0, 127, static_cast<int> (channel.getProperty (ids::stepPitch, 60)));

        if (wanted.isEmpty())
        {
            while (sampler.getNumSounds() > 0)
                sampler.removeSound (0);
            return;
        }

        if (sampler.getNumSounds() == 0)
            sampler.addSound (wanted, File (wanted).getFileNameWithoutExtension(), 0.0, 0.0, 0.0f);
        else if (sampler.getSoundMedia (0) != wanted)
            sampler.setSoundMedia (0, wanted);

        if (sampler.getNumSounds() > 0 && sampler.getKeyNote (0) != root)
            sampler.setSoundParams (0, root, 0, 127);
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
            }

            syncInstrument (*track, channel);
            if (track->getName() != channel[ids::name].toString())
                track->setName (channel[ids::name].toString());
            if (track->isMuted (false) != static_cast<bool> (channel[ids::mute]))
                track->setMute (static_cast<bool> (channel[ids::mute]));
            if (track->isSolo (false) != static_cast<bool> (channel[ids::solo]))
                track->setSolo (static_cast<bool> (channel[ids::solo]));

            if (auto* volume = track->getVolumePlugin())
            {
                const auto wantedGain = static_cast<float> (channel[ids::gainDb]);
                const auto wantedPan = static_cast<float> (channel[ids::pan]);
                if (std::abs (volume->getVolumeDb() - wantedGain) > 1.0e-4f) volume->setVolumeDb (wantedGain);
                if (std::abs (volume->getPan() - wantedPan) > 1.0e-4f) volume->setPan (wantedPan);
            }
        }

        std::set<String> wanted;
        for (auto channel : channels())
            wanted.insert (uidOf (channel));

        for (auto lane : lanes())
            if (laneHasAudio (uidOf (lane)))
            {
                const auto trackID = laneTrackID (uidOf (lane));
                wanted.insert (trackID);

                auto* track = trackFor (trackID);
                if (track == nullptr)
                {
                    const auto count = te::getAudioTracks (edit).size();
                    edit.ensureNumberOfAudioTracks (count + 1);
                    track = te::getAudioTracks (edit)[count];
                    require (track != nullptr, "Cannot create lane track");
                    track->state.setProperty ("coComposeId", trackID, nullptr);
                }

                if (track->getName() != lane[ids::name].toString())
                    track->setName (lane[ids::name].toString());
                if (track->isMuted (false) != static_cast<bool> (lane[ids::mute]))
                    track->setMute (static_cast<bool> (lane[ids::mute]));
            }

        for (auto insert : mixer())
        {
            const auto trackID = insertTrackID (uidOf (insert));
            wanted.insert (trackID);

            auto* track = trackFor (trackID);
            if (track == nullptr)
            {
                const auto count = te::getAudioTracks (edit).size();
                edit.ensureNumberOfAudioTracks (count + 1);
                track = te::getAudioTracks (edit)[count];
                require (track != nullptr, "Cannot create insert track");
                track->state.setProperty ("coComposeId", trackID, nullptr);
            }

            const auto insertName = String (static_cast<int> (insert[ids::index])) + " "
                                      + insert[ids::name].toString();
            if (track->getName() != insertName) track->setName (insertName);
            if (track->isMuted (false) != static_cast<bool> (insert[ids::mute]))
                track->setMute (static_cast<bool> (insert[ids::mute]));

            if (auto* volume = track->getVolumePlugin())
            {
                const auto wantedGain = static_cast<float> (insert[ids::gainDb]);
                const auto wantedPan = static_cast<float> (insert.getProperty (ids::pan, 0.0));
                if (std::abs (volume->getVolumeDb() - wantedGain) > 1.0e-4f) volume->setVolumeDb (wantedGain);
                if (std::abs (volume->getPan() - wantedPan) > 1.0e-4f) volume->setPan (wantedPan);
            }

            syncEffects (*track, insert);
        }

        for (auto* track : te::getAudioTracks (edit))
            if (wanted.find (stableID (track->state)) == wanted.end())
                edit.deleteTrack (track);

        syncRouting();

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

    /** Puts each insert's effects on its bus track in the model's order, keeping any
        plugin that is already the right one so its settings survive a reorder. */
    void syncEffects (te::AudioTrack& track, ValueTree insert)
    {
        StringArray wanted;
        for (auto effect : insert)
            if (effect.hasType (ids::EFFECT))
                wanted.add (uidOf (effect));

        for (auto* plugin : track.pluginList)
            if (plugin->state.hasProperty (ids::pluginEffect)
                 && ! wanted.contains (plugin->state[ids::pluginEffect].toString()))
                plugin->deleteFromParent();

        int slot = 0;
        for (auto effect : insert)
        {
            if (! effect.hasType (ids::EFFECT))
                continue;

            const auto effectID = uidOf (effect);
            const auto wantedType = enginePluginFor (effect[ids::type].toString());

            te::Plugin* plugin = nullptr;
            for (auto* candidate : track.pluginList)
                if (candidate->state[ids::pluginEffect].toString() == effectID)
                    plugin = candidate;

            if (plugin != nullptr && plugin->getPluginType() != wantedType)
            {
                plugin->deleteFromParent();
                plugin = nullptr;
            }

            if (plugin == nullptr)
            {
                if (wantedType.isEmpty())
                    continue;

                auto created = edit.getPluginCache().createNewPlugin (wantedType, {});
                if (created == nullptr)
                    continue;

                created->state.setProperty (ids::pluginEffect, effectID, nullptr);
                track.pluginList.insertPlugin (*created, slot, nullptr);
                plugin = created.get();
            }

            // Rendering runs after every change, so it must write nothing when nothing
            // differs; an idle write would land in the caller's undo transaction.
            const auto shouldBeEnabled = ! static_cast<bool> (effect[ids::bypass]);
            if (plugin->isEnabled() != shouldBeEnabled)
                plugin->setEnabled (shouldBeEnabled);

            // Not every effect has a wet control, so drive the one it does have.
            const auto wetValue = static_cast<float> (effect.getProperty (ids::wet, 1.0));
            auto wetParameter = plugin->getAutomatableParameterByID ("wet");
            if (wetParameter == nullptr)
                wetParameter = plugin->getAutomatableParameterByID ("mix");

            if (wetParameter != nullptr
                 && std::abs (wetParameter->valueRange.convertTo0to1 (wetParameter->getCurrentExplicitValue()) - wetValue) > 1.0e-6f)
                wetParameter->setParameter (wetParameter->valueRange.convertFrom0to1 (wetValue), sendNotification);

            // Reordering the chain has to move the plugin, not rebuild it, or its
            // settings would be lost every time the order changed.
            const auto current = track.pluginList.state.indexOf (plugin->state);
            if (current >= 0 && current != slot)
                track.pluginList.state.moveChild (current, jlimit (0, track.pluginList.state.getNumChildren() - 1, slot),
                                                  nullptr);

            ++slot;
        }
    }

    /** Channels and lanes feed their insert, inserts feed whatever they are routed to,
        and a send taps a copy of an insert into another one. */
    void syncRouting()
    {
        for (auto channel : channels())
            routeTo (trackFor (uidOf (channel)), static_cast<int> (channel[ids::insert]));

        for (auto lane : lanes())
            if (laneHasAudio (uidOf (lane)))
                routeTo (trackFor (laneTrackID (uidOf (lane))), 1);

        for (auto insert : mixer())
        {
            auto* track = trackFor (insertTrackID (uidOf (insert)));
            if (track == nullptr)
                continue;

            const auto destination = insert.getProperty (ids::output, masterInsert).toString();
            auto* target = destination == masterInsert || wouldFeedBack (uidOf (insert), destination)
                             ? nullptr : trackFor (insertTrackID (destination));

            if (track->getOutput().getDestinationTrack() != target)
            {
                if (target != nullptr) track->getOutput().setOutputToTrack (target);
                else                   track->getOutput().setOutputToDefaultDevice (false);
            }

            syncSends (*track, insert);
        }
    }

    void routeTo (te::AudioTrack* track, int slot)
    {
        if (track == nullptr)
            return;

        auto insert = insertForSlot (slot);
        auto* target = insert.isValid() ? trackFor (insertTrackID (uidOf (insert))) : nullptr;

        if (track->getOutput().getDestinationTrack() == target)
            return;

        if (target != nullptr) track->getOutput().setOutputToTrack (target);
        else                   track->getOutput().setOutputToDefaultDevice (false);
    }

    void syncSends (te::AudioTrack& track, ValueTree insert)
    {
        StringArray wanted;
        for (auto send : insert)
            if (send.hasType (ids::SEND) && ! wouldFeedBack (uidOf (insert), send[ids::target].toString()))
                wanted.add (uidOf (send));

        for (auto* plugin : track.pluginList)
            if (plugin->state.hasProperty (ids::pluginEffect)
                 && dynamic_cast<te::AuxSendPlugin*> (plugin) != nullptr
                 && ! wanted.contains (plugin->state[ids::pluginEffect].toString()))
                plugin->deleteFromParent();

        for (auto send : insert)
        {
            if (! send.hasType (ids::SEND))
                continue;

            const auto sendID = uidOf (send);
            if (! wanted.contains (sendID))
                continue;

            auto target = insertFor (send[ids::target].toString());
            auto* targetTrack = target.isValid() ? trackFor (insertTrackID (uidOf (target))) : nullptr;
            if (targetTrack == nullptr)
                continue;

            const auto bus = static_cast<int> (target[ids::index]);
            ensureAuxReturn (*targetTrack, bus);

            te::AuxSendPlugin* aux = nullptr;
            for (auto* candidate : track.pluginList)
                if (candidate->state[ids::pluginEffect].toString() == sendID)
                    aux = dynamic_cast<te::AuxSendPlugin*> (candidate);

            if (aux == nullptr)
            {
                auto created = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});
                if (created == nullptr)
                    continue;

                created->state.setProperty (ids::pluginEffect, sendID, nullptr);
                track.pluginList.insertPlugin (*created, track.pluginList.size(), nullptr);
                aux = dynamic_cast<te::AuxSendPlugin*> (created.get());
            }

            if (aux == nullptr)
                continue;

            const auto sendLevel = static_cast<float> (send.getProperty (ids::level, -6.0));
            if (aux->busNumber != bus) aux->busNumber = bus;
            if (std::abs (aux->getGainDb() - sendLevel) > 1.0e-4f) aux->setGainDb (sendLevel);
        }
    }

    void ensureAuxReturn (te::AudioTrack& track, int bus)
    {
        for (auto* plugin : track.pluginList)
            if (auto* aux = dynamic_cast<te::AuxReturnPlugin*> (plugin))
                if (aux->busNumber == bus)
                    return;

        if (auto created = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {}))
        {
            if (auto* aux = dynamic_cast<te::AuxReturnPlugin*> (created.get()))
                aux->busNumber = bus;
            track.pluginList.insertPlugin (*created, 0, nullptr);
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
            const auto patternBeats = std::max (0.001, static_cast<double> (pattern[ids::length]));
            const auto offsetBeats = std::max (0.0, static_cast<double> (instance.getProperty (ids::offset, 0.0)));

            for (auto sequence : pattern)
            {
                if (! sequence.hasType (ids::SEQUENCE) || sequence.getNumChildren() == 0)
                    continue;

                const auto channelID = sequence[ids::channel].toString();
                auto* track = trackFor (channelID);
                if (track == nullptr)
                    continue;

                wanted.insert (instanceID + "/" + channelID);
                syncClip (*track, instanceID, channelID, sequence, startBeat, lengthBeats,
                          patternBeats, offsetBeats);
            }
        }

        std::set<String> wantedAudio;
        for (auto clip : instances())
            if (clip.hasType (ids::AUDIO) && syncAudioClip (clip))
                wantedAudio.insert (uidOf (clip));

        for (auto* track : te::getAudioTracks (edit))
        {
            const auto existingClips = track->getClips();
            for (auto* existing : existingClips)
            {
                if (existing->state.hasProperty (ids::clipInstance))
                {
                    const auto key = existing->state[ids::clipInstance].toString() + "/"
                                   + existing->state[ids::clipChannel].toString();
                    if (wanted.find (key) == wanted.end())
                        existing->removeFromParent();
                }
                else if (existing->state.hasProperty (ids::clipAudio))
                {
                    if (wantedAudio.find (existing->state[ids::clipAudio].toString()) == wantedAudio.end())
                        existing->removeFromParent();
                }
            }
        }
    }

    /** A placement shows its pattern tiled from offsetBeats, for lengthBeats. A clip
        shorter than the pattern is a slice of it; a longer one repeats it. */
    void syncClip (te::AudioTrack& track, const String& instanceID, const String& channelID,
                   ValueTree sequence, double startBeat, double lengthBeats,
                   double patternBeats, double offsetBeats)
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

        const auto repeats = static_cast<int> (std::ceil ((offsetBeats + lengthBeats) / patternBeats));

        for (int repeat = 0; repeat < std::min (repeats, 512); ++repeat)
        {
            for (auto note : sequence)
            {
                if (! note.hasType (ids::NOTE))
                    continue;

                const auto sourceStart = static_cast<double> (note[ids::start]) + repeat * patternBeats;
                const auto start = sourceStart - offsetBeats;
                auto length = std::max (0.001, static_cast<double> (note[ids::length]));

                if (start + length <= 1.0e-6 || start >= lengthBeats - 1.0e-6)
                    continue;

                // A note the slice starts inside keeps only the part that is inside it.
                const auto visibleStart = std::max (0.0, start);
                length = std::min (length - (visibleStart - start), lengthBeats - visibleStart);
                if (length <= 1.0e-6)
                    continue;

                const auto noteID = uidOf (note) + "#" + String (repeat);
                keep.insert (noteID);

                const auto position = te::BeatPosition::fromBeats (visibleStart);
                const auto duration = te::BeatDuration::fromBeats (length);
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
        }

        const auto existingNotes = notes.getNotes();
        for (auto* existing : existingNotes)
            if (keep.find (existing->state["coComposeId"].toString()) == keep.end())
                notes.removeNote (*existing, nullptr);
    }

    /** One wave clip per audio placement, on its lane's own track. */
    bool syncAudioClip (ValueTree clip)
    {
        auto* track = trackFor (laneTrackID (clip[ids::lane].toString()));
        const File source (clip[ids::file].toString());
        if (track == nullptr || ! source.existsAsFile()
             || edit.engine.getAudioFileFormatManager().readFormatManager
                    .findFormatForFileExtension (source.getFileExtension()) == nullptr)
            return false;

        const auto clipID = uidOf (clip);
        const auto startBeat = static_cast<double> (clip[ids::start]);
        const auto lengthBeats = std::max (0.01, static_cast<double> (clip[ids::length]));
        const auto range = te::TimeRange (atBeat (startBeat), atBeat (startBeat + lengthBeats));
        const auto offsetSeconds = te::TimeDuration::fromSeconds (
            (atBeat (static_cast<double> (clip.getProperty (ids::offset, 0.0))) - atBeat (0.0)).inSeconds());

        te::WaveAudioClip* wave = nullptr;
        for (auto* existing : track->getClips())
            if (existing->state[ids::clipAudio].toString() == clipID)
                wave = dynamic_cast<te::WaveAudioClip*> (existing);

        if (wave == nullptr)
        {
            wave = track->insertWaveClip (clip[ids::name].toString(), source,
                                          { range, offsetSeconds }, false).get();
            if (wave == nullptr)
                return false;
            wave->state.setProperty (ids::clipAudio, clipID, nullptr);
        }

        if (wave->getPosition().time != range || wave->getPosition().offset != offsetSeconds)
            wave->setPosition ({ range, offsetSeconds });

        wave->setName (clip[ids::name].toString());
        wave->setGainDB (static_cast<float> (clip.getProperty (ids::gainDb, 0.0)));
        wave->setSpeedRatio (jlimit (0.1, 10.0, static_cast<double> (clip.getProperty (ids::speed, 1.0))));
        wave->setFadeIn (te::TimeDuration::fromSeconds (static_cast<double> (clip.getProperty (ids::fadeIn, 0.0))));
        wave->setFadeOut (te::TimeDuration::fromSeconds (static_cast<double> (clip.getProperty (ids::fadeOut, 0.0))));
        return true;
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
            channel.setProperty (ids::instrument, kindOf (instrumentOf (*track)), nullptr);
            channel.setProperty (ids::sample, "", nullptr);
            channel.setProperty (ids::stepPitch, 60, nullptr);
            channel.setProperty (ids::stepLength, stepBeats, nullptr);
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
                instance.setProperty (ids::offset, 0.0, nullptr);

                // Adopt the clip that is already playing instead of rebuilding it.
                clip->state.setProperty (ids::clipInstance, uidOf (instance), nullptr);
                clip->state.setProperty (ids::clipChannel, channelID, nullptr);
            }
        }
    }
};
}
