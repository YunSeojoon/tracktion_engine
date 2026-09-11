#pragma once

#include "Model.h"

namespace live
{
/** Note editing for one channel inside one pattern.

    Notes are the model's NOTE children, so every edit here reaches every placement of
    the pattern and takes one undo, exactly like an edit arriving from outside. The
    keyboard on the left previews through the channel's real instrument.
*/
class PianoRollEditor final : public Component,
                              private Timer
{
public:
    PianoRollEditor (Model& m, const String& patternID, const String& channelID)
        : model (m), pattern (patternID), channel (channelID)
    {
        snap.addItem ("1/1", 1);
        snap.addItem ("1/2", 2);
        snap.addItem ("1/4", 3);
        snap.addItem ("1/8", 4);
        snap.addItem ("1/16", 5);
        snap.addItem ("Off", 6);
        snap.setSelectedId (5, dontSendNotification);

        zoom.setSliderStyle (Slider::LinearHorizontal);
        zoom.setRange (12.0, 90.0, 1.0);
        zoom.setValue (26.0, dontSendNotification);
        zoom.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        zoom.onValueChange = [this] { resized(); repaint(); };

        tools.addItem ("Select", 1);
        tools.addItem ("Draw", 2);
        tools.setSelectedId (2, dontSendNotification);
        tools.onChange = [this]
        {
            tool = tools.getSelectedId() == 1 ? select : draw;
            describeWhatIsOpen();
        };

        quantise.onClick = [this] { quantiseSelection(); };
        duplicate.onClick = [this] { duplicateSelection(); };
        deleteNotes.onClick = [this] { deleteSelection(); };

        heading.setColour (Label::textColourId, theme::text);
        heading.setFont (Font (FontOptions (12.0f, Font::bold)));

        hint.setColour (Label::textColourId, theme::textDim);
        hint.setFont (Font (FontOptions (11.0f)));
        hint.setText ("Drag to move, drag the right edge to resize, Alt-drag for velocity, "
                      "Ctrl+D duplicate, Q quantise, Delete removes", dontSendNotification);

        grid = std::make_unique<Grid> (*this);
        lane = std::make_unique<VelocityLane> (*this);
        viewport.onScroll = [this] { lane->repaint(); };
        viewport.setViewedComponent (grid.get(), false);
        viewport.setScrollBarsShown (true, true);

        addAndMakeVisible (viewport);
        addAndMakeVisible (*lane);
        addAndMakeVisible (heading);
        addAndMakeVisible (tools);
        addAndMakeVisible (snap);
        addAndMakeVisible (zoom);
        addAndMakeVisible (quantise);
        addAndMakeVisible (duplicate);
        addAndMakeVisible (deleteNotes);
        addAndMakeVisible (hint);

        setWantsKeyboardFocus (true);
        describeWhatIsOpen();
        setSize (1000, 580);
        startTimer (200);
    }

    ~PianoRollEditor() override { stopTimer(); releasePreview(); }

    /** A held preview note whose window loses the keyboard would sound forever: the
        mouse-up that was going to stop it goes somewhere else now. */
    void focusLost (FocusChangeType) override { releasePreview(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        heading.setBounds (r.removeFromTop (18));
        auto bar = r.removeFromTop (28);
        tools.setBounds (bar.removeFromLeft (90).reduced (2));
        snap.setBounds (bar.removeFromLeft (86).reduced (2));
        zoom.setBounds (bar.removeFromLeft (150).reduced (2));
        quantise.setBounds (bar.removeFromLeft (90).reduced (2));
        duplicate.setBounds (bar.removeFromLeft (90).reduced (2));
        deleteNotes.setBounds (bar.removeFromLeft (80).reduced (2));
        hint.setBounds (r.removeFromBottom (18));
        lane->setBounds (r.removeFromBottom (velocityLane));
        viewport.setBounds (r);
        layOutGrid();
    }

    void paint (Graphics& g) override { g.fillAll (theme::panel); }

    bool keyPressed (const KeyPress& key) override
    {
        if (key == KeyPress::deleteKey || key == KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (key == KeyPress ('d', ModifierKeys::ctrlModifier, 0)) { duplicateSelection(); return true; }
        if (key == KeyPress ('q', ModifierKeys::noModifiers, 0)) { quantiseSelection(); return true; }
        if (key == KeyPress ('a', ModifierKeys::ctrlModifier, 0))
        {
            selected.clear();
            for (auto note : sequence())
                selected.add (Model::uidOf (note));
            grid->repaint();
            return true;
        }
        return false;
    }

    /** Adds a note the same way clicking an empty cell does, and selects it. */
    bool addNote (int pitch, double startBeat, double lengthBeats, int velocity)
    {
        auto tree = patternTree();
        if (! tree.isValid())
            return false;

        const auto length = jlimit (0.0625, patternBeats(), lengthBeats);
        const auto start = jlimit (0.0, std::max (0.0, patternBeats() - length), startBeat);

        undo().beginNewTransaction ("Add note");
        auto notes = model.sequenceFor (tree, channel, &undo());
        auto note = model.addNote (notes, jlimit (lowestNote, highestNote, pitch), start, length,
                                   jlimit (1, 127, velocity), &undo());
        model.renderIfNeeded();
        selected.clearQuick();
        selected.add (Model::uidOf (note));
        grid->repaint();
        return true;
    }

    /** Shows a change that has been suggested but not made, as an outline where each
        note would go. It is drawn beside the music rather than instead of it, so the
        two are never confused, and nothing here can be selected, dragged or deleted:
        there is nothing there to touch until somebody presses Apply.

        A change about some other pattern or channel is not this editor's business and
        is dropped rather than drawn somewhere it does not belong. */
    void showSuggested (const String& patternID, const String& channelID, const var& diff)
    {
        suggestedChange = (patternID == pattern && channelID == channel) ? diff : var();
        repaint();
    }

    const var& suggested() const { return suggestedChange; }

    static constexpr int keyboardWidth = 54;
    static constexpr int velocityLane = 70;
    static constexpr int lowestNote = 24, highestNote = 108;

private:
    //==========================================================================
    var suggestedChange;

    ValueTree patternTree() const { return model.patternFor (pattern); }
    ValueTree sequence() const { return Model::findSequence (patternTree(), channel); }


    double patternBeats() const
    {
        auto tree = patternTree();
        return tree.isValid() ? std::max (1.0, static_cast<double> (tree[ids::length])) : 1.0;
    }

    double snapBeats() const
    {
        switch (snap.getSelectedId())
        {
            case 1:  return 4.0;
            case 2:  return 2.0;
            case 3:  return 1.0;
            case 4:  return 0.5;
            case 5:  return 0.25;
            default: return 0.0;
        }
    }

    double snapped (double beat) const
    {
        const auto step = snapBeats();
        return step <= 0.0 ? beat : std::round (beat / step) * step;
    }

    double beatWidth() const { return zoom.getValue(); }
    static constexpr int noteHeight = 11;

    void layOutGrid()
    {
        if (grid == nullptr)
            return;

        const auto width = keyboardWidth + roundToInt (patternBeats() * beatWidth()) + 8;
        const auto height = (highestNote - lowestNote + 1) * noteHeight;
        grid->setSize (std::max (width, viewport.getWidth() - viewport.getScrollBarThickness()),
                       std::max (height, viewport.getHeight() - viewport.getScrollBarThickness()));

        if (! scrolledToNotes)
        {
            scrolledToNotes = true;
            scrollToNotes();
        }
    }

    void timerCallback() override
    {
        const auto signature = JSON::toString (var (sequence().toXmlString()), true) + String (patternBeats());
        if (signature != lastSignature)
        {
            lastSignature = signature;
            layOutGrid();
            grid->repaint();
        }

        // How far an edit here reaches depends on how many places the pattern is
        // played, and that can change while the window is open - somebody drops another
        // copy into the arrangement, or an outside edit does. The heading has to be
        // true when it is read, not when the window was opened.
        auto places = 0;
        for (auto clip : model.instances())
            if (clip[ids::pattern].toString() == pattern)
                ++places;

        if (places != lastPlacementCount)
        {
            lastPlacementCount = places;
            describeWhatIsOpen();
        }
    }

    //==========================================================================
    UndoManager& undo() const { return model.edit.getUndoManager(); }

    ValueTree noteWithID (const String& noteID) const
    {
        return Model::withID (sequence(), ids::NOTE, noteID);
    }

    void deleteSelection()
    {
        if (selected.isEmpty())
            return;

        auto notes = sequence();
        undo().beginNewTransaction ("Delete notes");
        for (const auto& noteID : selected)
            if (auto note = Model::withID (notes, ids::NOTE, noteID); note.isValid())
                notes.removeChild (note, &undo());
        selected.clear();
        model.renderIfNeeded();
        grid->repaint();
    }

    void duplicateSelection()
    {
        if (selected.isEmpty())
            return;

        auto notes = sequence();
        double span = 0.0;
        for (const auto& noteID : selected)
            if (auto note = Model::withID (notes, ids::NOTE, noteID); note.isValid())
                span = std::max (span, static_cast<double> (note[ids::start])
                                        + static_cast<double> (note[ids::length]));

        const auto shift = std::max (snapBeats() > 0.0 ? snapBeats() : 0.25, span - earliestSelected());
        StringArray copies;

        undo().beginNewTransaction ("Duplicate notes");
        for (const auto& noteID : selected)
        {
            auto note = Model::withID (notes, ids::NOTE, noteID);
            if (! note.isValid())
                continue;

            const auto start = static_cast<double> (note[ids::start]) + shift;
            const auto length = static_cast<double> (note[ids::length]);
            if (start + length > patternBeats() + 1.0e-6)
                continue;

            auto copy = model.addNote (notes, static_cast<int> (note[ids::pitch]), start, length,
                                       static_cast<int> (note[ids::velocity]), &undo());
            copies.add (Model::uidOf (copy));
        }

        selected = copies;
        model.renderIfNeeded();
        grid->repaint();
    }

    double earliestSelected() const
    {
        auto notes = sequence();
        double earliest = patternBeats();
        for (const auto& noteID : selected)
            if (auto note = Model::withID (notes, ids::NOTE, noteID); note.isValid())
                earliest = std::min (earliest, static_cast<double> (note[ids::start]));
        return earliest;
    }

    void quantiseSelection()
    {
        const auto step = snapBeats();
        if (step <= 0.0)
            return;

        auto notes = sequence();
        auto targets = selected;
        if (targets.isEmpty())
            for (auto note : notes)
                targets.add (Model::uidOf (note));

        undo().beginNewTransaction ("Quantise notes");
        for (const auto& noteID : targets)
            if (auto note = Model::withID (notes, ids::NOTE, noteID); note.isValid())
            {
                const auto length = static_cast<double> (note[ids::length]);
                const auto start = jlimit (0.0, std::max (0.0, patternBeats() - length),
                                           std::round (static_cast<double> (note[ids::start]) / step) * step);
                note.setProperty (ids::start, start, &undo());
            }

        model.renderIfNeeded();
        grid->repaint();
    }

    /** Says what this editor is looking at, and what editing it will reach.

        A pattern can be placed in several parts of the song, and editing its notes is
        heard in all of them. Someone who opened the editor by double-clicking one clip
        will reasonably think they are editing that clip. They are not, unless it is the
        only placement - so the window says so before they touch anything. */
    void describeWhatIsOpen()
    {
        auto tree = patternTree();
        auto owner = model.channelFor (channel);

        auto places = 0;
        for (auto clip : model.instances())
            if (clip[ids::pattern].toString() == pattern)
                ++places;

        String text;
        text << (tree.isValid() ? tree[ids::name].toString() : String ("(no pattern)"))
             << "   -   " << (owner.isValid() ? owner[ids::name].toString() : String ("(no channel)"));

        if (places > 1)
            text << "   -   played in " << places << " places, and editing changes all of them";
        else if (places == 1)
            text << "   -   played in one place";
        else
            text << "   -   not placed in the song yet";

        heading.setText (text, dontSendNotification);
    }

    void preview (int pitch)
    {
        if (pitch == previewing)
            return;

        releasePreview();
        if (auto* track = model.trackFor (channel))
        {
            track->playGuideNote (pitch, te::MidiChannel (1), 100, true, false, false);
            previewing = pitch;
        }
    }

    void releasePreview()
    {
        if (previewing >= 0)
            if (auto* track = model.trackFor (channel))
                track->turnOffGuideNotes();
        previewing = -1;
    }

    //==========================================================================
    /** The scrolled surface: keyboard, note grid and velocity lane. */
    class Grid final : public Component
    {
    public:
        explicit Grid (PianoRollEditor& o) : owner (o) { setWantsKeyboardFocus (false); }

        void paint (Graphics& g) override
        {
            const auto beats = owner.patternBeats();
            const auto perBeat = owner.beatWidth();
            const auto gridHeight = getHeight();

            g.fillAll (theme::panelHeader);

            for (int pitch = lowestNote; pitch <= highestNote; ++pitch)
            {
                const auto y = rowY (pitch);
                const auto black = MidiMessage::isMidiNoteBlack (pitch);

                g.setColour (black ? Colour (0xff161c28) : theme::row);
                g.fillRect (keyboardWidth, y, getWidth() - keyboardWidth, noteHeight);

                g.setColour (black ? theme::sunken : theme::text);
                g.fillRect (0, y, keyboardWidth - 2, noteHeight - 1);

                if (pitch % 12 == 0)
                {
                    g.setColour (theme::textFaint);
                    g.drawText ("C" + String (pitch / 12 - 1), 3, y - 1, keyboardWidth - 8, noteHeight,
                                Justification::centredLeft);
                }
            }

            for (double beat = 0.0; beat <= beats + 1.0e-6; beat += 1.0)
            {
                const auto x = keyboardWidth + roundToInt (beat * perBeat);
                const auto bar = std::abs (std::fmod (beat, 4.0)) < 1.0e-6;
                g.setColour (bar ? theme::edgeStrong : theme::edge);
                g.fillRect (x, 0, bar ? 2 : 1, gridHeight);
            }

            // Everything past the end of the pattern is not part of it.
            const auto patternEnd = keyboardWidth + roundToInt (beats * perBeat);
            if (patternEnd < getWidth())
            {
                g.setColour (Colour (0xd0101620));
                g.fillRect (patternEnd, 0, getWidth() - patternEnd, gridHeight);
            }

            auto notes = owner.sequence();
            for (auto note : notes)
            {
                const auto area = noteArea (note);
                const auto picked = owner.selected.contains (Model::uidOf (note));
                const auto velocity = static_cast<int> (note[ids::velocity]);

                g.setColour (picked ? theme::warn : theme::good);
                g.fillRoundedRectangle (area.toFloat(), 2.0f);
                g.setColour (theme::sunken);
                g.drawRoundedRectangle (area.toFloat(), 2.0f, 1.0f);

                // A louder note is drawn brighter, so velocity reads without the lane.
                g.setColour (Colours::white.withAlpha (velocity / 400.0f));
                g.fillRoundedRectangle (area.toFloat().reduced (1.5f), 1.5f);
            }

            paintSuggested (g, perBeat);

            if (! rubberBand.isEmpty())
            {
                g.setColour (Colour (0x40ffd479));
                g.fillRect (rubberBand);
                g.setColour (theme::warn);
                g.drawRect (rubberBand, 1);
            }
        }

        void mouseDown (const MouseEvent& e) override
        {
            owner.grabKeyboardFocus();

            if (e.x < keyboardWidth)
            {
                owner.preview (pitchAt (e.y));
                return;
            }

            auto hit = noteAt (e.getPosition());

            if (! hit.isValid())
            {
                if (! e.mods.isShiftDown())
                    owner.selected.clear();

                if (e.mods.isRightButtonDown())
                {
                    dragMode = none;
                    rubberBand = {};
                    showEmptyMenu();
                    repaint();
                    return;
                }

                dragMode = rubber;
                rubberStart = e.getPosition();
                rubberBand = {};

                // Draw writes notes; Select selects them. Having one gesture do both -
                // clicking empty space wrote a note and also started a selection box -
                // meant every attempt to drag out a selection left a note behind.
                if (owner.tool == PianoRollEditor::draw && ! e.mods.isAltDown())
                    addNoteAt (e.getPosition());

                repaint();
                return;
            }

            const auto noteID = Model::uidOf (hit);

            if (e.mods.isRightButtonDown())
            {
                // It used to delete on the spot. Right-clicking a note is how a person
                // asks what they can do with it, and answering by destroying it is the
                // one answer they cannot undo before it happens.
                if (! owner.selected.contains (noteID))
                {
                    owner.selected.clearQuick();
                    owner.selected.add (noteID);
                    repaint();
                }

                showNoteMenu (noteID);
                return;
            }

            if (e.mods.isShiftDown())
            {
                if (owner.selected.contains (noteID)) owner.selected.removeString (noteID);
                else owner.selected.add (noteID);
            }
            else if (! owner.selected.contains (noteID))
            {
                owner.selected.clearQuick();
                owner.selected.add (noteID);
            }

            const auto area = noteArea (hit);
            dragMode = e.mods.isAltDown() ? velocity
                     : e.x > area.getRight() - 6 ? resize : move;
            dragAnchor = e.getPosition();
            captureStarts();
            owner.preview (static_cast<int> (hit[ids::pitch]));
            repaint();
        }

        void mouseDrag (const MouseEvent& e) override
        {
            if (dragMode == rubber)
            {
                rubberBand = Rectangle<int>::leftTopRightBottom (
                    std::min (rubberStart.x, e.x), std::min (rubberStart.y, e.y),
                    std::max (rubberStart.x, e.x), std::max (rubberStart.y, e.y));

                owner.selected.clearQuick();
                for (auto note : owner.sequence())
                    if (rubberBand.intersects (noteArea (note)))
                        owner.selected.add (Model::uidOf (note));

                repaint();
                return;
            }

            if (dragMode == none || starts.isEmpty())
                return;

            const auto beatDelta = (e.x - dragAnchor.x) / owner.beatWidth();
            const auto pitchDelta = (dragAnchor.y - e.y) / noteHeight;
            auto notes = owner.sequence();

            owner.undo().beginNewTransaction (dragMode == resize ? "Resize notes"
                                            : dragMode == velocity ? "Set velocity" : "Move notes");

            for (const auto& start : starts)
            {
                auto note = Model::withID (notes, ids::NOTE, start.id);
                if (! note.isValid())
                    continue;

                if (dragMode == move)
                {
                    const auto length = static_cast<double> (note[ids::length]);
                    note.setProperty (ids::start, jlimit (0.0, std::max (0.0, owner.patternBeats() - length),
                                                          owner.snapped (start.start + beatDelta)), &owner.undo());
                    note.setProperty (ids::pitch, jlimit (lowestNote, highestNote, start.pitch + pitchDelta),
                                      &owner.undo());
                }
                else if (dragMode == resize)
                {
                    const auto room = owner.patternBeats() - static_cast<double> (note[ids::start]);
                    const auto wanted = owner.snapped (start.length + beatDelta);
                    note.setProperty (ids::length, jlimit (0.0625, std::max (0.0625, room),
                                                           wanted <= 0.0 ? 0.0625 : wanted), &owner.undo());
                }
                else
                {
                    note.setProperty (ids::velocity,
                                      jlimit (1, 127, start.velocity + roundToInt ((dragAnchor.y - e.y) * 0.8)),
                                      &owner.undo());
                }
            }

            owner.model.renderIfNeeded();
            owner.lane->repaint();
            repaint();
        }

        void mouseUp (const MouseEvent&) override
        {
            dragMode = none;
            starts.clearQuick();
            rubberBand = {};
            owner.releasePreview();
            repaint();
        }

    private:
        struct Start { String id; double start, length; int pitch, velocity; };
        enum DragMode { none, move, resize, velocity, rubber };

        static int rowY (int pitch) { return (highestNote - pitch) * noteHeight; }
        static int pitchAt (int y) { return jlimit (lowestNote, highestNote, highestNote - y / noteHeight); }

        /** A suggested change, drawn as it would be rather than as it is. An outline,
            never a filled note, because a filled note is something that exists. */
        void paintSuggested (Graphics& g, double perBeat) const
        {
            auto* changes = owner.suggested()["notes"].getArray();
            if (changes == nullptr)
                return;

            auto notes = owner.sequence();

            for (const auto& entry : *changes)
            {
                const auto what = entry.getProperty ("what", "change").toString();
                auto note = Model::withID (notes, ids::NOTE, entry["id"].toString());

                // A field is either "it would become this", or unchanged and read from
                // the note as it stands, or - for a note being added - given outright.
                auto field = [&entry, &note] (const char* name, const Identifier& existing,
                                              double fallback)
                {
                    if (entry[name].isObject())
                        return static_cast<double> (entry[name]["now"]);
                    if (entry.hasProperty (name))
                        return static_cast<double> (entry[name]);
                    return note.isValid() ? static_cast<double> (note[existing]) : fallback;
                };

                if (what == "remove")
                {
                    if (! note.isValid())
                        continue;

                    const auto area = noteArea (note).toFloat();
                    g.setColour (theme::danger);
                    g.drawLine (area.getX(), area.getCentreY(), area.getRight(), area.getCentreY(), 2.0f);
                    continue;
                }

                const auto pitch = roundToInt (field ("pitch", ids::pitch, 60.0));
                const auto start = field ("start_beat", ids::start, 0.0);
                const auto length = field ("length_beats", ids::length, 1.0);

                const Rectangle<int> area { keyboardWidth + roundToInt (start * perBeat),
                                            rowY (jlimit (lowestNote, highestNote, pitch)),
                                            std::max (4, roundToInt (length * perBeat)),
                                            noteHeight - 1 };

                g.setColour (theme::accent.withAlpha (0.18f));
                g.fillRoundedRectangle (area.toFloat(), 2.0f);
                g.setColour (theme::accent);
                g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 2.0f, 1.5f);

                // A line from where it is to where it would be, so a moved note reads as
                // one note moving rather than two notes.
                if (what == "change" && note.isValid())
                {
                    const auto from = noteArea (note).toFloat();
                    g.setColour (theme::accentDim);
                    g.drawLine (from.getCentreX(), from.getCentreY(),
                                area.toFloat().getCentreX(), area.toFloat().getCentreY(), 1.0f);
                }
            }
        }

        /** The menu for empty grid: what a person can do here when they have not
            pointed at a note. Kept small on purpose - a long menu of things that need
            a selection, all greyed out, tells nobody anything. */
        void showEmptyMenu()
        {
            PopupMenu menu;
            menu.addItem (1, "Select tool", true, owner.tool == PianoRollEditor::select);
            menu.addItem (2, "Draw tool", true, owner.tool == PianoRollEditor::draw);
            menu.addSeparator();
            menu.addItem (3, "Select everything", owner.sequence().getNumChildren() > 0);
            menu.addSeparator();
            menu.addItem (4, "Ask AI about this part");

            menu.showMenuAsync (PopupMenu::Options(), [this] (int chosen)
            {
                switch (chosen)
                {
                    case 1: owner.setTool (PianoRollEditor::select); break;
                    case 2: owner.setTool (PianoRollEditor::draw);   break;
                    case 3:
                        owner.selected.clearQuick();
                        for (auto note : owner.sequence())
                            owner.selected.add (Model::uidOf (note));
                        break;
                    case 4: if (owner.askAboutSelection) owner.askAboutSelection(); break;
                    default: break;
                }

                repaint();
            });
        }

        /** What can be done to the notes that are picked out. Everything here acts on
            the whole selection and goes in as one transaction, so taking it back is one
            undo however many notes it touched. */
        void showNoteMenu (const String& noteID)
        {
            PopupMenu menu;
            const auto many = owner.selected.size() > 1;
            const auto what = many ? " " + String (owner.selected.size()) + " notes" : String();

            menu.addItem (1, "Up a semitone" + what);
            menu.addItem (2, "Down a semitone" + what);
            menu.addItem (3, "Up an octave" + what);
            menu.addItem (4, "Down an octave" + what);
            menu.addSeparator();
            menu.addItem (5, "Quantise to the grid" + what);
            menu.addSeparator();
            menu.addItem (6, "Ask AI about " + (many ? String ("these notes") : String ("this note")));
            menu.addSeparator();
            menu.addItem (7, "Delete" + what);

            menu.showMenuAsync (PopupMenu::Options(), [this, noteID] (int chosen)
            {
                if (chosen == 0)
                    return;

                // The music may have moved while the menu sat open.
                if (! Model::withID (owner.sequence(), ids::NOTE, noteID).isValid())
                    return;

                switch (chosen)
                {
                    case 1: owner.transposeSelection (1);   break;
                    case 2: owner.transposeSelection (-1);  break;
                    case 3: owner.transposeSelection (12);  break;
                    case 4: owner.transposeSelection (-12); break;
                    case 5: owner.quantiseSelection();      break;
                    case 6: if (owner.askAboutSelection) owner.askAboutSelection(); break;
                    case 7: owner.deleteSelection();        break;
                    default: break;
                }

                repaint();
            });
        }

        Rectangle<int> noteArea (ValueTree note) const
        {
            const auto x = keyboardWidth + roundToInt (static_cast<double> (note[ids::start]) * owner.beatWidth());
            const auto w = std::max (4, roundToInt (static_cast<double> (note[ids::length]) * owner.beatWidth()));
            return { x, rowY (static_cast<int> (note[ids::pitch])), w, noteHeight - 1 };
        }

        ValueTree noteAt (Point<int> position) const
        {
            for (auto note : owner.sequence())
                if (noteArea (note).contains (position))
                    return note;
            return {};
        }

        void addNoteAt (Point<int> position)
        {
            auto tree = owner.patternTree();
            if (! tree.isValid())
                return;

            const auto step = owner.snapBeats() > 0.0 ? owner.snapBeats() : 0.25;
            const auto beat = owner.snapped ((position.x - keyboardWidth) / owner.beatWidth());

            owner.addNote (pitchAt (position.y), beat, step, 100);
            owner.preview (pitchAt (position.y));
        }

        void captureStarts()
        {
            starts.clearQuick();
            auto notes = owner.sequence();
            for (const auto& noteID : owner.selected)
                if (auto note = Model::withID (notes, ids::NOTE, noteID); note.isValid())
                    starts.add ({ noteID, static_cast<double> (note[ids::start]),
                                  static_cast<double> (note[ids::length]),
                                  static_cast<int> (note[ids::pitch]),
                                  static_cast<int> (note[ids::velocity]) });
        }

        PianoRollEditor& owner;
        Array<Start> starts;
        DragMode dragMode = none;
        Point<int> dragAnchor, rubberStart;
        Rectangle<int> rubberBand;
    };

    /** Centres the view on the notes that are there, so an existing pattern does not
        open showing an empty octave. */
    void scrollToNotes()
    {
        int highest = 72, lowest = 48;
        auto notes = sequence();
        if (notes.getNumChildren() > 0)
        {
            highest = 0;
            lowest = 127;
            for (auto note : notes)
            {
                const auto pitch = static_cast<int> (note[ids::pitch]);
                highest = std::max (highest, pitch);
                lowest = std::min (lowest, pitch);
            }
        }

        const auto centre = (highestNote - (highest + lowest) / 2) * noteHeight;
        viewport.setViewPosition (0, jmax (0, centre - viewport.getHeight() / 2));
        lane->repaint();
    }

    /** The velocity of every note, pinned under the grid and scrolled with it. */
    class VelocityLane final : public Component
    {
    public:
        explicit VelocityLane (PianoRollEditor& o) : owner (o) {}

        void paint (Graphics& g) override
        {
            g.fillAll (theme::sunken);
            g.setColour (theme::edge);
            g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));

            const auto offset = owner.viewport.getViewPositionX();
            for (auto note : owner.sequence())
            {
                const auto x = keyboardWidth + roundToInt (static_cast<double> (note[ids::start]) * owner.beatWidth()) - offset;
                const auto w = std::max (3, roundToInt (static_cast<double> (note[ids::length]) * owner.beatWidth()) - 2);
                const auto velocity = static_cast<int> (note[ids::velocity]);
                const auto barHeight = roundToInt (velocity / 127.0 * (getHeight() - 8));

                g.setColour (owner.selected.contains (Model::uidOf (note)) ? theme::warn : theme::clipFill);
                g.fillRect (x, getHeight() - barHeight - 3, w, barHeight);
            }

            g.setColour (theme::textFaint);
            g.setFont (Font (FontOptions (10.0f)));
            g.drawText ("VELOCITY", 4, 2, 70, 12, Justification::centredLeft);
        }

    private:
        PianoRollEditor& owner;
    };

