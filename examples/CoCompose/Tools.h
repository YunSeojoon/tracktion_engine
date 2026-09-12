#pragma once

#include "Attachment.h"
#include "Proposal.h"

namespace live
{

/** The one way anything outside the app reads or changes the music.

    The point of putting it here rather than in each caller is that there is exactly
    one place where units are decided, identifiers are resolved, scope is enforced and
    errors are named. The chat panel inside the app and a script outside it go through
    this same code, so "the model said it worked but the CLI says it did not" cannot
    happen: there is nothing for them to disagree about.

    A caller declares what it wants. It never runs code here. Every answer says what
    revision it was true at, and every refusal says why in a word a caller can branch on
    rather than a sentence it has to read.

    Reading is implemented now. Writing is deliberately not: a tool that does not exist
    yet is not listed in capabilities, because a model told about a tool will try to use
    it, and being told "no such tool" after the fact is worse than never offering it.
*/
namespace tools
{
    /** Bumped when the shape of a request or an answer changes in a way a caller would
        notice. A caller that sends a version this build does not know is refused rather
        than guessed at. */
    const int contractVersion = 1;

    /** The words a caller branches on. Anything that is not one of these is a bug here,
        not a case for the caller to handle. */
    namespace errors
    {
        const char* const invalidArgument = "INVALID_ARGUMENT";
        const char* const notFound        = "NOT_FOUND";
        const char* const staleRevision   = "STALE_REVISION";
        const char* const outOfScope      = "OUT_OF_SCOPE";
        const char* const locked          = "LOCKED";
        const char* const unsupported     = "UNSUPPORTED";
        const char* const cancelled       = "CANCELLED";
        const char* const ioError         = "IO_ERROR";
    }
}

/** Thrown by a tool that cannot do what was asked. Carries the word as well as the
    sentence, so a caller can decide what to do without parsing English. */
struct ToolError : public std::runtime_error
{
    ToolError (const char* errorCode, const String& message, bool canRetry = false)
        : std::runtime_error (message.toStdString()), code (errorCode), retryable (canRetry) {}

    String code;
    bool retryable;
};

//==============================================================================
class ToolService
{
public:
    ToolService (Model& m, Selection& s, const String& project, const String& session)
        : model (m), selection (s), projectID (project), sessionID (session) {}

    /** Answers one request. Never throws: a failure is an answer too, and a caller that
        has to catch exceptions across a file or a socket has been given the wrong shape.

        The same request_id asked twice gets the same answer back without the work being
        done again. That matters most for writes, which do not exist yet, but a reader
        that retries after a timeout should not be punished for it either. */
    var handle (const var& request, int revision)
    {
        const auto requestID = request["request_id"].toString();

        if (requestID.isNotEmpty() && answered.contains (requestID))
            return answered[requestID];

        var answer;

        try
        {
            require (request.isObject(), "A tool request must be an object");

            const auto version = static_cast<int> (request.getProperty ("contract_version",
                                                                        tools::contractVersion));
            if (version != tools::contractVersion)
                throw ToolError (tools::errors::unsupported,
                                 "This build speaks contract version " + String (tools::contractVersion)
                                   + ", not " + String (version));

            const auto tool = request["tool"].toString();
            const auto arguments = request.getProperty ("arguments", var (new DynamicObject()));

            answer = object ({ { "status", "ok" },
                               { "request_id", requestID },
                               { "contract_version", tools::contractVersion },
                               { "revision", revision },
                               { "result", run (tool, arguments, revision) } });
        }
        catch (const ToolError& e)
        {
            answer = failure (requestID, revision, e.code, e.what(), e.retryable);
        }
        catch (const std::exception& e)
        {
            // Anything that got here is a bad argument that slipped past a check.
            answer = failure (requestID, revision, tools::errors::invalidArgument, e.what(), false);
        }

        if (requestID.isNotEmpty())
            answered.set (requestID, answer);

        return answer;
    }

    /** What this build can actually do. Read tools only, and said so plainly: a caller
        must not have to discover by trying. */
    /** A proposal that has been made and checked, for something that needs to read it
        rather than apply it - previewing, for instance. Null when there is no such
        proposal, which a caller has to handle: proposals do not outlive the session. */
    Proposal* proposalFor (const String& id)
    {
        return proposals.contains (id) ? &proposals.getReference (id) : nullptr;
    }

    var capabilities() const
    {
        Array<var> readTools;
        for (const auto* name : { "get_capabilities", "get_selection", "inspect_region",
                                  "inspect_insert", "inspect_pattern",
                                  "create_proposal", "apply_proposal", "get_proposal" })
            readTools.add (name);

        // Exactly what a proposal may change. Anything not here is refused, whatever a
        // caller asks for, so this list is the promise rather than the prompt.
        Array<var> writeKinds;
        for (const auto* name : { "note.pitch", "note.start_beat", "note.length_beats",
                                  "note.velocity", "note.add", "note.remove",
                                  "parameter.value",
                                  // Announced only now that a comparison can include it:
                                  // a kind listed here is one a model will try, and
                                  // offering something that cannot be previewed means
                                  // offering a change nobody can hear before taking it.
                                  "clip.start_beat", "clip.lane", "clip.copy", "clip.remove",
                                  "clip.make_unique",
                                  "effect.add", "effect.remove", "effect.move",
                                  "effect.bypass", "send.add", "send.remove" })
            writeKinds.add (name);

        return object ({
            { "contract_version", tools::contractVersion },
            { "project_id", projectID },
            { "session_id", sessionID },
            { "tools", readTools },
            { "writes", writeKinds },
            { "notes", "A proposal is checked when it is made and again when it is applied. "
                       "Applying one is a single undo. Within a region that was attached "
                       "a clip can be moved along the song or to another lane, copied to "
                       "another place, taken out, or given its own copy of the pattern it "
                       "shares. A copy of a clip points at the same pattern, so the two are "
                       "one part played twice; making one unique is the opposite, and on "
                       "its own it changes nothing anybody can hear. On an insert that was "
                       "attached, an effect can be added, removed, reordered or bypassed, "
                       "and a send added or removed; only effects this machine has are "
                       "accepted, and a send that would feed a signal back into itself is "
                       "refused." },
            { "units", object ({ { "time", "quarter-note beats, ranges are [start_beat, end_beat)" },
                                 { "pitch", "MIDI note number, 0-127" },
                                 { "velocity", "1-127" },
                                 { "parameter", "normalised 0..1" },
                                 { "gain", "decibels" },
                                 { "pan", "-1 left to 1 right" } }) },
            { "audio", object ({ { "can_send_audio", false },
                                 { "reason", "This build sends no audio to anything: the "
                                             "attachment is the music as structure, never a "
                                             "recording of it" } }) },
            { "limits", object ({ { "max_notes_per_answer", maxNotes },
                                  { "max_region_beats", maxRegionBeats } }) } });
    }

private:
    var run (const String& tool, const var& arguments, int revision)
    {
        if (tool == "get_capabilities") return capabilities();
        if (tool == "get_selection")    return currentSelection (revision);
        if (tool == "inspect_region")   return inspectRegion (arguments);
        if (tool == "inspect_insert")   return inspectInsert (arguments);
        if (tool == "inspect_pattern")  return inspectPattern (arguments);
        if (tool == "create_proposal")  return createProposal (arguments, revision);
        if (tool == "get_proposal")     return getProposal (arguments);
        if (tool == "apply_proposal")   return applyProposal (arguments, revision);

        // Named in the contract but not built yet. Saying so by name is more use than
        // "unknown tool", because it tells a caller this is a version problem.
        for (const auto* later : { "preview_proposal", "get_operation", "cancel_operation" })
            if (tool == later)
                throw ToolError (tools::errors::unsupported,
                                 String (later) + " is in the contract but not in this build");

        throw ToolError (tools::errors::invalidArgument, "No such tool: " + tool);
    }

