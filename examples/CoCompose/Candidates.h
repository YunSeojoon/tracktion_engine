#pragma once

#include "Support.h"

namespace live
{

/** The alternatives that were offered, kept so a person can go back to one.

    Asking twice is how composing with an assistant actually goes: three versions of
    the same eight bars, listen to each, take the second. Until now the second one was
    gone by the time the third arrived - a proposal lived in memory for as long as the
    app did, and a person who wanted the earlier one had to ask for it again and hope.
    Hoping is the problem: the model does not have the earlier one either, so what
    comes back is a fourth version being described as the second.

    So a candidate keeps everything needed to speak about it later without asking
    anyone to remember: what it would do, the music it was worked out against, whether
    it was taken, and the fingerprints of the two files it was compared with. The
    fingerprints rather than the files, because a preview writes to the same two paths
    every time - a candidate that still names them would be pointing at whatever was
    rendered last, which is the confident kind of wrong.

    None of this is music. Keeping a candidate does not move the revision and does not
    enter the undo history, and throwing one away deletes nothing that is playing: a
    candidate that was adopted is a record that it was, not the notes themselves. That
    is the whole reason discarding the ones you did not want is safe.
*/
struct Candidate
{
    String id;              // the proposal it was made from
    String description;     // what it was called when it was offered
    String requestID;       // the question it answers, so siblings can be found
    int baseRevision = 0;   // the music it was worked out against
    int64 atMillis = 0;
    bool adopted = false;

    /** What it would do, kept as the diff the tools produced. Verbatim on purpose: a
        candidate read back tomorrow has to say what it said when it was offered, and
        recomputing it against music that has moved would say something else. */
    var diff;

    /** The comparison it was listened to with, if one was made. Empty means nobody has
        rendered it yet, which is different from having rendered it and heard nothing. */
    String beforeFingerprint, afterFingerprint;
    String previewNote;

    var toJSON() const
    {
        return object ({ { "id", id },
                         { "description", description },
                         { "request_id", requestID },
                         { "base_revision", baseRevision },
                         { "at_ms", atMillis },
                         { "adopted", adopted },
                         { "diff", diff },
                         { "preview", object ({ { "before_fingerprint", beforeFingerprint },
                                                { "after_fingerprint", afterFingerprint },
                                                { "note", previewNote } }) } });
    }

    static Candidate fromJSON (const var& entry)
    {
        Candidate kept;
        kept.id = entry["id"].toString();
        kept.description = entry["description"].toString();
        kept.requestID = entry["request_id"].toString();
        kept.baseRevision = static_cast<int> (entry["base_revision"]);
        kept.atMillis = static_cast<int64> (entry["at_ms"]);
        kept.adopted = static_cast<bool> (entry["adopted"]);
        kept.diff = entry["diff"];
        kept.beforeFingerprint = entry["preview"]["before_fingerprint"].toString();
        kept.afterFingerprint = entry["preview"]["after_fingerprint"].toString();
        kept.previewNote = entry["preview"]["note"].toString();
        return kept;
    }
};

class Shelf
{
public:
    Shelf (File storage, String project)
        : file (std::move (storage)), projectID (std::move (project))
    {
        load();
    }

    const std::vector<Candidate>& all() const { return kept; }

    /** Every candidate offered for one question, oldest first. This is what "less
        complicated than the last one" has to point at: without it, a follow-up is
        resolved by whatever the model remembers, and the model is the one thing here
        that cannot be relied on to remember. */
    std::vector<Candidate> forRequest (const String& requestID) const
    {
        std::vector<Candidate> mine;
        for (const auto& one : kept)
            if (one.requestID == requestID)
                mine.push_back (one);
        return mine;
    }

    const Candidate* find (const String& id) const
    {
        for (const auto& one : kept)
            if (one.id == id)
                return &one;
        return nullptr;
    }

    /** The one offered most recently, which is what a person means by "that one". */
    const Candidate* mostRecent() const
    {
        return kept.empty() ? nullptr : &kept.back();
    }

    /** Keeps a proposal as a candidate. Offering the same proposal twice updates what
        is there rather than shelving it twice - a second look at one alternative is
        not a second alternative. */
    void keep (const Candidate& candidate)
    {
        for (auto& one : kept)
            if (one.id == candidate.id)
            {
                const auto wasAdopted = one.adopted;
                one = candidate;
                one.adopted = wasAdopted || candidate.adopted;
                save();
                return;
            }

        kept.push_back (candidate);
        save();
    }

    /** Records that a comparison was rendered for a candidate. The app knows the
        fingerprints; the shelf keeps them so a later look can tell whether the files
        on disk are still the ones this candidate was heard with. */
    bool noteComparison (const String& id, const String& before, const String& after,
                         const String& note)
    {
        for (auto& one : kept)
            if (one.id == id)
            {
                one.beforeFingerprint = before;
                one.afterFingerprint = after;
                one.previewNote = note;
                save();
                return true;
            }
        return false;
    }

    /** Marks one as taken. The others are left exactly as they are: a person who takes
        the second of three has not rejected the first and the third, and a shelf that
        cleared them would be deciding that for them. */
    bool adopt (const String& id)
    {
        for (auto& one : kept)
            if (one.id == id)
            {
                one.adopted = true;
                save();
                return true;
            }
        return false;
    }

    /** Throws one away. This removes a record of an offer, never any music: if it was
        adopted, the notes it put in the song stay where they are and Undo is still the
        way to take them back. */
    bool discard (const String& id)
    {
        for (size_t i = 0; i < kept.size(); ++i)
            if (kept[i].id == id)
            {
                kept.erase (kept.begin() + static_cast<long> (i));
                save();
                return true;
            }
        return false;
    }

    /** Everything about the shelf that the app reports, in one short string.

        The inspector packet is only rewritten when a key made of what it describes
        moves, so anything in the packet that is missing from the key stops being
        reported the moment it changes. That has now caught four different fields in
        this app; the shelf is not going to be the fifth, and keeping the key here
        rather than at the call site is what stops it drifting from the contents. */
    String shape() const
    {
        String key (kept.size());
        for (const auto& one : kept)
            key << "|" << one.id << (one.adopted ? "+" : "-") << one.afterFingerprint;
        return key;
    }

    var snapshot() const
    {
        Array<var> entries;
        for (const auto& one : kept)
            entries.add (one.toJSON());

        return object ({ { "project_id", projectID }, { "candidates", entries } });
    }

private:
    void load()
    {
        if (! file.existsAsFile())
            return;

        const auto stored = JSON::parse (file.loadFileAsString());
        if (! stored.isObject())
            return;

        // A shelf belongs to a project. A copied project folder must not inherit the
        // alternatives offered for the original - the same rule the notes follow, for
        // the same reason: they were about music that is now somewhere else.
        if (stored["project_id"].toString() != projectID)
            return;

        if (auto* saved = stored["candidates"].getArray())
            for (const auto& entry : *saved)
                kept.push_back (Candidate::fromJSON (entry));
    }

    void save() const
    {
        file.getParentDirectory().createDirectory();
        atomicWrite (file, JSON::toString (snapshot(), false));
    }

    File file;
    String projectID;
    std::vector<Candidate> kept;
};

} // namespace live
