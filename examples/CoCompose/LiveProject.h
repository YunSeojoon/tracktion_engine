#pragma once

#include "Support.h"
#include "Model.h"

namespace live
{
// Restoring a ValueTree property alone does not restore a parameter's explicit
// playback value. Undo must call the same parameter API as the original edit.
class ParameterAction final : public UndoableAction
{
public:
    ParameterAction (te::Edit& e, const String& plugin, const String& parameter, float oldValue, float newValue)
        : edit (e), pluginID (plugin), parameterID (parameter), before (oldValue), after (newValue) {}
    bool perform() override { return set (after); }
    bool undo() override { return set (before); }
private:
    te::Edit& edit;
    String pluginID, parameterID;
    float before, after;
    bool set (float value)
    {
        for (auto* plugin : te::getAllPlugins (edit, true))
            if (plugin->itemID.toString() == pluginID)
                if (auto parameter = plugin->getAutomatableParameterByID (parameterID))
                {
                    parameter->setParameter (value, sendNotification);
                    return true;
                }
        return false;
    }
};

/** Converts a schema 1 document — flat tracks holding their own clips — into the
    pattern model. Each track becomes a channel with its own playlist lane, and each
    clip becomes a pattern placed once, keeping every original id so external tools
    keep referring to the same objects. */
inline var upgradeToSchema2 (const var& root)
{
    require (root["tracks"].isArray(), "Schema 1 document needs a tracks array");

    Array<var> channels, patterns, lanes, clips, inserts;
    int slot = 0;

    for (const auto& track : *root["tracks"].getArray())
    {
        const auto channelID = id (track);
        const auto trackName = track["name"].toString();
        ++slot;

        channels.add (object ({ { "id", channelID }, { "name", trackName },
            { "gain_db", track["gain_db"] }, { "pan", 0.0 }, { "mute", track["mute"] },
            { "solo", track["solo"] }, { "insert", slot },
            { "parameters", track["parameters"].isArray() ? track["parameters"] : var (Array<var>()) } }));
        inserts.add (object ({ { "id", channelID + "-insert" }, { "index", slot },
            { "name", trackName }, { "gain_db", 0.0 }, { "pan", 0.0 }, { "mute", false } }));

        const auto laneID = channelID + "-lane";
        lanes.add (object ({ { "id", laneID }, { "name", trackName }, { "mute", false } }));

        if (! track["clips"].isArray())
            continue;

        for (const auto& clip : *track["clips"].getArray())
        {
            const auto clipID = id (clip);
            patterns.add (object ({ { "id", clipID }, { "name", clip["name"] },
                { "length", clip["length"] },
                { "sequences", Array<var> { object ({ { "channel", channelID },
                    { "notes", clip["notes"] } }) } } }));
            clips.add (object ({ { "id", clipID + "-placement" }, { "lane", laneID },
                { "pattern", clipID }, { "start", clip["start"] }, { "length", clip["length"] } }));
        }
    }

    return object ({ { "schema", modelSchema }, { "revision", root["revision"] },
        { "session_id", root["session_id"] }, { "request_id", root["request_id"] },
        { "bpm", root["bpm"] }, { "channels", channels }, { "patterns", patterns },
        { "playlist", object ({ { "lanes", lanes }, { "clips", clips } }) },
        { "mixer", object ({ { "inserts", inserts } }) } });
}

class Project
{
public:
    Project (te::Engine& engine, const File& jsonFile)
        : source (jsonFile), stateFile (source.getSiblingFile ("state.json")),
          statusFile (source.getSiblingFile ("sync-status.json")),
          nativeFile (source.getSiblingFile ("session.tracktionedit"))
    {
        require (source.getParentDirectory().createDirectory().wasOk(), "Cannot create project folder");
        require (source.hasFileExtension ("json") && ! StringArray { "state.json", "sync-status.json",
            "control.json", "control-status.json" }.contains (source.getFileName(), true), "Use a separate project.json input file");
        require (source.getSize() <= 8 * 1024 * 1024, "project.json exceeds 8 MB");
        const bool hadNativeSession = nativeFile.existsAsFile();
        // Loading happens once, at startup. Live updates never replace this Edit.
        edit = hadNativeSession ? te::loadEditFromFile (engine, nativeFile)
                                       : te::createEmptyEdit (engine, nativeFile);
        require (edit != nullptr, "Cannot open native session");
        edit->editFileRetriever = [file = nativeFile] { return file; };
        edit->playInStopEnabled = true;
        // Converts a pre-pattern session in place; a pattern session loads unchanged.
        model = std::make_unique<Model> (*edit);
        model->renderIfNeeded();
        revision = static_cast<int> (edit->state.getProperty ("coComposeRevision", 0));
        if (source.existsAsFile() && ! hadNativeSession)
        {
            const auto contents = source.loadFileAsString();
            try
            {
                auto data = parse (contents);
                const auto savedRevision = static_cast<int> (number (data, "revision", 0, 2000000000, true));
                require (savedRevision >= revision, "Saved JSON is older than native session; recover from state.json");
                data = accept (data);
                validate (data);
                apply (data);
                revision = savedRevision;
                lastSeen = contents;
            }
            catch (const std::exception& e)
            {
                error = e.what();
                lastSeen = contents;
            }
        }
        else if (! hadNativeSession)
            apply (example());

        // Seed the eight-bar example once. Live edits never overwrite transport choices.
        if (! hadNativeSession)
        {
            edit->getTransport().setLoopRange ({ te::TimePosition(), std::max (te::TimePosition::fromSeconds (2),
                te::TimePosition::fromSeconds (edit->getLength().inSeconds())) });
            edit->getTransport().looping = true;
        }
        edit->getUndoManager().clearUndoHistory();
        lastModel = text (snapshot());
        if (error.isNotEmpty()) syncState = "rejected";
        publish();
        if (! source.existsAsFile()) atomicWrite (source, stateFile.loadFileAsString());
        lastSeen = source.loadFileAsString();
    }

    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<Model> model;
    const File source, stateFile, statusFile, nativeFile;
    const String sessionID = Uuid().toString();
    String error;
    String syncState = "synced";
    int revision = 0, applied = 0;