    var currentSelection (int revision) const
    {
        return object ({ { "revision", revision },
                         { "channel", selection.channel() },
                         { "pattern", selection.pattern() },
                         { "lane", selection.lane() },
                         { "insert", selection.insert() } });
    }

    /** What is playing between two beats, and a little either side of it. The context
        either side is readable but is marked as context: a caller must be able to tell
        what it was asked about from what it was given to understand it. */
    var inspectRegion (const var& arguments)
    {
        const auto from = beat (arguments, "start_beat");
        const auto to = beat (arguments, "end_beat");

        if (to <= from)
            throw ToolError (tools::errors::invalidArgument,
                             "end_beat must be after start_beat");

        if (to - from > maxRegionBeats)
            throw ToolError (tools::errors::invalidArgument,
                             "A region may cover at most " + String (maxRegionBeats)
                               + " beats; this one covers " + String (to - from, 2));

        const auto padding = arguments.hasProperty ("context_beats")
                               ? jlimit (0.0, 64.0, static_cast<double> (arguments["context_beats"]))
                               : 4.0;

        StringArray lanes;
        if (arguments["lanes"].isArray())
            for (const auto& lane : *arguments["lanes"].getArray())
            {
                const auto laneID = lane.toString();
                if (! model.laneFor (laneID).isValid())
                    throw ToolError (tools::errors::notFound, "No such lane: " + laneID);
                lanes.add (laneID);
            }

        Array<var> inRange, around;

        // One budget for the whole answer. The region proper is described first and
        // spends from it first, so when a song is dense it is the surrounding context
        // that thins out rather than the thing that was asked about.
        auto budget = maxNotes;
        auto contextBudget = maxNotes / 4;

        for (auto clip : model.instances())
        {
            const auto start = static_cast<double> (clip[ids::start]);
            const auto end = start + static_cast<double> (clip[ids::length]);

            if (! lanes.isEmpty() && ! lanes.contains (clip[ids::lane].toString()))
                continue;

            if (end > from && start < to)             inRange.add (describeClip (clip, budget));
            else if (end > from - padding && start < to + padding)
                                                       around.add (describeClip (clip, contextBudget));
        }

        return object ({ { "start_beat", from }, { "end_beat", to },
                         { "bars", Attachments (model).barRange (from, to) },
                         { "lanes", stringsOf (lanes) },
                         { "tempo", model.edit.tempoSequence.getBpmAt (
                                        model.edit.tempoSequence.toTime (
                                            te::BeatPosition::fromBeats (from))) },
                         { "notes_budget", maxNotes },
                         { "notes_left_out", budget <= 0 },
                         { "clips", inRange },
                         { "context_beats", padding },
                         { "context_clips", around } });
    }

    /** The signal path: what is on it, in what order, and where it goes. What a plugin
        keeps inside itself is not readable, and the answer says so rather than leaving a
        caller to assume the list is everything. */
    var inspectInsert (const var& arguments)
    {
        const auto insertID = arguments["insert"].toString();
        auto insert = model.insertFor (insertID);

        if (! insert.isValid())
            throw ToolError (tools::errors::notFound, "No such insert: " + insertID);

        Array<var> effects;
        auto* track = model.trackFor (Model::insertTrackID (insertID));

        for (auto child : insert)
        {
            if (! child.hasType (ids::EFFECT))
                continue;

            Array<var> parameters;
            if (track != nullptr)
                for (auto* plugin : track->pluginList)
                    if (plugin->state[ids::pluginEffect].toString() == Model::uidOf (child))
                        for (auto* parameter : plugin->getAutomatableParameters())
                            parameters.add (object ({
                                { "id", parameter->paramID },
                                { "name", parameter->getParameterName() },
                                { "value", parameter->valueRange.convertTo0to1 (
                                               parameter->getCurrentExplicitValue()) } }));

            effects.add (object ({ { "id", Model::uidOf (child) },
                                   { "type", child[ids::type].toString() },
                                   { "name", model.effectName (child[ids::type].toString()) },
                                   { "bypass", static_cast<bool> (child[ids::bypass]) },
                                   { "wet", static_cast<double> (child.getProperty (ids::wet, 1.0)) },
                                   { "parameters", parameters } }));
        }

        Array<var> sends;
        for (auto child : insert)
            if (child.hasType (ids::SEND))
                sends.add (object ({ { "target", child[ids::target].toString() },
                                     { "level_db", static_cast<double> (child.getProperty (ids::level, -6.0)) } }));

        Array<var> fedBy;
        for (auto channel : model.channels())
            if (static_cast<int> (channel[ids::insert]) == static_cast<int> (insert[ids::index]))
                fedBy.add (object ({ { "id", Model::uidOf (channel) },
                                     { "name", channel[ids::name].toString() } }));

        return object ({ { "id", insertID },
                         { "index", static_cast<int> (insert[ids::index]) },
                         { "name", insert[ids::name].toString() },
                         { "gain_db", static_cast<double> (insert[ids::gainDb]) },
                         { "pan", static_cast<double> (insert[ids::pan]) },
                         { "mute", static_cast<bool> (insert[ids::mute]) },
                         { "output", insert.getProperty (ids::output, masterInsert).toString() },
                         { "effects", effects },
                         { "sends", sends },
                         { "fed_by", fedBy },
                         { "opaque_state",
                           "A plugin's own settings are not readable here; only the parameters listed are." } });
    }

