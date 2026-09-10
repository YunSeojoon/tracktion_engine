#pragma once

#include "Model.h"
#include "PianoRoll.h"
#include "PlaylistGrid.h"

namespace live
{
/** The four-area FL-style work surface: Browser on the left, Channel Rack above the
    Mixer in the centre, and the Pattern picker above the Playlist on the right.

    Every panel reads the live model and every control writes back to it through the
    Edit's undo manager, so a click here and an external AI edit take the same path.
    Panel sizes, visibility and the current selection are stored in the Edit, so a
    reopened session comes back with the same work surface.
*/

enum PanelIndex { panelBrowser = 0, panelChannelRack, panelMixer, panelPatternPicker, panelPlaylist, numPanels };

inline const char* panelName (int index)
{
    switch (index)
    {
        case panelBrowser:       return "Browser";
        case panelChannelRack:   return "Channel Rack";
        case panelMixer:         return "Mixer";
        case panelPatternPicker: return "Pattern picker";
        default:                 return "Playlist";
    }
}

//==============================================================================
/** A titled, focusable frame around one panel's content. */
class Panel final : public Component
{
public:
    Panel (const String& panelTitle, Component& panelContent)
        : title (panelTitle), content (panelContent)
    {
        setWantsKeyboardFocus (true);
        addAndMakeVisible (content);
    }

    void paint (Graphics& g) override
    {
        const auto focused = hasKeyboardFocus (true);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (Colour (0xff1c2331));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (focused ? Colour (0xff83dec0) : Colour (0xff2b3446));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, focused ? 1.8f : 1.0f);
        g.setColour (focused ? Colour (0xff83dec0) : Colour (0xff8698b6));
        g.setFont (Font (FontOptions (12.0f, Font::bold)));
        g.drawText (title.toUpperCase(), getLocalBounds().removeFromTop (headerHeight).reduced (9, 0),
                    Justification::centredLeft);
    }

    void resized() override
    {
        content.setBounds (getLocalBounds().withTrimmedTop (headerHeight).reduced (6, 5));
    }

    void mouseDown (const MouseEvent&) override { grabKeyboardFocus(); }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override   { repaint(); }

    static constexpr int headerHeight = 22;

    String title;
    Component& content;
};


//==============================================================================
class ProjectBrowser final : public Component,
                             private ListBoxModel
{
public:
    ProjectBrowser (Model& m, Selection& s, te::Engine& e, std::function<void()> onChange)
        : model (m), selection (s), engine (e), changed (std::move (onChange))
    {
        list.setModel (this);
        list.setRowHeight (19);
        list.setColour (ListBox::backgroundColourId, Colours::transparentBlack);
        addAndMakeVisible (list);
    }

    void resized() override { list.setBounds (getLocalBounds()); }

    void refresh()
    {
        Array<Row> rebuilt;
        rebuilt.add ({ "Project", {}, {}, true });
        addSection (rebuilt, "Channels", ids::CHANNEL, model.channels(), "channel");
        addSection (rebuilt, "Patterns", ids::PATTERN, model.patterns(), "pattern");
        addSection (rebuilt, "Playlist lanes", ids::LANE, model.lanes(), "lane");
        addSection (rebuilt, "Mixer inserts", ids::INSERT, model.mixer(), "insert");

        rebuilt.add ({ "Scanned plugins", {}, {}, true });
        auto& known = engine.getPluginManager().knownPluginList;
        if (known.getNumTypes() == 0)
            rebuilt.add ({ "  (none scanned yet)", {}, {}, false });
        else
            for (const auto& type : known.getTypes())
                rebuilt.add ({ "  " + type.name, {}, {}, false });

        if (! sameAs (rebuilt))
        {
            rows = std::move (rebuilt);
            list.updateContent();
        }
        list.repaint();
    }

private:
    struct Row { String label, kind, id; bool header; };

    void addSection (Array<Row>& into, const String& title, const Identifier& type,
                     ValueTree parent, const String& kind)
    {
        into.add ({ "  " + title, {}, {}, true });
        for (auto child : parent)
            if (child.hasType (type))
                into.add ({ "    " + child[ids::name].toString(), kind, Model::uidOf (child), false });
    }

    bool sameAs (const Array<Row>& other) const
    {
        if (other.size() != rows.size())
            return false;
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i].label != other[i].label || rows[i].id != other[i].id)
                return false;
        return true;
    }

    int getNumRows() override { return rows.size(); }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool) override
    {
        if (! isPositiveAndBelow (row, rows.size()))
            return;

        const auto& item = rows.getReference (row);
        const auto picked = ! item.id.isEmpty()
                             && ((item.kind == "channel" && item.id == selection.channel())
                              || (item.kind == "pattern" && item.id == selection.pattern())
                              || (item.kind == "lane" && item.id == selection.lane()));

        if (picked)
        {
            g.setColour (Colour (0xff2f4f5f));
            g.fillRect (0, 0, width, height);
        }

        g.setColour (item.header ? Colour (0xff8698b6) : Colours::white.withAlpha (0.88f));
        g.setFont (Font (FontOptions (item.header ? 12.0f : 13.0f, item.header ? Font::bold : Font::plain)));
        g.drawText (item.label, 4, 0, width - 8, height, Justification::centredLeft);
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (! isPositiveAndBelow (row, rows.size()))
            return;

        const auto& item = rows.getReference (row);
        if (item.kind == "channel") selection.setChannel (item.id);
        else if (item.kind == "pattern") selection.setPattern (item.id);
        else if (item.kind == "lane") selection.setLane (item.id);
        else return;

        if (changed != nullptr) changed();
    }

    Model& model;
    Selection& selection;
    te::Engine& engine;
    std::function<void()> changed;
    ListBox list;
    Array<Row> rows;
};

//==============================================================================
/** The sixteenth-note grid for one channel in the selected pattern. A lit step is a
    note starting inside it; clicking writes or removes that note in the pattern, so
    the whole playlist hears the change. */