    var snapshot()
    {
        Array<var> channels;
        for (auto channel : model->channels())
        {
            const auto channelID = Model::uidOf (channel);
            auto* track = model->trackFor (channelID);
            auto* volume = track != nullptr ? track->getVolumePlugin() : nullptr;

            Array<var> parameters;
            if (track != nullptr)
                for (auto* plugin : track->pluginList)
                    for (auto* parameter : plugin->getAutomatableParameters())
                        parameters.add (object ({ { "plugin_id", plugin->itemID.toString() },
                            { "plugin_name", plugin->getName() }, { "id", parameter->paramID },
                            { "name", parameter->getParameterName() },
                            { "value", parameter->valueRange.convertTo0to1 (parameter->getCurrentExplicitValue()) } }));

            // Read the engine back where it owns the value, so state.json reports what plays.
            channels.add (object ({ { "id", channelID },
                { "name", track != nullptr ? track->getName() : channel[ids::name].toString() },
                { "gain_db", volume != nullptr ? volume->getVolumeDb() : static_cast<float> (channel[ids::gainDb]) },
                { "pan", volume != nullptr ? volume->getPan() : static_cast<float> (channel[ids::pan]) },
                { "mute", track != nullptr ? track->isMuted (false) : static_cast<bool> (channel[ids::mute]) },
                { "solo", track != nullptr ? track->isSolo (false) : static_cast<bool> (channel[ids::solo]) },
                { "insert", static_cast<int> (channel[ids::insert]) },
                { "parameters", parameters } }));
        }

        Array<var> patterns;
        for (auto pattern : model->patterns())
        {
            Array<var> sequences;
            for (auto sequence : pattern)
            {
                if (! sequence.hasType (ids::SEQUENCE))
                    continue;

                Array<var> notes;
                for (auto note : sequence)
                    notes.add (object ({ { "id", Model::uidOf (note) },
                        { "pitch", static_cast<int> (note[ids::pitch]) },
                        { "velocity", static_cast<int> (note[ids::velocity]) },
                        { "start", static_cast<double> (note[ids::start]) },
                        { "length", static_cast<double> (note[ids::length]) } }));
                sequences.add (object ({ { "channel", sequence[ids::channel].toString() }, { "notes", notes } }));
            }
            patterns.add (object ({ { "id", Model::uidOf (pattern) }, { "name", pattern[ids::name].toString() },
                { "length", static_cast<double> (pattern[ids::length]) }, { "sequences", sequences } }));
        }

        Array<var> lanes, clips;
        for (auto lane : model->lanes())
            lanes.add (object ({ { "id", Model::uidOf (lane) }, { "name", lane[ids::name].toString() },
                { "mute", static_cast<bool> (lane[ids::mute]) } }));
        for (auto instance : model->instances())
            clips.add (object ({ { "id", Model::uidOf (instance) },
                { "lane", instance[ids::lane].toString() }, { "pattern", instance[ids::pattern].toString() },
                { "start", static_cast<double> (instance[ids::start]) },
                { "length", static_cast<double> (instance[ids::length]) } }));

        Array<var> inserts;
        for (auto insert : model->mixer())
            inserts.add (object ({ { "id", Model::uidOf (insert) }, { "index", static_cast<int> (insert[ids::index]) },
                { "name", insert[ids::name].toString() },
                { "gain_db", static_cast<double> (insert[ids::gainDb]) },
                { "pan", static_cast<double> (insert[ids::pan]) },
                { "mute", static_cast<bool> (insert[ids::mute]) } }));

        return object ({ { "schema", modelSchema }, { "bpm", edit->tempoSequence.getTempo (0)->getBpm() },
                         { "channels", channels }, { "patterns", patterns },
                         { "playlist", object ({ { "lanes", lanes }, { "clips", clips } }) },
                         { "mixer", object ({ { "inserts", inserts } }) },
                         { "engine", engineReadback() } });
    }

