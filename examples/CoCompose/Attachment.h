#pragma once

#include "Model.h"

namespace live
{

/** What a person picked out of the song to talk about.

    A selection in a panel is a moving thing: click somewhere else and it is gone. An
    attachment is the opposite. It is taken once, at the moment it is attached, and
    from then on it names exactly the same music no matter where the person clicks
    next. That is the whole point - a question about bars 9 to 16 must still be about
    bars 9 to 16 when the answer arrives.

    So everything here is stable identifiers and beat ranges, never panel state and
    never array positions. Names are carried for the person to read, not to find
    anything by: two channels can share a name, and a channel can be renamed after the
    attachment was made.

    An attachment holds no music. It is a reference, and the thing it refers to can be
    deleted while it exists - which is why every attachment can say whether what it
    points at is still there.
*/
struct Attachment
{
    enum class Kind { region, notes, insert };

    String id;                  // this attachment, for the life of the conversation
    Kind kind = Kind::region;
    int takenAtRevision = 0;    // what the music looked like when it was taken

    // Region: which lanes and which beats.
    StringArray laneIDs, clipIDs, channelIDs;
    double startBeat = 0.0, endBeat = 0.0;

    // Notes: which pattern, which channel's part of it, and which notes.
    String patternID, noteChannelID;
    StringArray noteIDs;

    // Insert: which mixer insert.
    String insertID;

    // Names as they read when this was taken, for the card only.
    StringArray displayNames;
    int patternUseCount = 0;    // how many placements share the pattern behind this

    static String kindName (Kind k)
    {
        switch (k)
        {
            case Kind::notes:  return "notes";
            case Kind::insert: return "insert";
            default:           return "region";
        }
    }
};

//==============================================================================
/** Turns what a panel has selected into an attachment, and answers questions about
    one afterwards. Everything it produces is read from the model at the moment it is
    called; nothing here changes the music. */
class Attachments
{
public:
    explicit Attachments (Model& m) : model (m) {}

    /** Bars nine to sixteen on these lanes. The beat range is taken as given rather
        than from the clips, so a region can cover empty space - a person asking "what
        should go here" is asking about a gap. */
    Attachment fromRegion (const StringArray& lanes, double startBeat, double endBeat, int revision) const
    {
        Attachment a;
        a.id = Uuid().toString();
        a.kind = Attachment::Kind::region;
        a.takenAtRevision = revision;
        a.laneIDs = lanes;
        a.startBeat = std::min (startBeat, endBeat);
        a.endBeat = std::max (startBeat, endBeat);

        // Which clips actually fall in the range, and which channels they play through.
        for (auto clip : model.instances())
        {
            const auto clipStart = static_cast<double> (clip[ids::start]);
            const auto clipEnd = clipStart + static_cast<double> (clip[ids::length]);

            if (! lanes.isEmpty() && ! lanes.contains (clip[ids::lane].toString()))
                continue;

            // Half-open: a clip that ends exactly where the region starts is not in it.
            if (clipEnd <= a.startBeat || clipStart >= a.endBeat)
                continue;

            a.clipIDs.add (Model::uidOf (clip));

            auto pattern = model.patternFor (clip[ids::pattern].toString());
            for (auto sequence : pattern)
                if (sequence.hasType (ids::SEQUENCE))
                    a.channelIDs.addIfNotAlreadyThere (sequence[ids::channel].toString());
        }

        for (const auto& channelID : a.channelIDs)
            a.displayNames.add (nameOfChannel (channelID));

        return a;
    }

    /** Some notes of one channel's part of one pattern. The pattern may be placed more
        than once, so the count of placements comes with it: changing these notes
        changes every one of them unless the person asks for this placement alone. */
    Attachment fromNotes (const String& patternID, const String& channelID,
                          const StringArray& notes, int revision) const
    {
        Attachment a;
        a.id = Uuid().toString();
        a.kind = Attachment::Kind::notes;
        a.takenAtRevision = revision;
        a.patternID = patternID;
        a.noteChannelID = channelID;
        a.noteIDs = notes;
        a.patternUseCount = placementsOf (patternID);

        auto pattern = model.patternFor (patternID);
        a.displayNames.add (pattern.isValid() ? pattern[ids::name].toString() : String());
        a.displayNames.add (nameOfChannel (channelID));

        // The beats the notes actually cover, so a card can say where to look.
        bool any = false;
        if (auto sequence = Model::findSequence (pattern, channelID); sequence.isValid())
            for (auto note : sequence)
            {
                if (! note.hasType (ids::NOTE) || ! notes.contains (Model::uidOf (note)))
                    continue;

                const auto from = static_cast<double> (note[ids::start]);
                const auto to = from + static_cast<double> (note[ids::length]);
                a.startBeat = any ? std::min (a.startBeat, from) : from;
                a.endBeat = any ? std::max (a.endBeat, to) : to;
                any = true;
            }

        return a;
    }