class StepGrid final : public Component,
                       public SettableTooltipClient
{
public:
    StepGrid (Model& m, Selection& s, const String& channelID, std::function<void()> onChange)
        : model (m), selection (s), id (channelID), changed (std::move (onChange)) {}

    static constexpr int stepWidth = 14;

    int stepCount() const
    {
        auto pattern = model.patternFor (selection.pattern());
        if (! pattern.isValid())
            return 0;
        return jlimit (0, 256, roundToInt (static_cast<double> (pattern[ids::length]) / stepBeats));
    }

    int preferredWidth() const { return stepCount() * stepWidth; }

    void paint (Graphics& g) override
    {
        auto sequence = Model::findSequence (model.patternFor (selection.pattern()), id);
        const auto steps = stepCount();

        for (int step = 0; step < steps; ++step)
        {
            const auto area = Rectangle<int> (step * stepWidth, 0, stepWidth - 2, getHeight()).reduced (0, 3);
            const auto onBeat = step % 4 == 0;
            const auto lit = noteInStep (sequence, step).isValid();

            g.setColour (lit ? Colour (0xff7ddc9a)
                             : onBeat ? Colour (0xff39455c) : Colour (0xff28303f));
            g.fillRoundedRectangle (area.toFloat(), 2.0f);

            if (! lit && step % 16 == 0 && step > 0)
            {
                g.setColour (Colour (0xff55617a));
                g.fillRect (step * stepWidth - 1, 0, 1, getHeight());
            }
        }
    }

    void mouseDown (const MouseEvent& e) override { toggleStep (e.x / stepWidth); }
    void mouseDrag (const MouseEvent& e) override
    {
        const auto step = e.x / stepWidth;
        if (step != lastDragged)
            toggleStep (step);
    }
    void mouseUp (const MouseEvent&) override { lastDragged = -1; }

    /** Turns one step on or off, the same way a click on it does. */
    bool toggleStep (int step)
    {
        auto pattern = model.patternFor (selection.pattern());
        if (! pattern.isValid() || step < 0 || step >= stepCount())
            return false;

        lastDragged = step;
        auto channel = model.channelFor (id);
        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction ("Edit step");

        auto sequence = model.sequenceFor (pattern, id, &undo);
        if (auto existing = noteInStep (sequence, step); existing.isValid())
        {
            sequence.removeChild (existing, &undo);
        }
        else
        {
            const auto length = std::max (0.001, static_cast<double> (channel.getProperty (ids::stepLength, stepBeats)));
            const auto room = static_cast<double> (pattern[ids::length]) - step * stepBeats;
            model.addNote (sequence, jlimit (0, 127, static_cast<int> (channel.getProperty (ids::stepPitch, 60))),
                           step * stepBeats, std::min (length, room), 100, &undo);
        }

        model.renderIfNeeded();
        if (changed != nullptr) changed();
        repaint();
        return true;
    }

private:
    ValueTree noteInStep (ValueTree sequence, int step) const
    {
        if (! sequence.isValid())
            return {};

        const auto from = step * stepBeats;
        for (auto note : sequence)
        {
            const auto start = static_cast<double> (note[ids::start]);
            if (start >= from - 1.0e-6 && start < from + stepBeats - 1.0e-6)
                return note;
        }
        return {};
    }

    Model& model;
    Selection& selection;
    const String id;
    std::function<void()> changed;
    int lastDragged = -1;
};

//==============================================================================
/** One channel: name, instrument, mute/solo, fader, pan, mixer insert, and the step
    grid for the pattern currently selected in the picker. */
class ChannelRow final : public Component
{
public:
    ChannelRow (Model& m, Selection& s, const String& channelID, std::function<void()> onChange)
        : model (m), selection (s), id (channelID), changed (onChange), steps (m, s, channelID, onChange)
    {
        name.setEditable (false, true, false);
        name.setColour (Label::textColourId, Colours::white);
        name.onTextChange = [this]
        {
            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Rename channel");
            channel().setProperty (ids::name, name.getText(), &undo);
            model.renderIfNeeded();
        };

        mute.setClickingTogglesState (true);
        solo.setClickingTogglesState (true);
        mute.onClick = [this] { write (ids::mute, mute.getToggleState(), "Mute channel"); };
        solo.onClick = [this] { write (ids::solo, solo.getToggleState(), "Solo channel"); };

        gain.setSliderStyle (Slider::RotaryVerticalDrag);
        gain.setRange (-60.0, 6.0, 0.1);
        gain.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        gain.setTooltip ("Channel volume");
        gain.onValueChange = [this] { write (ids::gainDb, gain.getValue(), "Channel volume"); };

        insert.setSliderStyle (Slider::IncDecButtons);
        insert.setRange (1.0, 256.0, 1.0);
        insert.setTextBoxStyle (Slider::TextBoxLeft, false, 26, 20);
        insert.setTooltip ("Mixer insert");
        insert.onValueChange = [this] { write (ids::insert, static_cast<int> (insert.getValue()), "Mixer insert"); };

        pan.setSliderStyle (Slider::RotaryVerticalDrag);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        pan.setTooltip ("Channel pan");
        pan.onValueChange = [this] { write (ids::pan, pan.getValue(), "Channel pan"); };

        instrument.onChange = [this]
        {
            const auto choice = instrument.getSelectedId() - 1;
            if (! isPositiveAndBelow (choice, instruments.size()) || refreshing)
                return;

            const auto wanted = instruments.getReference (choice).first;
            if (wanted == channel()[ids::instrument].toString())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Change instrument");
            channel().setProperty (ids::instrument, wanted, &undo);
            if (wanted == builtInSampler && channel()[ids::sample].toString().isEmpty())
                chooseSample();
            model.renderIfNeeded();
        };

        openInstrument.onClick = [this]
        {
            if (auto* track = model.trackFor (id))
                if (auto* plugin = Model::instrumentOf (*track))
                    plugin->showWindowExplicitly();
        };

        sample.onClick = [this] { chooseSample(); };
        stepSettings.onClick = [this] { showStepSettings(); };

        for (auto* child : std::initializer_list<Component*> { &name, &mute, &solo, &gain, &pan, &insert,
                                                              &instrument, &openInstrument, &sample, &stepSettings, &steps })
            addAndMakeVisible (*child);
    }