    /** What the engine is actually going to play: the MIDI clips derived from the
        playlist. Read-only — it is ignored when a request is applied — but it is the
        only way an external tool can tell that a model edit reached the engine. */
    var engineReadback()
    {
        Array<var> tracks;
        for (auto* track : te::getAudioTracks (*edit))
        {
            Array<var> clips;
            for (auto* base : track->getClips())
            {
                auto* clip = dynamic_cast<te::MidiClip*> (base);
                if (clip == nullptr)
                    continue;

                Array<var> notes;
                for (auto* note : clip->getSequence().getNotes())
                    notes.add (object ({ { "pitch", note->getNoteNumber() }, { "velocity", note->getVelocity() },
                        { "start", note->getStartBeat().inBeats() }, { "length", note->getLengthBeats().inBeats() } }));

                const auto range = clip->getPosition().time;
                const auto start = edit->tempoSequence.toBeats (range.getStart()).inBeats();
                clips.add (object ({ { "clip", clip->state[ids::clipInstance].toString() },
                    { "channel", clip->state[ids::clipChannel].toString() },
                    { "start", start },
                    { "length", edit->tempoSequence.toBeats (range.getEnd()).inBeats() - start },
                    { "notes", notes } }));
            }
            tracks.add (object ({ { "channel", stableID (track->state) }, { "name", track->getName() },
                                  { "clips", clips } }));
        }
        return object ({ { "tracks", tracks } });
    }

    // Called only on the JUCE message thread, never the audio callback.
    void poll()
    {
        bool changed = false;
        try
        {
            if (persistencePending)
            {
                changed = true;
                error.clear();
                syncState = "synced";
                publish();
                changed = false;
            }
            // UI and undo both edit the model; the engine is re-derived before readback.
            model->renderIfNeeded();
            const auto current = text (snapshot());
            const bool pluginStateChanged = (++ticks % 8 == 0 && pluginSignature() != lastPlugins);
            if (current != lastModel || pluginStateChanged)
            {
                ++revision;
                changed = true;
                lastModel = current;
                publish();
                changed = false;
            }

            if (source.getSize() > 8 * 1024 * 1024)
                throw std::runtime_error ("project.json exceeds 8 MB");
            const auto incoming = source.loadFileAsString();
            if (incoming == lastSeen) return;
            // Wait for two identical reads so an editor's partial save is not applied.
            if (incoming != pending) { pending = incoming; return; }
            lastSeen = incoming;
            auto data = parse (incoming);
            lastRequest = data["request_id"].toString();
            require (data["session_id"].toString() == sessionID,
                     "Session changed: read state.json from the running app");
            require (static_cast<int> (number (data, "revision", 0, 2000000000, true)) == revision,
                     "Revision conflict: read state.json and reapply your changes");
            data = accept (data);
            validate (data); // Validate the entire change before touching the model.
            apply (data);
            ++revision;
            ++applied;
            changed = true;
            error.clear();
            syncState = "synced";
            lastModel = text (snapshot()); // Read back the live engine, not the input.
            publish();
        }
        catch (const std::exception& e)
        {
            error = e.what();
            syncState = changed ? "applied_unpersisted" : "rejected";
            writeStatus();
        }
    }

