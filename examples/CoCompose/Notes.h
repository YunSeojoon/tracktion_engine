#pragma once

#include "Support.h"

namespace live
{

/** What is known about a project, kept apart by who said it.

    Three kinds of thing get called "the notes" and collapsing them is how a chat
    assistant starts confidently building on something nobody agreed to:

    A person's condition is a rule. "Keep it in D minor", "the drums stay as they are".
    It was decided by the person - typed, or offered by the assistant and then accepted -
    and it holds until they change it.

    A guess is the assistant's reading of the music. "This sounds like D minor." It may
    be right and it is still a guess, so it carries who said it and against which
    revision, and it never becomes a condition on its own. Something that arrives as a
    guess and is later treated as a rule is exactly how an assistant ends up insisting
    on a key the person never chose.

    And the current request is neither: it is what is being asked right now, and it
    expires when the answer comes.

    None of this is music. Writing a note does not move the revision, does not enter the
    undo history, and cannot be undone by Ctrl+Z - because a person who undoes an edit
    is undoing an edit, not forgetting a decision they made an hour ago. It is stored
    beside the project, with the conversation, for the same reason the conversation is.
*/
struct Note
{
    enum class Kind { condition, guess, request, todo };

    /** Where in the song a note is about, and what should happen to that when the music
        moves underneath it.

        Two answers are both right, for different notes, and guessing which is meant is
        how a note ends up pointing at the wrong bar. "The drop is too early" is about a
        moment in the arrangement and stays there when a clip is dragged past it.
        "Rewrite this fill" is about a particular clip and follows it. A note that
        cannot say which it is has to be treated as one of them, and either choice is
        wrong half the time.

        And a clip can be deleted. A note that followed it then points at nothing, and
        the honest thing is to say so rather than to quietly re-aim it at whatever is
        nearest or to keep showing the place it used to be as though it were still
        there. It keeps where it was, and it says the link is broken. */
    enum class Anchor { nowhere, time, clip };

    String id;
    Kind kind = Kind::condition;
    String text;
    int64 atMillis = 0;

    Anchor anchor = Anchor::nowhere;
    double fromBeat = 0.0, toBeat = 0.0;   // where it points, in song beats
    String clipID;                         // for a note that follows a clip
    bool done = false;                     // for a todo

    /** For a guess: what it was read from, so a later turn can be told the music has
        moved since. Zero for a condition, which is about intent rather than a state. */
    int aboutRevision = 0;

    /** For a guess that a person has since accepted, the guess it came from. Kept so
        the history of a decision is visible: this was suggested, and then agreed. */
    String promotedFrom;

    static String kindName (Kind k)
    {
        switch (k)
        {
            case Kind::guess:   return "guess";
            case Kind::request: return "request";
            case Kind::todo:    return "todo";
            default:            return "condition";
        }
    }

    static String anchorName (Anchor a)
    {
        switch (a)
        {
            case Anchor::time: return "time";
            case Anchor::clip: return "clip";
            default:           return "nowhere";
        }
    }

    static Anchor anchorNamed (const String& name)
    {
        if (name == "time") return Anchor::time;
        if (name == "clip") return Anchor::clip;
        return Anchor::nowhere;
    }

    static Kind kindNamed (const String& name)
    {
        if (name == "todo")    return Kind::todo;
        if (name == "guess")   return Kind::guess;
        if (name == "request") return Kind::request;
        return Kind::condition;
    }
};

//==============================================================================
/** How far the assistant is being asked to go.

    This is a request, not a permission, and the difference is the whole point. Asking
    for a bolder idea says something about what would be welcome; it says nothing about
    what may be touched. A person who selects eight notes and asks for something
    adventurous is asking for an adventurous eight notes.

    Nothing in the app reads this to decide what is allowed. Scope comes from the
    attachment and the conditions come from what was agreed, both checked after the
    suggestion arrives and regardless of what was asked for. The strength reaches the
    model as a sentence and stops there - which is why turning it up cannot widen
    anything, rather than merely being expected not to.
*/
enum class Strength { tidy, rework, fresh };

inline String strengthName (Strength strength)
{
    switch (strength)
    {
        case Strength::rework: return "rework";
        case Strength::fresh:  return "fresh";
        default:               return "tidy";
    }
}

inline Strength strengthNamed (const String& name)
{
    if (name == "rework") return Strength::rework;
    if (name == "fresh")  return Strength::fresh;
    return Strength::tidy;
}

/** What the model is told it means. Deliberately about taste and never about reach. */
inline String describeStrength (Strength strength)
{
    switch (strength)
    {
        case Strength::rework:
            return "They want a real change, not a polish. Reharmonising, a different "
                   "shape, a different rhythm are all welcome - within what they attached "
                   "and whatever they asked you to keep.";
        case Strength::fresh:
            return "They want a new idea rather than a variation of this one. Start from "
                   "what the music is doing rather than from these exact notes - still "
                   "only within what they attached and whatever they asked you to keep.";
        default:
            return "They want this tidied rather than rewritten: small corrections, "
                   "nothing a listener would call a different part.";
    }
}

//==============================================================================
class ProjectNotes
{
public:
    ProjectNotes (File storage, String project)
        : file (std::move (storage)), projectID (std::move (project))
    {
        load();
    }