    void paint (Graphics& g) override
    {
        g.setColour (selection.channel() == id ? Colour (0xff2f4f5f) : Colour (0xff222b3b));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 3.0f);
    }

    void resized() override
    {
        // The step grid is what a Channel Rack is for, so the controls beside it stay
        // compact and everything left over goes to the steps.
        auto r = getLocalBounds().reduced (6, 3);
        name.setBounds (r.removeFromLeft (116));
        mute.setBounds (r.removeFromLeft (24).reduced (1));
        solo.setBounds (r.removeFromLeft (24).reduced (1));
        instrument.setBounds (r.removeFromLeft (114).reduced (2));
        openInstrument.setBounds (r.removeFromLeft (26).reduced (1));
        sample.setBounds (r.removeFromLeft (32).reduced (1));
        gain.setBounds (r.removeFromLeft (32));
        pan.setBounds (r.removeFromLeft (32));
        insert.setBounds (r.removeFromLeft (62).reduced (1));
        stepSettings.setBounds (r.removeFromLeft (40).reduced (1));
        steps.setBounds (r.withTrimmedLeft (8));
    }

    static constexpr int controlsWidth = 510;

    int preferredWidth() const { return controlsWidth + steps.preferredWidth(); }

    bool toggleStep (int step) { return steps.toggleStep (step); }

    void mouseDown (const MouseEvent&) override
    {
        selection.setChannel (id);
        if (changed != nullptr) changed();
    }

    void refresh()
    {
        auto tree = channel();
        if (! tree.isValid())
            return;

        const ScopedValueSetter<bool> guard (refreshing, true);

        if (! name.isBeingEdited()) name.setText (tree[ids::name].toString(), dontSendNotification);
        mute.setToggleState (static_cast<bool> (tree[ids::mute]), dontSendNotification);
        solo.setToggleState (static_cast<bool> (tree[ids::solo]), dontSendNotification);
        gain.setValue (static_cast<double> (tree[ids::gainDb]), dontSendNotification);
        pan.setValue (static_cast<double> (tree.getProperty (ids::pan, 0.0)), dontSendNotification);
        insert.setValue (static_cast<double> (tree[ids::insert]), dontSendNotification);

        const auto available = model.availableInstruments();
        if (available != instruments)
        {
            instruments = available;
            instrument.clear (dontSendNotification);
            for (int i = 0; i < instruments.size(); ++i)
                instrument.addItem (instruments.getReference (i).second, i + 1);
        }

        const auto wanted = tree[ids::instrument].toString();
        for (int i = 0; i < instruments.size(); ++i)
            if (instruments.getReference (i).first == wanted)
                instrument.setSelectedId (i + 1, dontSendNotification);

        sample.setEnabled (wanted == builtInSampler);
        sample.setTooltip (tree[ids::sample].toString());

        const auto stepLength = static_cast<double> (tree.getProperty (ids::stepLength, stepBeats));
        stepSettings.setButtonText (stepLength >= 4.0 ? "1b" : stepLength >= 2.0 ? "1/2"
                                  : stepLength >= 1.0 ? "1/4" : stepLength >= 0.5 ? "1/8" : "1/16");
        stepSettings.setTooltip ("Step length and pitch: "
                                  + MidiMessage::getMidiNoteName (static_cast<int> (tree.getProperty (ids::stepPitch, 60)),
                                                                  true, true, 3));

        auto sequence = Model::findSequence (model.patternFor (selection.pattern()), id);
        const auto count = sequence.isValid() ? sequence.getNumChildren() : 0;
        steps.setTooltip (count == 1 ? "1 note in pattern" : String (count) + " notes in pattern");
        steps.repaint();
        repaint();
    }

    const String id;

private:
    ValueTree channel() const { return model.channelFor (id); }

    template <typename Value>
    void write (const Identifier& property, Value value, const String& description)
    {
        if (refreshing)
            return;

        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction (description);
        channel().setProperty (property, value, &undo);
        model.renderIfNeeded();
    }

    /** What a step writes: how long the note is and which pitch it plays. A drum
        channel wants a short note on its own key; a bass channel a longer one. */
    void showStepSettings()
    {
        static const std::pair<const char*, double> lengths[] = {
            { "1/16", 0.25 }, { "1/8", 0.5 }, { "1/4", 1.0 }, { "1/2", 2.0 }, { "1 bar", 4.0 } };

        auto tree = channel();
        const auto currentLength = static_cast<double> (tree.getProperty (ids::stepLength, stepBeats));
        const auto currentPitch = static_cast<int> (tree.getProperty (ids::stepPitch, 60));

        PopupMenu lengthMenu;
        for (int i = 0; i < numElementsInArray (lengths); ++i)
            lengthMenu.addItem (i + 1, String ("Step length  ") + lengths[i].first, true,
                                std::abs (currentLength - lengths[i].second) < 1.0e-6);

        PopupMenu pitchMenu;
        for (int octave = 1; octave <= 7; ++octave)
            for (int semitone = 0; semitone < 12; semitone += (octave == 1 || octave == 7) ? 12 : 1)
            {
                const auto pitch = octave * 12 + semitone;
                pitchMenu.addItem (100 + pitch, MidiMessage::getMidiNoteName (pitch, true, true, 3),
                                   true, pitch == currentPitch);
            }

        PopupMenu menu;
        menu.addSubMenu ("Step length", lengthMenu);
        menu.addSubMenu ("Step pitch", pitchMenu);
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (stepSettings),
            [this] (int choice)
            {
                if (choice <= 0)
                    return;

                if (choice >= 100)
                    write (ids::stepPitch, choice - 100, "Step pitch");
                else
                    write (ids::stepLength, lengths[choice - 1].second, "Step length");

                if (changed != nullptr) changed();
            });
    }

    void chooseSample()
    {
        chooser = std::make_unique<FileChooser> ("Choose a sample for " + channel()[ids::name].toString(),
                                                 File(), model.edit.engine.getAudioFileFormatManager()
                                                              .readFormatManager.getWildcardForAllFormats());
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
            [this] (const FileChooser& result)
            {
                const auto file = result.getResult();
                if (! file.existsAsFile())
                    return;

                auto& undo = model.edit.getUndoManager();
                undo.beginNewTransaction ("Load sample");
                channel().setProperty (ids::instrument, builtInSampler, &undo);
                channel().setProperty (ids::sample, file.getFullPathName(), &undo);
                model.renderIfNeeded();
                if (changed != nullptr) changed();
            });
    }

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    Label name;
    TextButton mute { "M" }, solo { "S" }, openInstrument { "..." }, sample { "WAV" }, stepSettings { "1/16" };
    ComboBox instrument;
    Slider gain, pan, insert;
    StepGrid steps;
    Array<std::pair<String, String>> instruments;
    std::unique_ptr<FileChooser> chooser;
    bool refreshing = false;
};

