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

        return object ({ { "attachments", cardsOut }, { "draft", entry.getText() } });
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
    TextEditor entry, transcript;
    TextButton inspect, clear, send, stop;
    Label note, connection;
};

} // namespace live
