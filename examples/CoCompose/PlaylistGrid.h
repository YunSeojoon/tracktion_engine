#pragma once

#include "Model.h"

namespace live
{
/** The arrangement: playlist lanes down, bars across, pattern placements as clips.

    A clip is a reference to a pattern, so dragging one around never copies music. It
    carries the beat it starts at, how long it plays and how far into the pattern it
    begins, which is what lets a clip be a slice of a pattern or a repeat of it.
*/
class PlaylistGrid final : public Component,
                           public DragAndDropTarget,
                           private Timer
{
public:
    PlaylistGrid (Model& m, Selection& s, std::function<void()> onChange)
        : model (m), selection (s), changed (std::move (onChange))
    {
        setWantsKeyboardFocus (true);
        startTimerHz (20);
    }

    ~PlaylistGrid() override { stopTimer(); }

    static constexpr int laneWidth = 96, rulerHeight = 22, laneHeight = 30;

    double beatWidth() const { return zoom; }
    void setZoom (double pixelsPerBeat) { zoom = jlimit (1.5, 40.0, pixelsPerBeat); resized(); repaint(); }
    double getZoom() const { return zoom; }

    void setSnap (double beats) { snap = beats; }
    double getSnap() const { return snap; }

    /** Wide enough for the arrangement plus a bar of room to extend it. */
    int preferredWidth() const { return laneWidth + roundToInt ((arrangementBeats() + 8.0) * beatWidth()) + 20; }
    int preferredHeight() const { return rulerHeight + std::max (1, model.lanes().getNumChildren()) * laneHeight + 8; }

    double arrangementBeats() const
    {
        double end = 4.0;
        for (auto instance : model.instances())
            end = std::max (end, static_cast<double> (instance[ids::start])
                                  + static_cast<double> (instance[ids::length]));
        return end;
    }

    //==========================================================================
    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff161d29));

        const auto beats = std::max (arrangementBeats() + 8.0, (getWidth() - laneWidth) / beatWidth());

        paintLoopRange (g, beats);
        paintGrid (g, beats);
        paintLanes (g);
        paintClips (g);
        paintRuler (g, beats);
        paintPlayhead (g);

        if (! rubberBand.isEmpty())
        {
            g.setColour (Colour (0x40ffd479));
            g.fillRect (rubberBand);
            g.setColour (Colour (0xffffd479));
            g.drawRect (rubberBand, 1);
        }
    }

    void resized() override {}

    //==========================================================================
    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (e.y < rulerHeight)
        {
            loopAnchor = beatAt (e.x);
            setLoopRange (loopAnchor, loopAnchor);
            dragMode = loop;
            return;
        }

        auto hit = clipAt (e.getPosition());

        if (! hit.isValid())
        {
            if (! e.mods.isShiftDown())
                selected.clear();

            if (e.mods.isRightButtonDown())
            {
                dragMode = none;
            }
            else if (e.mods.isAltDown())
            {
                dragMode = rubber;
                rubberStart = e.getPosition();
            }
            else
            {
                place (e.getPosition());
            }

            repaint();
            return;
        }

        const auto clipID = Model::uidOf (hit);

        if (e.mods.isRightButtonDown())
        {
            undo().beginNewTransaction ("Delete clip");
            model.instances().removeChild (hit, &undo());
            selected.removeString (clipID);
            model.renderIfNeeded();
            notify();
            return;
        }

        if (e.mods.isShiftDown())
        {
            if (selected.contains (clipID)) selected.removeString (clipID);
            else selected.add (clipID);
        }
        else if (! selected.contains (clipID))
        {
            selected.clearQuick();
            selected.add (clipID);
        }

        selection.setPattern (hit[ids::pattern].toString());
        selection.setLane (hit[ids::lane].toString());

        const auto area = clipArea (hit);
        dragMode = e.x > area.getRight() - 7 ? resize : move;
        dragAnchor = e.getPosition();
        if (e.mods.isCtrlDown() && dragMode == move)
            copySelection();
        captureStarts();
        notify();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (dragMode == loop)
        {
            setLoopRange (std::min (loopAnchor, beatAt (e.x)), std::max (loopAnchor, beatAt (e.x)));
            repaint();
            return;
        }

        if (dragMode == rubber)
        {
            rubberBand = Rectangle<int>::leftTopRightBottom (std::min (rubberStart.x, e.x), std::min (rubberStart.y, e.y),
                                                            std::max (rubberStart.x, e.x), std::max (rubberStart.y, e.y));
            selected.clearQuick();
            for (auto instance : model.instances())
                if (rubberBand.intersects (clipArea (instance)))
                    selected.add (Model::uidOf (instance));
            repaint();
            return;
        }

        if (dragMode == none || starts.isEmpty())
            return;

        const auto beatDelta = (e.x - dragAnchor.x) / beatWidth();
        const auto laneDelta = (e.y - dragAnchor.y) / laneHeight;

        undo().beginNewTransaction (dragMode == resize ? "Resize clips" : "Move clips");

        for (const auto& start : starts)
        {
            auto instance = model.instanceFor (start.id);
            if (! instance.isValid())
                continue;

            if (dragMode == move)
            {
                instance.setProperty (ids::start, std::max (0.0, snapped (start.start + beatDelta)), &undo());

                const auto laneIndex = jlimit (0, std::max (0, model.lanes().getNumChildren() - 1),
                                               start.lane + laneDelta);
                if (auto lane = model.lanes().getChild (laneIndex); lane.isValid())
                    instance.setProperty (ids::lane, Model::uidOf (lane), &undo());
            }
            else
            {
                instance.setProperty (ids::length, std::max (0.25, snapped (start.length + beatDelta)), &undo());
            }
        }

        model.renderIfNeeded();
        notify();
    }

    void mouseUp (const MouseEvent&) override
    {
        dragMode = none;
        starts.clearQuick();
        rubberBand = {};
        repaint();
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (e.y < rulerHeight)
            return;

        if (auto hit = clipAt (e.getPosition()); hit.isValid())
            split (hit, beatAt (e.x));
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& wheel) override
    {
        if (e.mods.isCtrlDown())
            setZoom (zoom * (1.0 + wheel.deltaY));
        else
            Component::mouseWheelMove (e, wheel);
    }

    bool keyPressed (const KeyPress& key) override
    {
        if (key == KeyPress::deleteKey || key == KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (key == KeyPress ('d', ModifierKeys::ctrlModifier, 0)) { duplicateSelection(); return true; }
        if (key == KeyPress ('u', ModifierKeys::ctrlModifier, 0)) { makeSelectionUnique(); return true; }
        return false;
    }

    //==========================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return model.patternFor (details.description.toString()).isValid();
    }

    void itemDragEnter (const SourceDetails& details) override { itemDragMove (details); }

    void itemDragMove (const SourceDetails& details) override
    {
        dropPreview = { snapped (beatAt (details.localPosition.x)), laneIndexAt (details.localPosition.y) };
        repaint();
    }

    void itemDragExit (const SourceDetails&) override { dropPreview = {}; repaint(); }

    void itemDropped (const SourceDetails& details) override
    {
        const auto patternID = details.description.toString();
        auto lane = model.lanes().getChild (laneIndexAt (details.localPosition.y));
        dropPreview = {};

        if (! lane.isValid() || ! model.patternFor (patternID).isValid())
            return;

        undo().beginNewTransaction ("Drop pattern");
        auto instance = model.addInstance (Model::uidOf (lane), patternID,
                                           std::max (0.0, snapped (beatAt (details.localPosition.x))), &undo());
        model.renderIfNeeded();
        selected.clearQuick();
        selected.add (Model::uidOf (instance));
        selection.setLane (Model::uidOf (lane));
        selection.setPattern (patternID);
        notify();
    }

    //==========================================================================
    // The same operations the toolbar buttons and the UI script use.
    bool deleteSelection()
    {
        if (selected.isEmpty())
            return false;

        undo().beginNewTransaction ("Delete clips");
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
                model.instances().removeChild (instance, &undo());
        selected.clear();
        model.renderIfNeeded();
        notify();
        return true;
    }

    bool duplicateSelection()
    {
        if (selected.isEmpty())
            return false;

        double span = 0.0, first = std::numeric_limits<double>::max();
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
            {
                const auto start = static_cast<double> (instance[ids::start]);
                first = std::min (first, start);
                span = std::max (span, start + static_cast<double> (instance[ids::length]));
            }

        const auto shift = std::max (0.25, span - first);
        StringArray copies;

        undo().beginNewTransaction ("Duplicate clips");
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
                copies.add (Model::uidOf (copyOf (instance, static_cast<double> (instance[ids::start]) + shift)));

        selected = copies;
        model.renderIfNeeded();
        notify();
        return true;
    }

    bool makeSelectionUnique()
    {
        if (selected.isEmpty())
            return false;

        undo().beginNewTransaction ("Make clips unique");
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
                selection.setPattern (Model::uidOf (model.makeUnique (instance, &undo())));

        model.renderIfNeeded();
        notify();
        return true;
    }

    /** Cuts a clip in two at a beat, the second half continuing further into the
        pattern so the music runs on unchanged. */
    bool split (ValueTree instance, double beat)
    {
        if (! instance.isValid())
            return false;

        const auto start = static_cast<double> (instance[ids::start]);
        const auto length = static_cast<double> (instance[ids::length]);
        const auto at = snapped (beat);

        if (at <= start + 1.0e-6 || at >= start + length - 1.0e-6)
            return false;

        undo().beginNewTransaction ("Split clip");
        auto second = copyOf (instance, at);
        second.setProperty (ids::length, start + length - at, &undo());
        second.setProperty (ids::offset, static_cast<double> (instance.getProperty (ids::offset, 0.0)) + (at - start),
                            &undo());
        instance.setProperty (ids::length, at - start, &undo());

        selected.clearQuick();
        selected.add (Model::uidOf (second));
        model.renderIfNeeded();
        notify();
        return true;
    }

    bool splitSelectionAt (double beat)
    {
        bool any = false;
        for (const auto& clipID : StringArray (selected))
            any = split (model.instanceFor (clipID), beat) || any;
        return any;
    }

    /** Adds the selected pattern at a point, the way the pencil does. */
    bool place (Point<int> position)
    {
        auto lane = model.lanes().getChild (laneIndexAt (position.y));
        auto pattern = model.patternFor (selection.pattern());
        if (! lane.isValid() || ! pattern.isValid())
            return false;

        undo().beginNewTransaction ("Place pattern");
        auto instance = model.addInstance (Model::uidOf (lane), selection.pattern(),
                                           std::max (0.0, snapped (beatAt (position.x))), &undo());
        model.renderIfNeeded();
        selected.clearQuick();
        selected.add (Model::uidOf (instance));
        selection.setLane (Model::uidOf (lane));
        notify();
        return true;
    }

    /** Entry points for the diagnostic UI script, in grid coordinates. */
    bool placeAt (int laneIndex, double beat)
    {
        return place ({ xForBeat (beat) + 2, rulerHeight + laneIndex * laneHeight + laneHeight / 2 });
    }

    bool selectClipAt (int laneIndex, double beat)
    {
        auto hit = clipAt ({ xForBeat (beat) + 2, rulerHeight + laneIndex * laneHeight + laneHeight / 2 });
        if (! hit.isValid())
            return false;

        selected.clearQuick();
        selected.add (Model::uidOf (hit));
        selection.setPattern (hit[ids::pattern].toString());
        selection.setLane (hit[ids::lane].toString());
        notify();
        return true;
    }

    bool moveSelection (double beatDelta, int laneDelta)
    {
        if (selected.isEmpty())
            return false;

        undo().beginNewTransaction ("Move clips");
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
            {
                instance.setProperty (ids::start,
                                      std::max (0.0, static_cast<double> (instance[ids::start]) + beatDelta), &undo());
                const auto laneIndex = jlimit (0, std::max (0, model.lanes().getNumChildren() - 1),
                                               laneIndexOf (instance) + laneDelta);
                if (auto lane = model.lanes().getChild (laneIndex); lane.isValid())
                    instance.setProperty (ids::lane, Model::uidOf (lane), &undo());
            }

        model.renderIfNeeded();
        notify();
        return true;
    }

    StringArray selectedClips() const { return selected; }

