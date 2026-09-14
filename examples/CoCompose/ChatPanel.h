#pragma once

#include "Attachment.h"
#include "Conversation.h"

namespace live
{

/** Where a person talks about the song with something that can read it.

    At this stage the panel does no talking. It holds what has been attached, shows
    what each attachment actually refers to, and lets a person go to it, drop it, or
    point it at wherever they are looking now. Sending comes later; being sure about
    what would be sent comes first.

    Nothing here touches the music. Attaching, removing, scrolling and typing are all
    layout and conversation, so none of it advances the project's revision or lands in
    the undo history - a person must be able to ask a question in the middle of an edit
    without that question becoming part of the edit.
*/
class ChatPanel final : public Component
{
public:
    ChatPanel (Model& m, Selection& s, std::function<void (const Attachment&)> goTo)
        : model (m), selection (s), attachments (m), reveal (std::move (goTo))
    {
        cards.setInterceptsMouseClicks (false, true);
        viewport.setViewedComponent (&cards, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        entry.setMultiLine (true, true);
        entry.setReturnKeyStartsNewLine (true);
        entry.setTextToShowWhenEmpty ("Attach something and ask about it", theme::textFaint);

        // Escape steps out of the box. It does not throw away what was typed: somebody
        // pressing Escape wants the keyboard back for the song, and losing a paragraph
        // to get it is a worse trade than the one they were offering. Space and Delete
        // are already held away from the transport while this has focus, and this is
        // how a person gives the focus up on purpose.
        entry.onEscapeKey = [this]
        {
            entry.giveAwayKeyboardFocus();
            if (onLeftTheBox)
                onLeftTheBox();
        };
        addAndMakeVisible (entry);

        inspect.setButtonText ("What gets sent");
        inspect.onClick = [this] { showInspector(); };
        clear.setButtonText ("Clear");
        clear.onClick = [this] { attached.clear(); rebuild(); };
        addAndMakeVisible (inspect);
        addAndMakeVisible (clear);

        transcript.setMultiLine (true, true);
        transcript.setReadOnly (true);
        transcript.setScrollbarsShown (true);
        transcript.setCaretVisible (false);
        transcript.setColour (TextEditor::backgroundColourId, theme::sunken);
        transcript.setColour (TextEditor::outlineColourId, theme::edge);
        addAndMakeVisible (transcript);

        change.setMultiLine (true, true);
        change.setReadOnly (true);
        change.setCaretVisible (false);
        change.setColour (TextEditor::backgroundColourId, theme::panelHeader);
        change.setColour (TextEditor::outlineColourId, theme::accentDim);
        addAndMakeVisible (change);

        apply.setButtonText ("Apply change");
        // chosenProposal(), not offered. Preview already used the picked one while
        // Apply used whatever the conversation offered last, so hearing the first of
        // two suggestions and pressing Apply took the second - and when both were
        // worked out at the same revision nothing refused it.
        apply.onClick = [this] { if (onApply && chosenProposal().isNotEmpty())
                                     onApply (chosenProposal()); };
        apply.setVisible (false);
        addAndMakeVisible (apply);

        // Listening to a suggestion before taking it was already built and had no way
        // in: the only caller was the diagnostic script, so the acceptance sheet's
        // "press Preview" was not a step a person had skipped, it was a step with no
        // button. This is that button.
        preview.setButtonText ("Preview A/B");
        preview.onClick = [this]
        {
            if (onPreview && chosenProposal().isNotEmpty())
                onPreview (chosenProposal());
        };
        preview.setVisible (false);
        addAndMakeVisible (preview);

        // Hearing the two halves. They appear once there is something to hear and not
        // before: a button that is there but does nothing teaches a person to distrust
        // the ones that do.
        playA.setButtonText ("Play A");
        playB.setButtonText ("Play B");
        playA.onClick = [this] { if (onListen) onListen ("before"); };
        playB.onClick = [this] { if (onListen) onListen ("after"); };
        playA.setVisible (false);
        playB.setVisible (false);
        addAndMakeVisible (playA);
        addAndMakeVisible (playB);

        previewState.setFont (theme::small_());
        previewState.setColour (Label::textColourId, theme::textFaint);
        previewState.setVisible (false);
        addAndMakeVisible (previewState);

        // What the person has decided, what the assistant has guessed, and what is
        // still to do. All three went to the model and none of them was on screen, so
        // the one participant who could not see what had been agreed was the one who
        // agreed to it.
        decisions.setMultiLine (true, true);
        decisions.setReadOnly (true);
        decisions.setCaretVisible (false);
        decisions.setColour (TextEditor::backgroundColourId, theme::sunken);
        decisions.setColour (TextEditor::outlineColourId, theme::edge);
        decisions.setVisible (false);
        addAndMakeVisible (decisions);

        addCondition.setButtonText ("Decide...");
        addCondition.onClick = [this] { askForACondition(); };
        addAndMakeVisible (addCondition);

        // How far a suggestion may go. It is a preference about taste, not about reach,
        // and the label says so because the two are easy to confuse and only one of them
        // is safe to widen.
        strength.addItem ("Nudge it", 1);
        strength.addItem ("Rework it", 2);
        strength.addItem ("Something new", 3);
        strength.setTooltip ("How far a suggestion may go. It does not change what a "
                             "suggestion is allowed to touch - that is what you attach.");
        strength.onChange = [this]
        {
            if (onStrength)
                onStrength (strength.getSelectedId() == 3 ? "fresh"
                          : strength.getSelectedId() == 2 ? "rework"
                                                          : "tidy");
        };
        addAndMakeVisible (strength);

        // The alternatives already offered. Asking three times and going back to the
        // second was a thing the app could do and a person could not reach: the shelf
        // was written to a file and read by nothing on screen.
        candidates.setTextWhenNoChoicesAvailable ("no earlier suggestions");
        candidates.setTextWhenNothingSelected ("earlier suggestions");
        // Choosing from the list chooses; it does not start a render. Picking an
        // earlier suggestion in order to hear it is two thoughts - which one, and then
        // listen - and an app that renders on the first of them has decided the second
        // for you. It also made "pick one and throw it away" start by rendering the
        // thing you were about to discard.
        // It does have to redraw what the panel says, though: the description, the
        // Preview and the Apply are all about whichever one is picked, and leaving the
        // words describing the previous choice is how a person ends up applying
        // something they did not read.
        candidates.onChange = [this] { if (onPicked) onPicked(); resized(); };
        candidates.setVisible (false);
        addAndMakeVisible (candidates);

        // Throwing one away. It removes a record of an offer and no music at all, which
        // is why it can sit next to the list without being frightening - but the button
        // says "Forget" rather than "Delete" because the two would read the same and
        // only one of them is true.
        forget.setButtonText ("Forget");
        forget.setTooltip ("Takes this suggestion off the list. It changes no music: if "
                           "you already applied it, the notes stay and Ctrl+Z is still "
                           "how you take them back.");
        forget.onClick = [this]
        {
            // Nothing picked means the most recent one, which is what "forget that" means
            // when somebody says it out loud. Doing nothing silently is the answer a
            // person cannot tell from a broken button.
            const auto which = chosenProposal();

            if (which.isNotEmpty() && onForget)
                onForget (which);
        };
        forget.setVisible (false);
        addAndMakeVisible (forget);

        send.setButtonText ("Ask");
        send.onClick = [this] { if (onSend) onSend(); };
        stop.setButtonText ("Stop");
        stop.onClick = [this] { if (onCancel) onCancel(); };
        stop.setEnabled (false);
        addAndMakeVisible (send);
        addAndMakeVisible (stop);

        note.setJustificationType (Justification::topLeft);
        note.setFont (theme::small_());
        note.setColour (Label::textColourId, theme::textFaint);
        addAndMakeVisible (note);

        connection.setFont (theme::small_());
        connection.setColour (Label::textColourId, theme::textFaint);
        addAndMakeVisible (connection);

        rebuild();
    }

    /** Takes what a panel has selected and freezes it. From here on the attachment
        names the same music however the selection moves. */
    void attach (Attachment a)
    {
        attached.push_back (std::move (a));
        rebuild();
    }

    const std::vector<Attachment>& current() const { return attached; }

    /** Re-takes one attachment from wherever the person is looking now. This is the
        only way an attachment changes what it points at, and it is a button they press
        - never something that happens quietly underneath them. */
    void refreshFromSelection (int index, const Attachment& replacement)
    {
        if (isPositiveAndBelow (index, static_cast<int> (attached.size())))
        {
            const auto keptID = attached[static_cast<size_t> (index)].id;
            attached[static_cast<size_t> (index)] = replacement;
            attached[static_cast<size_t> (index)].id = keptID;
            rebuild();
        }
    }

    void remove (int index)
    {
        if (isPositiveAndBelow (index, static_cast<int> (attached.size())))
        {
            attached.erase (attached.begin() + index);
            rebuild();
        }
    }

    /** Puts the keyboard in the question box, which is what clicking it does. Worth
        reaching from a script because "is the person typing" is the thing transport
        keys have to ask before they fire. */
    bool focusEntry()
    {
        // Ask the window manager for the keyboard first: JUCE only routes keys to a
        // component whose window the OS considers active, so grabbing focus inside an
        // inactive window quietly does nothing.
        if (auto* top = getTopLevelComponent())
        {
            top->toFront (true);
            if (auto* peer = top->getPeer())
                peer->grabFocus();
        }

        entry.grabKeyboardFocus();
        return true;
    }

    bool entryHasFocus() const { return entry.hasKeyboardFocus (true); }

    /** Escape, delivered to the box rather than to the handler behind it.

        It does not ask whether the box holds the keyboard first. This machine will not
        give the app window the keyboard from a script at all - that is measured, not
        assumed - so requiring it would mean the key could never be delivered here and
        the handler could never be driven. Whether the box has the focus in real use is
        a person's half; whether Escape does the right thing when it arrives is this
        one. */
    bool pressEscapeInTheBox()
    {
        return entry.keyPressed (KeyPress (KeyPress::escapeKey));
    }

    String draft() const { return entry.getText(); }
    void setDraft (const String& text) { entry.setText (text, dontSendNotification); }

    /** Everything a person types stays theirs. A failed send, a cancel, a reconnect -
        none of them may empty this box, because retyping a question is the one thing a
        person will not forgive. It is cleared once, when the question has actually been
        asked. */
    void clearDraft() { entry.clear(); }

    void takeAttachments (std::vector<Attachment>& into)
    {
        into = attached;
        attached.clear();
        rebuild();
    }

    std::function<void()> onSend, onCancel;
    std::function<void (const String&)> onApply, onPreview, onPickCandidate, onListen;
    std::function<void (const String&)> onStrength, onDecide, onAcceptGuess, onForget;
    /** Called when the choice of suggestion changes, so the panel can be redrawn about
        the new one. */
    std::function<void()> onPicked;
    /** Called when Escape gives the keyboard back, so the app can put it somewhere
        useful rather than nowhere. */
    std::function<void()> onLeftTheBox;

    /** The proposal currently being offered, if any. Empty once it has been applied, so
        the same change cannot be applied twice by pressing the button again. */
    String offeredProposal() const { return offered; }

    /** Which suggestion the buttons act on: the one picked from the list, or the one
        just offered when nothing is picked. */
    String chosenProposal() const
    {
        const auto picked = candidates.getSelectedId() - 1;

        if (isPositiveAndBelow (picked, candidateIDs.size()))
            return candidateIDs[picked];

        return offered;
    }

    /** What is known about this project, on screen, in the three kinds it is kept in.

        A condition is a rule the person set. A guess is the assistant's reading and is
        marked as one, because a guess that reads like a rule is how an assistant ends
        up defending a key nobody chose. A todo is work not yet done. They are laid out
        in that order and each says which it is, since the whole reason they are stored
        apart is that collapsing them loses the difference. */
    void setProjectNotes (const var& kept, const String& howFar)
    {
        auto* entries = kept["notes"].getArray();
        const auto shape = JSON::toString (kept, true) + howFar;

        if (shape == notesShape)
            return;

        notesShape = shape;

        String text;
        auto conditions = 0, guesses = 0, todos = 0;

        if (entries != nullptr)
            for (const auto& one : *entries)
            {
                const auto kind = one["kind"].toString();

                if (kind == "condition")
                {
                    text << "DECIDED  " << one["text"].toString() << newLine;
                    ++conditions;
                }
                else if (kind == "guess")
                {
                    text << "guessed  " << one["text"].toString()
                         << "   (read at revision " << one["about_revision"].toString() << ")" << newLine;
                    ++guesses;
                }
                else if (kind == "todo")
                {
                    text << (static_cast<bool> (one["done"]) ? "done     " : "to do    ")
                         << one["text"].toString();

                    const auto anchor = one["anchor"].toString();
                    if (anchor == "time")
                        text << "   (beats " << one["from_beat"].toString()
                             << " to " << one["to_beat"].toString() << ")";
                    else if (anchor == "clip")
                        text << (one["clip"].toString().startsWith ("gone:")
                                   ? "   (the clip this was about has gone)"
                                   : "   (follows a clip)");

                    text << newLine;
                    ++todos;
                }
            }

        if (text.isEmpty())
            text = "Nothing decided yet. Decide... writes down something a suggestion "
                   "must respect.";

        decisions.setText (text, dontSendNotification);
        decisions.setVisible (conditions + guesses + todos > 0 || addCondition.isVisible());

        const auto wanted = howFar == "fresh" ? 3 : howFar == "rework" ? 2 : 1;
        if (strength.getSelectedId() != wanted)
            strength.setSelectedId (wanted, dontSendNotification);

        resized();
    }

    String notesOnScreen() const { return decisions.getText(); }

    /** Picks one from the list, the way a person does - selection only, no render.
        Returns false when that suggestion is not on the list to be picked. */
    bool pickCandidate (const String& proposalID)
    {
        for (int i = 0; i < candidateIDs.size(); ++i)
            if (candidateIDs[i] == proposalID)
            {
                candidates.setSelectedId (i + 1, sendNotificationSync);
                return true;
            }

        return false;
    }

    /** Whether there are two halves to hear, and which one is playing.

        "Playing A" on the button itself rather than only in a line of text, because a
        person comparing two nearly identical files is looking at the thing they are
        about to press, and an A/B where you cannot tell which is which is two sounds
        and a guess. */
    void setSomethingToHear (bool ready, const String& whichIsPlaying)
    {
        const auto shape = (ready ? "1" : "0") + whichIsPlaying;
        if (shape == listenShape)
            return;

        listenShape = shape;
        playA.setVisible (ready);
        playB.setVisible (ready);
        playA.setButtonText (whichIsPlaying == "before" ? "Playing A" : "Play A");
        playB.setButtonText (whichIsPlaying == "after" ? "Playing B" : "Play B");
        resized();
    }

    String listeningShape() const { return listenShape; }

    /** The alternatives offered so far, newest last, as a person would count them.

        A candidate worked out against music that has since moved cannot simply be
        applied, and the list says so beside it rather than letting somebody pick one
        and meet a refusal. "Stale" is not a warning about something that might go
        wrong; it is a fact about which music the suggestion read. */
    void setCandidates (const var& shelf, int currentRevision)
    {
        Array<var> entries;
        if (auto* held = shelf["candidates"].getArray())
            entries = *held;

        StringArray wanted;
        for (const auto& one : entries)
            wanted.add (one["id"].toString() + "|" + String (static_cast<int> (one["base_revision"]))
                          + (static_cast<bool> (one["adopted"]) ? "+" : "-"));

        // Rebuilt only when it would look different, because a combo box that is
        // repopulated on a timer cannot be opened: the list closes under the pointer.
        const auto shape = wanted.joinIntoString (",") + "@" + String (currentRevision);
        if (shape == candidateShape)
            return;

        candidateShape = shape;
        candidateIDs.clear();
        candidates.clear (dontSendNotification);

        auto number = 1;
        for (const auto& one : entries)
        {
            const auto revision = static_cast<int> (one["base_revision"]);
            auto label = String (number) + ". " + one["description"].toString();

            if (label.trim().endsWith ("."))
                label += "(no description)";

            if (static_cast<bool> (one["adopted"]))
                label += "  - taken";
            else if (revision != currentRevision)
                label += "  - worked out at revision " + String (revision)
                           + ", the music is at " + String (currentRevision);

            candidateIDs.add (one["id"].toString());
            candidates.addItem (label, number++);
        }

        candidates.setVisible (! candidateIDs.isEmpty());
        forget.setVisible (! candidateIDs.isEmpty());
        resized();
    }

    /** What the app knows about the comparison being rendered, in one line a person can
        read. Empty hides it, because a line that says nothing is a line in the way.

        The wording says what is happening rather than naming a stage: "rendering the
        second half" is something to wait for, "renderingAfter" is a thing to decode. */
    void setPreviewState (const String& what)
    {
        if (previewState.getText() == what)
            return;

        previewState.setText (what, dontSendNotification);
        previewState.setVisible (what.isNotEmpty());
        resized();
    }

    /** Draws the conversation. Called whenever it changes, including while an answer is
        still arriving, so the text grows as it comes in. */
    void showConversation (const Conversation& conversation, bool waiting, const String& connectionNote)
    {
        String text;

        for (const auto& message : conversation.messages())
        {
            text << (message.from == ChatMessage::From::person ? "you" : "ai") << "  ";

            if (! message.attachments.empty())
            {
                StringArray names;
                for (const auto& a : message.attachments)
                    names.add (attachments.summary (a));
                text << "[" << names.joinIntoString ("] [") << "]  ";
            }

            text << newLine << message.text << newLine;
            if (message.streaming)
                text << "..." << newLine;
            text << newLine;
        }

        if (text != transcript.getText())
        {
            transcript.setText (text, dontSendNotification);
            transcript.moveCaretToEnd();
        }

        send.setEnabled (! waiting);
        stop.setEnabled (waiting);
        connection.setText (connectionNote, dontSendNotification);

        // A change worked out but not made. Shown as what each note was and would
        // become, so a person decides from the numbers rather than from a promise.
        offered.clear();
        String changeText;

        // Which one the panel is talking about. Picked from the list if anything is
        // picked, otherwise the last one offered - and the description, the Preview and
        // the Apply all have to be about that same one or the words on screen belong to
        // a different change from the button underneath them.
        const auto picked = chosenProposal();
        auto describedThePicked = false;

        for (const auto& message : conversation.messages())
        {
            if (message.proposalProblem.isNotEmpty())
                changeText = "A change was suggested but could not be used: " + message.proposalProblem;

            if (message.proposalID.isEmpty() || ! message.proposalSummary.isObject())
                continue;

            const auto done = static_cast<bool> (message.proposalSummary["applied"]);

            if (! done)
                offered = message.proposalID;

            // Once the picked one has been described, later messages do not overwrite
            // it. When nothing is picked the last one wins, as it always did.
            if (describedThePicked)
                continue;

            if (picked.isNotEmpty() && message.proposalID != picked)
                continue;

            describedThePicked = picked.isNotEmpty();
            changeText = message.proposalSummary["description"].toString();
            if (changeText.isEmpty())
                changeText = "A suggested change";

            changeText << "  (" << message.proposalSummary["notes_changed"].toString() << " changed, "
                       << message.proposalSummary["notes_added"].toString() << " added, "
                       << message.proposalSummary["notes_removed"].toString() << " removed)";

            // A note change edits the pattern, and a pattern can be played in more than
            // one place. Saying so before Apply is the difference between a change the
            // person meant and a change they hear three times.
            const auto placements = static_cast<int> (message.proposalSummary["placements"]);
            if (placements > 1 && static_cast<int> (message.proposalSummary["notes_changed"])
                                   + static_cast<int> (message.proposalSummary["notes_added"])
                                   + static_cast<int> (message.proposalSummary["notes_removed"]) > 0)
                changeText << newLine << "   this pattern is played in " << placements
                           << " places, and all of them change";

            if (auto* notes = message.proposalDiff["notes"].getArray())
                for (int i = 0; i < jmin (6, notes->size()); ++i)
                {
                    const auto& edited = (*notes)[i];
                    changeText << newLine << "   " << edited["what"].toString();
                    for (const auto* field : { "pitch", "start_beat", "length_beats", "velocity" })
                        if (edited[field].isObject())
                            changeText << "  " << field << " " << edited[field]["was"].toString()
                                       << " -> " << edited[field]["now"].toString();
                }

            if (done)
                changeText << newLine << "   applied";
        }

        change.setText (changeText, dontSendNotification);

        // Everything below asks the same question: what is this panel about right now.
        const auto acting = chosenProposal();
        apply.setVisible (acting.isNotEmpty());
        apply.setEnabled (acting.isNotEmpty() && ! waiting);

        // Listening and taking are offered together: a person who can press Apply can
        // hear what it would do first, and one without the other is the choice this
        // panel used to make for them.
        preview.setVisible (acting.isNotEmpty() || ! candidateIDs.isEmpty());
        preview.setEnabled (acting.isNotEmpty() && ! waiting);

        if (offered.isEmpty())
            setPreviewState ({});
    }

    /** What an attachment would carry, in the form a person can read before it goes
        anywhere. The same description is what the context packet is built from. */
    String describe (const Attachment& a) const
    {
        String text;
        text << Attachment::kindName (a.kind) << "  " << attachments.summary (a) << newLine;
        text << "  attachment " << a.id.substring (0, 8) << ", taken at revision "
             << a.takenAtRevision << newLine;

        switch (a.kind)
        {
            case Attachment::Kind::notes:
                text << "  pattern " << a.patternID << newLine
                     << "  channel " << a.noteChannelID << newLine
                     << "  notes   " << a.noteIDs.size() << " (" << attachments.survivingNotes (a)
                     << " still there)" << newLine
                     << "  beats   " << String (a.startBeat, 3) << " to " << String (a.endBeat, 3) << newLine
                     << "  this pattern is placed " << a.patternUseCount
                     << (a.patternUseCount == 1 ? " time" : " times") << newLine;
                break;

            case Attachment::Kind::insert:
                text << "  insert  " << a.insertID << newLine;
                break;

            case Attachment::Kind::region:
            default:
                text << "  beats   " << String (a.startBeat, 3) << " to " << String (a.endBeat, 3)
                     << "  (" << attachments.barRange (a.startBeat, a.endBeat) << ")" << newLine
                     << "  lanes   " << (a.laneIDs.isEmpty() ? String ("all") : a.laneIDs.joinIntoString (", ")) << newLine
                     << "  clips   " << a.clipIDs.size() << newLine
                     << "  channels " << a.channelIDs.joinIntoString (", ") << newLine;
                break;
        }

        if (! attachments.stillExists (a))
            text << "  what this pointed at is no longer in the song" << newLine;

        return text;
    }

    /** The same thing the inspector dialog shows, as data. Written beside the project
        so a person - or a check - can read exactly what an attachment refers to without
        opening a window, and so the context packet has one source rather than two. */
    var inspectorState() const
    {
        Array<var> cardsOut;

        for (const auto& a : attached)
        {
            auto card = object ({ { "id", a.id },
                                   { "kind", Attachment::kindName (a.kind) },
                                   { "summary", attachments.summary (a) },
                                   { "taken_at_revision", a.takenAtRevision },
                                   { "exists", attachments.stillExists (a) },
                                   { "start_beat", a.startBeat },
                                   { "end_beat", a.endBeat } });

            auto* fields = card.getDynamicObject();

            switch (a.kind)
            {
                case Attachment::Kind::notes:
                    fields->setProperty ("pattern", a.patternID);
                    fields->setProperty ("channel", a.noteChannelID);
                    fields->setProperty ("notes", a.noteIDs.size());
                    fields->setProperty ("notes_present", attachments.survivingNotes (a));
                    fields->setProperty ("pattern_use_count", a.patternUseCount);
                    break;

                case Attachment::Kind::insert:
                    fields->setProperty ("insert", a.insertID);
                    break;

                case Attachment::Kind::region:
                default:
                    fields->setProperty ("lanes", stringsOf (a.laneIDs));
                    fields->setProperty ("clips", stringsOf (a.clipIDs));
                    fields->setProperty ("channels", stringsOf (a.channelIDs));
                    break;
            }

            cardsOut.add (card);
        }

        // What is actually on screen and reachable with a pointer. A check can drive
        // the handlers either way; what it cannot otherwise tell is whether a person
        // has anything to press, which is exactly what was missing.
        Array<var> shelfOnScreen;
        for (int i = 0; i < candidates.getNumItems(); ++i)
            shelfOnScreen.add (candidates.getItemText (i));

        return object ({ { "attachments", cardsOut },
                         { "draft", entry.getText() },
                         { "typing", entry.hasKeyboardFocus (true) },
                         { "offered_proposal", offered },
                         // What the buttons would act on, and the words above them.
                         // These two disagreeing is exactly the fault this reports.
                         { "chosen_proposal", chosenProposal() },
                         { "change", change.getText() },
                         { "buttons", object ({ { "apply", apply.isVisible() && apply.isEnabled() },
                                                { "preview", preview.isVisible() && preview.isEnabled() },
                                                { "ask", send.isEnabled() },
                                                { "stop", stop.isEnabled() } }) },
                         { "preview_state", previewState.getText() },
                         { "decisions_on_screen", decisions.getText() },
                         { "strength_on_screen", strength.getText() },
                         { "listening", object ({ { "offered", playA.isVisible() },
                                                  { "a", playA.getButtonText() },
                                                  { "b", playB.getButtonText() } }) },
                         { "candidates_on_screen", shelfOnScreen } });
    }

    /** Everything inspectorState reports that nothing else in the key would move.

        The packet is rewritten only when a key built from what it describes changes, so
        a field missing from that key stops being reported the moment it matters. Five
        fields in this app have been caught by that; this one is in the key because of
        them, not after it. */
    /** Presses one of the panel's buttons by name, the way a pointer would: a button
        that is hidden or disabled does nothing and says so, which is the whole point of
        pressing it rather than calling what it calls. */
    bool pressButton (const String& named)
    {
        auto* which = named == "apply"   ? &apply
                    : named == "preview" ? &preview
                    : named == "ask"     ? &send
                    : named == "stop"    ? &stop
                    : named == "play_a"  ? &playA
                    : named == "play_b"  ? &playB
                    : named == "forget"  ? &forget
                                         : nullptr;

        if (which == nullptr || ! which->isVisible() || ! which->isEnabled())
            return false;

        which->triggerClick();
        return true;
    }

    String buttonShape() const
    {
        String key;
        key << (apply.isVisible() && apply.isEnabled() ? "a" : "-")
            << (preview.isVisible() && preview.isEnabled() ? "p" : "-")
            << (send.isEnabled() ? "s" : "-")
            << (stop.isEnabled() ? "x" : "-")
            << "|" << previewState.getText()
            << "|" << candidateShape
            << "|" << listenShape
            << "|" << notesShape
            // Which one the panel is about. Picking from the list changes nothing else
            // in this key, and leaving it out kept the packet describing the previous
            // choice - the same trap this file has fallen into before.
            << "|" << chosenProposal();
        return key;
    }

    /** Writing down something a suggestion has to respect. A condition is the one kind
        a person creates, which is why this is the only box here that types into. */
    void askForACondition()
    {
        auto* box = new AlertWindow ("Something to keep",
                                     "What must a suggestion respect? For example: "
                                     "keep the drums as they are.",
                                     MessageBoxIconType::NoIcon);
        box->addTextEditor ("text", {});
        box->addButton ("Keep it", 1, KeyPress (KeyPress::returnKey));
        box->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

        box->enterModalState (true, ModalCallbackFunction::create (
            [this, box] (int result)
            {
                std::unique_ptr<AlertWindow> owned (box);
                const auto typed = owned->getTextEditorContents ("text").trim();

                if (result == 1 && typed.isNotEmpty() && onDecide)
                    onDecide (typed);
            }), false);
    }

    String inspectorText() const
    {
        if (attached.empty())
            return "Nothing attached yet.";

        String text;
        for (const auto& a : attached)
            text << describe (a) << newLine;

        return text;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (120);

        auto buttons = bottom.removeFromBottom (24);
        send.setBounds (buttons.removeFromLeft (56).reduced (1));
        stop.setBounds (buttons.removeFromLeft (52).reduced (1));
        inspect.setBounds (buttons.removeFromLeft (104).reduced (1));
        clear.setBounds (buttons.removeFromLeft (56).reduced (1));

        entry.setBounds (bottom.removeFromBottom (48).reduced (0, 2));
        if (previewState.isVisible())
            previewState.setBounds (r.removeFromBottom (14));

        if (candidates.isVisible())
        {
            auto row = r.removeFromBottom (22);
            forget.setBounds (row.removeFromRight (70).reduced (1));
            candidates.setBounds (row.reduced (1));
        }

        {
            auto row = r.removeFromBottom (22);
            addCondition.setBounds (row.removeFromLeft (86).reduced (1));
            strength.setBounds (row.removeFromLeft (130).reduced (1));
        }

        if (decisions.isVisible())
            decisions.setBounds (r.removeFromBottom (jmin (72, r.getHeight() / 4)).reduced (0, 2));

        if (apply.isVisible() || playA.isVisible())
        {
            auto row = r.removeFromBottom (24);

            if (apply.isVisible())
            {
                apply.setBounds (row.removeFromLeft (120).reduced (1));
                preview.setBounds (row.removeFromLeft (110).reduced (1));
            }

            if (playA.isVisible())
            {
                playA.setBounds (row.removeFromLeft (78).reduced (1));
                playB.setBounds (row.removeFromLeft (78).reduced (1));
            }
        }
        change.setBounds (r.removeFromBottom (jmin (96, r.getHeight() / 3)).reduced (0, 2));
        connection.setBounds (bottom.removeFromBottom (14));
        note.setBounds (bottom.removeFromBottom (14));

        // The attachments take a fixed share at the top; the conversation gets the rest,
        // because that is what grows.
        const auto cardRoom = jmin (r.getHeight() / 2, 8 + 52 * jmax (1, cardViews.size()));
        viewport.setBounds (r.removeFromTop (cardRoom));
        transcript.setBounds (r.reduced (0, 2));
        layOutCards();
    }

private:
    static var stringsOf (const StringArray& from)
    {
        Array<var> out;
        for (const auto& item : from)
            out.add (item);
        return out;
    }

    struct Card final : public Component
    {
        Card (String text, bool present) : line (std::move (text)), exists (present)
        {
            go.setButtonText ("Go");
            update.setButtonText ("Update");
            drop.setButtonText ("Remove");
            for (auto* b : { &go, &update, &drop })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (*b);
            }
        }

        void paint (Graphics& g) override
        {
            const auto area = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (exists ? theme::row : theme::row.withAlpha (0.5f));
            g.fillRoundedRectangle (area, 3.0f);
            g.setColour (exists ? theme::edge : theme::danger.withAlpha (0.6f));
            g.drawRoundedRectangle (area, 3.0f, 1.0f);

            g.setColour (exists ? theme::text : theme::textFaint);
            g.setFont (theme::body());
            g.drawText (line, getLocalBounds().reduced (8, 4).removeFromTop (18),
                        Justification::centredLeft, true);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 4).removeFromBottom (20);
            go.setBounds (r.removeFromLeft (44).reduced (1));
            update.setBounds (r.removeFromLeft (62).reduced (1));
            drop.setBounds (r.removeFromLeft (66).reduced (1));
        }