    /** One channel's notes in one pattern, and how many places that pattern is used -
        because changing these notes changes all of them. */
    var inspectPattern (const var& arguments)
    {
        const auto patternID = arguments["pattern"].toString();
        auto pattern = model.patternFor (patternID);

        if (! pattern.isValid())
            throw ToolError (tools::errors::notFound, "No such pattern: " + patternID);

        const auto channelID = arguments["channel"].toString();
        if (channelID.isNotEmpty() && ! model.channelFor (channelID).isValid())
            throw ToolError (tools::errors::notFound, "No such channel: " + channelID);

        Array<var> parts;
        int total = 0;

        for (auto sequence : pattern)
        {
            if (! sequence.hasType (ids::SEQUENCE))
                continue;

            const auto owner = sequence[ids::channel].toString();
            if (channelID.isNotEmpty() && owner != channelID)
                continue;

            Array<var> notes;
            for (auto note : sequence)
            {
                if (! note.hasType (ids::NOTE))
                    continue;

                if (++total > maxNotes)
                    throw ToolError (tools::errors::invalidArgument,
                                     "This pattern holds more than " + String (maxNotes)
                                       + " notes; ask for one channel at a time");

                notes.add (object ({ { "id", Model::uidOf (note) },
                                     { "pitch", static_cast<int> (note[ids::pitch]) },
                                     { "start_beat", static_cast<double> (note[ids::start]) },
                                     { "length_beats", static_cast<double> (note[ids::length]) },
                                     { "velocity", static_cast<int> (note[ids::velocity]) } }));
            }

            auto channel = model.channelFor (owner);
            parts.add (object ({ { "channel", owner },
                                 { "channel_name", channel.isValid() ? channel[ids::name].toString() : String() },
                                 { "instrument", channel.isValid() ? channel[ids::instrument].toString() : String() },
                                 { "notes", notes } }));
        }

        Array<var> placements;
        for (auto clip : model.instances())
            if (clip[ids::pattern].toString() == patternID)
                placements.add (object ({ { "id", Model::uidOf (clip) },
                                          { "lane", clip[ids::lane].toString() },
                                          { "start_beat", static_cast<double> (clip[ids::start]) },
                                          { "length_beats", static_cast<double> (clip[ids::length]) } }));

        return object ({ { "id", patternID },
                         { "name", pattern[ids::name].toString() },
                         { "length_beats", static_cast<double> (pattern[ids::length]) },
                         { "parts", parts },
                         { "placements", placements },
                         { "placement_count", placements.size() },
                         { "shared", placements.size() > 1 } });
    }

    //==========================================================================
    // Proposals.

    /** Works out a change and keeps it. Nothing about the music moves here: that is the
        whole point of a proposal existing separately from applying one. */
    var createProposal (const var& arguments, int revision)
    {
        Proposal proposal;
        proposal.id = Uuid().toString();
        proposal.requestID = arguments["request_id"].toString();
        proposal.description = arguments["description"].toString();
        proposal.baseRevision = arguments.hasProperty ("base_revision")
                                  ? static_cast<int> (arguments["base_revision"]) : revision;
        proposal.keeps = Keeps::fromJson (arguments["keeps"]);

        if (proposal.baseRevision != revision)
            throw ToolError (tools::errors::staleRevision,
                             "This was worked out against revision " + String (proposal.baseRevision)
                               + " and the project is now at " + String (revision)
                               + "; read it again and make a new one", true);

        // Scope is whatever the caller says it is. That is not a hole; it is the
        // division of labour. This service trusts its caller - the app, or a script a
        // person chose to run - and the app is where scope is derived from what was
        // attached and a model's reply is stripped of any scope it tried to bring. An
        // earlier comment here said a caller "cannot widen its own scope", which was
        // true of the app and false of this function, and a review found the gap
        // between the two.
        proposal.patternID = arguments["pattern"].toString();
        proposal.channelID = arguments["channel"].toString();

        // An empty list means nothing may be touched, not everything. It used to mean
        // "any note of this channel's part", so a question with no notes attached -
        // about a mixer insert, say - produced a proposal that could rewrite the part.
        // "No permission" and "the whole part" have to be different values, and the
        // caller says which by listing the notes it was given.
        if (arguments["allowed_notes"].isArray())
            for (const auto& id : *arguments["allowed_notes"].getArray())
                proposal.allowedNotes.add (id.toString());

        if (arguments["allowed_inserts"].isArray())
            for (const auto& id : *arguments["allowed_inserts"].getArray())
                proposal.allowedInserts.add (id.toString());

        // Which placements may be moved, from the region that was attached. Empty means
        // none, the same as the note list - a caller with no region attached may not
        // move anything, however precisely it names a clip.
        if (arguments["allowed_clips"].isArray())
            for (const auto& id : *arguments["allowed_clips"].getArray())
                proposal.allowedClips.add (id.toString());

        auto sequence = Model::findSequence (model.patternFor (proposal.patternID), proposal.channelID);

        proposal.placements = 0;
        for (auto clip : model.instances())
            if (clip[ids::pattern].toString() == proposal.patternID)
                ++proposal.placements;

        if (arguments["notes"].isArray() && ! arguments["notes"].getArray()->isEmpty())
        {
            // No pattern and channel at all means no notes were attached. Saying "no
            // such part" would be true and beside the point: the part is not missing,
            // the permission is.
            if (proposal.patternID.isEmpty() || proposal.channelID.isEmpty())
                throw ToolError (tools::errors::outOfScope,
                                 "No notes were attached, so none may be changed");

            if (! sequence.isValid())
                throw ToolError (tools::errors::notFound,
                                 "No part for that channel in that pattern");

            for (const auto& entry : *arguments["notes"].getArray())
                proposal.notes.push_back (readNoteChange (entry, sequence, proposal));
        }

        if (arguments["parameters"].isArray())
            for (const auto& entry : *arguments["parameters"].getArray())
                proposal.parameters.push_back (readParameterChange (entry, proposal));

        if (arguments["clips"].isArray())
            for (const auto& entry : *arguments["clips"].getArray())
                proposal.clips.push_back (readClipChange (entry, proposal));

        if (arguments["chain"].isArray())
            for (const auto& entry : *arguments["chain"].getArray())
                proposal.effects.push_back (readEffectChange (entry, proposal));

        if (proposal.notes.empty() && proposal.parameters.empty() && proposal.clips.empty()
             && proposal.effects.empty())
            throw ToolError (tools::errors::invalidArgument, "A proposal must change something");

        checkKeeps (proposal);

        const auto id = proposal.id;
        proposals.set (id, std::move (proposal));
        return object ({ { "proposal", proposals.getReference (id).summary() },
                         { "diff", diffOf (proposals.getReference (id)) } });
    }