private:
    struct Start { String id; double start, length; int lane; };
    enum DragMode { none, move, resize, rubber, loop };

    UndoManager& undo() const { return model.edit.getUndoManager(); }
    void notify() { if (changed != nullptr) changed(); repaint(); }

    double snapped (double beat) const { return snap <= 0.0 ? beat : std::round (beat / snap) * snap; }
    double beatAt (int x) const { return std::max (0.0, (x - laneWidth) / beatWidth()); }
    int xForBeat (double beat) const { return laneWidth + roundToInt (beat * beatWidth()); }

    int laneIndexAt (int y) const
    {
        return jlimit (0, std::max (0, model.lanes().getNumChildren() - 1), (y - rulerHeight) / laneHeight);
    }

    int laneIndexOf (ValueTree instance) const
    {
        auto lanes = model.lanes();
        for (int i = 0; i < lanes.getNumChildren(); ++i)
            if (Model::uidOf (lanes.getChild (i)) == instance[ids::lane].toString())
                return i;
        return 0;
    }

    Rectangle<int> clipArea (ValueTree instance) const
    {
        const auto x = xForBeat (static_cast<double> (instance[ids::start]));
        const auto w = std::max (6, roundToInt (static_cast<double> (instance[ids::length]) * beatWidth()));
        return { x, rulerHeight + laneIndexOf (instance) * laneHeight + 2, w, laneHeight - 4 };
    }

    ValueTree clipAt (Point<int> position) const
    {
        for (auto instance : model.instances())
            if (clipArea (instance).contains (position))
                return instance;
        return {};
    }

    ValueTree copyOf (ValueTree instance, double startBeat)
    {
        auto copy = instance.createCopy();
        copy.setProperty (ids::uid, Uuid().toString(), nullptr);
        copy.setProperty (ids::start, std::max (0.0, startBeat), nullptr);
        model.instances().appendChild (copy, &undo());
        return copy;
    }

    void copySelection()
    {
        StringArray copies;
        undo().beginNewTransaction ("Copy clips");
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
                copies.add (Model::uidOf (copyOf (instance, static_cast<double> (instance[ids::start]))));
        selected = copies;
    }

    void captureStarts()
    {
        starts.clearQuick();
        for (const auto& clipID : selected)
            if (auto instance = model.instanceFor (clipID); instance.isValid())
                starts.add ({ clipID, static_cast<double> (instance[ids::start]),
                              static_cast<double> (instance[ids::length]), laneIndexOf (instance) });
    }

    void setLoopRange (double fromBeat, double toBeat)
    {
        auto& tempo = model.edit.tempoSequence;
        const auto from = tempo.toTime (te::BeatPosition::fromBeats (snapped (fromBeat)));
        const auto to = tempo.toTime (te::BeatPosition::fromBeats (std::max (snapped (toBeat), snapped (fromBeat) + 1.0)));
        model.edit.getTransport().setLoopRange ({ from, to });
        model.edit.getTransport().looping = true;
    }

    //==========================================================================
    void paintGrid (Graphics& g, double beats)
    {
        for (double beat = 0.0; beat <= beats; beat += 1.0)
        {
            const auto bar = std::abs (std::fmod (beat, 4.0)) < 1.0e-6;
            if (! bar && beatWidth() < 8.0)
                continue;

            g.setColour (bar ? Colour (0xff333e52) : Colour (0xff222a38));
            g.fillRect (xForBeat (beat), rulerHeight, 1, getHeight() - rulerHeight);
        }
    }

    void paintLanes (Graphics& g)
    {
        auto lanes = model.lanes();
        for (int i = 0; i < lanes.getNumChildren(); ++i)
        {
            auto lane = lanes.getChild (i);
            const auto y = rulerHeight + i * laneHeight;
            const auto picked = Model::uidOf (lane) == selection.lane();

            g.setColour (picked ? Colour (0xff2b3f4d) : Colour (0xff1b2330));
            g.fillRect (0, y, laneWidth - 2, laneHeight - 1);

            g.setColour (static_cast<bool> (lane[ids::mute]) ? Colour (0xff6c7689) : Colours::white.withAlpha (0.85f));
            g.setFont (Font (FontOptions (12.0f)));
            g.drawText (lane[ids::name].toString(), 6, y, laneWidth - 14, laneHeight - 1, Justification::centredLeft);

            g.setColour (Colour (0xff222a38));
            g.fillRect (0, y + laneHeight - 1, getWidth(), 1);
        }
    }

    void paintClips (Graphics& g)
    {
        for (auto instance : model.instances())
        {
            auto pattern = model.patternFor (instance[ids::pattern].toString());
            const auto area = clipArea (instance);
            const auto picked = selected.contains (Model::uidOf (instance));
            const auto colour = patternColour (instance[ids::pattern].toString());

            g.setColour (picked ? colour.brighter (0.5f) : colour);
            g.fillRoundedRectangle (area.toFloat(), 3.0f);
            g.setColour (picked ? Colours::white : Colour (0xff0e131b));
            g.drawRoundedRectangle (area.toFloat(), 3.0f, picked ? 1.6f : 1.0f);

            // Where the pattern starts over inside a clip that repeats it.
            const auto patternBeats = std::max (0.25, static_cast<double> (pattern[ids::length]));
            const auto offsetBeats = static_cast<double> (instance.getProperty (ids::offset, 0.0));
            const auto lengthBeats = static_cast<double> (instance[ids::length]);
            g.setColour (Colour (0x50000000));
            for (double repeat = patternBeats - std::fmod (offsetBeats, patternBeats);
                 repeat < lengthBeats - 1.0e-6; repeat += patternBeats)
                g.fillRect (area.getX() + roundToInt (repeat * beatWidth()), area.getY(), 1, area.getHeight());

            if (area.getWidth() > 34)
            {
                g.setColour (Colours::black.withAlpha (0.7f));
                g.setFont (Font (FontOptions (11.0f)));
                g.drawText (pattern[ids::name].toString(), area.reduced (5, 0), Justification::centredLeft, false);
            }
        }

        if (dropPreview.lane >= 0)
        {
            const auto x = xForBeat (dropPreview.beat);
            g.setColour (Colour (0x80ffd479));
            g.fillRect (x, rulerHeight + dropPreview.lane * laneHeight + 2, 4, laneHeight - 4);
        }
    }

    void paintRuler (Graphics& g, double beats)
    {
        g.setColour (Colour (0xff10161f));
        g.fillRect (0, 0, getWidth(), rulerHeight);
        g.setColour (Colour (0xff2a3242));
        g.fillRect (0, rulerHeight - 1, getWidth(), 1);

        g.setFont (Font (FontOptions (10.0f)));
        for (double beat = 0.0; beat <= beats; beat += 4.0)
        {
            const auto x = xForBeat (beat);
            g.setColour (Colour (0xff3d4a60));
            g.fillRect (x, 6, 1, rulerHeight - 7);

            if (beatWidth() * 4.0 >= 26.0)
            {
                g.setColour (Colour (0xff8698b6));
                g.drawText (String (roundToInt (beat / 4.0) + 1), x + 3, 2, 40, rulerHeight - 4,
                            Justification::centredLeft, false);
            }
        }
    }

    void paintLoopRange (Graphics& g, double)
    {
        const auto range = model.edit.getTransport().getLoopRange();
        auto& tempo = model.edit.tempoSequence;
        const auto from = xForBeat (tempo.toBeats (range.getStart()).inBeats());
        const auto to = xForBeat (tempo.toBeats (range.getEnd()).inBeats());

        if (to > from)
        {
            g.setColour (Colour (0x18ffd479));
            g.fillRect (from, rulerHeight, to - from, getHeight() - rulerHeight);
            g.setColour (Colour (0x60ffd479));
            g.fillRect (from, 0, to - from, rulerHeight);
        }
    }

    void paintPlayhead (Graphics& g)
    {
        const auto beat = model.edit.tempoSequence.toBeats (model.edit.getTransport().getPosition()).inBeats();
        g.setColour (Colour (0xffffe17d));
        g.fillRect (xForBeat (beat), 0, 2, getHeight());
    }

    Colour patternColour (const String& patternID) const
    {
        auto patterns = model.patterns();
        for (int i = 0; i < patterns.getNumChildren(); ++i)
            if (Model::uidOf (patterns.getChild (i)) == patternID)
                return Colour::fromHSV (std::fmod (0.36f + i * 0.17f, 1.0f), 0.45f, 0.72f, 1.0f);
        return Colour (0xff6fd39a);
    }

    void timerCallback() override
    {
        // Only the playhead moves between edits, so repaint the column it is in.
        const auto beat = model.edit.tempoSequence.toBeats (model.edit.getTransport().getPosition()).inBeats();
        const auto x = xForBeat (beat);
        if (x != lastPlayheadX)
        {
            repaint (std::min (x, lastPlayheadX) - 2, 0, std::abs (x - lastPlayheadX) + 6, getHeight());
            lastPlayheadX = x;
        }
    }

    struct DropPreview { double beat = 0.0; int lane = -1; };

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    StringArray selected;
    Array<Start> starts;
    DragMode dragMode = none;
    Point<int> dragAnchor, rubberStart;
    Rectangle<int> rubberBand;
    DropPreview dropPreview;
    double zoom = 9.0, snap = 1.0, loopAnchor = 0.0;
    int lastPlayheadX = 0;
};
}
