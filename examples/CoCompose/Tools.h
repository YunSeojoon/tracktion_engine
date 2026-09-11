#pragma once

#include "Attachment.h"

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
    var capabilities() const
    {
        Array<var> readTools;
        for (const auto* name : { "get_capabilities", "get_selection", "inspect_region",
                                  "inspect_insert", "inspect_pattern" })
            readTools.add (name);

        return object ({
            { "contract_version", tools::contractVersion },
            { "project_id", projectID },
            { "session_id", sessionID },
            { "tools", readTools },
            { "writes", var (Array<var>()) },
            { "notes", "Write tools are not implemented in this build and are not offered." },
            { "units", object ({ { "time", "quarter-note beats, ranges are [start_beat, end_beat)" },
                                 { "pitch", "MIDI note number, 0-127" },
                                 { "velocity", "1-127" },
                                 { "parameter", "normalised 0..1" },
                                 { "gain", "decibels" },
                                 { "pan", "-1 left to 1 right" } }) },
            { "audio", object ({ { "can_send_audio", false },
                                 { "reason", "No connection is wired up yet" } }) },
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

        // Named in the contract but not built yet. Saying so by name is more use than
        // "unknown tool", because it tells a caller this is a version problem.
        for (const auto* later : { "create_proposal", "preview_proposal", "apply_proposal",
                                   "get_operation", "cancel_operation" })
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

        for (auto clip : model.instances())
        {
            const auto start = static_cast<double> (clip[ids::start]);
            const auto end = start + static_cast<double> (clip[ids::length]);

            if (! lanes.isEmpty() && ! lanes.contains (clip[ids::lane].toString()))
                continue;

            if (end > from && start < to)             inRange.add (describeClip (clip));
            else if (end > from - padding && start < to + padding)
                                                       around.add (describeClip (clip));
        }

        return object ({ { "start_beat", from }, { "end_beat", to },
                         { "bars", Attachments (model).barRange (from, to) },
                         { "lanes", stringsOf (lanes) },
                         { "tempo", model.edit.tempoSequence.getBpmAt (
                                        model.edit.tempoSequence.toTime (
                                            te::BeatPosition::fromBeats (from))) },
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

    var describeClip (ValueTree clip) const
    {
        auto pattern = model.patternFor (clip[ids::pattern].toString());
        return object ({ { "id", Model::uidOf (clip) },
                         { "lane", clip[ids::lane].toString() },
                         { "pattern", clip[ids::pattern].toString() },
                         { "pattern_name", pattern.isValid() ? pattern[ids::name].toString() : String() },
                         { "start_beat", static_cast<double> (clip[ids::start]) },
                         { "length_beats", static_cast<double> (clip[ids::length]) } });
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
};

} // namespace live