//==============================================================================
class ChannelRack final : public Component
{
public:
    ChannelRack (Model& m, Selection& s, std::function<void()> onChange, std::function<void()> openPianoRoll)
        : model (m), selection (s), changed (std::move (onChange))
    {
        pianoRoll.onClick = std::move (openPianoRoll);

        add.onClick = [this]
        {
            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Add channel");
            auto channel = model.addChannel ("Channel " + String (model.channels().getNumChildren() + 1), &undo);
            model.renderIfNeeded();
            selection.setChannel (Model::uidOf (channel));
            if (changed != nullptr) changed();
        };

        remove.onClick = [this]
        {
            auto channel = model.channelFor (selection.channel());
            if (! channel.isValid())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Remove channel");
            model.channels().removeChild (channel, &undo);
            model.renderIfNeeded();
            selection.setChannel ({});
            if (changed != nullptr) changed();
        };

        rows.setInterceptsMouseClicks (false, true);
        viewport.setViewedComponent (&rows, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);
        addAndMakeVisible (add);
        addAndMakeVisible (remove);
        addAndMakeVisible (pianoRoll);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (26);
        add.setBounds (bar.removeFromLeft (110).reduced (2));
        remove.setBounds (bar.removeFromLeft (130).reduced (2));
        pianoRoll.setBounds (bar.removeFromLeft (110).reduced (2));
        viewport.setBounds (r);
        layoutRows();
    }

    void refresh()
    {
        StringArray wanted;
        for (auto channel : model.channels())
            wanted.add (Model::uidOf (channel));

        StringArray present;
        for (auto* row : channelRows)
            present.add (row->id);

        if (present != wanted)
        {
            channelRows.clear();
            for (const auto& channelID : wanted)
            {
                auto* row = channelRows.add (new ChannelRow (model, selection, channelID, changed));
                rows.addAndMakeVisible (row);
            }
            layoutRows();
        }

        remove.setEnabled (model.channelFor (selection.channel()).isValid());
        pianoRoll.setEnabled (model.channelFor (selection.channel()).isValid()
                               && model.patternFor (selection.pattern()).isValid());
        for (auto* row : channelRows)
            row->refresh();

        layoutRows(); // The step grid grows and shrinks with the selected pattern.
    }

    /** Drives one step button, for the diagnostic UI script. */
    bool toggleStep (int channelIndex, int step)
    {
        if (! isPositiveAndBelow (channelIndex, channelRows.size()))
            return false;
        return channelRows[channelIndex]->toggleStep (step);
    }

private:
    void layoutRows()
    {
        auto width = std::max (560, viewport.getWidth() - viewport.getScrollBarThickness());
        for (auto* row : channelRows)
            width = std::max (width, row->preferredWidth());

        rows.setSize (width, std::max (viewport.getHeight(), channelRows.size() * rowHeight));
        for (int i = 0; i < channelRows.size(); ++i)
            channelRows[i]->setBounds (0, i * rowHeight, width, rowHeight);
    }

    static constexpr int rowHeight = 38;

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    Viewport viewport;
    Component rows;
    OwnedArray<ChannelRow> channelRows;
    TextButton add { "+ Channel" }, remove { "Remove channel" }, pianoRoll { "Piano roll" };
};

//==============================================================================
/** One mixer strip. Inserts are modelled and stored now; the audio still runs
    straight from each channel to the master until M5 wires the routing. */
class MixerStrip final : public Component
{
public:
    MixerStrip (Model& m, const String& insertID, te::VolumeAndPanPlugin* masterPlugin)
        : model (m), id (insertID), master (masterPlugin)
    {
        name.setJustificationType (Justification::centred);
        name.setColour (Label::textColourId, Colours::white);
        name.setFont (Font (FontOptions (12.0f, Font::bold)));

        feeds.setJustificationType (Justification::centred);
        feeds.setColour (Label::textColourId, Colour (0xff8698b6));
        feeds.setFont (Font (FontOptions (11.0f)));

        gain.setSliderStyle (Slider::LinearVertical);
        gain.setRange (-60.0, 6.0, 0.1);
        gain.setTextBoxStyle (Slider::TextBoxBelow, false, 56, 18);
        gain.onValueChange = [this]
        {
            if (master != nullptr) { master->setVolumeDb (static_cast<float> (gain.getValue())); return; }
            write (ids::gainDb, gain.getValue(), "Insert volume");
        };

        pan.setSliderStyle (Slider::LinearHorizontal);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        pan.onValueChange = [this]
        {
            if (master != nullptr) { master->setPan (static_cast<float> (pan.getValue())); return; }
            write (ids::pan, pan.getValue(), "Insert pan");
        };

        mute.setClickingTogglesState (true);
        mute.onClick = [this]
        {
            if (master != nullptr) return;
            write (ids::mute, mute.getToggleState(), "Mute insert");
        };
        mute.setEnabled (master == nullptr);

        for (auto* child : std::initializer_list<Component*> { &name, &gain, &pan, &mute, &feeds })
            addAndMakeVisible (*child);
    }

    void paint (Graphics& g) override
    {
        g.setColour (master != nullptr ? Colour (0xff2a3550) : Colour (0xff222b3b));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 3.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 5);
        name.setBounds (r.removeFromTop (16));
        feeds.setBounds (r.removeFromBottom (14));
        mute.setBounds (r.removeFromBottom (22).reduced (6, 1));
        pan.setBounds (r.removeFromBottom (20).reduced (2, 0));
        gain.setBounds (r);
    }

    void refresh()
    {
        if (master != nullptr)
        {
            name.setText ("Master", dontSendNotification);
            gain.setValue (master->getVolumeDb(), dontSendNotification);
            pan.setValue (master->getPan(), dontSendNotification);
            feeds.setText ("all inserts", dontSendNotification);
            return;
        }

        auto insert = model.insertFor (id);
        if (! insert.isValid())
            return;

        const auto slot = static_cast<int> (insert[ids::index]);
        name.setText (String (slot) + "  " + insert[ids::name].toString(), dontSendNotification);
        gain.setValue (static_cast<double> (insert[ids::gainDb]), dontSendNotification);
        pan.setValue (static_cast<double> (insert[ids::pan]), dontSendNotification);
        mute.setToggleState (static_cast<bool> (insert[ids::mute]), dontSendNotification);

        int fed = 0;
        for (auto channel : model.channels())
            if (static_cast<int> (channel[ids::insert]) == slot)
                ++fed;
        feeds.setText (fed == 1 ? "1 channel" : String (fed) + " channels", dontSendNotification);
    }

    const String id;