    var getProposal (const var& arguments)
    {
        const auto id = arguments["proposal"].toString();
        if (! proposals.contains (id))
            throw ToolError (tools::errors::notFound, "No such proposal: " + id);

        return object ({ { "proposal", proposals.getReference (id).summary() },
                         { "diff", diffOf (proposals.getReference (id)) } });
    }

    /** Everything is checked again here, against the music as it is now rather than as
        it was when the proposal was made. Then it goes in as one undo, or not at all. */
    var applyProposal (const var& arguments, int revision)
    {
        const auto id = arguments["proposal"].toString();
        if (! proposals.contains (id))
            throw ToolError (tools::errors::notFound, "No such proposal: " + id);

        auto& proposal = proposals.getReference (id);

        // Pressing apply twice is one change, not two. The second press is told it was
        // already done rather than doing it again.
        if (proposal.applied)
            return object ({ { "already_applied", true },
                             { "proposal", proposal.summary() } });

        if (proposal.baseRevision != revision)
            throw ToolError (tools::errors::staleRevision,
                             "The project moved from revision " + String (proposal.baseRevision)
                               + " to " + String (revision) + " since this was worked out",
                             true);

        auto sequence = Model::findSequence (model.patternFor (proposal.patternID), proposal.channelID);

        proposal.placements = 0;
        for (auto clip : model.instances())
            if (clip[ids::pattern].toString() == proposal.patternID)
                ++proposal.placements;

        if (! proposal.notes.empty() && ! sequence.isValid())
            throw ToolError (tools::errors::notFound, "The part this would change is gone");

        // Everything that must hold is checked before anything is written, so a refusal
        // leaves the music exactly as it was rather than half changed.
        for (const auto& change : proposal.notes)
        {
            if (change.what == NoteChange::What::add)
                continue;

            if (! Model::withID (sequence, ids::NOTE, change.noteID).isValid())
                throw ToolError (tools::errors::notFound,
                                 "A note this would change is no longer there: " + change.noteID);

            if (! proposal.allowedNotes.contains (change.noteID))
                throw ToolError (tools::errors::outOfScope,
                                 "That note was not part of what was attached");
        }

        checkKeeps (proposal);

        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction (proposal.description.isNotEmpty()
                                    ? proposal.description : String ("Apply AI proposal"));

        for (const auto& change : proposal.notes)
        {
            if (change.what == NoteChange::What::remove)
            {
                sequence.removeChild (Model::withID (sequence, ids::NOTE, change.noteID), &undo);
                continue;
            }

            if (change.what == NoteChange::What::add)
            {
                ValueTree note (ids::NOTE);
                note.setProperty (ids::uid, Uuid().toString(), nullptr);
                // Filled in when the change was read and checked, not now: a default
                // decided at the last moment is a value nothing has verified.
                note.setProperty (ids::pitch, *change.pitch, nullptr);
                note.setProperty (ids::start, *change.startBeat, nullptr);
                note.setProperty (ids::length, *change.lengthBeats, nullptr);
                note.setProperty (ids::velocity, *change.velocity, nullptr);
                sequence.appendChild (note, &undo);
                continue;
            }

            auto note = Model::withID (sequence, ids::NOTE, change.noteID);
            if (change.pitch)       note.setProperty (ids::pitch, *change.pitch, &undo);
            if (change.startBeat)   note.setProperty (ids::start, *change.startBeat, &undo);
            if (change.lengthBeats) note.setProperty (ids::length, *change.lengthBeats, &undo);
            if (change.velocity)    note.setProperty (ids::velocity, *change.velocity, &undo);
        }

        // Placements are in the tree, so they go into the same transaction the notes
        // did and come back with them on one Undo.
        for (const auto& change : proposal.clips)
        {
            auto clip = model.placementFor (change.clipID);
            if (! clip.isValid())
                continue;

            if (change.what == ClipChange::What::makeUnique)
            {
                model.makeUnique (clip, &undo);
            }
            else if (change.what == ClipChange::What::remove)
            {
                model.instances().removeChild (clip, &undo);
            }
            else if (change.what == ClipChange::What::copy)
            {
                auto copy = clip.createCopy();
                copy.setProperty (ids::uid, Uuid().toString(), nullptr);
                if (change.startBeat) copy.setProperty (ids::start, *change.startBeat, nullptr);
                if (change.laneID)    copy.setProperty (ids::lane, *change.laneID, nullptr);
                model.instances().appendChild (copy, &undo);
            }
            else
            {
                if (change.startBeat) clip.setProperty (ids::start, *change.startBeat, &undo);
                if (change.laneID)    clip.setProperty (ids::lane, *change.laneID, &undo);
            }
        }

        // Chain changes are tree edits too, so they join the same transaction.
        for (const auto& change : proposal.effects)
        {
            auto insert = model.insertFor (change.insertID);
            if (! insert.isValid())
                continue;

            if (change.what == EffectChange::What::add)
            {
                model.addEffect (insert, change.effectType, &undo);
            }
            else if (change.what == EffectChange::What::send)
            {
                model.addSend (insert, change.targetID, change.level, &undo);
            }
            else
            {
                for (int i = insert.getNumChildren(); --i >= 0;)
                {
                    auto child = insert.getChild (i);

                    if (change.what == EffectChange::What::unsend)
                    {
                        if (child.hasType (ids::SEND)
                             && child[ids::target].toString() == change.targetID)
                        {
                            insert.removeChild (i, &undo);
                            break;
                        }
                        continue;
                    }

                    if (! child.hasType (ids::EFFECT) || Model::uidOf (child) != change.effectID)
                        continue;

                    if (change.what == EffectChange::What::remove)
                        insert.removeChild (i, &undo);
                    else if (change.what == EffectChange::What::bypass)
                        child.setProperty (ids::bypass, change.on, &undo);
                    else
                        insert.moveChild (i, change.toIndex, &undo);

                    break;
                }
            }
        }

        // A parameter's playback value does not live in the tree, so setting it would
        // fall outside the transaction the notes went into and the one Undo the person
        // is promised would leave it behind. It goes in as an action instead.
        for (const auto& change : proposal.parameters)
            if (auto* plugin = model.pluginFor (change.ownerID, change.pluginID))
                if (auto parameter = plugin->getAutomatableParameterByID (change.parameterID))
                    undo.perform (new ParameterAction (model.edit, plugin->itemID.toString(),
                                                       parameter->paramID,
                                                       parameter->getCurrentExplicitValue(),
                                                       parameter->valueRange.convertFrom0to1 (
                                                           static_cast<float> (change.value))));

        model.renderIfNeeded();
        proposal.applied = true;

        return object ({ { "applied", true },
                         { "proposal", proposal.summary() },
                         { "undo_description", undo.getUndoDescription() } });
    }

