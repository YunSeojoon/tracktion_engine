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
    enum class Kind { condition, guess, request };

    String id;
    Kind kind = Kind::condition;
    String text;
    int64 atMillis = 0;

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
            default:            return "condition";
        }
    }

    static Kind kindNamed (const String& name)
    {
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

        return object ({ { "conditions", conditions },
                         { "guesses", guesses },
                         { "request", request },
                         { "strength", strengthName (howFar) },
                         { "strength_means", describeStrength (howFar) } });
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
                               { "promoted_from", note.promotedFrom } }));

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