    const std::vector<Note>& all() const { return entries; }

    /** How far to go, remembered between questions because it is a working preference
        rather than part of any one request. Never consulted when deciding what a
        proposal may touch. */
    Strength strength() const { return howFar; }

    void setStrength (Strength wanted)
    {
        howFar = wanted;
        save();
    }

    /** A person's decision. The only way a condition is created. */
    String addCondition (const String& text)
    {
        return add (Note::Kind::condition, text, 0, {});
    }

    /** The assistant's reading, which is not a decision. */
    String addGuess (const String& text, int aboutRevision)
    {
        return add (Note::Kind::guess, text, aboutRevision, {});
    }

    /** Something to come back to, pinned to a stretch of the song or tied to a clip.

        A todo is not a condition: it is work that has not been done, not a rule about
        how the music must be. Keeping them apart matters for the same reason the other
        kinds are kept apart - an assistant that reads "rewrite this fill" as a rule
        will defend the fill it was asked to replace. */
    String addTodo (const String& text, Note::Anchor anchor, double fromBeat, double toBeat,
                    const String& clipID)
    {
        const auto id = add (Note::Kind::todo, text, 0, {});
        if (id.isEmpty())
            return id;

        for (auto& note : entries)
            if (note.id == id)
            {
                note.anchor = anchor;
                note.fromBeat = fromBeat;
                note.toBeat = toBeat;
                note.clipID = clipID;
            }

        save();
        return id;
    }

    /** Marks a todo done. It stays on the list: what was done is part of what happened,
        and a list that erases finished work cannot answer "did we ever fix that". */
    bool complete (const String& id)
    {
        for (auto& note : entries)
            if (note.id == id && note.kind == Note::Kind::todo)
            {
                note.done = true;
                save();
                return true;
            }
        return false;
    }

    /** Moves a clip-tracking note to where its clip now is, or marks it broken if the
        clip has gone. Called by whoever can see the arrangement; the notes cannot look
        at the music themselves and must not guess.

        Returns true if anything moved or broke, so a caller knows to write it out. */
    bool followClips (const std::function<bool (const String&, double&, double&)>& whereIsClip)
    {
        auto moved = false;

        for (auto& note : entries)
        {
            if (note.anchor != Note::Anchor::clip || note.clipID.isEmpty())
                continue;

            double from = note.fromBeat, to = note.toBeat;

            if (whereIsClip (note.clipID, from, to))
            {
                if (from != note.fromBeat || to != note.toBeat)
                {
                    note.fromBeat = from;
                    note.toBeat = to;
                    moved = true;
                }
            }
            else if (! note.clipID.startsWith ("gone:"))
            {
                // The clip is not there any more. Where it was is kept, and the link is
                // marked broken rather than quietly re-aimed at whatever is nearest -
                // a note that moves somewhere nobody put it is worse than one that
                // admits it lost its place.
                note.clipID = "gone:" + note.clipID;
                moved = true;
            }
        }

        if (moved)
            save();

        return moved;
    }

    String setRequest (const String& text)
    {
        for (int i = static_cast<int> (entries.size()); --i >= 0;)
            if (entries[static_cast<size_t> (i)].kind == Note::Kind::request)
                entries.erase (entries.begin() + i);

        return add (Note::Kind::request, text, 0, {});
    }

    /** Turns a guess into a condition, which only a person may ask for. The guess stays
        where it is: what was read and what was agreed are different facts, and keeping
        both is what makes it possible to say later "you suggested this, I said yes". */
    bool acceptGuess (const String& guessID)
    {
        for (const auto& note : entries)
            if (note.id == guessID && note.kind == Note::Kind::guess)
            {
                add (Note::Kind::condition, note.text, 0, guessID);
                return true;
            }

        return false;
    }

    bool remove (const String& id)
    {
        for (int i = static_cast<int> (entries.size()); --i >= 0;)
            if (entries[static_cast<size_t> (i)].id == id)
            {
                entries.erase (entries.begin() + i);
                save();
                return true;
            }

        return false;
    }