    /** One change to an insert's chain, checked before it is kept.

        The plugins are derived from the tree, so all of this is a tree edit - but what
        the tree may say is not everything a caller might ask for. An effect has to be
        one this machine actually has, a send has to point somewhere that exists and
        must not close a loop, and an index has to be inside the chain. */
    EffectChange readEffectChange (const var& entry, const Proposal& proposal) const
    {
        EffectChange change;
        change.insertID = entry["insert"].toString();

        if (! proposal.allowedInserts.contains (change.insertID))
            throw ToolError (tools::errors::outOfScope,
                             proposal.allowedInserts.isEmpty()
                               ? String ("No insert was attached, so no chain may be changed")
                               : "Insert " + change.insertID + " was not part of what was attached");

        auto insert = model.insertFor (change.insertID);
        if (! insert.isValid())
            throw ToolError (tools::errors::notFound, "No such insert: " + change.insertID);

        const auto what = entry["what"].toString();
        if (what == "add")         change.what = EffectChange::What::add;
        else if (what == "remove") change.what = EffectChange::What::remove;
        else if (what == "move")   change.what = EffectChange::What::move;
        else if (what == "bypass") change.what = EffectChange::What::bypass;
        else if (what == "send")   change.what = EffectChange::What::send;
        else if (what == "unsend") change.what = EffectChange::What::unsend;
        else
            throw ToolError (tools::errors::invalidArgument,
                             "A chain change's \"what\" must be add, remove, move, bypass, "
                             "send or unsend, not \"" + what + "\"");

        // How many effects are on this chain now, which is what an index is measured
        // against and what a removal is taken from.
        StringArray onTheChain;
        for (auto child : insert)
            if (child.hasType (ids::EFFECT))
                onTheChain.add (Model::uidOf (child));

        if (change.what == EffectChange::What::add)
        {
            change.effectType = entry["type"].toString();

            // Only what is actually here. A model naming a plugin this machine does not
            // have would otherwise produce a chain with a hole in it, discovered when
            // somebody presses play.
            auto known = false;
            for (const auto& available : model.availableEffects())
                if (available.first == change.effectType)
                    known = true;

            if (! known)
                throw ToolError (tools::errors::notFound,
                                 "No effect of that kind is installed: " + change.effectType);

            change.wasIndex = onTheChain.size();
            return change;
        }

        if (change.what == EffectChange::What::send || change.what == EffectChange::What::unsend)
        {
            change.targetID = entry["target"].toString();

            if (! model.insertFor (change.targetID).isValid())
                throw ToolError (tools::errors::notFound, "No such insert: " + change.targetID);

            if (change.targetID == change.insertID)
                throw ToolError (tools::errors::invalidArgument, "An insert cannot send to itself");

            if (change.what == EffectChange::What::send)
            {
                // A signal that reaches its own input does not get quieter. The app
                // already refuses this from its own menu; a proposal is refused for the
                // same reason and by the same test.
                if (model.wouldFeedBack (change.insertID, change.targetID))
                    throw ToolError (tools::errors::invalidArgument,
                                     "That send would feed the signal back into itself");

                change.level = entry.hasProperty ("level") ? number (entry, "level") : 0.25;

                if (change.level < 0.0 || change.level > 1.0)
                    throw ToolError (tools::errors::invalidArgument,
                                     "A send level is between 0 and 1");
            }

            return change;
        }

        change.effectID = entry["effect"].toString();
        change.wasIndex = onTheChain.indexOf (change.effectID);

        if (change.wasIndex < 0)
            throw ToolError (tools::errors::notFound,
                             "No such effect on that insert: " + change.effectID);

        for (auto child : insert)
            if (Model::uidOf (child) == change.effectID)
            {
                change.wasName = model.effectName (child[ids::type].toString());
                change.wasBypassed = static_cast<bool> (child[ids::bypass]);
            }

        if (change.what == EffectChange::What::bypass)
        {
            change.on = static_cast<bool> (entry.getProperty ("on", true));
            return change;
        }

        if (change.what == EffectChange::What::move)
        {
            change.toIndex = wholeNumber (entry, "to");

            if (change.toIndex < 0 || change.toIndex >= onTheChain.size())
                throw ToolError (tools::errors::invalidArgument,
                                 "There is no position " + String (change.toIndex)
                                   + " on that chain; it has " + String (onTheChain.size())
                                   + " effect(s)");
        }

        return change;
    }

