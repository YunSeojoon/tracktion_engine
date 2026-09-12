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

        /** What is known about the project, with the three kinds kept apart: what the
            person decided, what the assistant guessed, and what is being asked now. */
        var notes;

        /** The alternatives already offered, so a follow-up about "the last one" points
            at a candidate rather than at whatever the model infers from the transcript. */
        var candidates;
        int revision = 0;
    };

    /** Puts a question where the bridge will find it. */
    void ask (const Outgoing& outgoing)
    {
        pending = outgoing.requestID;
        pendingProject = outgoing.projectID;
        askedAtMillis = Time::getCurrentTime().toMilliseconds();
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
            { "notes", outgoing.notes },
            { "candidates", outgoing.candidates },
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

        /** A change the answer suggests, if it suggested one. It has been checked by
            nobody at this point - it is exactly what the bridge sent - and the app puts
            it through the same checks a script's would face before keeping it. */
        var change;

        /** Who produced this answer. A bridge can be stopped and another started with a
            different model half way through a conversation, and an answer already in
            flight belongs to the one that was asked. Without this the older answer
            arrives and is read as the new model's, which is the kind of wrong that
            cannot be spotted afterwards. */
        String provider, model;
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

        // A reply that says which project it answers has to say this one. A reply that
        // does not say is an older bridge and is taken on the request id alone, which
        // is unique per ask - so this adds a stated check on top of a lucky property,
        // and never turns an existing bridge away.
        const auto answersProject = reply["project_id"].toString();
        if (answersProject.isNotEmpty() && answersProject != pendingProject)
        {
            lastSeenReply = contents;
            return {};
        }

        lastSeenReply = contents;

        const auto status = reply["status"].toString();

        if (status == "error")
        {
            const auto failed = pending;
            pending.clear();
            return { Update::What::failed, failed, reply["message"].toString(),
                     reply.getProperty ("code", "IO_ERROR").toString(),
                     static_cast<bool> (reply.getProperty ("retryable", false)), var(),
                     reply["provider"].toString(), reply["model"].toString() };
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
            return { Update::What::finished, done, whole, {}, false, reply["change"],
                     reply["provider"].toString(), reply["model"].toString() };
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

    /** Whether something is listening now, rather than whether something once said it
        would. A bridge writes "ready" when it starts and clears it when it stops - but
        a process that is killed never gets to clear anything, and the file it leaves
        behind is a promise from a program that no longer exists. So it also writes the
        time, every couple of seconds, and a heartbeat that has stopped is a bridge that
        has stopped. A bridge too old to say how often it beats is given a default
        rather than being called dead for not knowing the question. */
    bool isConnected() const
    {
        auto stated = connection();
        if (! stated.isObject() || ! static_cast<bool> (stated["ready"]))
            return false;

        const auto beat = static_cast<int64> (stated.getProperty ("heartbeat_ms", 0));
        if (beat <= 0)
            return true;

        const auto interval = std::max (1000, static_cast<int> (
                                  stated.getProperty ("heartbeat_interval_ms", 2000)));

        // Several missed beats, not one: a machine under load skips one without the
        // bridge having gone anywhere, and calling it dead for that would be worse than
        // waiting another second.
        return Time::getCurrentTime().toMilliseconds() - beat < interval * 5;
    }

    /** How long a question has been outstanding, in milliseconds; zero when none is. */
    int64 waitingFor() const
    {
        return pending.isEmpty() ? 0
                                 : Time::getCurrentTime().toMilliseconds() - askedAtMillis;
    }

    /** Gives up on the question in flight, without telling the bridge anything: used
        when there is no bridge left to tell. Returns what was being waited on. */
    String abandon()
    {
        const auto lost = pending;
        pending.clear();
        return lost;
    }

private:
    File requestFile() const { return folder.getChildFile ("chat-request.json"); }
    File replyFile() const   { return folder.getChildFile ("chat-reply.json"); }

    File folder;
    int64 askedAtMillis = 0;
    String pending, pendingProject, lastSeenReply, streamedSoFar;
    StringArray cancelled;
};

} // namespace live
