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
                           public FileDragAndDropTarget,
                           private Timer
{
public:
    PlaylistGrid (Model& m, Selection& s, std::function<void()> onChange)
        : model (m), selection (s), changed (std::move (onChange)),
          thumbnailCache (64)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (20);
    }

    ~PlaylistGrid() override { stopTimer(); }

    static constexpr int laneWidth = 96, rulerHeight = 22, laneHeight = 30, curveHeight = 46;

    double beatWidth() const { return zoom; }
    void setZoom (double pixelsPerBeat) { zoom = jlimit (1.5, 40.0, pixelsPerBeat); resized(); repaint(); }
    double getZoom() const { return zoom; }

    void setSnap (double beats) { snap = beats; }
    double getSnap() const { return snap; }

    /** Wide enough for the arrangement plus a bar of room to extend it. */
    int preferredWidth() const { return laneWidth + roundToInt ((arrangementBeats() + 8.0) * beatWidth()) + 20; }
    int preferredHeight() const
    {
        return clipAreaBottom() + model.automation().getNumChildren() * curveHeight + 8;
    }

    int clipAreaBottom() const
    {
        return rulerHeight + std::max (1, model.lanes().getNumChildren()) * laneHeight;
    }

    /** Which automation curve a point on the surface belongs to, or -1 for the clips. */
    int curveIndexAt (int y) const
    {
        if (y < clipAreaBottom())
            return -1;

        const auto index = (y - clipAreaBottom()) / curveHeight;
        return index < model.automation().getNumChildren() ? index : -1;
    }

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
        g.fillAll (theme::panel);

        const auto beats = std::max (arrangementBeats() + 8.0, (getWidth() - laneWidth) / beatWidth());

        paintLoopRange (g, beats);
        paintGrid (g, beats);
        paintLanes (g);
        paintClips (g);
        paintCurves (g);
        paintRuler (g, beats);
        paintPlayhead (g);

        if (! rubberBand.isEmpty())
        {
            g.setColour (Colour (0x40ffd479));
            g.fillRect (rubberBand);
            g.setColour (theme::warn);
            g.drawRect (rubberBand, 1);
        }
    }

    void resized() override {}

    //==========================================================================
    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (const auto curve = curveIndexAt (e.y); curve >= 0)
        {
            editCurve (curve, e.getPosition(), e.mods.isRightButtonDown(),
                       e.mods.isCtrlDown() || e.mods.isAltDown());
            return;
        }

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

        if (! isAudio (hit))
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
        if (dragMode == curveShape)
        {
            bendSegment (e.getPosition());
            return;
        }

        if (dragMode == curvePoint)
        {
            movePoint (e.getPosition());
            return;
        }

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
            auto instance = model.placementFor (start.id);
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
        if (key == KeyPress::deleteKey || key == KeyPress::backspaceKey)
        {
            if (! deleteSelectedPoint())
                deleteSelection();
            return true;
        }
        if (key == KeyPress ('d', ModifierKeys::ctrlModifier, 0)) { duplicateSelection(); return true; }
        if (key == KeyPress ('u', ModifierKeys::ctrlModifier, 0)) { makeSelectionUnique(); return true; }
        return false;
    }

    //==========================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        const auto description = details.description.toString();
        return description.startsWith ("sample:")
                ? isAudioFile (File (description.fromFirstOccurrenceOf ("sample:", false, false)))
                : model.patternFor (description).isValid();
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
        const auto description = details.description.toString();
        auto lane = model.lanes().getChild (laneIndexAt (details.localPosition.y));
        dropPreview = {};

        if (description.startsWith ("sample:"))
        {
            addAudio (laneIndexAt (details.localPosition.y),
                      description.fromFirstOccurrenceOf ("sample:", false, false),
                      std::max (0.0, snapped (beatAt (details.localPosition.x))));
            return;
        }

        const auto patternID = description;
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
    bool isInterestedInFileDrag (const StringArray& files) override
    {
        for (const auto& path : files)
            if (isAudioFile (File (path)))
                return true;
        return false;
    }

    void fileDragMove (const StringArray&, int x, int y) override
    {
        dropPreview = { snapped (beatAt (x)), laneIndexAt (y) };
        repaint();
    }

    void fileDragExit (const StringArray&) override { dropPreview = {}; repaint(); }

    void filesDropped (const StringArray& files, int x, int y) override
    {
        dropPreview = {};
        auto lane = model.lanes().getChild (laneIndexAt (y));
        if (! lane.isValid())
            return;

        auto beat = std::max (0.0, snapped (beatAt (x)));
        undo().beginNewTransaction ("Drop audio");
        selected.clearQuick();

        for (const auto& path : files)
        {
            const File source (path);
            if (! isAudioFile (source))
                continue;

            const auto length = model.fileLengthInBeats (source);
            auto clip = model.addAudioClip (Model::uidOf (lane), source, beat, length, &undo());
            selected.add (Model::uidOf (clip));
            beat += length;
        }

        model.renderIfNeeded();
        selection.setLane (Model::uidOf (lane));
        notify();
    }

    bool isAudioFile (const File& source) const
    {
        return source.existsAsFile()
                && model.edit.engine.getAudioFileFormatManager().readFormatManager
                        .findFormatForFileExtension (source.getFileExtension()) != nullptr;
    }

    /** Places an audio file the way a drop does, for the diagnostic UI script. */
    bool addAudio (int laneIndex, const String& path, double beat)
    {
        auto lane = model.lanes().getChild (laneIndex);
        const File source (path);
        if (! lane.isValid() || ! isAudioFile (source))
            return false;

        undo().beginNewTransaction ("Add audio");
        auto clip = model.addAudioClip (Model::uidOf (lane), source, std::max (0.0, beat),
                                        model.fileLengthInBeats (source), &undo());
        model.renderIfNeeded();
        selected.clearQuick();
        selected.add (Model::uidOf (clip));
        selection.setLane (Model::uidOf (lane));
        notify();
        return true;
    }

    /** Trim, gain, fades and speed for the selected audio clips. */
    bool shapeSelection (const Identifier& property, double value)
    {
        bool any = false;
        undo().beginNewTransaction ("Shape audio clip");

        for (const auto& clipID : selected)
            if (auto clip = model.audioClipFor (clipID); clip.isValid())
            {
                clip.setProperty (property, value, &undo());
                any = true;
            }

        if (any)
        {
            model.renderIfNeeded();
            notify();
        }
        return any;
    }

    //==========================================================================
    // The same operations the toolbar buttons and the UI script use.
    bool deleteSelection()
    {
        if (selected.isEmpty())
            return false;

        undo().beginNewTransaction ("Delete clips");
        for (const auto& clipID : selected)
            if (auto instance = model.placementFor (clipID); instance.isValid())
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
            if (auto instance = model.placementFor (clipID); instance.isValid())
            {
                const auto start = static_cast<double> (instance[ids::start]);
                first = std::min (first, start);
                span = std::max (span, start + static_cast<double> (instance[ids::length]));
            }

        const auto shift = std::max (0.25, span - first);
        StringArray copies;

        undo().beginNewTransaction ("Duplicate clips");
        for (const auto& clipID : selected)
            if (auto instance = model.placementFor (clipID); instance.isValid())
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
            if (auto instance = model.placementFor (clipID); instance.isValid() && ! isAudio (instance))
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
            any = split (model.placementFor (clipID), beat) || any;
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

    //==========================================================================
    /** Automation points are edited where they play: click an empty spot to add one,
        drag it to move it in time and value, right-click to take it away. Holding a
        modifier and dragging the line between two points bends that segment instead,
        which is the shape the engine plays through. */
    void editCurve (int index, Point<int> position, bool remove, bool bend = false)
    {
        auto curve = model.automation().getChild (index);
        if (! curve.isValid())
            return;

        selectedCurve = index;
        const auto area = curveArea (index);
        auto hit = pointAt (curve, area, position);

        if (bend && ! hit.isValid())
        {
            auto segment = segmentAt (curve, position);
            if (segment.isValid())
            {
                selectedPoint = Model::uidOf (segment);
                bendAnchor = position;
                bendStart = static_cast<double> (segment.getProperty (ids::curve, 0.0));
                dragMode = curveShape;
                notify();
                return;
            }
        }

        if (remove)
        {
            if (hit.isValid())
            {
                undo().beginNewTransaction ("Delete automation point");
                curve.removeChild (hit, &undo());
                selectedPoint.clear();
                model.renderIfNeeded();
                notify();
            }
            return;
        }

        if (! hit.isValid())
        {
            undo().beginNewTransaction ("Add automation point");
            hit = model.addAutomationPoint (curve, std::max (0.0, snapped (beatAt (position.x))),
                                            valueAt (area, position.y), 0.0, &undo());
            model.renderIfNeeded();
        }

        selectedPoint = Model::uidOf (hit);
        dragMode = curvePoint;
        notify();
    }

    void movePoint (Point<int> position)
    {
        auto curve = model.automation().getChild (selectedCurve);
        if (! curve.isValid())
            return;

        auto point = Model::withID (curve, ids::POINT, selectedPoint);
        if (! point.isValid())
            return;

        const auto area = curveArea (selectedCurve);
        undo().beginNewTransaction ("Move automation point");
        point.setProperty (ids::time, std::max (0.0, snapped (beatAt (position.x))), &undo());
        point.setProperty (ids::value, valueAt (area, position.y), &undo());
        model.renderIfNeeded();
        notify();
    }

    /** The point that starts the segment under this position. A segment's shape lives
        on the point at its left-hand end, which is where the engine keeps it too. */
    ValueTree segmentAt (ValueTree curve, Point<int> position) const
    {
        const auto beat = beatAt (position.x);
        ValueTree before, after;

        for (auto point : curve)
        {
            if (! point.hasType (ids::POINT))
                continue;

            const auto time = static_cast<double> (point[ids::time]);
            if (time <= beat && (! before.isValid() || time >= static_cast<double> (before[ids::time])))
                before = point;
            if (time > beat && (! after.isValid() || time < static_cast<double> (after[ids::time])))
                after = point;
        }

        return after.isValid() ? before : ValueTree();
    }

    /** Dragging up bends the segment one way and down the other, over the height of the
        row. The engine reads -1 to 1, and 0 is the straight line. */
    void bendSegment (Point<int> position)
    {
        auto curve = model.automation().getChild (selectedCurve);
        auto point = Model::withID (curve, ids::POINT, selectedPoint);
        if (! point.isValid())
            return;

        const auto travel = (bendAnchor.y - position.y) / static_cast<double> (std::max (8, curveHeight / 2));
        undo().beginNewTransaction ("Bend automation curve");
        point.setProperty (ids::curve, jlimit (-1.0, 1.0, bendStart + travel), &undo());
        model.renderIfNeeded();
        notify();
    }

    ValueTree pointAt (ValueTree curve, Rectangle<int> area, Point<int> position) const
    {
        for (auto point : curve)
        {
            if (! point.hasType (ids::POINT))
                continue;

            if (pointPosition (area, point).getDistanceFrom (position.toFloat()) < 7.0f)
                return point;
        }
        return {};
    }

    bool deleteSelectedPoint()
    {
        auto curve = model.automation().getChild (selectedCurve);
        auto point = curve.isValid() ? Model::withID (curve, ids::POINT, selectedPoint) : ValueTree();
        if (! point.isValid())
            return false;

        undo().beginNewTransaction ("Delete automation point");
        curve.removeChild (point, &undo());
        selectedPoint.clear();
        model.renderIfNeeded();
        notify();
        return true;
    }

public:
    /** Adds a curve for a parameter and shows it, which is how the Automate button and
        the diagnostic script both start one. */
    bool automate (const String& ownerID, const String& pluginID, const String& parameterID)
    {
        if (model.automatableParameter (ownerID, pluginID, parameterID) == nullptr)
            return false;

        undo().beginNewTransaction ("Automate parameter");
        auto curve = model.curveFor (ownerID, pluginID, parameterID, &undo());
        model.renderIfNeeded();
        selectedCurve = model.automation().indexOf (curve);
        notify();
        return true;
    }

    /** Drives the curve editing the way a click and a drag do. */
    bool clickCurve (int index, double beat, double value)
    {
        auto curve = model.automation().getChild (index);
        if (! curve.isValid())
            return false;

        editCurve (index, positionOf (index, beat, value), false);
        dragMode = none;
        return true;
    }

    /** Where a beat and a normalised value sit inside a curve row. */
    Point<int> positionOf (int index, double beat, double value) const
    {
        const auto area = curveArea (index);
        return { xForBeat (beat),
                 roundToInt (area.getY() + 5 + (1.0 - jlimit (0.0, 1.0, value)) * (area.getHeight() - 12)) };
    }

    /** Drags the selected point somewhere else, the way the mouse does. */
    bool dragCurvePoint (int index, double beat, double value)
    {
        if (index != selectedCurve || selectedPoint.isEmpty())
            return false;

        movePoint (positionOf (index, beat, value));
        return true;
    }

    /** Bends the segment that covers this beat, the way holding a modifier and dragging
        the line does. `amount` is the distance dragged as a share of the row height. */
    bool bendCurve (int index, double beat, double amount)
    {
        auto curve = model.automation().getChild (index);
        if (! curve.isValid())
            return false;

        auto segment = segmentAt (curve, positionOf (index, beat, 0.5));
        if (! segment.isValid())
            return false;

        selectedCurve = index;
        selectedPoint = Model::uidOf (segment);
        bendAnchor = positionOf (index, beat, 0.5);
        bendStart = static_cast<double> (segment.getProperty (ids::curve, 0.0));
        bendSegment (bendAnchor.translated (0, -roundToInt (amount * std::max (8, curveHeight / 2))));
        dragMode = none;
        return true;
    }

    /** What the engine will play at this beat, read back through the same path the
        drawing uses. A check can compare it with what the row shows. */
    double curveValueAt (int index, double beat) const
    {
        auto curve = model.automation().getChild (index);
        auto* parameter = curve.isValid() ? parameterFor (curve) : nullptr;
        if (parameter == nullptr)
            return -1.0;

        const auto time = model.edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat));
        return parameter->valueRange.convertTo0to1 (
                   parameter->getCurve().getValueAt (time, parameter->getCurrentBaseValue()));
    }

    bool removeCurvePoint (int index, double beat)
    {
        auto curve = model.automation().getChild (index);
        if (! curve.isValid())
            return false;

        const auto area = curveArea (index);
        auto point = pointAt (curve, area, { xForBeat (beat), area.getCentreY() });

        // A click only lands on a point when the value matches too, so search by time.
        if (! point.isValid())
            for (auto candidate : curve)
                if (candidate.hasType (ids::POINT)
                     && std::abs (static_cast<double> (candidate[ids::time]) - beat) < 1.0e-6)
                    point = candidate;

        if (! point.isValid())
            return false;

        selectedCurve = index;
        selectedPoint = Model::uidOf (point);
        return deleteSelectedPoint();
    }

    bool removeCurve (int index)
    {
        auto curve = model.automation().getChild (index);
        if (! curve.isValid())
            return false;

        undo().beginNewTransaction ("Remove automation");
        model.automation().removeChild (curve, &undo());
        selectedCurve = -1;
        selectedPoint.clear();
        model.renderIfNeeded();
        notify();
        return true;
    }

    int selectedCurveIndex() const { return selectedCurve; }

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
        if (! isAudio (hit))
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
            if (auto instance = model.placementFor (clipID); instance.isValid())
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
    enum DragMode { none, move, resize, rubber, loop, curvePoint, curveShape };

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

    static bool isAudio (ValueTree clip) { return clip.hasType (ids::AUDIO); }

    /** Copies a placement of either kind, keeping its type. */
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
            if (auto instance = model.placementFor (clipID); instance.isValid())
                copies.add (Model::uidOf (copyOf (instance, static_cast<double> (instance[ids::start]))));
        selected = copies;
    }

    void captureStarts()
    {
        starts.clearQuick();
        for (const auto& clipID : selected)
            if (auto instance = model.placementFor (clipID); instance.isValid())
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

            g.setColour (bar ? theme::edgeStrong : theme::rowAlt);
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

            g.setColour (picked ? theme::selection : theme::panelHeader);
            g.fillRect (0, y, laneWidth - 2, laneHeight - 1);

            g.setColour (static_cast<bool> (lane[ids::mute]) ? theme::textFaint : Colours::white.withAlpha (0.85f));
            g.setFont (Font (FontOptions (12.0f)));
            g.drawText (lane[ids::name].toString(), 6, y, laneWidth - 14, laneHeight - 1, Justification::centredLeft);

            g.setColour (theme::rowAlt);
            g.fillRect (0, y + laneHeight - 1, getWidth(), 1);
        }
    }

    void paintClips (Graphics& g)
    {
        for (auto instance : model.instances())
        {
            if (isAudio (instance))
            {
                paintAudioClip (g, instance);
                continue;
            }

            auto pattern = model.patternFor (instance[ids::pattern].toString());
            const auto area = clipArea (instance);
            const auto picked = selected.contains (Model::uidOf (instance));
            const auto colour = patternColour (instance[ids::pattern].toString());

            g.setColour (picked ? colour.brighter (0.5f) : colour);
            g.fillRoundedRectangle (area.toFloat(), 3.0f);
            g.setColour (picked ? Colours::white : theme::sunken);
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

    /** An audio clip shows its waveform, its fades and whether the file is still there. */
    void paintAudioClip (Graphics& g, ValueTree clip)
    {
        const auto area = clipArea (clip);
        const auto picked = selected.contains (Model::uidOf (clip));
        const File source (clip[ids::file].toString());
        const auto missing = ! source.existsAsFile();

        g.setColour (missing ? Colour (0xff7a4a4a) : theme::clipFill);
        g.fillRoundedRectangle (area.toFloat(), 3.0f);

        if (! missing)
        {
            auto* thumbnail = thumbnailFor (source);
            if (thumbnail != nullptr && thumbnail->getTotalLength() > 0.0)
            {
                const auto offset = static_cast<double> (clip.getProperty (ids::offset, 0.0));
                auto& tempo = model.edit.tempoSequence;
                const auto from = tempo.toTime (te::BeatPosition::fromBeats (offset)).inSeconds();
                const auto span = tempo.toTime (te::BeatPosition::fromBeats (
                                      offset + static_cast<double> (clip[ids::length]))).inSeconds() - from;

                g.setColour (Colours::white.withAlpha (0.5f));
                thumbnail->drawChannels (g, area.reduced (2, 3), from, from + std::max (0.01, span), 1.0f);
            }
        }

        // Fades are drawn as the wedges they apply to the sound.
        auto& tempo = model.edit.tempoSequence;
        const auto secondsPerBeat = tempo.toTime (te::BeatPosition::fromBeats (1.0)).inSeconds();
        const auto fadeIn = static_cast<double> (clip.getProperty (ids::fadeIn, 0.0)) / std::max (0.001, secondsPerBeat);
        const auto fadeOut = static_cast<double> (clip.getProperty (ids::fadeOut, 0.0)) / std::max (0.001, secondsPerBeat);

        g.setColour (Colours::black.withAlpha (0.45f));
        if (fadeIn > 0.0)
        {
            Path wedge;
            wedge.addTriangle ((float) area.getX(), (float) area.getY(),
                               (float) area.getX() + (float) (fadeIn * beatWidth()), (float) area.getY(),
                               (float) area.getX(), (float) area.getBottom());
            g.fillPath (wedge);
        }
        if (fadeOut > 0.0)
        {
            Path wedge;
            wedge.addTriangle ((float) area.getRight(), (float) area.getY(),
                               (float) area.getRight() - (float) (fadeOut * beatWidth()), (float) area.getY(),
                               (float) area.getRight(), (float) area.getBottom());
            g.fillPath (wedge);
        }

        g.setColour (picked ? Colours::white : theme::sunken);
        g.drawRoundedRectangle (area.toFloat(), 3.0f, picked ? 1.6f : 1.0f);

        if (area.getWidth() > 34)
        {
            g.setColour (missing ? Colours::white : Colours::black.withAlpha (0.75f));
            g.setFont (Font (FontOptions (11.0f)));
            g.drawText (missing ? "missing: " + source.getFileName() : clip[ids::name].toString(),
                        area.reduced (5, 0), Justification::centredLeft, false);
        }
    }

    AudioThumbnail* thumbnailFor (const File& source)
    {
        const auto key = source.getFullPathName();
        if (auto* existing = thumbnails[key].get())
            return existing;

        auto thumbnail = std::make_unique<AudioThumbnail> (
            512, model.edit.engine.getAudioFileFormatManager().readFormatManager, thumbnailCache);
        thumbnail->setSource (new FileInputSource (source));
        auto* result = thumbnail.get();
        thumbnails[key] = std::move (thumbnail);
        return result;
    }

    /** One row per automation curve, drawn in song time under the clips, with its
        points editable in place. */
    void paintCurves (Graphics& g)
    {
        auto curves = model.automation();

        for (int index = 0; index < curves.getNumChildren(); ++index)
        {
            auto curve = curves.getChild (index);
            const auto area = curveArea (index);

            g.setColour (index == selectedCurve ? theme::selection : theme::panelHeader);
            g.fillRect (area);
            g.setColour (theme::rowAlt);
            g.fillRect (area.getX(), area.getBottom() - 1, getWidth(), 1);

            g.setColour (index == selectedCurve ? theme::selection : theme::panelHeader);
            g.fillRect (0, area.getY(), laneWidth - 2, area.getHeight() - 1);
            // The parameter first: it is what tells two curves on one channel apart.
            g.setColour (Colours::white.withAlpha (0.9f));
            g.setFont (Font (FontOptions (11.0f, Font::bold)));
            g.drawText (curve[ids::parameter].toString(), 6, area.getY() + 6, laneWidth - 12, 13,
                        Justification::centredLeft);
            g.setColour (theme::textDim);
            g.setFont (Font (FontOptions (10.0f)));
            g.drawText (curveOwnerName (curve), 6, area.getY() + 20, laneWidth - 12, 12,
                        Justification::centredLeft);

            // The line is read out of the engine rather than worked out again here, so
            // a bent segment is drawn exactly as it will be played. Where the plugin is
            // missing there is nothing to ask, and the points are joined straight.
            Path line;
            if (auto* parameter = parameterFor (curve))
                buildLineFromEngine (line, area, *parameter);
            else
                buildLineFromPoints (line, area, curve);

            if (! line.isEmpty())
            {
                g.setColour (theme::warn.withAlpha (0.85f));
                g.strokePath (line, PathStrokeType (1.4f));
            }

            for (auto point : curve)
            {
                if (! point.hasType (ids::POINT))
                    continue;

                const auto at = pointPosition (area, point);
                const auto picked = selectedCurve == index
                                     && selectedPoint == Model::uidOf (point);
                g.setColour (picked ? Colours::white : theme::warn);
                g.fillEllipse (at.x - 3.5f, at.y - 3.5f, 7.0f, 7.0f);
            }
        }
    }

    te::AutomatableParameter* parameterFor (ValueTree curve) const
    {
        return model.automatableParameter (curve[ids::source].toString(),
                                           curve[ids::plugin].toString(),
                                           curve[ids::parameter].toString());
    }

    /** Samples what the engine will actually play, one reading per pixel across the
        visible beats. Straight segments come out straight and bent ones come out bent,
        because the shape is the engine's answer rather than a second opinion. */
    void buildLineFromEngine (Path& line, Rectangle<int> area, te::AutomatableParameter& parameter) const
    {
        const auto& engineCurve = parameter.getCurve();
        if (engineCurve.getNumPoints() == 0)
            return;

        const auto range = parameter.valueRange;
        const auto fallback = parameter.getCurrentBaseValue();
        const auto left = std::max (laneWidth, 0);

        for (int x = left; x < getWidth(); ++x)
        {
            const auto beat = beatAt (x);
            const auto time = model.edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat));
            const auto value = jlimit (0.0f, 1.0f,
                                       range.convertTo0to1 (engineCurve.getValueAt (time, fallback)));
            const Point<float> at (static_cast<float> (x),
                                   static_cast<float> (area.getY() + 5 + (1.0 - value) * (area.getHeight() - 12)));

            if (x == left) line.startNewSubPath (at);
            else           line.lineTo (at);
        }
    }

    void buildLineFromPoints (Path& line, Rectangle<int> area, ValueTree curve) const
    {
        bool started = false;
        for (auto point : curve)
        {
            if (! point.hasType (ids::POINT))
                continue;

            const auto at = pointPosition (area, point);
            if (! started) { line.startNewSubPath (at); started = true; }
            else           line.lineTo (at);
        }
    }

    Rectangle<int> curveArea (int index) const
    {
        return { 0, clipAreaBottom() + index * curveHeight, getWidth(), curveHeight };
    }

    Point<float> pointPosition (Rectangle<int> area, ValueTree point) const
    {
        const auto value = jlimit (0.0, 1.0, static_cast<double> (point[ids::value]));
        return { static_cast<float> (xForBeat (static_cast<double> (point[ids::time]))),
                 static_cast<float> (area.getY() + 5 + (1.0 - value) * (area.getHeight() - 12)) };
    }

    double valueAt (Rectangle<int> area, int y) const
    {
        return jlimit (0.0, 1.0, 1.0 - (y - area.getY() - 5.0) / std::max (1, area.getHeight() - 12));
    }

    String curveOwnerName (ValueTree curve) const
    {
        auto owner = model.channelFor (curve[ids::source].toString());
        if (! owner.isValid())
            owner = model.insertFor (curve[ids::source].toString());

        return owner.isValid() ? owner[ids::name].toString() : String ("?");
    }

    void paintRuler (Graphics& g, double beats)
    {
        g.setColour (theme::sunken);
        g.fillRect (0, 0, getWidth(), rulerHeight);
        g.setColour (theme::edge);
        g.fillRect (0, rulerHeight - 1, getWidth(), 1);

        g.setFont (Font (FontOptions (10.0f)));
        for (double beat = 0.0; beat <= beats; beat += 4.0)
        {
            const auto x = xForBeat (beat);
            g.setColour (theme::edgeStrong);
            g.fillRect (x, 6, 1, rulerHeight - 7);

            if (beatWidth() * 4.0 >= 26.0)
            {
                g.setColour (theme::textDim);
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
        g.setColour (theme::warn);
        g.fillRect (xForBeat (beat), 0, 2, getHeight());
    }

    Colour patternColour (const String& patternID) const
    {
        auto patterns = model.patterns();
        for (int i = 0; i < patterns.getNumChildren(); ++i)
            if (Model::uidOf (patterns.getChild (i)) == patternID)
                return Colour::fromHSV (std::fmod (0.36f + i * 0.17f, 1.0f), 0.45f, 0.72f, 1.0f);
        return theme::good;
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
    Point<int> bendAnchor;
    double bendStart = 0.0;
    Point<int> dragAnchor, rubberStart;
    Rectangle<int> rubberBand;
    DropPreview dropPreview;
    AudioThumbnailCache thumbnailCache;
    std::map<String, std::unique_ptr<AudioThumbnail>> thumbnails;
    double zoom = 9.0, snap = 1.0, loopAnchor = 0.0;
    int lastPlayheadX = 0, selectedCurve = -1;
    String selectedPoint;
};
}
