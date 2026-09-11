#pragma once

#include "Attachment.h"

namespace live
{

/** A change that has been worked out but not made.

    Everything an assistant wants to do arrives here first. A proposal knows exactly
    which notes and parameters it would touch, what they are now, and what they would
    become - so a person can see it, and so the app can refuse it without having to
    trust whoever produced it.

    Three things are checked, and they are checked twice: once when the proposal is
    made, and again at the moment it is applied. In between, a person may have moved
    a note, changed the tempo, or undone something, and a proposal that was fine a
    minute ago may not be fine now.

      Scope   - it may only touch what was attached. Reading around the selection is
                encouraged; changing anything outside it is refused.
      Keeps   - what the person said must not change. "Keep the rhythm" means starts
                and lengths are compared before and after, not merely promised.
      Base    - the music it was worked out against. If that has moved, the proposal
                is stale and is refused rather than applied to something it never saw.

    A proposal is data. Applying one is a single undo, so taking it back is one press
    whatever it touched.
*/
struct NoteChange
{
    enum class What { change, add, remove };

    What what = What::change;
    String noteID;              // empty for an addition, which gets one when applied

    // Only the fields that are present are changed; the rest are left as they are.
    std::optional<int> pitch;
    std::optional<double> startBeat;
    std::optional<double> lengthBeats;
    std::optional<int> velocity;

    // What they are now, filled in when the proposal is made, so a person sees both.
    int wasPitch = 0, wasVelocity = 0;
    double wasStart = 0.0, wasLength = 0.0;
};

struct ParameterChange
{
    String ownerID;     // the channel or insert whose plugin this is
    String pluginID;
    String parameterID;
    double value = 0.0; // normalised
    double wasValue = 0.0;
};

/** What the person insisted must not change. A proposal is measured against these, not
    trusted to have respected them. */
struct Keeps
{
    bool pitch = false;
    bool rhythm = false;      // starts and lengths
    bool velocity = false;

    static Keeps fromJson (const var& from)
    {
        Keeps keeps;
        keeps.pitch = static_cast<bool> (from["pitch"]);
        keeps.rhythm = static_cast<bool> (from["rhythm"]);
        keeps.velocity = static_cast<bool> (from["velocity"]);
        return keeps;
    }

    var toJson() const
    {
        return object ({ { "pitch", pitch }, { "rhythm", rhythm }, { "velocity", velocity } });
    }

    bool any() const { return pitch || rhythm || velocity; }
};

struct Proposal
{
    String id;
    String requestID;
    String description;

    int baseRevision = 0;

    // What it is allowed to touch, taken from the attachment it was made against.
    String patternID, channelID;
    StringArray allowedNotes;   // empty means "any note of this channel's part"
    StringArray allowedInserts;

    Keeps keeps;

    std::vector<NoteChange> notes;
    std::vector<ParameterChange> parameters;

    /** How many places in the song this pattern is played. A note change edits the
        pattern, so it is heard everywhere the pattern is placed - and the person has to
        be told that before they press Apply, not after. */
    int placements = 1;

    bool applied = false;

    var summary() const
    {
        int changed = 0, added = 0, removed = 0;
        for (const auto& note : notes)
            switch (note.what)
            {
                case NoteChange::What::add:    ++added; break;
                case NoteChange::What::remove: ++removed; break;
                default:                       ++changed; break;
            }

        return object ({ { "id", id },
                         { "description", description },
                         { "base_revision", baseRevision },
                         { "applied", applied },
                         { "notes_changed", changed },
                         { "notes_added", added },
                         { "notes_removed", removed },
                         { "parameters_changed", static_cast<int> (parameters.size()) },
                         { "placements", placements },
                         { "keeps", keeps.toJson() } });
    }
};

} // namespace live
