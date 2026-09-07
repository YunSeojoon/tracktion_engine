#pragma once

#include <set>
#include <stdexcept>

namespace live
{
using namespace juce;
namespace te = tracktion;

inline var object (std::initializer_list<std::pair<Identifier, var>> fields)
{
    auto* result = new DynamicObject();
    for (const auto& [key, value] : fields)
        result->setProperty (key, value);
    return result;
}

inline String text (const var& value) { return JSON::toString (value, true); }

inline void require (bool condition, const String& error)
{
    if (! condition) throw std::runtime_error (error.toStdString());
}

inline double number (const var& v, const Identifier& key, double low, double high, bool integer = false)
{
    const auto n = v[key];
    require (n.isInt() || n.isInt64() || n.isDouble(), key.toString() + " must be numeric");
    const auto d = static_cast<double> (n);
    require (std::isfinite (d) && d >= low && d <= high && (! integer || std::floor (d) == d),
             key.toString() + " out of range");
    return d;
}

inline String id (const var& v)
{
    require (v["id"].isString(), "id must be a string");
    const auto s = v["id"].toString();
    require (s.isNotEmpty() && s.length() <= 100, "invalid id");
    return s;
}

inline void knownFields (const var& value, const String& allowed)
{
    require (value.isObject(), "Expected a JSON object");
    const auto names = StringArray::fromTokens (allowed, " ", "");
    for (const auto& property : value.getDynamicObject()->getProperties())
        require (names.contains (property.name.toString()), "Unknown field: " + property.name.toString());
}

inline String stableID (ValueTree state)
{
    if (! state.hasProperty ("coComposeId"))
        state.setProperty ("coComposeId", Uuid().toString(), nullptr);
    return state["coComposeId"].toString();
}

inline void atomicWrite (const File& file, const String& contents)
{
    require (file.getParentDirectory().createDirectory().wasOk(), "Cannot create project folder");
    TemporaryFile temporary (file);
    require (temporary.getFile().replaceWithText (contents), "Cannot write " + file.getFileName());
    require (temporary.overwriteTargetFileWithTemporary(), "Cannot replace " + file.getFileName());
}

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
        revision = static_cast<int> (edit->state.getProperty ("coComposeRevision", 0));
        if (source.existsAsFile() && ! hadNativeSession)
        {
            const auto contents = source.loadFileAsString();
            try
            {
                auto data = parse (contents);
                const auto savedRevision = static_cast<int> (number (data, "revision", 0, 2000000000, true));
                require (savedRevision >= revision, "Saved JSON is older than native session; recover from state.json");
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
    const File source, stateFile, statusFile, nativeFile;
    const String sessionID = Uuid().toString();
    String error;
    String syncState = "synced";
    int revision = 0, applied = 0;

    var snapshot()
    {
        Array<var> tracks;
        for (auto* track : te::getAudioTracks (*edit))
        {
            Array<var> clips, parameters;
            for (auto* base : track->getClips())
                if (auto* clip = dynamic_cast<te::MidiClip*> (base))
                {
                    Array<var> notes;
                    for (auto* note : clip->getSequence().getNotes())
                        notes.add (object ({ { "id", stableID (note->state) },
                            { "pitch", note->getNoteNumber() }, { "velocity", note->getVelocity() },
                            { "start", note->getStartBeat().inBeats() },
                            { "length", note->getLengthBeats().inBeats() } }));
                    const auto range = clip->getPosition().time;
                    const auto start = edit->tempoSequence.toBeats (range.getStart()).inBeats();
                    const auto end = edit->tempoSequence.toBeats (range.getEnd()).inBeats();
                    clips.add (object ({ { "id", stableID (clip->state) }, { "name", clip->getName() },
                        { "start", start }, { "length", end - start }, { "notes", notes } }));
                }
            for (auto* plugin : track->pluginList)
                for (auto* parameter : plugin->getAutomatableParameters())
                    parameters.add (object ({ { "plugin_id", plugin->itemID.toString() },
                        { "plugin_name", plugin->getName() }, { "id", parameter->paramID },
                        { "name", parameter->getParameterName() },
                        { "value", parameter->valueRange.convertTo0to1 (parameter->getCurrentExplicitValue()) } }));
            tracks.add (object ({ { "id", stableID (track->state) }, { "name", track->getName() },
                { "mute", track->isMuted (false) }, { "solo", track->isSolo (false) },
                { "gain_db", track->getVolumePlugin()->getVolumeDb() },
                { "clips", clips }, { "parameters", parameters } }));
        }
        return object ({ { "schema", 1 }, { "bpm", edit->tempoSequence.getTempo (0)->getBpm() },
                         { "tracks", tracks } });
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
            validate (data); // Validate the entire change before touching the Edit.
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
            { "track_count", te::getAudioTracks (*edit).size() } }), false));
    }

    static var example()
    {
        Array<var> notes;
        for (int i = 0; i < 32; ++i)
            notes.add (object ({ { "id", "note-" + String (i) }, { "pitch", 48 + (i % 4) * 3 },
                { "velocity", 88 }, { "start", i * 1.0 }, { "length", 0.75 } }));
        return object ({ { "schema", 1 }, { "revision", 0 }, { "bpm", 120.0 },
            { "tracks", Array<var> { object ({ { "id", "synth-1" }, { "name", "CoCompose Synth" },
                { "gain_db", -12.0 }, { "mute", false }, { "solo", false },
                { "clips", Array<var> { object ({ { "id", "phrase-1" }, { "name", "8 bars" },
                    { "start", 0.0 }, { "length", 32.0 }, { "notes", notes } }) } },
                { "parameters", Array<var>() } }) } } });
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

    te::AudioTrack* findTrack (const String& key)
    {
        for (auto* track : te::getAudioTracks (*edit))
            if (stableID (track->state) == key) return track;
        return nullptr;
    }

    te::AutomatableParameter* findParameter (te::AudioTrack* track, const var& p)
    {
        if (track != nullptr)
            for (auto* plugin : track->pluginList)
                if (plugin->itemID.toString() == p["plugin_id"].toString())
                    for (auto* param : plugin->getAutomatableParameters())
                        if (param->paramID == p["id"].toString()) return param;
        return nullptr;
    }

    void validate (const var& root)
    {
        knownFields (root, "schema revision session_id request_id bpm tracks");
        require (number (root, "schema", 1, 1, true) == 1, "Unsupported schema");
        number (root, "bpm", 30, 300);
        require (root["tracks"].isArray() && root["tracks"].size() <= 64, "tracks must be an array (max 64)");
        std::set<String> trackIDs, clipIDs, noteIDs;
        int noteCount = 0;
        for (const auto& track : *root["tracks"].getArray())
        {
            knownFields (track, "id name gain_db mute solo clips parameters");
            require (trackIDs.insert (id (track)).second, "Duplicate track id");
            require (track["name"].isString() && track["name"].toString().length() <= 200, "Invalid track name");
            number (track, "gain_db", -60, 6);
            require (track["mute"].isBool() && track["solo"].isBool(), "mute and solo must be boolean");
            require (track["clips"].isArray() && track["clips"].size() <= 256, "clips must be an array (max 256)");
            for (const auto& clip : *track["clips"].getArray())
            {
                knownFields (clip, "id name start length notes");
                require (clipIDs.insert (id (clip)).second, "Duplicate clip id");
                require (clip["name"].isString() && clip["name"].toString().length() <= 200, "Invalid clip name");
                number (clip, "start", 0, 100000);
                const auto length = number (clip, "length", 0.001, 100000);
                require (clip["notes"].isArray(), "notes must be an array");
                for (const auto& note : *clip["notes"].getArray())
                {
                    knownFields (note, "id pitch velocity start length");
                    require (++noteCount <= 20000, "Maximum 20000 notes");
                    require (noteIDs.insert (id (note)).second, "Duplicate note id");
                    number (note, "pitch", 0, 127, true);
                    number (note, "velocity", 1, 127, true);
                    const auto start = number (note, "start", 0, length);
                    require (start + number (note, "length", 0.001, length) <= length + 0.00001,
                             "Note exceeds clip length");
                }
            }
            require (track["parameters"].isArray(), "parameters must be an array");
            std::set<String> parameterIDs;
            for (const auto& parameter : *track["parameters"].getArray())
            {
                knownFields (parameter, "plugin_id plugin_name id name value");
                number (parameter, "value", 0, 1);
                require (parameterIDs.insert (parameter["plugin_id"].toString() + ":" + id (parameter)).second,
                         "Duplicate parameter");
                require (findParameter (findTrack (id (track)), parameter) != nullptr, "Unknown plugin parameter");
            }
        }
    }

    void apply (const var& root)
    {
        auto& undo = edit->getUndoManager();
        undo.beginNewTransaction ("External project edit");
        try
        {
        edit->tempoSequence.getTempo (0)->setBpm (static_cast<double> (root["bpm"]));
        std::set<String> retainedTracks;
        for (const auto& desired : *root["tracks"].getArray())
        {
            const auto trackID = id (desired);
            retainedTracks.insert (trackID);
            auto* track = findTrack (trackID);
            if (track == nullptr)
            {
                const auto count = te::getAudioTracks (*edit).size();
                edit->ensureNumberOfAudioTracks (count + 1);
                track = te::getAudioTracks (*edit)[count];
                track->state.setProperty ("coComposeId", trackID, &undo);
                auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
                require (synth != nullptr, "Cannot create built-in synth");
                track->pluginList.insertPlugin (*synth, 0, nullptr);
            }
            track->setName (desired["name"].toString());
            track->setMute (static_cast<bool> (desired["mute"]));
            track->setSolo (static_cast<bool> (desired["solo"]));
            // Apply explicit parameter edits first; gain_db is the authoritative track fader.
            for (const auto& p : *desired["parameters"].getArray())
                if (auto* param = findParameter (track, p))
                    if (std::abs (param->valueRange.convertTo0to1 (param->getCurrentExplicitValue()) - static_cast<float> (p["value"])) > 0.000001f)
                    {
                        const auto before = param->getCurrentExplicitValue();
                        const auto after = param->valueRange.convertFrom0to1 (static_cast<float> (p["value"]));
                        param->setParameter (after, sendNotification);
                        undo.perform (new ParameterAction (*edit, p["plugin_id"].toString(), param->paramID, before, after));
                    }
            track->getVolumePlugin()->setVolumeDb (static_cast<float> (desired["gain_db"]));

            std::set<String> retainedClips;
            for (const auto& c : *desired["clips"].getArray())
            {
                const auto clipID = id (c);
                retainedClips.insert (clipID);
                te::MidiClip* clip = nullptr;
                for (auto* existing : track->getClips())
                    if (stableID (existing->state) == clipID) clip = dynamic_cast<te::MidiClip*> (existing);
                const auto startBeat = static_cast<double> (c["start"]);
                const auto length = static_cast<double> (c["length"]);
                const auto start = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (startBeat));
                const auto end = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (startBeat + length));
                if (clip == nullptr)
                {
                    clip = track->insertMIDIClip (c["name"].toString(), { start, end }, nullptr).get();
                    require (clip != nullptr, "Cannot create MIDI clip");
                    clip->state.setProperty ("coComposeId", clipID, &undo);
                }
                clip->setName (c["name"].toString());
                clip->setPosition ({ { start, end }, clip->getPosition().offset });
                auto& sequence = clip->getSequence();
                std::set<String> retainedNotes;
                for (const auto& n : *c["notes"].getArray())
                {
                    const auto noteID = id (n);
                    retainedNotes.insert (noteID);
                    te::MidiNote* note = nullptr;
                    for (auto* existing : sequence.getNotes())
                        if (stableID (existing->state) == noteID) { note = existing; break; }
                    const auto position = te::BeatPosition::fromBeats (static_cast<double> (n["start"]));
                    const auto duration = te::BeatDuration::fromBeats (static_cast<double> (n["length"]));
                    if (note == nullptr)
                    {
                        note = sequence.addNote (static_cast<int> (n["pitch"]), position, duration,
                                                 static_cast<int> (n["velocity"]), 0, &undo);
                        note->state.setProperty ("coComposeId", noteID, &undo);
                    }
                    else
                    {
                        note->setNoteNumber (static_cast<int> (n["pitch"]), &undo);
                        note->setVelocity (static_cast<int> (n["velocity"]), &undo);
                        note->setStartAndLength (position, duration, &undo);
                    }
                }
                const auto oldNotes = sequence.getNotes();
                for (auto* note : oldNotes)
                    if (! retainedNotes.contains (stableID (note->state))) sequence.removeNote (*note, &undo);
            }
            const auto oldClips = track->getClips();
            for (auto* clip : oldClips)
                if (dynamic_cast<te::MidiClip*> (clip) != nullptr && ! retainedClips.contains (stableID (clip->state)))
                    clip->removeFromParent();
        }
        const auto oldTracks = te::getAudioTracks (*edit);
        for (auto* track : oldTracks)
            if (! retainedTracks.contains (stableID (track->state))) edit->deleteTrack (track);
        te::AudioTrack* previous = nullptr;
        int index = 0;
        for (const auto& desired : *root["tracks"].getArray())
        {
            auto* track = findTrack (id (desired));
            if (te::getAudioTracks (*edit)[index++] != track)
                edit->moveTrack (track, te::TrackInsertPoint (nullptr, previous));
            previous = track;
        }
        }
        catch (...)
        {
            undo.undoCurrentTransactionOnly();
            throw;
        }
        undo.beginNewTransaction();
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