    /** One mixer insert: the signal path, not the channel. */
    Attachment fromInsert (const String& insertID, int revision) const
    {
        Attachment a;
        a.id = Uuid().toString();
        a.kind = Attachment::Kind::insert;
        a.takenAtRevision = revision;
        a.insertID = insertID;

        auto insert = model.insertFor (insertID);
        if (insert.isValid())
            a.displayNames.add (String (static_cast<int> (insert[ids::index])) + " "
                                  + insert[ids::name].toString());

        return a;
    }

    /** Whether what this points at is still in the song. A deleted target is said to be
        missing rather than quietly re-pointed at something else. */
    bool stillExists (const Attachment& a) const
    {
        switch (a.kind)
        {
            case Attachment::Kind::notes:
            {
                auto sequence = Model::findSequence (model.patternFor (a.patternID), a.noteChannelID);
                if (! sequence.isValid())
                    return false;

                for (const auto& noteID : a.noteIDs)
                    if (Model::withID (sequence, ids::NOTE, noteID).isValid())
                        return true;

                return false;
            }

            case Attachment::Kind::insert:
                return model.insertFor (a.insertID).isValid();

            case Attachment::Kind::region:
            default:
                for (const auto& laneID : a.laneIDs)
                    if (model.laneFor (laneID).isValid())
                        return true;

                return a.laneIDs.isEmpty();
        }
    }

    /** How many of the notes an attachment named are still there. A person editing
        between asking and answering is normal, and the card says so. */
    int survivingNotes (const Attachment& a) const
    {
        auto sequence = Model::findSequence (model.patternFor (a.patternID), a.noteChannelID);
        if (! sequence.isValid())
            return 0;

        int found = 0;
        for (const auto& noteID : a.noteIDs)
            if (Model::withID (sequence, ids::NOTE, noteID).isValid())
                ++found;

        return found;
    }

    /** The line on the card. Bars, not beats: beats are what the model counts in and
        bars are what a person reads. */
    String summary (const Attachment& a) const
    {
        const auto missing = ! stillExists (a);

        switch (a.kind)
        {
            case Attachment::Kind::notes:
            {
                const auto left = survivingNotes (a);
                auto text = a.displayNames.size() > 1 ? a.displayNames[1] : String ("Notes");
                text += " - " + String (left) + (left == 1 ? " note" : " notes");
                if (left != a.noteIDs.size())
                    text += " of " + String (a.noteIDs.size());
                if (a.patternUseCount > 1)
                    text += ", pattern used " + String (a.patternUseCount) + " times";
                return missing ? text + " (gone)" : text;
            }

            case Attachment::Kind::insert:
                return (a.displayNames.isEmpty() ? String ("Insert") : "Insert " + a.displayNames[0])
                         + (missing ? " (gone)" : "");

            case Attachment::Kind::region:
            default:
            {
                auto text = barRange (a.startBeat, a.endBeat);
                if (! a.displayNames.isEmpty())
                    text += " - " + a.displayNames.joinIntoString ("/");
                return missing ? text + " (gone)" : text;
            }
        }
    }

    /** Bars counted from one, the way they are numbered on screen. */
    String barRange (double startBeat, double endBeat) const
    {
        const auto first = barNumber (startBeat);
        // A range that ends exactly on a bar line ends in the bar before it.
        const auto last = std::max (first, barNumber (std::max (startBeat, endBeat - 1.0e-6)));
        return first == last ? "bar " + String (first)
                             : "bars " + String (first) + "-" + String (last);
    }

    int barNumber (double beat) const
    {
        const auto position = model.edit.tempoSequence.toBarsAndBeats (
            model.edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat)));
        return position.bars + 1;
    }

private:
    String nameOfChannel (const String& channelID) const
    {
        auto channel = model.channelFor (channelID);
        return channel.isValid() ? channel[ids::name].toString() : String();
    }

    int placementsOf (const String& patternID) const
    {
        int count = 0;
        for (auto clip : model.instances())
            if (clip[ids::pattern].toString() == patternID)
                ++count;
        return count;
    }

    Model& model;
};

} // namespace live