    void undo()
    {
        edit->getUndoManager().undo();
        poll();
    }

    /** Writes the native session and state.json now, instead of waiting for the next
        change to be noticed. */
    void save()
    {
        model->renderIfNeeded();
        lastModel = text (snapshot());
        publish();
        error.clear();
        syncState = "synced";
        writeStatus();
    }

    void writeStatus()
    {
        atomicWrite (statusFile, JSON::toString (object ({ { "session_id", sessionID },
            { "revision", revision }, { "applied", applied }, { "error", error },
            { "status", syncState },
            { "edit_instance", String::toHexString (reinterpret_cast<int64> (edit.get())) },
            { "updated_at_ms", Time::getCurrentTime().toMilliseconds() },
            { "request_id", lastRequest },
            { "playing", edit->getTransport().isPlaying() },
            { "position_seconds", edit->getTransport().getPosition().inSeconds() },
            { "looping", static_cast<bool> (edit->getTransport().looping) },
            { "undo", edit->getUndoManager().getUndoDescription() },
            { "undo_actions", edit->getUndoManager().getNumActionsInCurrentTransaction() },
            { "channel_count", model->channels().getNumChildren() },
            { "pattern_count", model->patterns().getNumChildren() },
            { "clip_count", model->instances().getNumChildren() },
            { "track_count", te::getAudioTracks (*edit).size() } }), false));
    }

    static var example()
    {
        Array<var> notes;
        for (int i = 0; i < 32; ++i)
            notes.add (object ({ { "id", "note-" + String (i) }, { "pitch", 48 + (i % 4) * 3 },
                { "velocity", 88 }, { "start", i * 1.0 }, { "length", 0.75 } }));

        return object ({ { "schema", modelSchema }, { "revision", 0 }, { "bpm", 120.0 },
            { "channels", Array<var> { object ({ { "id", "synth-1" }, { "name", "CoCompose Synth" },
                { "gain_db", -12.0 }, { "pan", 0.0 }, { "mute", false }, { "solo", false },
                { "insert", 1 }, { "parameters", Array<var>() } }) } },
            { "patterns", Array<var> { object ({ { "id", "phrase-1" }, { "name", "8 bars" },
                { "length", 32.0 },
                { "sequences", Array<var> { object ({ { "channel", "synth-1" }, { "notes", notes } }) } } }) } },
            { "playlist", object ({
                { "lanes", Array<var> { object ({ { "id", "lane-1" }, { "name", "Playlist 1" }, { "mute", false } }) } },
                { "clips", Array<var> { object ({ { "id", "placement-1" }, { "lane", "lane-1" },
                    { "pattern", "phrase-1" }, { "start", 0.0 }, { "length", 32.0 } }) } } }) },
            { "mixer", object ({ { "inserts", Array<var> { object ({ { "id", "insert-1" }, { "index", 1 },
                { "name", "CoCompose Synth" }, { "gain_db", 0.0 }, { "pan", 0.0 }, { "mute", false } }) } } }) } });
    }

private:
    String lastModel, lastSeen, pending, lastRequest, lastPlugins;
    int ticks = 0;
    bool persistencePending = false;

    String pluginSignature()
    {
        String states;
        for (auto* track : te::getAudioTracks (*edit))
            for (auto* plugin : track->pluginList)
            {
                plugin->flushPluginStateToValueTree();
                states += plugin->state.toXmlString();
            }
        return String::toHexString (states.hashCode64());
    }

    static var parse (const String& contents)
    {
        var result;
        const auto parsed = JSON::parse (contents, result);
        require (parsed.wasOk() && result.isObject(), "Invalid JSON: " + parsed.getErrorMessage());
        return result;
    }

    /** Accepts either schema, so a project.json or a script written for the flat track
        model still applies to the pattern model. */
    static var accept (const var& root)
    {
        const auto version = static_cast<int> (number (root, "schema", 1, modelSchema, true));
        return version == modelSchema ? root : upgradeToSchema2 (root);
    }