private:
    template <typename Value>
    void write (const Identifier& property, Value value, const String& description)
    {
        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction (description);
        model.insertFor (id).setProperty (property, value, &undo);
    }

    Model& model;
    te::VolumeAndPanPlugin* master;
    Label name, feeds;
    Slider gain, pan;
    TextButton mute { "Mute" };
};

//==============================================================================
class MixerPanel final : public Component
{
public:
    explicit MixerPanel (Model& m) : model (m)
    {
        strips.setInterceptsMouseClicks (false, true);
        viewport.setViewedComponent (&strips, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        layoutStrips();
    }

    void refresh()
    {
        StringArray wanted { "master" };
        for (auto insert : model.mixer())
            wanted.add (Model::uidOf (insert));

        StringArray present;
        for (auto* strip : mixerStrips)
            present.add (strip->id);

        if (present != wanted)
        {
            mixerStrips.clear();
            for (const auto& insertID : wanted)
            {
                auto* strip = mixerStrips.add (new MixerStrip (model, insertID,
                    insertID == "master" ? model.edit.getMasterVolumePlugin().get() : nullptr));
                strips.addAndMakeVisible (strip);
            }
            layoutStrips();
        }

        for (auto* strip : mixerStrips)
            strip->refresh();
    }

private:
    void layoutStrips()
    {
        const auto height = std::max (120, viewport.getHeight() - viewport.getScrollBarThickness());
        strips.setSize (std::max (viewport.getWidth(), mixerStrips.size() * stripWidth), height);
        for (int i = 0; i < mixerStrips.size(); ++i)
            mixerStrips[i]->setBounds (i * stripWidth, 0, stripWidth, height);
    }

    static constexpr int stripWidth = 84;

    Model& model;
    Viewport viewport;
    Component strips;
    OwnedArray<MixerStrip> mixerStrips;
};

//==============================================================================
class PatternPicker final : public Component,
                            private ListBoxModel
{
public:
    PatternPicker (Model& m, Selection& s, std::function<void()> onChange)
        : model (m), selection (s), changed (std::move (onChange))
    {
        list.setModel (this);
        list.setRowHeight (20);
        list.setColour (ListBox::backgroundColourId, Colours::transparentBlack);

        add.onClick = [this]
        {
            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("New pattern");
            auto pattern = model.addPattern ("Pattern " + String (model.patterns().getNumChildren() + 1),
                                             defaultPatternBeats, &undo);
            model.renderIfNeeded();
            selection.setPattern (Model::uidOf (pattern));
            notify();
        };

        duplicate.onClick = [this]
        {
            auto source = model.patternFor (selection.pattern());
            if (! source.isValid())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Duplicate pattern");
            auto copy = source.createCopy();
            copy.setProperty (ids::uid, Uuid().toString(), nullptr);
            copy.setProperty (ids::name, source[ids::name].toString() + " copy", nullptr);
            for (auto sequence : copy)
                for (auto note : sequence)
                    note.setProperty (ids::uid, Uuid().toString(), nullptr);
            model.patterns().appendChild (copy, &undo);
            model.renderIfNeeded();
            selection.setPattern (Model::uidOf (copy));
            notify();
        };

        remove.onClick = [this]
        {
            auto pattern = model.patternFor (selection.pattern());
            if (! pattern.isValid())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Delete pattern");
            for (int i = model.instances().getNumChildren(); --i >= 0;)
                if (model.instances().getChild (i)[ids::pattern].toString() == selection.pattern())
                    model.instances().removeChild (i, &undo);
            model.patterns().removeChild (pattern, &undo);
            model.renderIfNeeded();
            selection.setPattern ({});
            notify();
        };

        addAndMakeVisible (list);
        for (auto* button : std::initializer_list<Component*> { &add, &duplicate, &remove })
            addAndMakeVisible (*button);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (26);
        add.setBounds (bar.removeFromLeft (58).reduced (2));
        duplicate.setBounds (bar.removeFromLeft (86).reduced (2));
        remove.setBounds (bar.removeFromLeft (72).reduced (2));
        list.setBounds (r);
    }

    void refresh()
    {
        StringArray rebuilt;
        for (auto pattern : model.patterns())
            rebuilt.add (Model::uidOf (pattern) + "\t" + pattern[ids::name].toString()
                         + "\t" + String (placements (Model::uidOf (pattern))));

        if (rebuilt != entries)
        {
            entries = rebuilt;
            list.updateContent();
        }

        duplicate.setEnabled (model.patternFor (selection.pattern()).isValid());
        remove.setEnabled (duplicate.isEnabled());
        list.repaint();
    }

private:
    void notify() { if (changed != nullptr) changed(); }

    int placements (const String& patternID) const
    {
        int count = 0;
        for (auto instance : model.instances())
            if (instance[ids::pattern].toString() == patternID)
                ++count;
        return count;
    }

    int getNumRows() override { return entries.size(); }

    /** Lets a pattern be dragged straight onto a playlist lane. */
    var getDragSourceDescription (const SparseSet<int>& rows) override
    {
        if (rows.isEmpty() || ! isPositiveAndBelow (rows[0], entries.size()))
            return {};

        return StringArray::fromTokens (entries[rows[0]], "\t", "")[0];
    }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool) override
    {
        if (! isPositiveAndBelow (row, entries.size()))
            return;

        const auto fields = StringArray::fromTokens (entries[row], "\t", "");
        if (fields[0] == selection.pattern())
        {
            g.setColour (Colour (0xff2f4f5f));
            g.fillRect (0, 0, width, height);
        }

        g.setColour (Colours::white.withAlpha (0.9f));
        g.setFont (Font (FontOptions (13.0f)));
        g.drawText (fields[1], 6, 0, width - 70, height, Justification::centredLeft);
        g.setColour (Colour (0xff8698b6));
        g.setFont (Font (FontOptions (11.0f)));
        g.drawText (fields[2] + "x", width - 62, 0, 56, height, Justification::centredRight);
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (! isPositiveAndBelow (row, entries.size()))
            return;

        selection.setPattern (StringArray::fromTokens (entries[row], "\t", "")[0]);
        notify();
    }

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    ListBox list;
    StringArray entries;
    TextButton add { "New" }, duplicate { "Duplicate" }, remove { "Delete" };
};

