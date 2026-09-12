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

/** Moving a placement: where a clip sits, and which lane it sits on.

    The first thing a proposal may do that is not about a note or a knob. A clip is a
    reference to a pattern, so moving one moves where that music is heard without
    copying or altering the music itself - which is why it can be offered before the
    harder arrangement edits: nothing here can damage a pattern.

    Scope comes from a region attachment, which already knows which clips fall inside
    it. A clip outside that is refused the same way a note outside an attachment is. */
struct ClipChange
{
    enum class What { move, copy, remove, makeUnique };

    What what = What::move;
    String clipID;              // the placement being moved, copied or taken out
    std::optional<double> startBeat;
    std::optional<String> laneID;

    // What they are now, so a person sees both sides before deciding.
    double wasStart = 0.0;
    String wasLane;

    static String whatName (What w)
    {
        switch (w)
        {
            case What::copy:       return "copy";
            case What::remove:     return "remove";
            case What::makeUnique: return "make_unique";
            default:               return "move";
        }
    }
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
    // Empty means nothing may be touched, not everything. It meant the opposite once,
    // and a question with no notes attached could therefore rewrite a whole part; the
    // fourth review reproduced it. "No permission" and "the whole part" have to be
    // different values, and the caller says which by listing what it was given.
    StringArray allowedNotes;
    StringArray allowedInserts;

    /** Which placements may be moved, from the region that was attached. Empty means
        none, for the same reason the note list does. */
    StringArray allowedClips;

    Keeps keeps;

    std::vector<NoteChange> notes;
    std::vector<ParameterChange> parameters;
    std::vector<ClipChange> clips;

    /** How many places in the song this pattern is played. A note change edits the
        pattern, so it is heard everywhere the pattern is placed - and the person has to
        be told that before they press Apply, not after. */
    int placements = 1;

    bool applied = false;

    /** Applies the note changes to a detached copy of the project tree.

        This is for previewing: the copy is rendered and thrown away, so there is no
        undo manager, no engine and no live model involved - and nothing here can reach
        the song, because the tree it is handed is not the song's.

        Parameter changes are not here, because they are not in the tree: they live
        inside the plugins. applyParametersTo does that half, on the copy's own
        plugins, and a preview runs both - a comparison that left half a proposal out
        would be worse than no comparison, because it would be believed. */
    bool applyNotesTo (ValueTree coCompose) const
    {
        auto patterns = coCompose.getChildWithName (ids::PATTERNS);
        if (! patterns.isValid())
            return false;

        ValueTree sequence;

        for (auto pattern : patterns)
            if (Model::uidOf (pattern) == patternID)
                for (auto part : pattern)
                    if (part.hasType (ids::SEQUENCE) && part[ids::channel].toString() == channelID)
                        sequence = part;

        if (! sequence.isValid())
            return false;

        for (const auto& change : notes)
        {
            if (change.what == NoteChange::What::add)
            {
                ValueTree note (ids::NOTE);
                note.setProperty (ids::uid, Uuid().toString(), nullptr);
                note.setProperty (ids::pitch, change.pitch.value_or (60), nullptr);
                note.setProperty (ids::start, change.startBeat.value_or (0.0), nullptr);
                note.setProperty (ids::length, change.lengthBeats.value_or (1.0), nullptr);
                note.setProperty (ids::velocity, change.velocity.value_or (100), nullptr);
                sequence.appendChild (note, nullptr);
                continue;
            }

            for (int i = sequence.getNumChildren(); --i >= 0;)
            {
                auto note = sequence.getChild (i);
                if (Model::uidOf (note) != change.noteID)
                    continue;

                if (change.what == NoteChange::What::remove)
                {
                    sequence.removeChild (i, nullptr);
                    break;
                }

                if (change.pitch)       note.setProperty (ids::pitch, *change.pitch, nullptr);
                if (change.startBeat)   note.setProperty (ids::start, *change.startBeat, nullptr);
                if (change.lengthBeats) note.setProperty (ids::length, *change.lengthBeats, nullptr);
                if (change.velocity)    note.setProperty (ids::velocity, *change.velocity, nullptr);
                break;
            }
        }

        return true;
    }

