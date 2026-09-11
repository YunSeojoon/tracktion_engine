#pragma once

#include "Support.h"
#include "Model.h"

namespace live
{
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
            { "solo", track["solo"] }, { "insert", slot }, { "instrument", builtInSynth },
            { "sample", "" }, { "step_pitch", 60 }, { "step_length", stepBeats }, { "arm", false },
            { "parameters", track["parameters"].isArray() ? track["parameters"] : var (Array<var>()) } }));
        inserts.add (object ({ { "id", channelID + "-insert" }, { "index", slot },
            { "name", trackName }, { "gain_db", 0.0 }, { "pan", 0.0 }, { "mute", false },
            { "output", masterInsert }, { "effects", Array<var>() }, { "sends", Array<var>() } }));

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
        // The Edit is about to be loaded, and a session can contain this plugin, so the
        // type has to exist before anything is restored.
        engine.getPluginManager().createBuiltInType<SaturationPlugin>();

        require (source.getParentDirectory().createDirectory().wasOk(), "Cannot create project folder");
        require (source.hasFileExtension ("json") && ! StringArray { "state.json", "sync-status.json",
            "control.json", "control-status.json" }.contains (source.getFileName(), true), "Use a separate project.json input file");
        require (source.getSize() <= 8 * 1024 * 1024, "project.json exceeds 8 MB");
        bool hadNativeSession = nativeFile.existsAsFile();
        // Loading happens once, at startup. Live updates never replace this Edit.
        edit = hadNativeSession ? te::loadEditFromFile (engine, nativeFile)
                                       : te::createEmptyEdit (engine, nativeFile);

        // A session that is gone or unreadable is what backups are for: a folder that
        // has backups had a session, so starting it over would lose the work. Falling
        // back is an explicit recovery at startup, not a way to sync changes.
        if (edit == nullptr
             || (hadNativeSession && edit->state.getNumChildren() == 0)
             || (! hadNativeSession && ! backupsNewestFirst().isEmpty()))
        {
            for (const auto& backup : backupsNewestFirst())
            {
                if (auto recovered = te::loadEditFromFile (engine, backup))
                {
                    edit = std::move (recovered);
                    hadNativeSession = true;
                    recoveredFrom = backup.getFileName();
                    break;
                }
            }
        }

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
        lastModel = text (snapshot (false));
        if (error.isNotEmpty()) syncState = "rejected";
        publish();
        if (! source.existsAsFile()) atomicWrite (source, stateFile.loadFileAsString());
        lastSeen = source.loadFileAsString();
    }

    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<Model> model;
    const File source, stateFile, statusFile, nativeFile;
    /** Two different lifetimes, deliberately kept apart.

        sessionID identifies this run of the app: it is what a live-sync request is
        checked against, and it is different every time the app starts.

        projectID identifies the project itself and outlives every run. A conversation
        is bound to it, so reopening a project reaches the same conversation while the
        session it now belongs to is a fresh one. Renaming or moving the folder keeps
        it, because it lives inside the session file rather than in the path; saving a
        copy clears it, because a copy is a different project and must not share a
        conversation with its original.
    */
    const String sessionID = Uuid().toString();

    String projectID() const
    {
        auto layout = edit->state.getChildWithName (layoutIds::LAYOUT);
        if (! layout.isValid())
        {
            layout = ValueTree (layoutIds::LAYOUT);
            edit->state.appendChild (layout, nullptr);
        }

        if (! layout.hasProperty (layoutIds::projectID))
            layout.setProperty (layoutIds::projectID, Uuid().toString(), nullptr);

        return layout[layoutIds::projectID].toString();
    }

    /** Called when a project is copied, so the copy earns an identity of its own the
        next time it is asked for one. */
    static void clearProjectID (te::Edit& target)
    {
        if (auto layout = target.state.getChildWithName (layoutIds::LAYOUT); layout.isValid())
            layout.removeProperty (layoutIds::projectID, nullptr);
    }
    String error;
    String recoveredFrom;
    String syncState = "synced";
    int revision = 0, applied = 0;

    var snapshot (bool includeDevice = true)
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
                { "instrument", Model::kindOf (track != nullptr ? Model::instrumentOf (*track) : nullptr) },
                { "sample", channel[ids::sample].toString() },
                { "step_pitch", static_cast<int> (channel.getProperty (ids::stepPitch, 60)) },
                { "step_length", static_cast<double> (channel.getProperty (ids::stepLength, stepBeats)) },
                { "arm", static_cast<bool> (channel.getProperty (ids::arm, false)) },
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

        Array<var> lanes, clips, audio;
        for (auto lane : model->lanes())
            lanes.add (object ({ { "id", Model::uidOf (lane) }, { "name", lane[ids::name].toString() },
                { "mute", static_cast<bool> (lane[ids::mute]) } }));

        for (auto instance : model->instances())
        {
            if (instance.hasType (ids::AUDIO))
            {
                const File audioFile (instance[ids::file].toString());
                audio.add (object ({ { "id", Model::uidOf (instance) },
                    { "name", instance[ids::name].toString() },
                    { "lane", instance[ids::lane].toString() },
                    { "file", audioFile.getFullPathName() },
                    { "missing", ! audioFile.existsAsFile() },
                    { "start", static_cast<double> (instance[ids::start]) },
                    { "length", static_cast<double> (instance[ids::length]) },
                    { "offset", static_cast<double> (instance.getProperty (ids::offset, 0.0)) },
                    { "gain_db", static_cast<double> (instance.getProperty (ids::gainDb, 0.0)) },
                    { "fade_in", static_cast<double> (instance.getProperty (ids::fadeIn, 0.0)) },
                    { "fade_out", static_cast<double> (instance.getProperty (ids::fadeOut, 0.0)) },
                    { "speed", static_cast<double> (instance.getProperty (ids::speed, 1.0)) } }));
                continue;
            }

            clips.add (object ({ { "id", Model::uidOf (instance) },
                { "lane", instance[ids::lane].toString() }, { "pattern", instance[ids::pattern].toString() },
                { "start", static_cast<double> (instance[ids::start]) },
                { "length", static_cast<double> (instance[ids::length]) },
                { "offset", static_cast<double> (instance.getProperty (ids::offset, 0.0)) } }));
        }

        Array<var> inserts;
        for (auto insert : model->mixer())
        {
            Array<var> effects, sends;
            auto* insertTrack = model->trackFor (Model::insertTrackID (Model::uidOf (insert)));

            for (auto child : insert)
            {
                if (child.hasType (ids::EFFECT))
                {
                    Array<var> parameters;
                    if (insertTrack != nullptr)
                        for (auto* plugin : insertTrack->pluginList)
                            if (plugin->state[ids::pluginEffect].toString() == Model::uidOf (child))
                                for (auto* parameter : plugin->getAutomatableParameters())
                                    parameters.add (object ({ { "plugin_id", plugin->itemID.toString() },
                                        { "plugin_name", plugin->getName() }, { "id", parameter->paramID },
                                        { "name", parameter->getParameterName() },
                                        { "value", parameter->valueRange.convertTo0to1 (parameter->getCurrentExplicitValue()) } }));

                    effects.add (object ({ { "id", Model::uidOf (child) },
                        { "type", child[ids::type].toString() },
                        { "bypass", static_cast<bool> (child[ids::bypass]) },
                        { "wet", static_cast<double> (child.getProperty (ids::wet, 1.0)) },
                        { "parameters", parameters } }));
                }
                else if (child.hasType (ids::SEND))
                {
                    sends.add (object ({ { "id", Model::uidOf (child) },
                        { "target", child[ids::target].toString() },
                        { "level", static_cast<double> (child.getProperty (ids::level, -6.0)) } }));
                }
            }

            inserts.add (object ({ { "id", Model::uidOf (insert) }, { "index", static_cast<int> (insert[ids::index]) },
                { "name", insert[ids::name].toString() },
                { "gain_db", static_cast<double> (insert[ids::gainDb]) },
                { "pan", static_cast<double> (insert[ids::pan]) },
                { "mute", static_cast<bool> (insert[ids::mute]) },
                { "output", insert.getProperty (ids::output, masterInsert).toString() },
                { "effects", effects }, { "sends", sends } }));
        }

        return object ({ { "schema", modelSchema }, { "bpm", edit->tempoSequence.getTempo (0)->getBpm() },
                         { "channels", channels }, { "patterns", patterns },
                         { "playlist", object ({ { "lanes", lanes }, { "clips", clips }, { "audio", audio } }) },
                         { "mixer", object ({ { "inserts", inserts } }) },
                         { "automation", automationSnapshot() },
                         { "engine", engineReadback (includeDevice) } });
    }

    /** Every automation curve, and what the engine currently makes of it. */
    var automationSnapshot()
    {
        Array<var> curves;
        for (auto lane : model->automation())
        {
            Array<var> points;
            for (auto point : lane)
                if (point.hasType (ids::POINT))
                    points.add (object ({ { "id", Model::uidOf (point) },
                        { "time", static_cast<double> (point[ids::time]) },
                        { "value", static_cast<double> (point[ids::value]) },
                        { "curve", static_cast<double> (point.getProperty (ids::curve, 0.0)) } }));

            auto* parameter = model->automatableParameter (lane[ids::source].toString(),
                                                           lane[ids::plugin].toString(),
                                                           lane[ids::parameter].toString());

            curves.add (object ({ { "id", Model::uidOf (lane) },
                { "source", lane[ids::source].toString() },
                { "plugin_id", lane[ids::plugin].toString() },
                { "parameter", lane[ids::parameter].toString() },
                { "parameter_found", parameter != nullptr },
                { "engine_points", parameter != nullptr ? parameter->getCurve().getNumPoints() : 0 },
                { "engine_samples", engineSamples (parameter) },
                { "points", points } }));
        }

        return object ({ { "curves", curves }, { "midi_mappings", midiMappings() } });
    }

    /** The MIDI controllers that move parameters. Kept by the engine inside the Edit, so
        they are saved and restored with the project; reported here so a tool can see what
        is mapped without opening a dialog. */
    var midiMappings()
    {
        Array<var> mappings;
        auto& controls = edit->getParameterControlMappings();

        for (int row = 0; row < controls.getNumControllerIDs(); ++row)
        {
            const auto mapping = controls.getMappingForRow (row);
            if (mapping.parameter == nullptr)
                continue;

            mappings.add (object ({ { "parameter", mapping.parameter->paramID },
                                    { "name", mapping.parameter->getFullName() },
                                    { "plugin_id", mapping.parameter->getOwnerID().toString() },
                                    { "controller", mapping.controllerID },
                                    { "channel", mapping.channelID } }));
        }

        return mappings;
    }

    /** Readings taken across the curve from the engine itself, evenly spaced between
        its first and last point. A straight segment reads as a straight line and a bent
        one does not, which is how a tool can tell that a shape reached the engine
        rather than only the model. */
    var engineSamples (te::AutomatableParameter* parameter)
    {
        Array<var> samples;
        if (parameter == nullptr)
            return samples;

        const auto& curve = parameter->getCurve();
        if (curve.getNumPoints() < 2)
            return samples;

        const auto from = curve.getPointTime (0).inSeconds();
        const auto to = curve.getPointTime (curve.getNumPoints() - 1).inSeconds();
        const auto fallback = parameter->getCurrentBaseValue();

        for (int step = 0; step <= 8; ++step)
        {
            const auto at = from + (to - from) * (step / 8.0);
            samples.add (parameter->valueRange.convertTo0to1 (
                             curve.getValueAt (te::TimePosition::fromSeconds (at), fallback)));
        }

        return samples;
    }

    /** What the engine is actually going to play: the MIDI clips derived from the
        playlist. Read-only — it is ignored when a request is applied — but it is the
        only way an external tool can tell that a model edit reached the engine. */
    var engineReadback (bool includeDevice = true)
    {
        Array<var> tracks;
        for (auto* track : te::getAudioTracks (*edit))
        {
            Array<var> clips, waves;
            for (auto* base : track->getClips())
            {
                if (auto* wave = dynamic_cast<te::WaveAudioClip*> (base))
                {
                    const auto waveRange = wave->getPosition().time;
                    waves.add (object ({ { "clip", wave->state[ids::clipAudio].toString() },
                        { "file", wave->getSourceFileReference().getFile().getFullPathName() },
                        { "start", edit->tempoSequence.toBeats (waveRange.getStart()).inBeats() },
                        { "length", edit->tempoSequence.toBeats (waveRange.getEnd()).inBeats()
                                      - edit->tempoSequence.toBeats (waveRange.getStart()).inBeats() },
                        { "offset_seconds", wave->getPosition().offset.inSeconds() },
                        { "gain_db", wave->getGainDB() },
                        { "fade_in", wave->getFadeIn().inSeconds() },
                        { "fade_out", wave->getFadeOut().inSeconds() },
                        { "speed", wave->getSpeedRatio() } }));
                    continue;
                }

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
            Array<var> plugins;
            for (auto* plugin : track->pluginList)
                plugins.add (object ({ { "type", plugin->getPluginType() }, { "name", plugin->getName() },
                                       { "enabled", plugin->isEnabled() },
                                       { "window_open", plugin->windowState != nullptr
                                                          && plugin->windowState->isWindowShowing() },
                                       { "effect", plugin->state[ids::pluginEffect].toString() } }));

            auto* destination = track->getOutput().getDestinationTrack();
            tracks.add (object ({ { "channel", stableID (track->state) }, { "name", track->getName() },
                                  { "output", destination != nullptr ? stableID (destination->state) : String ("master") },
                                  { "plugins", plugins },
                                  { "clips", clips }, { "audio", waves } }));
        }
        if (! includeDevice)
            return object ({ { "tracks", tracks } });

        return object ({ { "tracks", tracks }, { "device", deviceReadback() } });
    }

    /** The audio device the engine actually opened, so a compatibility report can say
        what it was run against rather than what was asked for. */
    var deviceReadback()
    {
        auto& deviceManager = edit->engine.getDeviceManager();
        auto* device = deviceManager.deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
            return object ({ { "open", false },
                             { "inputs_available", inputReadback() },
                             { "type", deviceManager.deviceManager.getCurrentAudioDeviceType() } });

        return object ({ { "inputs_available", inputReadback() },
                         { "open", device->isOpen() },
                         { "type", deviceManager.deviceManager.getCurrentAudioDeviceType() },
                         { "name", device->getName() },
                         { "sample_rate", device->getCurrentSampleRate() },
                         { "buffer_size", device->getCurrentBufferSizeSamples() },
                         { "bit_depth", device->getCurrentBitDepth() },
                         { "output_latency", device->getOutputLatencyInSamples() },
                         { "outputs", device->getActiveOutputChannels().countNumberOfSetBits() },
                         { "inputs", device->getActiveInputChannels().countNumberOfSetBits() } });
    }

    /** What could be recorded from: the MIDI and audio inputs the engine knows about
        and whether each is switched on. An acceptance report has to be able to say
        whether a keyboard was actually attached rather than assume one. */
    var inputReadback()
    {
        Array<var> inputs;
        for (auto* instance : edit->getAllInputDevices())
        {
            auto& input = instance->getInputDevice();
            inputs.add (object ({ { "name", input.getName() },
                                  { "kind", input.isMidi() ? "midi" : "audio" },
                                  { "type", input.getDeviceTypeDescription() },
                                  { "enabled", input.isEnabled() } }));
        }
        return inputs;
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
            // The audio device and the MIDI inputs are facts about the machine, not the
            // song. They are published so a tool can see them, but a keyboard being
            // plugged in is not an edit and must not make outstanding requests stale.
            // The inputs in particular arrive a second or so after the app starts.
            const auto current = text (snapshot (false));
            // Automation moves parameters while the transport runs, so a plugin's own
            // state changes constantly and means nothing. The check is for a person
            // turning a knob in a plugin window, so it runs when playback is stopped.
            const bool pluginStateChanged = ! edit->getTransport().isPlaying()
                                             && (++ticks % 8 == 0 && pluginSignature() != lastPlugins);
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
            const auto before = snapshot();
            apply (data);
            lastChange = summarise (before, snapshot());
            ++revision;
            ++applied;
            changed = true;
            error.clear();
            syncState = "synced";
            lastModel = text (snapshot (false)); // Read back the live engine, not the input.
            publish();
        }
        catch (const std::exception& e)
        {
            error = e.what();
            syncState = changed ? "applied_unpersisted" : "rejected";
            writeStatus();
        }
    }

    /** Every edit this app makes opens a named transaction. The engine also writes its
        own bookkeeping — a plugin folding its state back into the Edit, for one — and
        that lands in whatever transaction is open at the time. Undo therefore steps
        over the unnamed transactions and reverts the last thing a person actually did. */
    void undo()
    {
        auto& undoManager = edit->getUndoManager();
        undoManager.beginNewTransaction();

        while (undoManager.canUndo() && undoManager.getUndoDescription().isEmpty())
            undoManager.undo();

        if (undoManager.canUndo())
            undoManager.undo();

        poll();
    }

    void redo()
    {
        auto& undoManager = edit->getUndoManager();

        while (undoManager.canRedo() && undoManager.getRedoDescription().isEmpty())
            undoManager.redo();

        if (undoManager.canRedo())
            undoManager.redo();

        poll();
    }

    /** Copies every sample an audio clip uses into the project folder and points the
        clips at the copies, so the folder can be moved or handed on whole. */
    String collectSamples()
    {
        auto folder = source.getParentDirectory().getChildFile ("samples");
        require (folder.createDirectory().wasOk(), "Cannot create the samples folder");

        auto& undo = edit->getUndoManager();
        undo.beginNewTransaction ("Collect samples");
        int copied = 0, missing = 0;

        for (auto clip : model->instances())
        {
            if (! clip.hasType (ids::AUDIO))
                continue;

            const File current (clip[ids::file].toString());
            if (! current.existsAsFile())
            {
                ++missing;
                continue;
            }

            if (current.getParentDirectory() == folder)
                continue;

            auto target = folder.getChildFile (current.getFileName());
            for (int attempt = 2; target.existsAsFile() && target.getSize() != current.getSize(); ++attempt)
                target = folder.getChildFile (current.getFileNameWithoutExtension() + " " + String (attempt)
                                                + current.getFileExtension());

            if (target.existsAsFile() || current.copyFileTo (target))
            {
                clip.setProperty (ids::file, target.getFullPathName(), &undo);
                ++copied;
            }
        }

        model->renderIfNeeded();
        save();

        return "Collected " + String (copied) + (copied == 1 ? " sample" : " samples")
                + (missing > 0 ? "; " + String (missing) + " still missing" : "");
    }

    File backupFolder() const { return source.getSiblingFile ("backups"); }

    /** Newest first, so recovery takes the most recent one that opens. */
    Array<File> backupsNewestFirst() const
    {
        auto found = backupFolder().findChildFiles (File::findFiles, false, "session-*.tracktionedit");
        std::sort (found.begin(), found.end(),
                   [] (const File& a, const File& b) { return a.getFileName() > b.getFileName(); });
        return found;
    }

    /** Keeps a rolling set of copies of the session, so a crash or a bad edit costs at
        most the last minute of work rather than everything. */
    void writeBackupIfDue()
    {
        if (revision == backedUpRevision)
            return;

        const auto now = Time::getCurrentTime();
        if (lastBackup != Time() && (now - lastBackup).inSeconds() < backupIntervalSeconds)
            return;

        writeBackup();
    }

    /** For the moments that are expensive to lose — a take just kept, the app closing —
        where waiting for the next interval would be the wrong answer. */
    void writeBackup()
    {
        if (! nativeFile.existsAsFile() || ! backupFolder().createDirectory().wasOk())
            return;

        const auto now = Time::getCurrentTime();
        auto target = backupFolder().getChildFile ("session-" + now.formatted ("%Y%m%d-%H%M%S")
                                                     + ".tracktionedit");

        for (int suffix = 2; target.existsAsFile(); ++suffix)
            target = backupFolder().getChildFile ("session-" + now.formatted ("%Y%m%d-%H%M%S")
                                                    + "-" + String (suffix) + ".tracktionedit");

        if (! nativeFile.copyFileTo (target))
            return;

        lastBackup = now;
        backedUpRevision = revision;

        auto existing = backupsNewestFirst();
        for (int i = backupsToKeep; i < existing.size(); ++i)
            existing[i].deleteFile();
    }

    /** Makes a chosen backup the session the next run will open, keeping the current
        one beside it so nothing is thrown away. */
    String restoreBackup (const File& backup)
    {
        if (! backup.existsAsFile())
            return "That backup is not there any more";

        if (! backupFolder().createDirectory().wasOk())
            return "Cannot write to the backups folder";

        const auto aside = backupFolder().getChildFile ("session-replaced-"
                                                          + Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S")
                                                          + ".tracktionedit");
        if (nativeFile.existsAsFile() && ! nativeFile.copyFileTo (aside))
            return "Cannot set the current session aside";

        if (! backup.copyFileTo (nativeFile))
            return "Cannot write the session file";

        // The open Edit is never swapped underneath the user; the restored session is
        // what opens next time.
        return "Restored " + backup.getFileName() + "; reopen CoCompose to work on it";
    }

    /** Sample files an audio clip can no longer find. */
    StringArray missingAssets() const
    {
        StringArray missing;
        for (auto clip : model->instances())
            if (clip.hasType (ids::AUDIO))
            {
                const File asset (clip[ids::file].toString());
                if (! asset.existsAsFile())
                    missing.addIfNotAlreadyThere (asset.getFileName());
            }
        return missing;
    }

    /** Writes the native session and state.json now, instead of waiting for the next
        change to be noticed. */
    void save()
    {
        model->renderIfNeeded();
        lastModel = text (snapshot (false));
        publish();
        error.clear();
        syncState = "synced";
        writeStatus();
    }

    /** The last thing the app told the person, so a tool can read what happened rather
        than guess from what did not. Set by the editor. */
    String uiMessage;

    void writeStatus()
    {
        atomicWrite (statusFile, JSON::toString (object ({ { "session_id", sessionID },
            { "project_id", projectID() },
            { "revision", revision }, { "applied", applied }, { "error", error },
            { "status", syncState },
            { "edit_instance", String::toHexString (reinterpret_cast<int64> (edit.get())) },
            { "updated_at_ms", Time::getCurrentTime().toMilliseconds() },
            { "request_id", lastRequest },
            { "playing", edit->getTransport().isPlaying() },
            { "recording", edit->getTransport().isRecording() },
            { "position_seconds", edit->getTransport().getPosition().inSeconds() },
            { "looping", static_cast<bool> (edit->getTransport().looping) },
            { "undo", edit->getUndoManager().getUndoDescription() },
            { "undo_actions", edit->getUndoManager().getNumActionsInCurrentTransaction() },
            { "change", lastChange },
            { "message", uiMessage },
            { "recovered_from", recoveredFrom },
            { "backups", backupNames() },
            { "missing_assets", missingAssets().joinIntoString (", ") },
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
                { "insert", 1 }, { "arm", false }, { "parameters", Array<var>() } }) } },
            { "patterns", Array<var> { object ({ { "id", "phrase-1" }, { "name", "8 bars" },
                { "length", 32.0 },
                { "sequences", Array<var> { object ({ { "channel", "synth-1" }, { "notes", notes } }) } } }) } },
            { "playlist", object ({
                { "lanes", Array<var> { object ({ { "id", "lane-1" }, { "name", "Playlist 1" }, { "mute", false } }) } },
                { "clips", Array<var> { object ({ { "id", "placement-1" }, { "lane", "lane-1" },
                    { "pattern", "phrase-1" }, { "start", 0.0 }, { "length", 32.0 },
                    { "offset", 0.0 } }) } } }) },
            { "mixer", object ({ { "inserts", Array<var> { object ({ { "id", "insert-1" }, { "index", 1 },
                { "name", "CoCompose Synth" }, { "gain_db", 0.0 }, { "pan", 0.0 }, { "mute", false } }) } } }) } });
    }