    te::AutomatableParameter* findParameter (const String& channelID, const var& p)
    {
        if (auto* track = model->trackFor (channelID))
            for (auto* plugin : track->pluginList)
                if (plugin->itemID.toString() == p["plugin_id"].toString())
                    for (auto* param : plugin->getAutomatableParameters())
                        if (param->paramID == p["id"].toString()) return param;
        return nullptr;
    }

    void validate (const var& root)
    {
        // "engine" is a readback of what the model produced; it is accepted so a tool can
        // send state.json straight back, and ignored so it can never author anything.
        knownFields (root, "schema revision session_id request_id bpm channels patterns playlist mixer engine");
        require (static_cast<int> (number (root, "schema", modelSchema, modelSchema, true)) == modelSchema,
                 "Unsupported schema");
        number (root, "bpm", 30, 300);

        require (root["channels"].isArray() && root["channels"].size() <= 64, "channels must be an array (max 64)");
        std::set<String> channelIDs;
        for (const auto& channel : *root["channels"].getArray())
        {
            knownFields (channel, "id name gain_db pan mute solo insert parameters");
            require (channelIDs.insert (id (channel)).second, "Duplicate channel id");
            require (channel["name"].isString() && channel["name"].toString().length() <= 200, "Invalid channel name");
            number (channel, "gain_db", -60, 6);
            number (channel, "pan", -1, 1);
            number (channel, "insert", 1, 256, true);
            require (channel["mute"].isBool() && channel["solo"].isBool(), "mute and solo must be boolean");

            require (channel["parameters"].isArray(), "parameters must be an array");
            std::set<String> parameterIDs;
            for (const auto& parameter : *channel["parameters"].getArray())
            {
                knownFields (parameter, "plugin_id plugin_name id name value");
                number (parameter, "value", 0, 1);
                require (parameterIDs.insert (parameter["plugin_id"].toString() + ":" + id (parameter)).second,
                         "Duplicate parameter");
                require (findParameter (id (channel), parameter) != nullptr, "Unknown plugin parameter");
            }
        }

        require (root["patterns"].isArray() && root["patterns"].size() <= 512, "patterns must be an array (max 512)");
        std::set<String> patternIDs;
        int noteCount = 0;
        for (const auto& pattern : *root["patterns"].getArray())
        {
            knownFields (pattern, "id name length sequences");
            require (patternIDs.insert (id (pattern)).second, "Duplicate pattern id");
            require (pattern["name"].isString() && pattern["name"].toString().length() <= 200, "Invalid pattern name");
            const auto length = number (pattern, "length", 0.001, 100000);
            require (pattern["sequences"].isArray() && pattern["sequences"].size() <= 64,
                     "sequences must be an array (max 64)");

            std::set<String> sequenceChannels, noteIDs;
            for (const auto& sequence : *pattern["sequences"].getArray())
            {
                knownFields (sequence, "channel notes");
                const auto channelID = sequence["channel"].toString();
                require (channelIDs.count (channelID) > 0, "Sequence references an unknown channel");
                require (sequenceChannels.insert (channelID).second, "Duplicate sequence channel in one pattern");
                require (sequence["notes"].isArray(), "notes must be an array");

                for (const auto& note : *sequence["notes"].getArray())
                {
                    knownFields (note, "id pitch velocity start length");
                    require (++noteCount <= 20000, "Maximum 20000 notes");
                    require (noteIDs.insert (id (note)).second, "Duplicate note id in one pattern");
                    number (note, "pitch", 0, 127, true);
                    number (note, "velocity", 1, 127, true);
                    const auto start = number (note, "start", 0, length);
                    require (start + number (note, "length", 0.001, length) <= length + 0.00001,
                             "Note exceeds pattern length");
                }
            }
        }

        const auto playlist = root["playlist"];
        knownFields (playlist, "lanes clips");
        require (playlist["lanes"].isArray() && playlist["lanes"].size() <= 128, "lanes must be an array (max 128)");
        std::set<String> laneIDs, instanceIDs;
        for (const auto& lane : *playlist["lanes"].getArray())
        {
            knownFields (lane, "id name mute");
            require (laneIDs.insert (id (lane)).second, "Duplicate lane id");
            require (lane["name"].isString() && lane["name"].toString().length() <= 200, "Invalid lane name");
            require (lane["mute"].isBool(), "mute must be boolean");
        }
        require (playlist["clips"].isArray() && playlist["clips"].size() <= 4096, "clips must be an array (max 4096)");
        for (const auto& clip : *playlist["clips"].getArray())
        {
            knownFields (clip, "id lane pattern start length");
            require (instanceIDs.insert (id (clip)).second, "Duplicate playlist clip id");
            require (laneIDs.count (clip["lane"].toString()) > 0, "Playlist clip references an unknown lane");
            require (patternIDs.count (clip["pattern"].toString()) > 0, "Playlist clip references an unknown pattern");
            number (clip, "start", 0, 100000);
            number (clip, "length", 0.001, 100000);
        }

        const auto mixerState = root["mixer"];
        knownFields (mixerState, "inserts");
        require (mixerState["inserts"].isArray() && mixerState["inserts"].size() <= 256,
                 "inserts must be an array (max 256)");
        std::set<String> insertIDs;
        std::set<int> insertSlots;
        for (const auto& insert : *mixerState["inserts"].getArray())
        {
            knownFields (insert, "id index name gain_db pan mute");
            require (insertIDs.insert (id (insert)).second, "Duplicate insert id");
            require (insertSlots.insert (static_cast<int> (number (insert, "index", 1, 256, true))).second,
                     "Duplicate insert index");
            require (insert["name"].isString() && insert["name"].toString().length() <= 200, "Invalid insert name");
            number (insert, "gain_db", -60, 6);
            number (insert, "pan", -1, 1);
            require (insert["mute"].isBool(), "mute must be boolean");
        }
        for (const auto& channel : *root["channels"].getArray())
            require (insertSlots.count (static_cast<int> (channel["insert"])) > 0,
                     "Channel is assigned to a mixer insert that does not exist");
    }

