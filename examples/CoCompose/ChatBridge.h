#pragma once

#include "Conversation.h"
#include "Tools.h"

namespace live
{

/** How the app talks to something that can answer.

    The app does not speak to a provider itself. It writes down what it wants to ask,
    and a separate program - the bridge - picks that up, talks to whatever it talks to,
    and writes the answer back. Three reasons, in order of how much they matter:

    A key never enters this process. It is not in the project, not in the conversation,
    not in a log, and not in a crash dump, because it was never here. The bridge holds
    it, or holds nothing and uses a local model, and that is the bridge's business.

    Which provider is used stops being the app's concern. A different bridge is a
    different connection, and nothing in here changes.

    And the app keeps working. Talking to a model is slow and can fail; none of that
    happens on the message thread, because none of it happens in this process at all.

    The shape is the one already used for control.json: one file in, one file out,
    request ids to tie them together, and a half-written file ignored until it is whole.
*/
class ChatBridge
{
public:
    explicit ChatBridge (File projectFile) : folder (projectFile.getParentDirectory()) {}

    struct Outgoing
    {
        String requestID;
        String projectID, conversationID;
        String message;
        var attachments;      // what was attached, already read out of the music
        var history;          // what came before, for a provider with no memory
        int revision = 0;
    };

    /** Puts a question where the bridge will find it. */
    void ask (const Outgoing& outgoing)
    {
        pending = outgoing.requestID;
        lastSeenReply.clear();
        streamedSoFar.clear();

        requestFile().deleteFile();
        replyFile().deleteFile();

        atomicWrite (requestFile(), JSON::toString (object ({
            { "schema", 1 },
            { "request_id", outgoing.requestID },
            { "project_id", outgoing.projectID },
            { "conversation_id", outgoing.conversationID },
            { "revision", outgoing.revision },
            { "message", outgoing.message },
            { "attachments", outgoing.attachments },
            { "history", outgoing.history },
            { "asked_at_ms", Time::getCurrentTime().toMilliseconds() } }), false));
    }

    /** Asks the bridge to stop. Whether it can is the bridge's business; the app stops
        listening either way, and a late answer to a cancelled request is ignored. */
    void cancel()
    {
        if (pending.isEmpty())
            return;

        atomicWrite (folder.getChildFile ("chat-cancel.json"),
                     JSON::toString (object ({ { "request_id", pending } }), false));
        cancelled.add (pending);
        pending.clear();
    }

    bool isWaiting() const { return pending.isNotEmpty(); }
    const String& waitingOn() const { return pending; }

    struct Update
    {
        enum class What { nothing, streaming, finished, failed };
        What what = What::nothing;
        String requestID;
        String text;        // the new part while streaming, the whole thing when finished
        String errorCode;   // only when failed
        bool retryable = false;
    };

    /** Called on the message thread, often. Reads whatever the bridge has written and
        says what changed - nothing, more text, done, or gone wrong. */
    Update poll()
    {
        if (pending.isEmpty())
            return {};

        const auto contents = replyFile().loadFileAsString();
        if (contents.isEmpty() || contents == lastSeenReply)
            return {};

        auto reply = JSON::parse (contents);
        if (! reply.isObject())
            return {};   // half written; it will be whole in a moment

        const auto requestID = reply["request_id"].toString();
        if (requestID != pending)
            return {};   // an answer to something else, or to something cancelled

        lastSeenReply = contents;

        const auto status = reply["status"].toString();

        if (status == "error")
        {
            const auto failed = pending;
            pending.clear();
            return { Update::What::failed, failed, reply["message"].toString(),
                     reply.getProperty ("code", "IO_ERROR").toString(),
                     static_cast<bool> (reply.getProperty ("retryable", false)) };
        }

        const auto whole = reply["text"].toString();

        if (status == "streaming")
        {
            // The bridge writes the whole answer so far, so what is new is whatever was
            // not there last time. That way a missed poll costs nothing.
            const auto addition = whole.startsWith (streamedSoFar)
                                    ? whole.substring (streamedSoFar.length()) : whole;
            streamedSoFar = whole;
            return { Update::What::streaming, requestID, addition };
        }

        if (status == "ok")
        {
            const auto done = pending;
            pending.clear();
            return { Update::What::finished, done, whole };
        }

        return {};
    }

    /** Whether anything is listening. A bridge writes this down when it starts, so the
        app can say "nothing is connected" instead of waiting forever. */
    var connection() const
    {
        auto stated = JSON::parse (folder.getChildFile ("chat-bridge.json").loadFileAsString());
        return stated.isObject() ? stated : var();
    }

    bool isConnected() const
    {
        auto stated = connection();
        return stated.isObject() && static_cast<bool> (stated["ready"]);
    }

private:
    File requestFile() const { return folder.getChildFile ("chat-request.json"); }
    File replyFile() const   { return folder.getChildFile ("chat-reply.json"); }

    File folder;
    String pending, lastSeenReply, streamedSoFar;
    StringArray cancelled;
};

} // namespace live
