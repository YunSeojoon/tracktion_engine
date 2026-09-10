#pragma once

#include "../common/Components.h"
#include "Model.h"

namespace live
{
/** The four-area FL-style work surface: Browser on the left, Channel Rack above the
    Mixer in the centre, and the Pattern picker above the Playlist on the right.

    Every panel reads the live model and every control writes back to it through the
    Edit's undo manager, so a click here and an external AI edit take the same path.
    Panel sizes, visibility and the current selection are stored in the Edit, so a
    reopened session comes back with the same work surface.
*/
namespace layoutIds
{
    const Identifier LAYOUT ("COCOMPOSELAYOUT");
    const Identifier visible ("visible"), sizes ("sizes"), selectedChannel ("selectedChannel"),
        selectedPattern ("selectedPattern"), selectedLane ("selectedLane"), patternMode ("patternMode");
}

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
/** What the panels agree is currently being worked on. Stored in the Edit so it
    survives a reopen; not undoable, because selecting is not an edit. */
class Selection
{
public:
    explicit Selection (ValueTree layoutState) : state (layoutState) {}

    String channel() const { return state[layoutIds::selectedChannel].toString(); }
    String pattern() const { return state[layoutIds::selectedPattern].toString(); }
    String lane()    const { return state[layoutIds::selectedLane].toString(); }

    void setChannel (const String& value) { state.setProperty (layoutIds::selectedChannel, value, nullptr); }
    void setPattern (const String& value) { state.setProperty (layoutIds::selectedPattern, value, nullptr); }
    void setLane (const String& value)    { state.setProperty (layoutIds::selectedLane, value, nullptr); }

    ValueTree state;
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
/** One channel: name, mute/solo, fader, mixer insert, and how much the selected
    pattern asks this channel to play. */
class ChannelRow final : public Component
{
public:
    ChannelRow (Model& m, Selection& s, const String& channelID, std::function<void()> onChange)
        : model (m), selection (s), id (channelID), changed (std::move (onChange))
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
        mute.onClick = [this] { toggle (ids::mute, mute.getToggleState(), "Mute channel"); };
        solo.onClick = [this] { toggle (ids::solo, solo.getToggleState(), "Solo channel"); };

        gain.setSliderStyle (Slider::LinearHorizontal);
        gain.setRange (-60.0, 6.0, 0.1);
        gain.setTextBoxStyle (Slider::TextBoxRight, false, 52, 20);
        gain.setTextValueSuffix (" dB");
        gain.onValueChange = [this] { toggle (ids::gainDb, gain.getValue(), "Channel volume"); };

        insert.setSliderStyle (Slider::IncDecButtons);
        insert.setRange (1.0, 256.0, 1.0);
        insert.setTextBoxStyle (Slider::TextBoxLeft, false, 40, 20);
        insert.onValueChange = [this] { toggle (ids::insert, static_cast<int> (insert.getValue()), "Mixer insert"); };

        instrument.onClick = [this]
        {
            if (auto* track = model.trackFor (id))
                for (auto* plugin : track->pluginList)
                    if (plugin->getName() != "Volume & Pan" && plugin->getName() != "Level Meter")
                    {
                        plugin->showWindowExplicitly();
                        return;
                    }
        };

        notes.setColour (Label::textColourId, Colour (0xff8698b6));
        notes.setJustificationType (Justification::centredRight);

        for (auto* child : std::initializer_list<Component*> { &name, &mute, &solo, &gain, &insert, &instrument, &notes })
            addAndMakeVisible (*child);
    }

    void paint (Graphics& g) override
    {
        g.setColour (selection.channel() == id ? Colour (0xff2f4f5f) : Colour (0xff222b3b));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 3.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 3);
        name.setBounds (r.removeFromLeft (150));
        mute.setBounds (r.removeFromLeft (34).reduced (2));
        solo.setBounds (r.removeFromLeft (34).reduced (2));
        instrument.setBounds (r.removeFromLeft (74).reduced (2));
        insert.setBounds (r.removeFromRight (96).reduced (2));
        notes.setBounds (r.removeFromRight (110));
        gain.setBounds (r.reduced (2, 0));
    }

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