//==============================================================================
/** The arrangement, with the lane list built into the grid itself. */
class PlaylistPanel final : public Component
{
public:
    PlaylistPanel (Model& m, Selection& s, std::function<void()> onChange)
        : model (m), selection (s), changed (onChange),
          grid (std::make_unique<PlaylistGrid> (m, s, onChange))
    {
        viewport.setViewedComponent (grid.get(), false);
        viewport.setScrollBarsShown (true, true);

        addLane.onClick = [this]
        {
            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Add playlist lane");
            auto lane = model.addLane ("Lane " + String (model.lanes().getNumChildren() + 1), &undo);
            model.renderIfNeeded();
            selection.setLane (Model::uidOf (lane));
            notify();
        };

        removeLane.onClick = [this]
        {
            auto lane = model.laneFor (selection.lane());
            if (! lane.isValid())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Remove playlist lane");
            for (int i = model.instances().getNumChildren(); --i >= 0;)
                if (model.instances().getChild (i)[ids::lane].toString() == selection.lane())
                    model.instances().removeChild (i, &undo);
            model.lanes().removeChild (lane, &undo);
            model.renderIfNeeded();
            selection.setLane ({});
            notify();
        };

        muteLane.onClick = [this]
        {
            auto lane = model.laneFor (selection.lane());
            if (! lane.isValid())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Mute playlist lane");
            lane.setProperty (ids::mute, ! static_cast<bool> (lane[ids::mute]), &undo);
            model.renderIfNeeded();
            notify();
        };

        duplicate.onClick = [this] { grid->duplicateSelection(); };
        makeUnique.onClick = [this] { grid->makeSelectionUnique(); };
        remove.onClick = [this] { grid->deleteSelection(); };

        for (const auto& choice : { std::pair<const char*, double> { "Bar", 4.0 },
                                    { "1/2", 2.0 }, { "1/4", 1.0 }, { "1/8", 0.5 }, { "Off", 0.0 } })
            snapChoices.add (choice.second);

        snap.addItem ("Snap: bar", 1);
        snap.addItem ("Snap: 1/2", 2);
        snap.addItem ("Snap: 1/4", 3);
        snap.addItem ("Snap: 1/8", 4);
        snap.addItem ("Snap: off", 5);
        snap.setSelectedId (1, dontSendNotification);
        snap.onChange = [this] { grid->setSnap (snapChoices[snap.getSelectedId() - 1]); };

        zoom.setSliderStyle (Slider::LinearHorizontal);
        zoom.setRange (2.0, 40.0, 0.5);
        zoom.setValue (grid->getZoom(), dontSendNotification);
        zoom.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        zoom.setTooltip ("Zoom");
        zoom.onValueChange = [this] { grid->setZoom (zoom.getValue()); layOutGrid(); };

        addAndMakeVisible (viewport);
        for (auto* child : std::initializer_list<Component*> { &addLane, &removeLane, &muteLane,
                                                              &duplicate, &makeUnique, &remove, &snap, &zoom })
            addAndMakeVisible (*child);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (26);
        addLane.setBounds (bar.removeFromLeft (62).reduced (1));
        removeLane.setBounds (bar.removeFromLeft (62).reduced (1));
        muteLane.setBounds (bar.removeFromLeft (52).reduced (1));
        duplicate.setBounds (bar.removeFromLeft (74).reduced (1));
        makeUnique.setBounds (bar.removeFromLeft (74).reduced (1));
        remove.setBounds (bar.removeFromLeft (62).reduced (1));
        snap.setBounds (bar.removeFromLeft (92).reduced (1));
        zoom.setBounds (bar.reduced (2, 1));
        viewport.setBounds (r);
        layOutGrid();
    }

    void refresh()
    {
        const auto hasLane = model.laneFor (selection.lane()).isValid();
        const auto hasClips = ! grid->selectedClips().isEmpty();
        removeLane.setEnabled (hasLane);
        muteLane.setEnabled (hasLane);
        duplicate.setEnabled (hasClips);
        makeUnique.setEnabled (hasClips);
        remove.setEnabled (hasClips);
        layOutGrid();
        grid->repaint();
    }

    PlaylistGrid& getGrid() { return *grid; }

private:
    void notify() { if (changed != nullptr) changed(); }

    void layOutGrid()
    {
        grid->setSize (std::max (grid->preferredWidth(), viewport.getWidth() - viewport.getScrollBarThickness()),
                       std::max (grid->preferredHeight(), viewport.getHeight() - viewport.getScrollBarThickness()));
    }

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    std::unique_ptr<PlaylistGrid> grid;
    Viewport viewport;
    TextButton addLane { "+ Lane" }, removeLane { "- Lane" }, muteLane { "Mute" },
               duplicate { "Duplicate" }, makeUnique { "Unique" }, remove { "Delete" };
    ComboBox snap;
    Slider zoom;
    Array<double> snapChoices;
};