    /** One placement move, checked against what was attached and against the shape of
        the arrangement before it is kept. */
    ClipChange readClipChange (const var& entry, const Proposal& proposal) const
    {
        ClipChange change;
        change.clipID = entry["id"].toString();

        const auto what = entry.getProperty ("what", "move").toString();
        if (what != "move" && what != "copy" && what != "remove" && what != "make_unique")
            throw ToolError (tools::errors::invalidArgument,
                             "A clip change's \"what\" must be move, copy, remove or "
                             "make_unique, not \"" + what + "\"");

        change.what = what == "copy" ? ClipChange::What::copy
                    : what == "remove" ? ClipChange::What::remove
                    : what == "make_unique" ? ClipChange::What::makeUnique
                                            : ClipChange::What::move;

        if (! proposal.allowedClips.contains (change.clipID))
            throw ToolError (tools::errors::outOfScope,
                             proposal.allowedClips.isEmpty()
                               ? String ("No region was attached, so no clip may be moved")
                               : "Clip " + change.clipID + " was not part of what was attached");

        auto clip = model.placementFor (change.clipID);
        if (! clip.isValid())
            throw ToolError (tools::errors::notFound, "No such clip: " + change.clipID);

        change.wasStart = static_cast<double> (clip[ids::start]);
        change.wasLane = clip[ids::lane].toString();

        if (entry.hasProperty ("start_beat"))
        {
            const auto start = number (entry, "start_beat");

            // Before the beginning is not a place. A clip there would play from part way
            // through itself or not at all, depending on who read it, which is the sort
            // of thing that is discovered much later.
            if (start < 0.0)
                throw ToolError (tools::errors::invalidArgument,
                                 "A clip cannot start before the beginning of the song");

            change.startBeat = start;
        }

        if (entry.hasProperty ("lane"))
        {
            const auto lane = entry["lane"].toString();
            if (! model.laneFor (lane).isValid())
                throw ToolError (tools::errors::notFound, "No such lane: " + lane);

            change.laneID = lane;
        }

        if (change.what == ClipChange::What::makeUnique)
        {
            if (change.startBeat || change.laneID)
                throw ToolError (tools::errors::invalidArgument,
                                 "Making a clip unique does not move it; ask for both "
                                 "separately if that is what you mean");

            // Nothing to break if it is already alone, and saying so is more use than
            // quietly doing nothing: a caller that asked for this wanted the sharing
            // gone, and should be told it was never there.
            auto sharing = 0;
            for (auto other : model.instances())
                if (other[ids::pattern].toString() == clip[ids::pattern].toString())
                    ++sharing;

            if (sharing <= 1)
                throw ToolError (tools::errors::invalidArgument,
                                 "This clip is the only one playing its pattern, so it is "
                                 "already unique");

            return change;
        }

        if (change.what == ClipChange::What::remove)
        {
            // Taking a placement out is not a move to nowhere. Saying where as well
            // would be a request that contradicts itself, and quietly ignoring half of
            // it is how a caller ends up believing something it did not get.
            if (change.startBeat || change.laneID)
                throw ToolError (tools::errors::invalidArgument,
                                 "Removing a clip does not take a start_beat or a lane");

            return change;
        }

        if (change.what == ClipChange::What::copy && ! change.startBeat)
            throw ToolError (tools::errors::invalidArgument,
                             "A copy has to say where it goes, or it would land on top of "
                             "the clip it came from");

        if (! change.startBeat && ! change.laneID)
            throw ToolError (tools::errors::invalidArgument,
                             "A clip move has to say where to: a start_beat, a lane, or both");

        return change;
    }

    NoteChange readNoteChange (const var& entry, ValueTree sequence, const Proposal& proposal) const
    {
        NoteChange change;
        const auto what = entry.getProperty ("what", "change").toString();

        // An unrecognised verb used to be treated as "change", so a typo - or a model
        // inventing "move" - silently became an edit of something. A word this service
        // does not know is a request it does not understand.
        if (what != "add" && what != "remove" && what != "change")
            throw ToolError (tools::errors::invalidArgument,
                             "A note change's \"what\" must be add, remove or change, not \"" + what + "\"");

        change.what = what == "add" ? NoteChange::What::add
                    : what == "remove" ? NoteChange::What::remove
                                       : NoteChange::What::change;
        change.noteID = entry["id"].toString();

        if (change.what != NoteChange::What::add)
        {
            auto note = Model::withID (sequence, ids::NOTE, change.noteID);
            if (! note.isValid())
                throw ToolError (tools::errors::notFound, "No such note: " + change.noteID);

            if (! proposal.allowedNotes.contains (change.noteID))
                throw ToolError (tools::errors::outOfScope,
                                 "Note " + change.noteID + " was not part of what was attached");

            change.wasPitch = static_cast<int> (note[ids::pitch]);
            change.wasStart = static_cast<double> (note[ids::start]);
            change.wasLength = static_cast<double> (note[ids::length]);
            change.wasVelocity = static_cast<int> (note[ids::velocity]);
        }

        if (entry.hasProperty ("pitch"))
        {
            const auto pitch = wholeNumber (entry, "pitch");
            if (pitch < 0 || pitch > 127)
                throw ToolError (tools::errors::invalidArgument,
                                 "pitch must be a MIDI note number from 0 to 127");
            change.pitch = pitch;
        }

        if (entry.hasProperty ("velocity"))
        {
            const auto velocity = wholeNumber (entry, "velocity");
            if (velocity < 1 || velocity > 127)
                throw ToolError (tools::errors::invalidArgument, "velocity must be from 1 to 127");
            change.velocity = velocity;
        }

        if (entry.hasProperty ("start_beat"))
        {
            const auto start = number (entry, "start_beat");
            if (start < 0.0)
                throw ToolError (tools::errors::invalidArgument, "start_beat cannot be negative");
            change.startBeat = start;
        }

        if (entry.hasProperty ("length_beats"))
        {
            const auto length = number (entry, "length_beats");
            if (length <= 0.0)
                throw ToolError (tools::errors::invalidArgument, "length_beats must be above zero");
            change.lengthBeats = length;
        }

        const auto patternLength = static_cast<double> (model.patternFor (proposal.patternID)[ids::length]);

        // What an added note is gets decided once, here, and everything downstream reads
        // it from the change rather than filling in its own idea. The two ends used to
        // disagree: validation treated a missing length as zero and let it through, and
        // applying treated it as one beat, so a note a third longer than its own pattern
        // could be approved on the strength of a length nobody was going to use.
        if (change.what == NoteChange::What::add)
        {
            if (! change.pitch)       change.pitch = 60;
            if (! change.velocity)    change.velocity = 100;
            if (! change.startBeat)   change.startBeat = 0.0;
            if (! change.lengthBeats) change.lengthBeats = patternLength > 0.0
                                                            ? std::min (1.0, patternLength) : 1.0;
        }

        // A note must stay inside the pattern it belongs to, or it would not be heard.
        const auto start = change.startBeat.value_or (change.wasStart);
        const auto length = change.lengthBeats.value_or (change.wasLength);

        if (patternLength > 0.0 && start + length > patternLength + 1.0e-6)
            throw ToolError (tools::errors::outOfScope,
                             "That note would run past the end of the pattern");

        return change;
    }