        if (! name.isBeingEdited()) name.setText (tree[ids::name].toString(), dontSendNotification);
        mute.setToggleState (static_cast<bool> (tree[ids::mute]), dontSendNotification);
        solo.setToggleState (static_cast<bool> (tree[ids::solo]), dontSendNotification);
        gain.setValue (static_cast<double> (tree[ids::gainDb]), dontSendNotification);
        insert.setValue (static_cast<double> (tree[ids::insert]), dontSendNotification);

        auto sequence = Model::findSequence (model.patternFor (selection.pattern()), id);
        const auto count = sequence.isValid() ? sequence.getNumChildren() : 0;
        notes.setText (count == 0 ? "no notes in pattern" : String (count) + " notes in pattern",
                       dontSendNotification);
        repaint();
    }

    const String id;

private:
    ValueTree channel() const { return model.channelFor (id); }

    template <typename Value>
    void toggle (const Identifier& property, Value value, const String& description)
    {
        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction (description);
        channel().setProperty (property, value, &undo);
        model.renderIfNeeded();
    }

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    Label name;
    TextButton mute { "M" }, solo { "S" }, instrument { "Instrument" };
    Slider gain, insert;
    Label notes;
};

//==============================================================================
class ChannelRack final : public Component
{
public:
    ChannelRack (Model& m, Selection& s, std::function<void()> onChange)
        : model (m), selection (s), changed (std::move (onChange))
    {
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
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        addAndMakeVisible (add);
        addAndMakeVisible (remove);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (26);
        add.setBounds (bar.removeFromLeft (110).reduced (2));
        remove.setBounds (bar.removeFromLeft (130).reduced (2));
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
        for (auto* row : channelRows)
            row->refresh();
    }

private:
    void layoutRows()
    {
        const auto width = std::max (520, viewport.getWidth() - viewport.getScrollBarThickness());
        rows.setSize (width, std::max (viewport.getHeight(), channelRows.size() * rowHeight));
        for (int i = 0; i < channelRows.size(); ++i)
            channelRows[i]->setBounds (0, i * rowHeight, width, rowHeight);
    }