    /** What to tell a model, with the three kinds kept apart. A model that cannot see
        which is which will treat its own earlier guess as something it was told. */
    var forContext() const
    {
        Array<var> conditions, guesses;
        String request;

        for (const auto& note : entries)
        {
            if (note.kind == Note::Kind::condition)
                conditions.add (object ({ { "id", note.id }, { "text", note.text },
                                          { "agreed_by", "the person" } }));
            else if (note.kind == Note::Kind::guess)
                guesses.add (object ({ { "id", note.id }, { "text", note.text },
                                       { "guessed_by", "the assistant" },
                                       { "about_revision", note.aboutRevision } }));
            else
                request = note.text;
        }

        Array<var> todos;
        for (const auto& note : entries)
        {
            if (note.kind != Note::Kind::todo || note.done)
                continue;

            const auto lost = note.clipID.startsWith ("gone:");
            todos.add (object ({ { "id", note.id }, { "text", note.text },
                                 { "anchor", Note::anchorName (note.anchor) },
                                 { "from_beat", note.fromBeat },
                                 { "to_beat", note.toBeat },
                                 { "lost_its_clip", lost },
                                 { "where", lost
                                     ? "the clip this was about has been deleted; these "
                                       "beats are where it used to be"
                                     : "beats in the arrangement" } }));
        }

        return object ({ { "conditions", conditions },
                         { "guesses", guesses },
                         { "todos", todos },
                         { "request", request },
                         { "strength", strengthName (howFar) },
                         { "strength_means", describeStrength (howFar) } });
    }

    /** Everything the app reports about the notes, in one short string.

        The inspector packet is rewritten only when a key built from what it describes
        moves, and a note that slides along with its clip does not change how many notes
        there are. The shelf keeps its key beside its contents for this reason; so does
        this, and for the same one. */
    String shape() const
    {
        String key (entries.size());
        key << "/" << strengthName (howFar);

        for (const auto& note : entries)
            key << "|" << note.id << (note.done ? "x" : "-")
                << Note::anchorName (note.anchor) << note.fromBeat << "," << note.toBeat
                << note.clipID;

        return key;
    }

    var asJson() const { return snapshot(); }

private:
    String add (Note::Kind kind, const String& text, int aboutRevision, const String& from)
    {
        if (text.trim().isEmpty())
            return {};

        Note note;
        note.id = Uuid().toString();
        note.kind = kind;
        note.text = text.trim();
        note.atMillis = Time::getCurrentTime().toMilliseconds();
        note.aboutRevision = aboutRevision;
        note.promotedFrom = from;

        entries.push_back (std::move (note));
        save();
        return entries.back().id;
    }

    var snapshot() const
    {
        Array<var> out;
        for (const auto& note : entries)
            out.add (object ({ { "id", note.id },
                               { "kind", Note::kindName (note.kind) },
                               { "text", note.text },
                               { "at_ms", note.atMillis },
                               { "about_revision", note.aboutRevision },
                               { "promoted_from", note.promotedFrom },
                               { "anchor", Note::anchorName (note.anchor) },
                               { "from_beat", note.fromBeat },
                               { "to_beat", note.toBeat },
                               { "clip", note.clipID },
                               { "done", note.done } }));

        return object ({ { "schema", 1 }, { "project_id", projectID },
                         { "strength", strengthName (howFar) }, { "notes", out } });
    }

    void load()
    {
        if (! file.existsAsFile())
            return;

        const auto stored = JSON::parse (file.loadFileAsString());
        if (! stored.isObject())
            return;

        // Notes belong to a project. A file that says it belongs to a different one is
        // left alone rather than adopted - that is what a copied project folder looks
        // like, and the copy must not inherit decisions made about the original.
        if (stored["project_id"].toString() != projectID)
            return;

        howFar = strengthNamed (stored["strength"].toString());

        if (auto* saved = stored["notes"].getArray())
            for (const auto& entry : *saved)
            {
                Note note;
                note.id = entry["id"].toString();
                note.kind = Note::kindNamed (entry["kind"].toString());
                note.text = entry["text"].toString();
                note.atMillis = static_cast<int64> (entry["at_ms"]);
                note.aboutRevision = static_cast<int> (entry["about_revision"]);
                note.anchor = Note::anchorNamed (entry["anchor"].toString());
                note.fromBeat = static_cast<double> (entry["from_beat"]);
                note.toBeat = static_cast<double> (entry["to_beat"]);
                note.clipID = entry["clip"].toString();
                note.done = static_cast<bool> (entry["done"]);
                note.promotedFrom = entry["promoted_from"].toString();

                // A request is about the moment it was made. Restoring one from a
                // previous run would hand the assistant a question nobody is asking.
                if (note.kind != Note::Kind::request)
                    entries.push_back (std::move (note));
            }
    }

    void save() const
    {
        file.getParentDirectory().createDirectory();
        atomicWrite (file, JSON::toString (snapshot(), false));
    }

    File file;
    String projectID;
    Strength howFar = Strength::tidy;
    std::vector<Note> entries;
};

} // namespace live