    /** Replaces the desired parts of the model, keeping objects the request still
        lists and deleting the ones it dropped. */
    void apply (const var& root)
    {
        auto& undo = edit->getUndoManager();
        undo.beginNewTransaction ("External project edit");
        try
        {
            edit->tempoSequence.getTempo (0)->setBpm (static_cast<double> (root["bpm"]));

            applyList (model->mixer(), ids::INSERT, root["mixer"]["inserts"], undo,
                       [] (ValueTree insert, const var& desired, UndoManager* um)
                       {
                           insert.setProperty (ids::index, static_cast<int> (desired["index"]), um);
                           insert.setProperty (ids::name, desired["name"].toString(), um);
                           insert.setProperty (ids::gainDb, static_cast<double> (desired["gain_db"]), um);
                           insert.setProperty (ids::pan, static_cast<double> (desired["pan"]), um);
                           insert.setProperty (ids::mute, static_cast<bool> (desired["mute"]), um);
                       });

            applyList (model->channels(), ids::CHANNEL, root["channels"], undo,
                       [] (ValueTree channel, const var& desired, UndoManager* um)
                       {
                           channel.setProperty (ids::name, desired["name"].toString(), um);
                           channel.setProperty (ids::gainDb, static_cast<double> (desired["gain_db"]), um);
                           channel.setProperty (ids::pan, static_cast<double> (desired["pan"]), um);
                           channel.setProperty (ids::mute, static_cast<bool> (desired["mute"]), um);
                           channel.setProperty (ids::solo, static_cast<bool> (desired["solo"]), um);
                           channel.setProperty (ids::insert, static_cast<int> (desired["insert"]), um);
                       });

            applyList (model->patterns(), ids::PATTERN, root["patterns"], undo,
                       [this] (ValueTree pattern, const var& desired, UndoManager* um)
                       {
                           pattern.setProperty (ids::name, desired["name"].toString(), um);
                           pattern.setProperty (ids::length, static_cast<double> (desired["length"]), um);
                           applySequences (pattern, desired["sequences"], um);
                       });

            applyList (model->lanes(), ids::LANE, root["playlist"]["lanes"], undo,
                       [] (ValueTree lane, const var& desired, UndoManager* um)
                       {
                           lane.setProperty (ids::name, desired["name"].toString(), um);
                           lane.setProperty (ids::mute, static_cast<bool> (desired["mute"]), um);
                       });

            applyList (model->instances(), ids::INSTANCE, root["playlist"]["clips"], undo,
                       [] (ValueTree instance, const var& desired, UndoManager* um)
                       {
                           instance.setProperty (ids::lane, desired["lane"].toString(), um);
                           instance.setProperty (ids::pattern, desired["pattern"].toString(), um);
                           instance.setProperty (ids::start, static_cast<double> (desired["start"]), um);
                           instance.setProperty (ids::length, static_cast<double> (desired["length"]), um);
                       });

            // The model owns the channel fader, so render it before the explicit
            // parameter edits that are read back from the engine.
            model->render();

            for (const auto& channel : *root["channels"].getArray())
                for (const auto& p : *channel["parameters"].getArray())
                    if (auto* param = findParameter (id (channel), p))
                        if (std::abs (param->valueRange.convertTo0to1 (param->getCurrentExplicitValue())
                                       - static_cast<float> (p["value"])) > 0.000001f)
                        {
                            const auto before = param->getCurrentExplicitValue();
                            const auto after = param->valueRange.convertFrom0to1 (static_cast<float> (p["value"]));
                            param->setParameter (after, sendNotification);
                            undo.perform (new ParameterAction (*edit, p["plugin_id"].toString(), param->paramID, before, after));
                        }
        }
        catch (...)
        {
            undo.undoCurrentTransactionOnly();
            model->render();
            throw;
        }
        undo.beginNewTransaction();
    }