    /** Moves the placements on a detached copy of the tree, for a preview.

        Clips are in the tree, so unlike a parameter this needs nothing but the tree -
        and unlike a note change it does not touch a pattern, so the music itself is
        the same music in a different place. */
    int applyClipsTo (ValueTree coCompose) const
    {
        auto playlist = coCompose.getChildWithName (ids::PLAYLIST);
        auto instances = playlist.isValid() ? playlist.getChildWithName (ids::CLIPS) : ValueTree();
        if (! instances.isValid())
            return 0;

        auto done = 0;

        for (const auto& change : clips)
            for (int i = instances.getNumChildren(); --i >= 0;)
            {
                auto clip = instances.getChild (i);
                if (Model::uidOf (clip) != change.clipID)
                    continue;

                if (change.what == ClipChange::What::makeUnique)
                {
                    // The clip stops sharing its pattern and gets one of its own. On a
                    // copy, with no undo manager and no engine, this is the same two
                    // steps the live path takes: copy the pattern with fresh note ids,
                    // and point the placement at the copy.
                    auto patterns = coCompose.getChildWithName (ids::PATTERNS);
                    if (! patterns.isValid())
                        break;

                    for (auto pattern : patterns)
                        if (Model::uidOf (pattern) == clip[ids::pattern].toString())
                        {
                            auto own = pattern.createCopy();
                            own.setProperty (ids::uid, Uuid().toString(), nullptr);
                            for (auto sequence : own)
                                for (auto note : sequence)
                                    note.setProperty (ids::uid, Uuid().toString(), nullptr);

                            patterns.appendChild (own, nullptr);
                            clip.setProperty (ids::pattern, Model::uidOf (own), nullptr);
                            break;
                        }
                }
                else if (change.what == ClipChange::What::remove)
                {
                    instances.removeChild (i, nullptr);
                }
                else if (change.what == ClipChange::What::copy)
                {
                    // A copy of the placement, not of the music: the new clip points at
                    // the same pattern, so the two are the same part played twice and
                    // editing either edits both - which is what a placement is for.
                    auto copy = clip.createCopy();
                    copy.setProperty (ids::uid, Uuid().toString(), nullptr);
                    if (change.startBeat) copy.setProperty (ids::start, *change.startBeat, nullptr);
                    if (change.laneID)    copy.setProperty (ids::lane, *change.laneID, nullptr);
                    instances.appendChild (copy, nullptr);
                }
                else
                {
                    if (change.startBeat) clip.setProperty (ids::start, *change.startBeat, nullptr);
                    if (change.laneID)    clip.setProperty (ids::lane, *change.laneID, nullptr);
                }

                ++done;
                break;
            }

        return done;
    }

    /** The other half, on a copy's plugins rather than a copy's tree.

        `copy` is a model wrapper over the Edit the render will run, so this reaches its
        plugins the way the live path reaches the song's - and, like applyNotesTo, it
        touches nothing that outlives the render, because the Edit it is given is thrown
        away when the comparison is done. No undo manager: there is nothing to take back
        from an Edit that is about to stop existing.

        Returns how many of the changes found something to set. A parameter that is not
        there - a plugin removed since the proposal was worked out - is the caller's to
        report, because a comparison missing part of what Apply would do is exactly the
        thing this whole path exists to avoid. */
    int applyParametersTo (Model& copy) const
    {
        auto set = 0;

        for (const auto& change : parameters)
            if (auto* plugin = copy.pluginFor (change.ownerID, change.pluginID))
                if (auto parameter = plugin->getAutomatableParameterByID (change.parameterID))
                {
                    parameter->setParameter (parameter->valueRange.convertFrom0to1 (
                                                 static_cast<float> (change.value)),
                                             juce::sendNotification);
                    ++set;
                }

        return set;
    }

    int countClips (ClipChange::What what) const
    {
        auto n = 0;
        for (const auto& change : clips)
            if (change.what == what)
                ++n;
        return n;
    }

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
                         { "clips_moved", countClips (ClipChange::What::move) },
                         { "clips_added", countClips (ClipChange::What::copy) },
                         { "clips_removed", countClips (ClipChange::What::remove) },
                         { "clips_made_unique", countClips (ClipChange::What::makeUnique) },
                         { "placements", placements },
                         { "keeps", keeps.toJson() } });
    }
};

} // namespace live