        String line;
        bool exists;
        TextButton go, update, drop;
    };

    void rebuild()
    {
        cardViews.clear();

        for (size_t i = 0; i < attached.size(); ++i)
        {
            const auto& a = attached[i];
            auto* card = cardViews.add (new Card (attachments.summary (a), attachments.stillExists (a)));
            cards.addAndMakeVisible (card);

            const auto index = static_cast<int> (i);
            card->go.onClick = [this, index] { revealAt (index); };
            card->drop.onClick = [this, index] { remove (index); };
            card->update.onClick = [this, index] { if (onRefresh) onRefresh (index); };
        }

        note.setText (attached.empty()
                        ? "Select in the Playlist, piano roll or Mixer, then Ask AI."
                        : String (attached.size()) + (attached.size() == 1 ? " attachment" : " attachments"),
                      dontSendNotification);

        layOutCards();
        repaint();
    }

    void layOutCards()
    {
        const auto height = 52;
        cards.setSize (std::max (10, viewport.getWidth() - viewport.getScrollBarThickness()),
                       std::max (viewport.getHeight(), height * cardViews.size()));

        for (int i = 0; i < cardViews.size(); ++i)
            cardViews[i]->setBounds (0, i * height, cards.getWidth(), height);
    }

    void revealAt (int index)
    {
        if (reveal != nullptr && isPositiveAndBelow (index, static_cast<int> (attached.size())))
            reveal (attached[static_cast<size_t> (index)]);
    }

    void showInspector()
    {
        auto* view = new TextEditor();
        view->setMultiLine (true, true);
        view->setReadOnly (true);
        view->setSize (520, 360);
        view->setText (inspectorText(), dontSendNotification);

        DialogWindow::LaunchOptions options;
        options.content.setOwned (view);
        options.dialogTitle = "What gets sent";
        options.dialogBackgroundColour = theme::panel;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.launchAsync();
    }

public:
    /** Asked when a card's Update button is pressed, because only the workspace knows
        what is selected right now. */
    std::function<void (int)> onRefresh;

private:
    Model& model;
    Selection& selection;
    Attachments attachments;
    std::function<void (const Attachment&)> reveal;

    std::vector<Attachment> attached;
    OwnedArray<Card> cardViews;
    Component cards;
    Viewport viewport;
    TextEditor entry, transcript, change;
    TextButton inspect, clear, send, stop, apply, preview, playA, playB, addCondition, forget;
    TextEditor decisions;
    ComboBox strength;
    String notesShape;
    String listenShape;
    Label previewState;
    ComboBox candidates;
    StringArray candidateIDs;
    String candidateShape;
    String offered;
    Label note, connection;
};

} // namespace live
