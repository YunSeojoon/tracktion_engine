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

        quantise.onClick = [this] { quantiseSelection(); };
        duplicate.onClick = [this] { duplicateSelection(); };
        deleteNotes.onClick = [this] { deleteSelection(); };

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
        addAndMakeVisible (snap);
        addAndMakeVisible (zoom);
        addAndMakeVisible (quantise);
        addAndMakeVisible (duplicate);
        addAndMakeVisible (deleteNotes);
        addAndMakeVisible (hint);

        setWantsKeyboardFocus (true);
        setSize (1000, 560);
        startTimer (200);
    }

    ~PianoRollEditor() override { stopTimer(); releasePreview(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bar = r.removeFromTop (28);
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

    static constexpr int keyboardWidth = 54;
    static constexpr int velocityLane = 70;
    static constexpr int lowestNote = 24, highestNote = 108;

private:
    //==========================================================================
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
                dragMode = e.mods.isRightButtonDown() ? none : rubber;
                rubberStart = e.getPosition();
                rubberBand = {};

                if (! e.mods.isRightButtonDown() && ! e.mods.isAltDown())
                    addNoteAt (e.getPosition());

                repaint();
                return;
            }

            const auto noteID = Model::uidOf (hit);

            if (e.mods.isRightButtonDown())
            {
                owner.undo().beginNewTransaction ("Delete note");
                owner.sequence().removeChild (hit, &owner.undo());
                owner.selected.removeString (noteID);
                owner.model.renderIfNeeded();
                repaint();
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
    ComboBox snap;
    Slider zoom;
    TextButton quantise { "Quantise" }, duplicate { "Duplicate" }, deleteNotes { "Delete" };
    Label hint;
    StringArray selected;

public:
    /** Which notes are picked out right now, so a question can be asked about exactly
        those and nothing else. */
    StringArray selectedNotes() const { return selected; }

private:
    String lastSignature;
    int previewing = -1;

    friend class Grid;
};
}