    static constexpr int rowHeight = 30;

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    Viewport viewport;
    Component rows;
    OwnedArray<ChannelRow> channelRows;
    TextButton add { "+ Channel" }, remove { "Remove channel" };
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
/** Playlist lanes beside the engine timeline. Placing and moving clips by mouse is
    M3; for now the lane list and the Place button drive the arrangement and the
    timeline shows the clips the model produced. */
class PlaylistPanel final : public Component,
                            private ListBoxModel
{
public:
    PlaylistPanel (Model& m, Selection& s, te::SelectionManager& sm, std::function<void()> onChange)
        : model (m), selection (s), changed (std::move (onChange)),
          timeline (std::make_unique<EditComponent> (m.edit, sm))
    {
        list.setModel (this);
        list.setRowHeight (20);
        list.setColour (ListBox::backgroundColourId, Colours::transparentBlack);

        addLane.onClick = [this]
        {
            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Add playlist lane");
            auto lane = model.addLane ("Lane " + String (model.lanes().getNumChildren() + 1), &undo);
            model.renderIfNeeded();
            selection.setLane (Model::uidOf (lane));
            notify();
        };

        place.onClick = [this]
        {
            auto lane = model.laneFor (selection.lane());
            auto pattern = model.patternFor (selection.pattern());
            if (! lane.isValid() || ! pattern.isValid())
                return;

            auto& undo = model.edit.getUndoManager();
            undo.beginNewTransaction ("Place pattern");
            model.addInstance (selection.lane(), selection.pattern(),
                               model.laneEndBeat (selection.lane()), &undo);
            model.renderIfNeeded();
            notify();
        };

        // Track headers, footers and device rows do not fit a side panel, and the rack
        // already carries the per-channel controls; the timeline needs the width.
        auto& view = timeline->getEditViewState();
        view.showHeaders = false;
        view.showFooters = false;
        view.showMidiDevices = false;
        view.showWaveDevices = false;
        view.viewX1 = te::TimePosition();
        view.viewX2 = te::TimePosition::fromSeconds (20.0);

        addAndMakeVisible (list);
        addAndMakeVisible (*timeline);
        addAndMakeVisible (addLane);
        addAndMakeVisible (place);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (26);
        addLane.setBounds (bar.removeFromLeft (80).reduced (2));
        place.setBounds (bar.removeFromLeft (110).reduced (2));
        list.setBounds (r.removeFromLeft (jlimit (70, 140, r.getWidth() / 4)));
        timeline->setBounds (r.withTrimmedLeft (4));
    }

    void refresh()
    {
        StringArray rebuilt;
        for (auto lane : model.lanes())
        {
            const auto laneID = Model::uidOf (lane);
            int count = 0;
            for (auto instance : model.instances())
                if (instance[ids::lane].toString() == laneID)
                    ++count;
            rebuilt.add (laneID + "\t" + lane[ids::name].toString() + "\t" + String (count));
        }

        if (rebuilt != entries)
        {
            entries = rebuilt;
            list.updateContent();
        }

        place.setEnabled (model.laneFor (selection.lane()).isValid()
                           && model.patternFor (selection.pattern()).isValid());
        list.repaint();
        timeline->repaint();
    }

private:
    void notify() { if (changed != nullptr) changed(); }

    int getNumRows() override { return entries.size(); }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool) override
    {
        if (! isPositiveAndBelow (row, entries.size()))
            return;

        const auto fields = StringArray::fromTokens (entries[row], "\t", "");
        if (fields[0] == selection.lane())
        {
            g.setColour (Colour (0xff2f4f5f));
            g.fillRect (0, 0, width, height);
        }

        g.setColour (Colours::white.withAlpha (0.9f));
        g.setFont (Font (FontOptions (13.0f)));
        g.drawText (fields[1], 6, 0, width - 46, height, Justification::centredLeft);
        g.setColour (Colour (0xff8698b6));
        g.setFont (Font (FontOptions (11.0f)));
        g.drawText (fields[2] + "x", width - 42, 0, 36, height, Justification::centredRight);
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (! isPositiveAndBelow (row, entries.size()))
            return;

        selection.setLane (StringArray::fromTokens (entries[row], "\t", "")[0]);
        notify();
    }

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    std::unique_ptr<EditComponent> timeline;
    ListBox list;
    StringArray entries;
    TextButton addLane { "+ Lane" }, place { "Place pattern" };
};

//==============================================================================
class Workspace final : public Component
{
public:
    Workspace (Model& m, te::SelectionManager& sm, te::Engine& engine)
        : model (m),
          layout (m.edit.state.getOrCreateChildWithName (layoutIds::LAYOUT, nullptr)),
          selection (layout)
    {
        auto onChange = [this] { refresh(); };

        browser = std::make_unique<ProjectBrowser> (model, selection, engine, onChange);
        rack = std::make_unique<ChannelRack> (model, selection, onChange);
        mixer = std::make_unique<MixerPanel> (model);
        picker = std::make_unique<PatternPicker> (model, selection, onChange);
        playlist = std::make_unique<PlaylistPanel> (model, selection, sm, onChange);

        Component* contents[numPanels] = { browser.get(), rack.get(), mixer.get(), picker.get(), playlist.get() };
        for (int i = 0; i < numPanels; ++i)
        {
            panels[i] = std::make_unique<Panel> (panelName (i), *contents[i]);
            addAndMakeVisible (*panels[i]);
        }

        const auto stored = StringArray::fromTokens (layout[layoutIds::sizes].toString(), " ", "");
        const double fallbacks[4] = { 210.0, 520.0, 290.0, 170.0 };
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

    double desired[4] = { 210.0, 520.0, 290.0, 170.0 };

    std::unique_ptr<ProjectBrowser> browser;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<MixerPanel> mixer;
    std::unique_ptr<PatternPicker> picker;
    std::unique_ptr<PlaylistPanel> playlist;
    std::unique_ptr<Panel> panels[numPanels];

    Component centreHolder, rightHolder;
    StretchableLayoutManager columns, centre, right;
    std::unique_ptr<StretchableLayoutResizerBar> columnBars[2], centreBar, rightBar;
};
}