    struct NotifyingViewport final : Viewport
    {
        void visibleAreaChanged (const Rectangle<int>&) override { if (onScroll) onScroll(); }
        std::function<void()> onScroll;
    };

    Model& model;
    const String pattern, channel;
    NotifyingViewport viewport;
    std::unique_ptr<Grid> grid;
    std::unique_ptr<VelocityLane> lane;
    bool scrolledToNotes = false;
    Label heading;
    ComboBox tools;
    ComboBox snap;
    Slider zoom;
    TextButton quantise { "Quantise" }, duplicate { "Duplicate" }, deleteNotes { "Delete" };
    Label hint;
    StringArray selected;

public:
    /** Which notes are picked out right now, so a question can be asked about exactly
        those and nothing else. */
    /** What the left button does on empty space. Two tools, named on screen, because a
        person needs to know which one they are holding before they click - and because
        one gesture doing both jobs did neither of them properly. */
    enum Tool { select, draw };
    Tool tool = draw;

    void setTool (Tool which)
    {
        tool = which;
        tools.setSelectedId (which == select ? 1 : 2, dontSendNotification);
        describeWhatIsOpen();
    }

    String heading_() const { return heading.getText(); }

    /** Clicks the grid where a pitch and a beat meet, through the real handlers.
        Same caveat as the arrangement's: this proves the gesture, not the window
        manager. */
    bool clickGrid (int pitch, double beat, bool rightButton = false)
    {
        if (grid == nullptr)
            return false;

        const auto mods = rightButton ? ModifierKeys (ModifierKeys::rightButtonModifier)
                                      : ModifierKeys (ModifierKeys::leftButtonModifier);
        const auto row = (highestNote - jlimit (lowestNote, highestNote, pitch)) * noteHeight;
        const Point<float> where ((float) (keyboardWidth + roundToInt (beat * beatWidth())),
                                  (float) (row + noteHeight / 2));

        const MouseEvent e (Desktop::getInstance().getMainMouseSource(), where, mods,
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, grid.get(), grid.get(),
                            Time::getCurrentTime(), where, Time::getCurrentTime(), 1, false);
        grid->mouseDown (e);
        grid->mouseUp (e);
        return true;
    }

    StringArray selectedNotes() const { return selected; }

    /** Asks the app to attach whatever is picked out here to the chat. Set by the app;
        without it the menu item simply does nothing rather than lying about what it
        would do. */
    std::function<void()> askAboutSelection;

    void transposeSelection (int semitones)
    {
        if (selected.isEmpty())
            return;

        undo().beginNewTransaction (semitones > 0 ? "Transpose up" : "Transpose down");

        for (const auto& id : selected)
            if (auto note = Model::withID (sequence(), ids::NOTE, id); note.isValid())
                note.setProperty (ids::pitch,
                                  jlimit (0, 127, static_cast<int> (note[ids::pitch]) + semitones),
                                  &undo());

        model.renderIfNeeded();
        grid->repaint();
    }


    /** Picks out notes by id, or clears the selection when given none. Selecting is not
        an edit: it changes what a question is about, never the music. */
    void selectNotes (const StringArray& ids)
    {
        selected = ids;
        repaint();
    }

private:
    String lastSignature;
    int previewing = -1;
    int lastPlacementCount = -1;

    friend class Grid;
};
}
