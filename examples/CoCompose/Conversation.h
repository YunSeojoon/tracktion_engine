#pragma once

#include "Attachment.h"

namespace live
{

/** The conversation about one project, and the fact that it is one conversation.

    A question is rarely the first one. "Make that less busy" only means anything if
    what came before is still there, so a new question joins the conversation the
    project already has rather than starting another. That conversation is bound to the
    project's permanent identity, not to this run of the app: closing the app, changing
    the model, closing the panel, or losing the network are none of them reasons to
    start again.

    It is kept beside the project rather than inside it, for two reasons. A conversation
    is not music - writing a message must not touch the revision or the undo history -
    and a person must be able to read, export or delete it without that meaning anything
    for the song.
*/
struct ChatMessage
{
    enum class From { person, assistant, system };

    String id;
    From from = From::person;
    String text;
    int64 atMillis = 0;

    /** What the message was about, frozen. Kept by value: an answer that arrives later
        must still describe what was actually asked, even if the music has moved on. */
    std::vector<Attachment> attachments;

    /** The music this was said against, so a later reply can be told it is out of date
        rather than silently applied to something else. */
    int revision = 0;

    /** Set while an answer is still arriving. */
    bool streaming = false;
    String requestID;

    static String fromName (From f)
    {
        switch (f)
        {
            case From::assistant: return "assistant";
            case From::system:    return "system";
            default:              return "person";
        }
    }

    static From fromName (const String& name)
    {
        if (name == "assistant") return From::assistant;
        if (name == "system")    return From::system;
        return From::person;
    }
};

//==============================================================================
class Conversation
{
public:
    /** Opens the conversation belonging to this project, creating one the first time.
        The file is named after the project so two projects in one folder - which the
        app does not do, but a person copying files might - cannot collide. */
    Conversation (File storage, String project)
        : file (std::move (storage)), projectID (std::move (project))
    {
        load();
    }

    const String& id() const           { return conversationID; }
    const String& belongsTo() const    { return projectID; }
    const std::vector<ChatMessage>& messages() const { return entries; }

    /** Starts a new conversation, keeping the old one. Only ever called because a
        person asked for it: nothing else in the app may clear a conversation. */
    void beginNew()
    {
        if (! entries.empty())
            archived.add (snapshot());

        conversationID = Uuid().toString();
        entries.clear();
        save();
    }

    int archivedCount() const { return archived.size(); }

    ChatMessage& add (ChatMessage message)
    {
        if (message.id.isEmpty())
            message.id = Uuid().toString();
        if (message.atMillis == 0)
            message.atMillis = Time::getCurrentTime().toMilliseconds();

        entries.push_back (std::move (message));
        save();
        return entries.back();
    }

    /** Adds to the answer that is still arriving. Saved as it goes, so an app that is
        closed mid-answer still has what was received. */
    void appendToStreaming (const String& requestID, const String& more)
    {
        for (auto& message : entries)
            if (message.streaming && message.requestID == requestID)
            {
                message.text += more;
                save();
                return;
            }
    }

    void finishStreaming (const String& requestID, const String& finalText = {})
    {
        for (auto& message : entries)
            if (message.streaming && message.requestID == requestID)
            {
                if (finalText.isNotEmpty())
                    message.text = finalText;
                message.streaming = false;
                save();
                return;
            }
    }

    bool isWaiting() const
    {
        for (const auto& message : entries)
            if (message.streaming)
                return true;
        return false;
    }

    String waitingOn() const
    {
        for (const auto& message : entries)
            if (message.streaming)
                return message.requestID;
        return {};
    }

    /** What came before, for a provider that has no memory of its own. Trimmed from the
        end, so the newest exchanges survive: they are the ones a follow-up refers to.
        The whole conversation stays on disk whatever this returns. */
    var recentForContext (int maxMessages = 12) const
    {
        Array<var> out;
        const auto first = std::max (0, static_cast<int> (entries.size()) - maxMessages);

        for (int i = first; i < static_cast<int> (entries.size()); ++i)
        {
            const auto& message = entries[static_cast<size_t> (i)];
            out.add (object ({ { "from", ChatMessage::fromName (message.from) },
                               { "text", message.text },
                               { "attachments", static_cast<int> (message.attachments.size()) } }));
        }

        return object ({ { "messages", out },
                         { "trimmed", first > 0 },
                         { "total", static_cast<int> (entries.size()) } });
    }