private:
    var backupNames() const
    {
        Array<var> names;
        for (const auto& backup : backupsNewestFirst())
            names.add (backup.getFileName());
        return names;
    }

    static constexpr int backupsToKeep = 10;
    static constexpr int backupIntervalSeconds = 60;

    Time lastBackup;
    int backedUpRevision = -1;

    String lastModel, lastSeen, pending, lastRequest, lastPlugins;
    var lastChange = object ({ { "bpm", false } });
    int ticks = 0;
    bool persistencePending = false;

    String pluginSignature()
    {
        // Flushing writes each plugin's own state back into the Edit. That is
        // bookkeeping, not an edit, so it must not join the user's undo transaction.
        const te::Edit::UndoTransactionInhibitor inhibitor (*edit);

        String states;
        for (auto* track : te::getAudioTracks (*edit))
            for (auto* plugin : track->pluginList)
            {
                plugin->flushPluginStateToValueTree();
                states += plugin->state.toXmlString();
            }
        return String::toHexString (states.hashCode64());
    }

    /** What changed between two snapshots, by section and by object id. This is the
        result of an edit, read back from the engine, not a copy of the request. */
    static var summarise (const var& before, const var& after)
    {
        auto* summary = new DynamicObject();
        summary->setProperty ("bpm", static_cast<double> (before["bpm"]) != static_cast<double> (after["bpm"]));

        compare (*summary, "channels", before["channels"], after["channels"]);
        compare (*summary, "patterns", before["patterns"], after["patterns"]);
        compare (*summary, "clips", before["playlist"]["clips"], after["playlist"]["clips"]);
        compare (*summary, "audio", before["playlist"]["audio"], after["playlist"]["audio"]);
        compare (*summary, "lanes", before["playlist"]["lanes"], after["playlist"]["lanes"]);
        compare (*summary, "inserts", before["mixer"]["inserts"], after["mixer"]["inserts"]);
        compare (*summary, "curves", before["automation"]["curves"], after["automation"]["curves"]);

        return summary;
    }

    static void compare (DynamicObject& into, const String& section, const var& before, const var& after)
    {
        std::map<String, String> was, now;
        collect (before, was);
        collect (after, now);

        Array<var> added, removed, changed;

        for (const auto& [key, value] : now)
        {
            const auto found = was.find (key);
            if (found == was.end()) added.add (key);
            else if (found->second != value) changed.add (key);
        }

        for (const auto& [key, value] : was)
        {
            ignoreUnused (value);
            if (now.find (key) == now.end())
                removed.add (key);
        }

        if (added.isEmpty() && removed.isEmpty() && changed.isEmpty())
            return;

        into.setProperty (section, object ({ { "added", added }, { "removed", removed },
                                            { "changed", changed } }));
    }

    static void collect (const var& list, std::map<String, String>& into)
    {
        if (auto* array = list.getArray())
            for (const auto& item : *array)
                into[item["id"].toString()] = JSON::toString (item, true);
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
        knownFields (root, "schema revision session_id request_id bpm channels patterns playlist mixer "
                           "automation engine");
        require (static_cast<int> (number (root, "schema", modelSchema, modelSchema, true)) == modelSchema,
                 "Unsupported schema");
        number (root, "bpm", 30, 300);

        require (root["channels"].isArray() && root["channels"].size() <= 64, "channels must be an array (max 64)");
        std::set<String> channelIDs;
        for (const auto& channel : *root["channels"].getArray())
        {
            knownFields (channel, "id name gain_db pan mute solo insert instrument sample "
                                  "step_pitch step_length arm parameters");
            require (channelIDs.insert (id (channel)).second, "Duplicate channel id");
            require (channel["name"].isString() && channel["name"].toString().length() <= 200, "Invalid channel name");
            number (channel, "gain_db", -60, 6);
            number (channel, "pan", -1, 1);
            number (channel, "insert", 1, 256, true);
            require (channel["mute"].isBool() && channel["solo"].isBool(), "mute and solo must be boolean");

            // The instrument fields arrived after schema 2 shipped, so they stay optional.
            if (channel.hasProperty ("instrument"))
                require (channel["instrument"].isString() && channel["instrument"].toString().length() <= 400,
                         "Invalid instrument");
            if (channel.hasProperty ("sample"))
            {
                require (channel["sample"].isString(), "sample must be a string");
                const auto file = channel["sample"].toString();
                require (file.isEmpty() || File::isAbsolutePath (file), "sample must be an absolute path");
            }
            if (channel.hasProperty ("step_pitch"))
                number (channel, "step_pitch", 0, 127, true);
            if (channel.hasProperty ("step_length"))
                number (channel, "step_length", 0.001, 64);

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
        knownFields (playlist, "lanes clips audio");
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
            knownFields (clip, "id lane pattern start length offset");
            require (instanceIDs.insert (id (clip)).second, "Duplicate playlist clip id");
            require (laneIDs.count (clip["lane"].toString()) > 0, "Playlist clip references an unknown lane");
            require (patternIDs.count (clip["pattern"].toString()) > 0, "Playlist clip references an unknown pattern");
            number (clip, "start", 0, 100000);
            number (clip, "length", 0.001, 100000);
            // A clip shows its pattern from this beat onwards; it arrived with the playlist editor.
            if (clip.hasProperty ("offset"))
                number (clip, "offset", 0, 100000);
        }

        if (playlist.hasProperty ("audio"))
        {
            require (playlist["audio"].isArray() && playlist["audio"].size() <= 2048,
                     "audio must be an array (max 2048)");
            for (const auto& clip : *playlist["audio"].getArray())
            {
                knownFields (clip, "id name lane file missing start length offset gain_db fade_in fade_out speed");
                require (instanceIDs.insert (id (clip)).second, "Duplicate playlist clip id");
                require (laneIDs.count (clip["lane"].toString()) > 0, "Audio clip references an unknown lane");
                require (clip["name"].isString() && clip["name"].toString().length() <= 200, "Invalid clip name");
                const auto path = clip["file"].toString();
                require (path.isNotEmpty() && File::isAbsolutePath (path), "Audio clip needs an absolute file path");
                // A file that is not there yet is reported as missing, but a file that is
                // there and is not audio is refused before anything is changed.
                const File audioFile (path);
                require (! audioFile.existsAsFile()
                          || edit->engine.getAudioFileFormatManager().readFormatManager
                                 .findFormatForFileExtension (audioFile.getFileExtension()) != nullptr,
                         "Not an audio file: " + audioFile.getFileName());
                number (clip, "start", 0, 100000);
                number (clip, "length", 0.01, 100000);
                if (clip.hasProperty ("offset"))   number (clip, "offset", 0, 100000);
                if (clip.hasProperty ("gain_db"))  number (clip, "gain_db", -60, 12);
                if (clip.hasProperty ("fade_in"))  number (clip, "fade_in", 0, 600);
                if (clip.hasProperty ("fade_out")) number (clip, "fade_out", 0, 600);
                if (clip.hasProperty ("speed"))    number (clip, "speed", 0.1, 10);
            }
        }

        if (root.hasProperty ("automation"))
        {
            const auto automationState = root["automation"];
            // midi_mappings is a readback: the engine owns them, so it is accepted and ignored.
            knownFields (automationState, "curves midi_mappings");
            require (automationState["curves"].isArray() && automationState["curves"].size() <= 256,
                     "curves must be an array (max 256)");

            std::set<String> curveIDs;
            for (const auto& curve : *automationState["curves"].getArray())
            {
                knownFields (curve, "id source plugin_id parameter parameter_found engine_points engine_samples points");
                require (curveIDs.insert (id (curve)).second, "Duplicate automation curve id");
                require (curve["source"].isString() && curve["source"].toString().isNotEmpty(),
                         "An automation curve needs a source");
                require (curve["plugin_id"].isString() && curve["parameter"].isString(),
                         "An automation curve needs a plugin and a parameter");
                require (model->automatableParameter (curve["source"].toString(), curve["plugin_id"].toString(),
                                                      curve["parameter"].toString()) != nullptr,
                         "Automation names a parameter that does not exist");
                require (curve["points"].isArray() && curve["points"].size() <= 2048,
                         "points must be an array (max 2048)");

                std::set<String> pointIDs;
                for (const auto& point : *curve["points"].getArray())
                {
                    knownFields (point, "id time value curve");
                    require (pointIDs.insert (id (point)).second, "Duplicate automation point id");
                    number (point, "time", 0, 100000);
                    number (point, "value", 0, 1);
                    if (point.hasProperty ("curve")) number (point, "curve", -1, 1);
                }
            }
        }

        const auto mixerState = root["mixer"];
        knownFields (mixerState, "inserts");
        require (mixerState["inserts"].isArray() && mixerState["inserts"].size() <= 256,
                 "inserts must be an array (max 256)");
        std::set<String> insertIDs;
        std::set<int> insertSlots;
        for (const auto& insert : *mixerState["inserts"].getArray())
        {
            knownFields (insert, "id index name gain_db pan mute output effects sends");
            require (insertIDs.insert (id (insert)).second, "Duplicate insert id");
            require (insertSlots.insert (static_cast<int> (number (insert, "index", 1, 256, true))).second,
                     "Duplicate insert index");
            require (insert["name"].isString() && insert["name"].toString().length() <= 200, "Invalid insert name");
            number (insert, "gain_db", -60, 6);
            number (insert, "pan", -1, 1);
            require (insert["mute"].isBool(), "mute must be boolean");
        }
        for (const auto& insert : *mixerState["inserts"].getArray())
        {
            if (insert.hasProperty ("output"))
            {
                const auto destination = insert["output"].toString();
                require (destination == masterInsert || insertIDs.count (destination) > 0,
                         "Insert is routed to a mixer insert that does not exist");
                require (destination != id (insert), "An insert cannot be routed to itself");
            }

            if (insert.hasProperty ("effects"))
            {
                require (insert["effects"].isArray() && insert["effects"].size() <= 16,
                         "effects must be an array (max 16)");
                std::set<String> effectIDs;
                for (const auto& effect : *insert["effects"].getArray())
                {
                    knownFields (effect, "id type bypass wet parameters");
                    require (effectIDs.insert (id (effect)).second, "Duplicate effect id");
                    // One of the six built in, or a scanned plugin. A plugin that is not
                    // installed here is still accepted, the way a missing instrument is,
                    // so a project written on another machine opens rather than fails;
                    // the slot stays empty and the readback shows it never loaded.
                    const auto effectType = effect["type"].toString();
                    require (enginePluginFor (effectType).isNotEmpty() || effectType.contains ("-"),
                             "Unknown effect: " + effectType);
                    require (effect["bypass"].isVoid() || effect["bypass"].isBool(), "bypass must be boolean");
                    if (effect.hasProperty ("wet")) number (effect, "wet", 0, 1);
                }
            }

            if (insert.hasProperty ("sends"))
            {
                require (insert["sends"].isArray() && insert["sends"].size() <= 16,
                         "sends must be an array (max 16)");
                std::set<String> sendTargets;
                for (const auto& send : *insert["sends"].getArray())
                {
                    knownFields (send, "id target level");
                    id (send);
                    const auto destination = send["target"].toString();
                    require (insertIDs.count (destination) > 0, "Send targets a mixer insert that does not exist");
                    require (destination != id (insert), "An insert cannot send to itself");
                    require (sendTargets.insert (destination).second, "Duplicate send target");
                    number (send, "level", -60, 6);
                }
            }
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
                       [this] (ValueTree insert, const var& desired, UndoManager* um)
                       {
                           insert.setProperty (ids::index, static_cast<int> (desired["index"]), um);
                           insert.setProperty (ids::name, desired["name"].toString(), um);
                           insert.setProperty (ids::gainDb, static_cast<double> (desired["gain_db"]), um);
                           insert.setProperty (ids::pan, static_cast<double> (desired["pan"]), um);
                           insert.setProperty (ids::mute, static_cast<bool> (desired["mute"]), um);
                           insert.setProperty (ids::output, desired.hasProperty ("output")
                                                                ? desired["output"].toString() : masterInsert, um);

                           if (desired.hasProperty ("effects"))
                               applyList (insert, ids::EFFECT, desired["effects"], *um,
                                          [] (ValueTree effect, const var& wanted, UndoManager* undoManager)
                                          {
                                              effect.setProperty (ids::type, wanted["type"].toString(), undoManager);
                                              effect.setProperty (ids::bypass, static_cast<bool> (wanted["bypass"]), undoManager);
                                              effect.setProperty (ids::wet, wanted.hasProperty ("wet")
                                                                                ? static_cast<double> (wanted["wet"]) : 1.0,
                                                                  undoManager);
                                          });

                           if (desired.hasProperty ("sends"))
                               applyList (insert, ids::SEND, desired["sends"], *um,
                                          [] (ValueTree send, const var& wanted, UndoManager* undoManager)
                                          {
                                              send.setProperty (ids::target, wanted["target"].toString(), undoManager);
                                              send.setProperty (ids::level, static_cast<double> (wanted["level"]), undoManager);
                                          });
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
                           if (desired.hasProperty ("instrument"))
                               channel.setProperty (ids::instrument, desired["instrument"].toString(), um);
                           if (desired.hasProperty ("sample"))
                               channel.setProperty (ids::sample, desired["sample"].toString(), um);
                           if (desired.hasProperty ("step_pitch"))
                               channel.setProperty (ids::stepPitch, static_cast<int> (desired["step_pitch"]), um);
                           if (desired.hasProperty ("step_length"))
                               channel.setProperty (ids::stepLength, static_cast<double> (desired["step_length"]), um);
                           if (desired.hasProperty ("arm"))
                               channel.setProperty (ids::arm, static_cast<bool> (desired["arm"]), um);
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
                           instance.setProperty (ids::offset, desired.hasProperty ("offset")
                                                                  ? static_cast<double> (desired["offset"]) : 0.0, um);
                       });

            applyList (model->instances(), ids::AUDIO,
                       root["playlist"].hasProperty ("audio") ? root["playlist"]["audio"] : var (Array<var>()), undo,
                       [] (ValueTree clip, const var& desired, UndoManager* um)
                       {
                           clip.setProperty (ids::name, desired["name"].toString(), um);
                           clip.setProperty (ids::lane, desired["lane"].toString(), um);
                           clip.setProperty (ids::file, desired["file"].toString(), um);
                           clip.setProperty (ids::start, static_cast<double> (desired["start"]), um);
                           clip.setProperty (ids::length, static_cast<double> (desired["length"]), um);
                           clip.setProperty (ids::offset, desired.hasProperty ("offset")
                                                              ? static_cast<double> (desired["offset"]) : 0.0, um);
                           clip.setProperty (ids::gainDb, desired.hasProperty ("gain_db")
                                                              ? static_cast<double> (desired["gain_db"]) : 0.0, um);
                           clip.setProperty (ids::fadeIn, desired.hasProperty ("fade_in")
                                                              ? static_cast<double> (desired["fade_in"]) : 0.0, um);
                           clip.setProperty (ids::fadeOut, desired.hasProperty ("fade_out")
                                                               ? static_cast<double> (desired["fade_out"]) : 0.0, um);
                           clip.setProperty (ids::speed, desired.hasProperty ("speed")
                                                             ? static_cast<double> (desired["speed"]) : 1.0, um);
                       });

            if (root.hasProperty ("automation"))
                applyList (model->automation(), ids::LANE_AUTOMATION, root["automation"]["curves"], undo,
                           [] (ValueTree lane, const var& desired, UndoManager* um)
                           {
                               lane.setProperty (ids::source, desired["source"].toString(), um);
                               lane.setProperty (ids::plugin, desired["plugin_id"].toString(), um);
                               lane.setProperty (ids::parameter, desired["parameter"].toString(), um);

                               applyList (lane, ids::POINT, desired["points"], *um,
                                          [] (ValueTree point, const var& wanted, UndoManager* undoManager)
                                          {
                                              point.setProperty (ids::time, static_cast<double> (wanted["time"]), undoManager);
                                              point.setProperty (ids::value, static_cast<double> (wanted["value"]), undoManager);
                                              point.setProperty (ids::curve, wanted.hasProperty ("curve")
                                                                                 ? static_cast<double> (wanted["curve"]) : 0.0,
                                                                 undoManager);
                                          });
                           });

            // The model owns the channel fader, so render it before the explicit
            // parameter edits that are read back from the engine.
            model->render();

            for (const auto& insert : *root["mixer"]["inserts"].getArray())
            {
                if (! insert.hasProperty ("effects"))
                    continue;

                auto* insertTrack = model->trackFor (Model::insertTrackID (id (insert)));
                if (insertTrack == nullptr)
                    continue;

                for (const auto& effect : *insert["effects"].getArray())
                    if (effect.hasProperty ("parameters"))
                        for (const auto& p : *effect["parameters"].getArray())
                            for (auto* plugin : insertTrack->pluginList)
                                if (plugin->itemID.toString() == p["plugin_id"].toString())
                                    if (auto param = plugin->getAutomatableParameterByID (p["id"].toString()))
                                        if (std::abs (param->valueRange.convertTo0to1 (param->getCurrentExplicitValue())
                                                       - static_cast<float> (p["value"])) > 0.000001f)
                                        {
                                            const auto before = param->getCurrentExplicitValue();
                                            const auto after = param->valueRange.convertFrom0to1 (static_cast<float> (p["value"]));
                                            param->setParameter (after, sendNotification);
                                            undo.perform (new ParameterAction (*edit, p["plugin_id"].toString(),
                                                                               param->paramID, before, after));
                                        }
            }

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

    /** Replaces every child of one type, leaving other types in the same parent alone,
        which is what lets pattern placements and audio clips share the playlist. */
    template <typename Update>
    static void applyList (ValueTree parent, const Identifier& type, const var& desiredList,
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
            if (parent.getChild (i).hasType (type) && ! order.contains (Model::uidOf (parent.getChild (i))))
                parent.removeChild (i, &undo);

        // Order the children of this type among themselves, leaving any others where
        // they are, which is what lets pattern placements and audio clips share a parent.
        int slot = 0;
        for (const auto& key : order)
        {
            auto child = Model::withID (parent, type, key);
            const auto current = parent.indexOf (child);
            if (current < 0)
                continue;

            int target = -1, seen = 0;
            for (int i = 0; i < parent.getNumChildren(); ++i)
                if (parent.getChild (i).hasType (type))
                {
                    if (seen == slot)
                    {
                        target = i;
                        break;
                    }
                    ++seen;
                }

            if (target >= 0 && target != current)
                parent.moveChild (current, target, &undo);

            ++slot;
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
        {
            // Saving is not an edit either.
            const te::Edit::UndoTransactionInhibitor inhibitor (*edit);
            edit->flushState();
        }
        lastPlugins = pluginSignature();
        atomicWrite (nativeFile, edit->state.createXml()->toString());
        atomicWrite (stateFile, contents);
        writeStatus();
        persistencePending = false;
    }
};
}