    template <typename Update>
    void applyList (ValueTree parent, const Identifier& type, const var& desiredList,
                    UndoManager& undo, Update update)
    {
        StringArray order;
        for (const auto& desired : *desiredList.getArray())
        {
            const auto key = id (desired);
            order.add (key);

            auto child = Model::withID (parent, type, key);
            if (! child.isValid())
            {
                child = ValueTree (type);
                child.setProperty (ids::uid, key, nullptr);
                parent.appendChild (child, &undo);
            }
            update (child, desired, &undo);
        }

        for (int i = parent.getNumChildren(); --i >= 0;)
            if (! order.contains (Model::uidOf (parent.getChild (i))))
                parent.removeChild (i, &undo);

        for (int target = 0; target < order.size(); ++target)
        {
            auto child = Model::withID (parent, type, order[target]);
            const auto current = parent.indexOf (child);
            if (current >= 0 && current != target)
                parent.moveChild (current, target, &undo);
        }
    }

    void applySequences (ValueTree pattern, const var& desiredList, UndoManager* undo)
    {
        StringArray channelsInUse;
        for (const auto& desired : *desiredList.getArray())
        {
            const auto channelID = desired["channel"].toString();
            channelsInUse.add (channelID);
            auto sequence = model->sequenceFor (pattern, channelID, undo);

            StringArray noteOrder;
            for (const auto& note : *desired["notes"].getArray())
            {
                const auto key = id (note);
                noteOrder.add (key);

                auto child = Model::withID (sequence, ids::NOTE, key);
                if (! child.isValid())
                {
                    child = ValueTree (ids::NOTE);
                    child.setProperty (ids::uid, key, nullptr);
                    sequence.appendChild (child, undo);
                }
                child.setProperty (ids::pitch, static_cast<int> (note["pitch"]), undo);
                child.setProperty (ids::velocity, static_cast<int> (note["velocity"]), undo);
                child.setProperty (ids::start, static_cast<double> (note["start"]), undo);
                child.setProperty (ids::length, static_cast<double> (note["length"]), undo);
            }

            for (int i = sequence.getNumChildren(); --i >= 0;)
                if (! noteOrder.contains (Model::uidOf (sequence.getChild (i))))
                    sequence.removeChild (i, undo);
        }

        for (int i = pattern.getNumChildren(); --i >= 0;)
        {
            auto sequence = pattern.getChild (i);
            if (sequence.hasType (ids::SEQUENCE) && ! channelsInUse.contains (sequence[ids::channel].toString()))
                pattern.removeChild (i, undo);
        }
    }

    void publish()
    {
        persistencePending = true;
        auto data = snapshot();
        data.getDynamicObject()->setProperty ("revision", revision);
        data.getDynamicObject()->setProperty ("request_id", lastRequest);
        data.getDynamicObject()->setProperty ("session_id", sessionID);
        const auto contents = JSON::toString (data, false);
        edit->state.setProperty ("coComposeRevision", revision, nullptr);
        edit->flushState();
        lastPlugins = pluginSignature();
        atomicWrite (nativeFile, edit->state.createXml()->toString());
        atomicWrite (stateFile, contents);
        writeStatus();
        persistencePending = false;
    }
};
}