    ParameterChange readParameterChange (const var& entry, const Proposal& proposal) const
    {
        ParameterChange change;
        change.ownerID = entry["owner"].toString();
        change.pluginID = entry["plugin"].toString();
        change.parameterID = entry["parameter"].toString();

        // Nothing attached means nothing may be touched. Notes are held in by the
        // pattern and channel the attachment named, which a parameter change has no
        // equivalent of - so an empty list here has to mean "none", not "any", or a
        // question about some notes could come back proposing to move a fader.
        if (! proposal.allowedInserts.contains (change.ownerID))
            throw ToolError (tools::errors::outOfScope, "That insert was not part of what was attached");

        auto* parameter = model.automatableParameter (change.ownerID, change.pluginID, change.parameterID);
        if (parameter == nullptr)
            throw ToolError (tools::errors::notFound,
                             "No such parameter: " + change.parameterID + " on " + change.pluginID);

        if (! entry.hasProperty ("value"))
            throw ToolError (tools::errors::invalidArgument, "A parameter change needs a value");

        const auto value = number (entry, "value");
        if (value < 0.0 || value > 1.0)
            throw ToolError (tools::errors::invalidArgument,
                             "A parameter value is normalised, from 0 to 1");

        if (parameter->hasAutomationPoints())
            throw ToolError (tools::errors::locked,
                             "That parameter is driven by an automation curve; changing the "
                             "stored value would not be heard");

        change.value = value;
        change.wasValue = parameter->valueRange.convertTo0to1 (parameter->getCurrentExplicitValue());
        return change;
    }

    /** A whole number from a field, or a refusal. static_cast<int> on a var turns
        "abc" into 0 and 60.7 into 60, both of which are inside the MIDI range and
        neither of which is what anybody sent. A pitch that arrives as a string or a
        fraction is a request that was not understood. */
    static int wholeNumber (const var& entry, const char* field)
    {
        const auto value = entry[field];

        if (value.isInt() || value.isInt64())
            return static_cast<int> (value);

        if (value.isDouble())
        {
            const auto asDouble = static_cast<double> (value);
            if (std::abs (asDouble - std::round (asDouble)) < 1.0e-9)
                return static_cast<int> (std::round (asDouble));
        }

        throw ToolError (tools::errors::invalidArgument,
                         String (field) + " must be a whole number, not " + JSON::toString (value, true));
    }

    static double number (const var& entry, const char* field)
    {
        const auto value = entry[field];
        if (value.isInt() || value.isInt64() || value.isDouble())
            return static_cast<double> (value);

        throw ToolError (tools::errors::invalidArgument,
                         String (field) + " must be a number, not " + JSON::toString (value, true));
    }

    /** Measures the proposal against what the person said must not change. Saying it
        kept the rhythm is not the same as having kept it. */
    static void checkKeeps (const Proposal& proposal)
    {
        if (! proposal.keeps.any())
            return;

        for (const auto& change : proposal.notes)
        {
            if (proposal.keeps.rhythm && change.what != NoteChange::What::change)
                throw ToolError (tools::errors::locked,
                                 "Adding or removing notes changes the rhythm, which was to be kept");

            if (proposal.keeps.pitch && change.pitch && *change.pitch != change.wasPitch)
                throw ToolError (tools::errors::locked, "The pitch was to be kept");

            if (proposal.keeps.rhythm && change.startBeat
                 && std::abs (*change.startBeat - change.wasStart) > 1.0e-6)
                throw ToolError (tools::errors::locked, "The rhythm was to be kept");

            if (proposal.keeps.rhythm && change.lengthBeats
                 && std::abs (*change.lengthBeats - change.wasLength) > 1.0e-6)
                throw ToolError (tools::errors::locked, "The note lengths were to be kept");

            if (proposal.keeps.velocity && change.velocity && *change.velocity != change.wasVelocity)
                throw ToolError (tools::errors::locked, "The velocities were to be kept");
        }
    }