//==============================================================================
class Workspace final : public Component,
                       public DragAndDropContainer
{
public:
    Workspace (Model& m, te::Engine& engine)
        : model (m),
          layout (m.edit.state.getOrCreateChildWithName (layoutIds::LAYOUT, nullptr)),
          selection (layout)
    {
        auto onChange = [this] { refresh(); };

        browser = std::make_unique<ProjectBrowser> (model, selection, engine, onChange);
        rack = std::make_unique<ChannelRack> (model, selection, onChange, [this] { openPianoRoll(); });
        mixer = std::make_unique<MixerPanel> (model);
        picker = std::make_unique<PatternPicker> (model, selection, onChange);
        playlist = std::make_unique<PlaylistPanel> (model, selection, onChange);

        Component* contents[numPanels] = { browser.get(), rack.get(), mixer.get(), picker.get(), playlist.get() };
        for (int i = 0; i < numPanels; ++i)
        {
            panels[i] = std::make_unique<Panel> (panelName (i), *contents[i]);
            addAndMakeVisible (*panels[i]);
        }

        const auto stored = StringArray::fromTokens (layout[layoutIds::sizes].toString(), " ", "");
        const double fallbacks[4] = { 190.0, 460.0, 290.0, 170.0 };
        for (int i = 0; i < 4; ++i)
        {
            const auto value = i < stored.size() ? stored[i].getDoubleValue() : 0.0;
            desired[i] = value > 1.0 ? value : fallbacks[i];
        }

        columns.setItemLayout (2, 260, 4000, -1.0);
        centre.setItemLayout (2, 100, 3000, -1.0);
        right.setItemLayout (2, 100, 3000, -1.0);

        columnBars[0] = std::make_unique<StretchableLayoutResizerBar> (&columns, 1, true);
        columnBars[1] = std::make_unique<StretchableLayoutResizerBar> (&columns, 3, true);
        centreBar = std::make_unique<StretchableLayoutResizerBar> (&centre, 1, false);
        rightBar = std::make_unique<StretchableLayoutResizerBar> (&right, 1, false);

        for (auto* bar : std::initializer_list<Component*> { columnBars[0].get(), columnBars[1].get() })
            addAndMakeVisible (*bar);

        addAndMakeVisible (centreHolder);
        addAndMakeVisible (rightHolder);
        centreHolder.addAndMakeVisible (*panels[panelChannelRack]);
        centreHolder.addAndMakeVisible (*centreBar);
        centreHolder.addAndMakeVisible (*panels[panelMixer]);
        rightHolder.addAndMakeVisible (*panels[panelPatternPicker]);
        rightHolder.addAndMakeVisible (*rightBar);
        rightHolder.addAndMakeVisible (*panels[panelPlaylist]);

        if (! layout.hasProperty (layoutIds::visible))
            layout.setProperty (layoutIds::visible, "11111", nullptr);

        if (selection.channel().isEmpty() && model.channels().getNumChildren() > 0)
            selection.setChannel (Model::uidOf (model.channels().getChild (0)));
        if (selection.pattern().isEmpty() && model.patterns().getNumChildren() > 0)
            selection.setPattern (Model::uidOf (model.patterns().getChild (0)));
        if (selection.lane().isEmpty() && model.lanes().getNumChildren() > 0)
            selection.setLane (Model::uidOf (model.lanes().getChild (0)));
    }

    ~Workspace() override { store(); }

    bool isPanelVisible (int index) const
    {
        const auto panelFlags = layout[layoutIds::visible].toString();
        return index < panelFlags.length() ? panelFlags[index] == '1' : true;
    }

    void setPanelVisible (int index, bool shouldBeVisible)
    {
        rememberSizes(); // Capture the current split before a hidden panel reports zero.
        auto panelFlags = layout[layoutIds::visible].toString().paddedRight ('1', numPanels);
        panelFlags = panelFlags.replaceSection (index, 1, shouldBeVisible ? "1" : "0");
        layout.setProperty (layoutIds::visible, panelFlags, nullptr);
        resized();
    }

    void togglePanel (int index) { setPanelVisible (index, ! isPanelVisible (index)); }

    /** Moves keyboard focus to the next visible panel, so the whole surface is
        reachable without the mouse. */
    /** Puts the keyboard on a panel so the surface is usable without touching the mouse. */
    void focusFirstPanel()
    {
        for (int i = 0; i < numPanels; ++i)
            if (isPanelVisible (i))
            {
                panels[i]->grabKeyboardFocus();
                return;
            }
    }

    void focusNextPanel()
    {
        int current = -1;
        for (int i = 0; i < numPanels; ++i)
            if (panels[i]->hasKeyboardFocus (true))
                current = i;

        for (int step = 1; step <= numPanels; ++step)
        {
            const auto next = (current + step + numPanels) % numPanels;
            if (isPanelVisible (next))
            {
                panels[next]->grabKeyboardFocus();
                return;
            }
        }
    }

    String focusedPanelName() const
    {
        for (int i = 0; i < numPanels; ++i)
            if (panels[i]->hasKeyboardFocus (true))
                return panelName (i);
        return "None";
    }

    void resized() override
    {
        rememberSizes();

        // A hidden panel keeps its remembered size but takes no space, and its resizer
        // bar goes with it, so restoring puts the surface back where it was.
        applyVisibility (getWidth(), getHeight());

        auto r = getLocalBounds();
        Component* columnComponents[5] = { panels[panelBrowser].get(), columnBars[0].get(),
                                           &centreHolder, columnBars[1].get(), &rightHolder };
        columns.layOutComponents (columnComponents, 5, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);

        Component* centreComponents[3] = { panels[panelChannelRack].get(), centreBar.get(), panels[panelMixer].get() };
        centre.layOutComponents (centreComponents, 3, 0, 0, centreHolder.getWidth(), centreHolder.getHeight(), true, true);

        Component* rightComponents[3] = { panels[panelPatternPicker].get(), rightBar.get(), panels[panelPlaylist].get() };
        right.layOutComponents (rightComponents, 3, 0, 0, rightHolder.getWidth(), rightHolder.getHeight(), true, true);
    }

    void refresh()
    {
        browser->refresh();
        rack->refresh();
        mixer->refresh();
        picker->refresh();
        playlist->refresh();
        for (auto& panel : panels)
            panel->repaint();
    }

    /** Opens the note editor for the selected channel in the selected pattern. Its
        window is owned here so it closes with the work surface. */
    void openPianoRoll()
    {
        auto channel = model.channelFor (selection.channel());
        auto pattern = model.patternFor (selection.pattern());
        if (! channel.isValid() || ! pattern.isValid())
            return;

        pianoRollWindow = std::make_unique<PianoRollWindow> (
            pattern[ids::name].toString() + "  -  " + channel[ids::name].toString(),
            std::make_unique<PianoRollEditor> (model, selection.pattern(), selection.channel()));
    }

    //==========================================================================
    // Entry points for the diagnostic UI script, so a check can drive the real
    // panels instead of writing to the model behind their backs.
    bool selectByIndex (const Identifier& what, int index)
    {
        auto parent = what == ids::CHANNEL ? model.channels()
                    : what == ids::PATTERN ? model.patterns()
                    : model.lanes();
        auto child = parent.getChild (index);
        if (! child.isValid())
            return false;

        if (what == ids::CHANNEL)      selection.setChannel (Model::uidOf (child));
        else if (what == ids::PATTERN) selection.setPattern (Model::uidOf (child));
        else                           selection.setLane (Model::uidOf (child));

        refresh();
        return true;
    }

    bool toggleStep (int channelIndex, int step) { return rack->toggleStep (channelIndex, step); }

    PlaylistGrid& playlistGrid() const { return playlist->getGrid(); }

    /** The open note editor, so --screenshots can capture it too. */
    Component* pianoRollContent() const
    {
        return pianoRollWindow != nullptr && pianoRollWindow->isVisible()
                 ? pianoRollWindow->getContentComponent() : nullptr;
    }

    bool addPianoRollNote (int pitch, double startBeat, double lengthBeats, int velocity)
    {
        if (pianoRollWindow == nullptr || ! pianoRollWindow->isVisible())
            openPianoRoll();

        if (auto* editor = pianoRollWindow != nullptr
                             ? dynamic_cast<PianoRollEditor*> (pianoRollWindow->getContentComponent())
                             : nullptr)
            return editor->addNote (pitch, startBeat, lengthBeats, velocity);

        return false;
    }

    void store()
    {
        rememberSizes();
        layout.setProperty (layoutIds::sizes, String (desired[0]) + " " + String (desired[1]) + " "
                                                  + String (desired[2]) + " " + String (desired[3]), nullptr);
    }

    Model& model;
    ValueTree layout;
    Selection selection;

private:
    struct PianoRollWindow final : DocumentWindow
    {
        PianoRollWindow (const String& windowTitle, std::unique_ptr<PianoRollEditor> editor)
            : DocumentWindow (windowTitle, Colour (0xff151b26), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (editor.release(), true);
            setResizable (true, false);
            setResizeLimits (620, 380, 4000, 2400);
            centreWithSize (1000, 560);
            setVisible (true);
            toFront (true);
        }

        void closeButtonPressed() override { setVisible (false); }
    };

    static constexpr int barSize = 6;

    /** Keeps the last size an item had while it was genuinely sharing space, so hiding
        a panel — which makes its neighbour stretch to fill — does not overwrite the
        split the user chose. */
    void rememberSizes()
    {
        const bool shared[4] = { isPanelVisible (panelBrowser),
                                 isPanelVisible (panelPatternPicker) || isPanelVisible (panelPlaylist),
                                 isPanelVisible (panelChannelRack) && isPanelVisible (panelMixer),
                                 isPanelVisible (panelPatternPicker) && isPanelVisible (panelPlaylist) };
        const double current[4] = { static_cast<double> (columns.getItemCurrentAbsoluteSize (0)),
                                    static_cast<double> (columns.getItemCurrentAbsoluteSize (4)),
                                    static_cast<double> (centre.getItemCurrentAbsoluteSize (2)),
                                    static_cast<double> (right.getItemCurrentAbsoluteSize (0)) };
        for (int i = 0; i < 4; ++i)
            if (shared[i] && current[i] > 1.0)
                desired[i] = current[i];
    }

    void applyVisibility (int width, int height)
    {
        const auto browserOn = isPanelVisible (panelBrowser);
        const auto rackOn = isPanelVisible (panelChannelRack);
        const auto mixerOn = isPanelVisible (panelMixer);
        const auto pickerOn = isPanelVisible (panelPatternPicker);
        const auto playlistOn = isPanelVisible (panelPlaylist);

        panels[panelBrowser]->setVisible (browserOn);
        panels[panelChannelRack]->setVisible (rackOn);
        panels[panelMixer]->setVisible (mixerOn);
        panels[panelPatternPicker]->setVisible (pickerOn);
        panels[panelPlaylist]->setVisible (playlistOn);

        columnBars[0]->setVisible (browserOn);
        columnBars[1]->setVisible (pickerOn || playlistOn);
        centreBar->setVisible (rackOn && mixerOn);
        rightBar->setVisible (pickerOn && playlistOn);

        const auto rightOn = pickerOn || playlistOn;
        const auto browserWidth = browserOn ? desired[0] : 0.0;
        const auto rightWidth = rightOn ? desired[1] : 0.0;
        const auto sideBars = (browserOn ? barSize : 0) + (rightOn ? barSize : 0);

        if (browserOn) columns.setItemLayout (0, 140, 480, browserWidth);
        else           columns.setItemLayout (0, 0, 0, 0);
        setBar (columns, 1, browserOn);

        if (rightOn) columns.setItemLayout (4, 200, 900, rightWidth);
        else         columns.setItemLayout (4, 0, 0, 0);
        setBar (columns, 3, rightOn);

        // A negative preferred size is a fraction of the whole area, which would starve
        // the fixed columns, so the stretchy item gets what is actually left over.
        columns.setItemLayout (2, 260, 4000,
                               std::max (260.0, width - browserWidth - rightWidth - sideBars));

        // The Channel Rack grows with the window and the Mixer keeps a fixed height;
        // on the right it is the other way round, so the Playlist gets the space.
        setStack (centre, rackOn, mixerOn, desired[2], false, height);
        setStack (right, pickerOn, playlistOn, desired[3], true, height);
    }

    static void setBar (StretchableLayoutManager& manager, int index, bool present)
    {
        const auto size = present ? barSize : 0;
        manager.setItemLayout (index, size, size, size);
    }

    static void setStack (StretchableLayoutManager& manager, bool firstOn, bool secondOn,
                          double fixedSize, bool fixedIsFirst, int available)
    {
        const auto fixIndex = fixedIsFirst ? 0 : 2;
        const bool bothOn = firstOn && secondOn;
        const auto fixed = bothOn ? fixedSize : 0.0;
        const auto stretchy = std::max (40.0, available - fixed - (bothOn ? barSize : 0));

        for (int item : { 0, 2 })
        {
            const auto on = item == 0 ? firstOn : secondOn;
            if (! on)
                manager.setItemLayout (item, 0, 0, 0);
            else if (item == fixIndex && bothOn)
                manager.setItemLayout (item, 80, 3000, fixed);
            else
                manager.setItemLayout (item, 40, 4000, stretchy);
        }

        setBar (manager, 1, bothOn);
    }

    double desired[4] = { 190.0, 460.0, 290.0, 170.0 };

    std::unique_ptr<ProjectBrowser> browser;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<MixerPanel> mixer;
    std::unique_ptr<PatternPicker> picker;
    std::unique_ptr<PlaylistPanel> playlist;
    std::unique_ptr<Panel> panels[numPanels];

    std::unique_ptr<PianoRollWindow> pianoRollWindow;
    Component centreHolder, rightHolder;
    StretchableLayoutManager columns, centre, right;
    std::unique_ptr<StretchableLayoutResizerBar> columnBars[2], centreBar, rightBar;
};
}