    var asJson() const { return snapshot(); }

    void deleteEverything()
    {
        entries.clear();
        archived.clear();
        conversationID = Uuid().toString();
        file.deleteFile();
    }

private:
    var snapshot() const
    {
        Array<var> out;
        for (const auto& message : entries)
        {
            Array<var> attached;
            for (const auto& a : message.attachments)
                attached.add (object ({ { "id", a.id },
                                        { "kind", Attachment::kindName (a.kind) },
                                        { "taken_at_revision", a.takenAtRevision },
                                        { "start_beat", a.startBeat },
                                        { "end_beat", a.endBeat },
                                        { "pattern", a.patternID },
                                        { "channel", a.noteChannelID },
                                        { "insert", a.insertID },
                                        { "notes", static_cast<int> (a.noteIDs.size()) } }));

            out.add (object ({ { "id", message.id },
                               { "from", ChatMessage::fromName (message.from) },
                               { "text", message.text },
                               { "at_ms", message.atMillis },
                               { "revision", message.revision },
                               { "streaming", message.streaming },
                               { "request_id", message.requestID },
                               { "attachments", attached } }));
        }

        return object ({ { "schema", 1 },
                         { "project_id", projectID },
                         { "conversation_id", conversationID },
                         { "messages", out },
                         { "archived", archived } });
    }

    void load()
    {
        conversationID = Uuid().toString();

        if (! file.existsAsFile())
            return;

        const auto stored = JSON::parse (file.loadFileAsString());
        if (! stored.isObject())
            return;

        // A conversation belongs to a project. A file that says it belongs to a
        // different one is left alone rather than adopted: that is what a copied
        // project folder looks like, and the copy must not inherit the original's
        // conversation.
        if (stored["project_id"].toString() != projectID)
            return;

        conversationID = stored["conversation_id"].toString();
        if (conversationID.isEmpty())
            conversationID = Uuid().toString();

        if (auto* older = stored["archived"].getArray())
            archived = *older;

        if (auto* stored_messages = stored["messages"].getArray())
            for (const auto& entry : *stored_messages)
            {
                ChatMessage message;
                message.id = entry["id"].toString();
                message.from = ChatMessage::fromName (entry["from"].toString());
                message.text = entry["text"].toString();
                message.atMillis = static_cast<int64> (entry["at_ms"]);
                message.revision = static_cast<int> (entry["revision"]);
                message.requestID = entry["request_id"].toString();
                // An answer that was still arriving when the app closed is not still
                // arriving now. It is kept, marked finished, rather than left pending
                // for something that will never write to it again.
                message.streaming = false;

                if (auto* attached = entry["attachments"].getArray())
                    for (const auto& item : *attached)
                    {
                        Attachment a;
                        a.id = item["id"].toString();
                        a.kind = item["kind"].toString() == "notes"  ? Attachment::Kind::notes
                               : item["kind"].toString() == "insert" ? Attachment::Kind::insert
                                                                     : Attachment::Kind::region;
                        a.takenAtRevision = static_cast<int> (item["taken_at_revision"]);
                        a.startBeat = static_cast<double> (item["start_beat"]);
                        a.endBeat = static_cast<double> (item["end_beat"]);
                        a.patternID = item["pattern"].toString();
                        a.noteChannelID = item["channel"].toString();
                        a.insertID = item["insert"].toString();
                        message.attachments.push_back (std::move (a));
                    }

                entries.push_back (std::move (message));
            }
    }

    void save() const
    {
        file.getParentDirectory().createDirectory();
        atomicWrite (file, JSON::toString (snapshot(), false));
    }

    File file;
    String projectID, conversationID;
    std::vector<ChatMessage> entries;
    Array<var> archived;
};

} // namespace live