    static var diffOf (const Proposal& proposal)
    {
        Array<var> notes;
        for (const auto& change : proposal.notes)
        {
            auto entry = object ({ { "what", change.what == NoteChange::What::add ? "add"
                                           : change.what == NoteChange::What::remove ? "remove"
                                                                                     : "change" },
                                   { "id", change.noteID } });
            auto* fields = entry.getDynamicObject();

            if (change.what == NoteChange::What::change)
            {
                if (change.pitch)       fields->setProperty ("pitch", object ({ { "was", change.wasPitch }, { "now", *change.pitch } }));
                if (change.startBeat)   fields->setProperty ("start_beat", object ({ { "was", change.wasStart }, { "now", *change.startBeat } }));
                if (change.lengthBeats) fields->setProperty ("length_beats", object ({ { "was", change.wasLength }, { "now", *change.lengthBeats } }));
                if (change.velocity)    fields->setProperty ("velocity", object ({ { "was", change.wasVelocity }, { "now", *change.velocity } }));
            }
            else if (change.what == NoteChange::What::add)
            {
                fields->setProperty ("pitch", change.pitch.value_or (60));
                fields->setProperty ("start_beat", change.startBeat.value_or (0.0));
                fields->setProperty ("length_beats", change.lengthBeats.value_or (1.0));
                fields->setProperty ("velocity", change.velocity.value_or (100));
                // Same values the apply will use: the diff is a promise about what
                // pressing the button does, so it cannot be computed differently.
            }
            else
            {
                fields->setProperty ("pitch", change.wasPitch);
                fields->setProperty ("start_beat", change.wasStart);
            }

            notes.add (entry);
        }

        Array<var> parameters;
        Array<var> clips;
        for (const auto& change : proposal.clips)
        {
            auto entry = object ({ { "what", ClipChange::whatName (change.what) },
                                   { "id", change.clipID } });

            if (change.what == ClipChange::What::move || change.what == ClipChange::What::copy)
            {
                entry.getDynamicObject()->setProperty ("start_beat",
                    object ({ { "was", change.wasStart },
                              { "now", change.startBeat.value_or (change.wasStart) } }));
                entry.getDynamicObject()->setProperty ("lane",
                    object ({ { "was", change.wasLane },
                              { "now", change.laneID.value_or (change.wasLane) } }));
            }

            clips.add (entry);
        }

        Array<var> chain;
        for (const auto& change : proposal.effects)
        {
            auto entry = object ({ { "what", EffectChange::whatName (change.what) },
                                   { "insert", change.insertID } });
            auto* fields = entry.getDynamicObject();

            if (change.what == EffectChange::What::add)
                fields->setProperty ("type", change.effectType);
            else if (change.what == EffectChange::What::send
                      || change.what == EffectChange::What::unsend)
                fields->setProperty ("target", change.targetID);
            else
                fields->setProperty ("effect", change.effectID);

            if (change.wasName.isNotEmpty())
                fields->setProperty ("name", change.wasName);

            if (change.what == EffectChange::What::move)
                fields->setProperty ("position", object ({ { "was", change.wasIndex },
                                                           { "now", change.toIndex } }));
            else if (change.what == EffectChange::What::bypass)
                fields->setProperty ("bypassed", object ({ { "was", change.wasBypassed },
                                                           { "now", change.on } }));
            else if (change.what == EffectChange::What::send)
                fields->setProperty ("level", change.level);

            chain.add (entry);
        }

        for (const auto& change : proposal.parameters)
            parameters.add (object ({ { "owner", change.ownerID },
                                      { "plugin", change.pluginID },
                                      { "parameter", change.parameterID },
                                      { "was", change.wasValue },
                                      { "now", change.value } }));

        return object ({ { "notes", notes }, { "parameters", parameters },
                         { "clips", clips }, { "chain", chain } });
    }

    /** A clip, and what is actually played in it.

        A name and a length do not tell anyone anything about the music: two clips both
        called "Pattern 2", both sixteen beats, can be a bass line and a cluster chord.
        Asking what is wrong with a stretch of a song and being handed only the labels
        is being asked to guess, so the notes come too.

        `budget` is how many notes the whole region may still spend, shared across its
        clips so that a region with twenty clips does not return twenty thousand notes.
        Whatever does not fit is counted rather than silently dropped - a caller that
        can see something was left out can ask for it. */
    var describeClip (ValueTree clip, int& budget) const
    {
        auto pattern = model.patternFor (clip[ids::pattern].toString());

        Array<var> parts;
        auto omitted = 0;

        if (pattern.isValid())
            for (auto sequence : pattern)
            {
                if (! sequence.hasType (ids::SEQUENCE))
                    continue;

                const auto owner = sequence[ids::channel].toString();
                auto channel = model.channelFor (owner);

                Array<var> notes;
                for (auto note : sequence)
                {
                    if (! note.hasType (ids::NOTE))
                        continue;

                    if (budget <= 0)
                    {
                        ++omitted;
                        continue;
                    }

                    --budget;
                    notes.add (object ({ { "id", Model::uidOf (note) },
                                         { "pitch", static_cast<int> (note[ids::pitch]) },
                                         { "start_beat", static_cast<double> (note[ids::start]) },
                                         { "length_beats", static_cast<double> (note[ids::length]) },
                                         { "velocity", static_cast<int> (note[ids::velocity]) } }));
                }

                if (notes.isEmpty() && omitted == 0)
                    continue;

                parts.add (object ({ { "channel", owner },
                                     { "channel_name", channel.isValid() ? channel[ids::name].toString() : String() },
                                     { "instrument", channel.isValid() ? channel[ids::instrument].toString() : String() },
                                     { "notes", notes } }));
            }

        return object ({ { "id", Model::uidOf (clip) },
                         { "lane", clip[ids::lane].toString() },
                         { "pattern", clip[ids::pattern].toString() },
                         { "pattern_name", pattern.isValid() ? pattern[ids::name].toString() : String() },
                         { "start_beat", static_cast<double> (clip[ids::start]) },
                         { "length_beats", static_cast<double> (clip[ids::length]) },
                         { "parts", parts },
                         { "notes_omitted", omitted } });
    }

    double beat (const var& arguments, const char* field) const
    {
        if (! arguments.hasProperty (field))
            throw ToolError (tools::errors::invalidArgument, String (field) + " is required");

        const auto value = arguments[field];
        if (! value.isDouble() && ! value.isInt() && ! value.isInt64())
            throw ToolError (tools::errors::invalidArgument,
                             String (field) + " must be a number of beats");

        return static_cast<double> (value);
    }

    static var stringsOf (const StringArray& from)
    {
        Array<var> out;
        for (const auto& item : from)
            out.add (item);
        return out;
    }

    static var failure (const String& requestID, int revision, const String& code,
                        const String& message, bool retryable)
    {
        return object ({ { "status", "error" },
                         { "request_id", requestID },
                         { "contract_version", tools::contractVersion },
                         { "revision", revision },
                         { "error", object ({ { "code", code },
                                              { "message", message },
                                              { "retryable", retryable } }) } });
    }

    static constexpr int maxNotes = 4000;
    static constexpr double maxRegionBeats = 4096.0;

    Model& model;
    Selection& selection;
    String projectID, sessionID;
    HashMap<String, var> answered;
    HashMap<String, Proposal> proposals;
};

} // namespace live
