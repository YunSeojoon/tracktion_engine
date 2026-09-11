#pragma once

#include "Model.h"
#include "PianoRoll.h"
#include "PlaylistGrid.h"
#include "Browser.h"
#include "Attachment.h"
#include "ChatPanel.h"

namespace live
{
/** The four-area FL-style work surface: Browser on the left, Channel Rack above the
    Mixer in the centre, and the Pattern picker above the Playlist on the right.

    Every panel reads the live model and every control writes back to it through the
    Edit's undo manager, so a click here and an external AI edit take the same path.
    Panel sizes, visibility and the current selection are stored in the Edit, so a
    reopened session comes back with the same work surface.
*/

enum PanelIndex { panelBrowser = 0, panelChannelRack, panelMixer, panelPatternPicker,
                  panelPlaylist, panelChat, numPanels };

inline const char* panelName (int index)
{
    switch (index)
    {
        case panelBrowser:       return "Browser";
        case panelChannelRack:   return "Channel Rack";
        case panelMixer:         return "Mixer";
        case panelPatternPicker: return "Pattern picker";
        case panelChat:          return "AI chat";
        default:                 return "Playlist";
    }
}

//==============================================================================
/** A titled, focusable frame around one panel's content, with the three window
    controls a person expects on a pane: collapse to its header, fill the surface,
    and put it away. Closing a panel is a layout decision and nothing more - it never
    throws away what the panel was holding. */
class Panel final : public Component
{
public:
    Panel (const String& panelTitle, Component& panelContent)
        : title (panelTitle), content (panelContent)
    {
        setWantsKeyboardFocus (true);
        addAndMakeVisible (content);

        // Drawn rather than typed, so they stay sharp at any display scale and do not
        // depend on a font having the glyphs.
        for (auto* button : { &collapse, &maximise, &close })
        {
            button->setWantsKeyboardFocus (false);
            addAndMakeVisible (*button);
        }

        collapse.onClick = [this] { if (onCollapse)  onCollapse(); };
        maximise.onClick = [this] { if (onMaximise)  onMaximise(); };
        close.onClick    = [this] { if (onClose)     onClose(); };

        collapse.setTooltip ("Minimise");
        maximise.setTooltip ("Maximise");
        close.setTooltip ("Close");
    }

    /** What the buttons ask the workspace to do. The panel itself decides nothing. */
    std::function<void()> onCollapse, onMaximise, onClose;

    void setCollapsed (bool shouldBeCollapsed)
    {
        collapsed = shouldBeCollapsed;
        content.setVisible (! collapsed);
        resized();
        repaint();
    }

    void setMaximised (bool shouldBeMaximised)
    {
        maximised = shouldBeMaximised;
        maximise.setShape (maximised);
        repaint();
    }

    bool isCollapsed() const { return collapsed; }

    void paint (Graphics& g) override
    {
        const auto focused = hasKeyboardFocus (true);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::panelHeader);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (focused ? theme::accent : theme::edge);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, focused ? 1.8f : 1.0f);
        g.setColour (focused ? theme::accent : theme::textDim);
        g.setFont (Font (FontOptions (12.0f, Font::bold)));
        // Leave room for the three controls so a long title never runs under them.
        g.drawText (title.toUpperCase(),
                    getLocalBounds().removeFromTop (headerHeight).reduced (9, 0)
                        .withTrimmedRight (3 * (headerHeight - 4)),
                    Justification::centredLeft);
    }

    void resized() override
    {
        auto header = getLocalBounds().removeFromTop (headerHeight).reduced (6, 4);
        const auto button = header.getHeight() + 4;
        close.setBounds (header.removeFromRight (button));
        maximise.setBounds (header.removeFromRight (button));
        collapse.setBounds (header.removeFromRight (button));

        content.setBounds (getLocalBounds().withTrimmedTop (headerHeight).reduced (6, 5));
    }

    void mouseDown (const MouseEvent&) override { grabKeyboardFocus(); }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override   { repaint(); }

    static constexpr int headerHeight = 22;

    /** The three controls, drawn as shapes. A button says what it does by its mark, not
        only by its colour, and the maximise mark changes once the panel is filling the
        surface so the same button reads as "put it back". */
    struct HeaderButton final : public Button
    {
        enum Kind { minimise, maximiseOrRestore, dismiss };

        explicit HeaderButton (Kind k) : Button ({}), kind (k) {}

        void setShape (bool restore) { showsRestore = restore; repaint(); }

        void paintButton (Graphics& g, bool over, bool down) override
        {
            const auto area = getLocalBounds().toFloat();
            if (over || down)
            {
                g.setColour (kind == dismiss ? theme::danger.withAlpha (down ? 0.5f : 0.3f)
                                             : theme::edgeStrong.withAlpha (down ? 0.9f : 0.6f));
                g.fillRoundedRectangle (area, 2.0f);
            }

            const auto mark = area.withSizeKeepingCentre (8.0f, 8.0f);
            g.setColour (over ? theme::text : theme::textDim);

            if (kind == minimise)
            {
                g.fillRect (mark.getX(), mark.getBottom() - 1.0f, mark.getWidth(), 1.0f);
            }
            else if (kind == maximiseOrRestore)
            {
                if (showsRestore)
                {
                    // Two overlapping frames: the shape of coming back to a smaller size.
                    g.drawRect (mark.withTrimmedRight (2.0f).withTrimmedTop (2.0f), 1.0f);
                    g.drawRect (mark.withTrimmedLeft (2.0f).withTrimmedBottom (2.0f), 1.0f);
                }
                else
                {
                    g.drawRect (mark, 1.0f);
                }
            }
            else
            {
                g.drawLine (mark.getX(), mark.getY(), mark.getRight(), mark.getBottom(), 1.2f);
                g.drawLine (mark.getX(), mark.getBottom(), mark.getRight(), mark.getY(), 1.2f);
            }
        }

        Kind kind;
        bool showsRestore = false;
    };

    HeaderButton collapse { HeaderButton::minimise };
    HeaderButton maximise { HeaderButton::maximiseOrRestore };
    HeaderButton close    { HeaderButton::dismiss };
    bool collapsed = false, maximised = false;

    String title;
    Component& content;
};


//==============================================================================
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

            g.setColour (lit ? theme::good
                             : onBeat ? Colour (0xff39455c) : theme::edge);
            g.fillRoundedRectangle (area.toFloat(), 2.0f);

            if (! lit && step % 16 == 0 && step > 0)
            {
                g.setColour (theme::textFaint);
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
        presets.onClick = [this] { showPresets(); };

        for (auto* child : std::initializer_list<Component*> { &name, &mute, &solo, &gain, &pan, &insert,
                                                              &instrument, &openInstrument, &sample, &stepSettings,
                                                              &presets, &steps })
            addAndMakeVisible (*child);
    }

    void paint (Graphics& g) override
    {
        g.setColour (selection.channel() == id ? theme::selection : theme::row);
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
        presets.setBounds (r.removeFromLeft (30).reduced (1));
        steps.setBounds (r.withTrimmedLeft (8));
    }

    static constexpr int controlsWidth = 540;

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

    File presetFolder() const
    {
        return File::getSpecialLocation (File::userApplicationDataDirectory)
                 .getChildFile ("CoCompose").getChildFile ("presets");
    }

    /** Keeps an instrument's settings under a name so another channel, or another
        song, can start from it. The file is the plugin's own state, so it only loads
        back onto the same kind of instrument. */
    void showPresets()
    {
        auto tree = channel();
        auto* track = model.trackFor (id);
        auto* instrumentPlugin = track != nullptr ? Model::instrumentOf (*track) : nullptr;

        if (! tree.isValid() || instrumentPlugin == nullptr)
            return;

        presetFiles = presetFolder().findChildFiles (File::findFiles, false, "*.cocomposepreset");

        PopupMenu menu;
        menu.addItem (1, "Save preset...");

        if (! presetFiles.isEmpty())
        {
            const auto wanted = Model::kindOf (instrumentPlugin);

            PopupMenu load;
            for (int i = 0; i < presetFiles.size(); ++i)
            {
                const auto savedFor = presetInstrument (presetFiles[i]);
                const auto fits = savedFor == wanted;
                // One that does not fit is still shown, so it is clear it exists and what
                // it is for, rather than quietly missing from the list.
                load.addItem (100 + i,
                              presetFiles[i].getFileNameWithoutExtension()
                                + (fits ? String() : "  (for " + readableInstrument (savedFor) + ")"),
                              fits);
            }

            menu.addSubMenu ("Load preset", load);
        }

        menu.addItem (2, "Open the presets folder", ! presetFiles.isEmpty());

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (presets),
            [this] (int choice)
            {
                if (choice == 1) savePreset();
                else if (choice == 2) presetFolder().revealToUser();
                else if (choice >= 100 && isPositiveAndBelow (choice - 100, presetFiles.size()))
                    loadPreset (presetFiles[choice - 100]);
            });
    }

public:
    /** Saves the instrument's settings under a name derived from the channel. */
    File savePreset()
    {
        auto* track = model.trackFor (id);
        auto* instrumentPlugin = track != nullptr ? Model::instrumentOf (*track) : nullptr;
        if (instrumentPlugin == nullptr || ! presetFolder().createDirectory().wasOk())
            return {};

        instrumentPlugin->flushPluginStateToValueTree();

        auto state = instrumentPlugin->state.createCopy();
        state.setProperty ("coComposePresetFor", Model::kindOf (instrumentPlugin), nullptr);
        state.removeProperty ("id", nullptr);

        auto file = presetFolder().getChildFile (File::createLegalFileName (
                        channel()[ids::name].toString() + " " + instrumentPlugin->getName()) + ".cocomposepreset");

        for (int suffix = 2; file.existsAsFile(); ++suffix)
            file = presetFolder().getChildFile (File::createLegalFileName (
                       channel()[ids::name].toString() + " " + instrumentPlugin->getName()
                         + " " + String (suffix)) + ".cocomposepreset");

        if (auto xml = state.createXml())
            if (xml->writeTo (file))
                return file;

        return {};
    }

    /** What a preset file was saved from, or an empty string if it is not one. */
    static String presetInstrument (const File& file)
    {
        if (auto xml = parseXML (file))
            if (auto state = ValueTree::fromXml (*xml); state.isValid())
                return state["coComposePresetFor"].toString();

        return {};
    }

    /** A short name for an instrument identifier, for saying what a preset is for. */
    String readableInstrument (const String& identifier) const
    {
        if (identifier == builtInSynth)   return "4OSC";
        if (identifier == builtInSampler) return "Sampler";

        for (const auto& available : model.availableInstruments())
            if (available.first == identifier)
                return available.second;

        // A plugin that is not installed here still has its name inside the identifier,
        // which is more use than showing the whole thing.
        const auto parts = StringArray::fromTokens (identifier, "-", {});
        return parts.size() >= 2 ? parts[1] : identifier;
    }

    /** Empty when it loaded, otherwise why it did not. Presets carry a plugin's own
        state, so one only means anything on the instrument it came from. */
    String loadPresetWithReason (const File& file)
    {
        auto* track = model.trackFor (id);
        auto* instrumentPlugin = track != nullptr ? Model::instrumentOf (*track) : nullptr;
        if (instrumentPlugin == nullptr)
            return "This channel has no instrument to load a preset onto";

        auto xml = parseXML (file);
        if (xml == nullptr)
            return file.getFileName() + " is not a preset file";

        auto state = ValueTree::fromXml (*xml);
        if (! state.isValid())
            return file.getFileName() + " could not be read";

        const auto savedFor = state["coComposePresetFor"].toString();
        const auto wanted = Model::kindOf (instrumentPlugin);

        if (savedFor.isEmpty())
            return file.getFileNameWithoutExtension() + " does not say which instrument it is for";

        // A preset for a different instrument would restore nothing useful.
        if (savedFor != wanted)
            return file.getFileNameWithoutExtension() + " is for " + readableInstrument (savedFor)
                     + ", and this channel is playing " + readableInstrument (wanted);

        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction ("Load preset");

        for (int i = 0; i < state.getNumProperties(); ++i)
        {
            const auto property = state.getPropertyName (i);
            if (property != Identifier ("id") && property != Identifier ("coComposePresetFor"))
                instrumentPlugin->state.setProperty (property, state[property], &undo);
        }

        instrumentPlugin->restorePluginStateFromValueTree (instrumentPlugin->state);
        model.renderIfNeeded();
        if (changed != nullptr) changed();
        return {};
    }

    bool loadPreset (const File& file) { return loadPresetWithReason (file).isEmpty(); }

    File newestPreset() const
    {
        auto found = presetFolder().findChildFiles (File::findFiles, false, "*.cocomposepreset");
        std::sort (found.begin(), found.end(), [] (const File& a, const File& b)
                   { return a.getLastModificationTime() > b.getLastModificationTime(); });
        return found.isEmpty() ? File() : found.getFirst();
    }

private:

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
    TextButton mute { "M" }, solo { "S" }, openInstrument { "..." }, sample { "WAV" },
               stepSettings { "1/16" }, presets { "P" };
    ComboBox instrument;
    Slider gain, pan, insert;
    StepGrid steps;
    Array<std::pair<String, String>> instruments;
    Array<File> presetFiles;
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

    ChannelRow* selectedRow() const
    {
        for (auto* row : channelRows)
            if (row->id == selection.channel())
                return row;
        return nullptr;
    }

    /** Opens the selected channel's instrument window, which is how a check can see
        whether a plugin's own editor comes up on this machine. */
    bool openSelectedPlugin() const
    {
        if (auto* row = selectedRow())
            if (auto* track = model.trackFor (row->id))
                if (auto* plugin = Model::instrumentOf (*track))
                {
                    plugin->showWindowExplicitly();
                    return true;
                }
        return false;
    }

    bool closePluginWindows() const
    {
        for (auto* track : te::getAudioTracks (model.edit))
            for (auto* plugin : track->pluginList)
                if (plugin->windowState != nullptr)
                    plugin->windowState->closeWindowExplicitly();
        return true;
    }

    bool saveSelectedPreset() const
    {
        auto* row = selectedRow();
        return row != nullptr && row->savePreset().existsAsFile();
    }

    /** Empty when it loaded, otherwise why not, so the app can say rather than do
        nothing when a preset does not fit the instrument that is there. */
    String loadNewestPresetOnSelected() const
    {
        auto* row = selectedRow();
        if (row == nullptr)
            return "No channel is selected";

        const auto newest = row->newestPreset();
        if (! newest.existsAsFile())
            return "There are no saved presets yet";

        return row->loadPresetWithReason (newest);
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
/** One mixer strip: a meter, the fader and pan, mute, the effect chain, the sends,
    and where the strip is routed. */
class MixerStrip final : public Component
{
public:
    MixerStrip (Model& m, const String& insertID, te::VolumeAndPanPlugin* masterPlugin,
                std::function<void()> onChange)
        : model (m), id (insertID), master (masterPlugin), changed (std::move (onChange))
    {
        name.setJustificationType (Justification::centred);
        name.setColour (Label::textColourId, Colours::white);
        name.setFont (Font (FontOptions (12.0f, Font::bold)));
        name.setEditable (false, master == nullptr, false);
        name.onTextChange = [this] { write (ids::name, name.getText(), "Rename insert"); };

        feeds.setJustificationType (Justification::centred);
        feeds.setColour (Label::textColourId, theme::textDim);
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

        effects.onClick = [this] { showEffectsMenu(); };
        routing.onClick = [this] { showRoutingMenu(); };
        effects.setEnabled (master == nullptr);
        routing.setEnabled (master == nullptr);

        chain.setJustificationType (Justification::centredLeft);
        chain.setColour (Label::textColourId, theme::accent);
        chain.setFont (Font (FontOptions (10.0f)));

        for (auto* child : std::initializer_list<Component*> { &name, &gain, &pan, &mute, &feeds, &effects,
                                                               &routing, &chain })
            addAndMakeVisible (*child);
    }

    ~MixerStrip() override
    {
        if (attached != nullptr)
            attached->measurer.removeClient (client);
    }

    void paint (Graphics& g) override
    {
        g.setColour (master != nullptr ? theme::selection : theme::row);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 3.0f);
        g.setColour (theme::edge);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 3.0f, 1.0f);

        // The meter sits beside the fader so a strip shows what it is actually passing.
        const auto meterArea = meterBounds();
        g.setColour (theme::sunken);
        g.fillRect (meterArea);

        // Green up to -9, amber to -1, red at the top: the bands are where they are so a
        // person can see they are close to clipping before they are.
        const auto asFraction = [] (float db) { return jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f); };
        const auto fill = [&] (float from, float to, Colour colour)
        {
            const auto top = meterArea.getBottom() - roundToInt (asFraction (to) * meterArea.getHeight());
            const auto bottom = meterArea.getBottom() - roundToInt (asFraction (from) * meterArea.getHeight());
            if (bottom > top)
            {
                g.setColour (colour);
                g.fillRect (meterArea.getX(), top, meterArea.getWidth(), bottom - top);
            }
        };

        fill (-60.0f, jmin (peak, -9.0f), theme::good);
        if (peak > -9.0f) fill (-9.0f, jmin (peak, -1.0f), theme::warn);
        if (peak > -1.0f) fill (-1.0f, peak, theme::danger);

        // The held peak, so a short transient is still visible a moment later.
        if (held > -59.0f)
        {
            const auto at = meterArea.getBottom() - roundToInt (asFraction (held) * meterArea.getHeight());
            g.setColour (held > -1.0f ? theme::danger : theme::text);
            g.fillRect (meterArea.getX(), at - 1, meterArea.getWidth(), 1);
        }
    }


    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 5);
        name.setBounds (r.removeFromTop (16));
        feeds.setBounds (r.removeFromBottom (13));
        routing.setBounds (r.removeFromBottom (19).reduced (2, 1));
        chain.setBounds (r.removeFromBottom (13));
        effects.setBounds (r.removeFromBottom (19).reduced (2, 1));
        mute.setBounds (r.removeFromBottom (20).reduced (6, 1));
        pan.setBounds (r.removeFromBottom (18).reduced (2, 0));
        gain.setBounds (r.removeFromRight (r.getWidth() - 12));
    }

    void refresh()
    {
        const auto previous = peak;
        peak = readPeak();
        // A strip is drawn with arcs and paths, so repainting all of them every tick is
        // real work. The meter only needs redrawing when it would look different.
        meterMoved = std::abs (peak - previous) > 0.4f;

        if (master != nullptr)
        {
            name.setText ("Master", dontSendNotification);
            gain.setValue (master->getVolumeDb(), dontSendNotification);
            pan.setValue (master->getPan(), dontSendNotification);
            feeds.setText ("all inserts", dontSendNotification);
            chain.setText ({}, dontSendNotification);
            if (meterMoved)
                repaint();
            return;
        }

        auto insert = model.insertFor (id);
        if (! insert.isValid())
            return;

        const auto slot = static_cast<int> (insert[ids::index]);
        if (! name.isBeingEdited())
            name.setText (String (slot) + "  " + insert[ids::name].toString(), dontSendNotification);
        gain.setValue (static_cast<double> (insert[ids::gainDb]), dontSendNotification);
        pan.setValue (static_cast<double> (insert[ids::pan]), dontSendNotification);
        mute.setToggleState (static_cast<bool> (insert[ids::mute]), dontSendNotification);

        StringArray names;
        int sendCount = 0;
        for (auto child : insert)
        {
            if (child.hasType (ids::EFFECT))
                names.add (static_cast<bool> (child[ids::bypass]) ? "(" + child[ids::type].toString() + ")"
                                                                  : child[ids::type].toString());
            else if (child.hasType (ids::SEND))
                ++sendCount;
        }

        chain.setText (names.isEmpty() ? "no effects" : names.joinIntoString (" > "), dontSendNotification);
        chain.setTooltip (chain.getText());

        const auto destination = insert.getProperty (ids::output, masterInsert).toString();
        auto target = model.insertFor (destination);
        routing.setButtonText (target.isValid() ? "> " + String (static_cast<int> (target[ids::index]))
                                                : "> Master");

        int fed = 0;
        for (auto channel : model.channels())
            if (static_cast<int> (channel[ids::insert]) == slot)
                ++fed;

        feeds.setText (String (fed) + (fed == 1 ? " ch" : " chs")
                        + (sendCount > 0 ? ", " + String (sendCount) + " snd" : ""), dontSendNotification);
        if (meterMoved)
            repaint();
    }

    const String id;

private:
    Rectangle<int> meterBounds() const
    {
        auto r = getLocalBounds().reduced (4, 5);
        r.removeFromTop (16);
        r.removeFromBottom (13 + 19 + 13 + 19 + 20 + 18);
        return r.removeFromLeft (10).reduced (1, 2);
    }

    /** A level meter reports to registered clients, so the strip keeps one attached to
        whichever meter belongs to it and reads what has arrived since the last frame. */
    te::LevelMeterPlugin* findMeter() const
    {
        if (master != nullptr)
        {
            for (auto* plugin : te::getAllPlugins (model.edit, true))
                if (auto* meter = dynamic_cast<te::LevelMeterPlugin*> (plugin))
                    if (meter->getOwnerTrack() == nullptr)
                        return meter;
            return nullptr;
        }

        if (auto* track = model.trackFor (Model::insertTrackID (id)))
            for (auto* plugin : track->pluginList)
                if (auto* meter = dynamic_cast<te::LevelMeterPlugin*> (plugin))
                    return meter;

        return nullptr;
    }

    float readPeak()
    {
        auto* meter = findMeter();

        if (meter != attached)
        {
            if (attached != nullptr)
                attached->measurer.removeClient (client);

            attached = meter;

            if (attached != nullptr)
                attached->measurer.addClient (client);
        }

        if (attached == nullptr)
            return -100.0f;

        const auto left = client.getAndClearAudioLevel (0).dB;
        const auto right = client.getAndClearAudioLevel (1).dB;
        const auto now = std::max (left, right);

        // Meters fall back gently instead of flickering with every buffer.
        held = now > held ? now : std::max (now, held - 3.0f);
        return held;
    }

    void notify() { if (changed != nullptr) changed(); }

    template <typename Value>
    void write (const Identifier& property, Value value, const String& description)
    {
        auto& undo = model.edit.getUndoManager();
        undo.beginNewTransaction (description);
        model.insertFor (id).setProperty (property, value, &undo);
        model.renderIfNeeded();
        notify();
    }

    void showEffectsMenu()
    {
        auto insert = model.insertFor (id);
        if (! insert.isValid())
            return;

        effectChoices = model.availableEffects();

        PopupMenu add;
        for (int i = 0; i < effectChoices.size(); ++i)
            add.addItem (100 + i, effectChoices[i].second);

        PopupMenu menu;
        menu.addSubMenu ("Add effect", add);

        int index = 0;
        for (auto child : insert)
        {
            if (! child.hasType (ids::EFFECT))
                continue;

            PopupMenu item;
            item.addItem (1000 + index * 10 + 0, "Open");
            item.addItem (1000 + index * 10 + 1, "Bypass", true, static_cast<bool> (child[ids::bypass]));
            item.addItem (1000 + index * 10 + 2, "Move up", index > 0);
            item.addItem (1000 + index * 10 + 3, "Move down");
            item.addItem (1000 + index * 10 + 4, "Remove");
            menu.addSubMenu (String (index + 1) + ". " + model.effectName (child[ids::type].toString()), item);
            ++index;
        }

        PopupMenu sendTo;
        for (auto other : model.mixer())
            if (Model::uidOf (other) != id && ! model.wouldFeedBack (id, Model::uidOf (other)))
                sendTo.addItem (500 + static_cast<int> (other[ids::index]),
                                String (static_cast<int> (other[ids::index])) + " " + other[ids::name].toString());

        menu.addSeparator();
        menu.addSubMenu ("Send to", sendTo, sendTo.containsAnyActiveItems());

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (effects),
                            [this] (int choice) { handleEffectChoice (choice); });
    }

    void handleEffectChoice (int choice)
    {
        auto insert = model.insertFor (id);
        if (choice <= 0 || ! insert.isValid())
            return;

        auto& undo = model.edit.getUndoManager();

        if (choice >= 100 && choice < 500)
        {
            if (! isPositiveAndBelow (choice - 100, effectChoices.size()))
                return;

            undo.beginNewTransaction ("Add effect");
            model.addEffect (insert, effectChoices[choice - 100].first, &undo);
        }
        else if (choice >= 500 && choice < 1000)
        {
            auto target = model.insertForSlot (choice - 500);
            if (! target.isValid())
                return;

            undo.beginNewTransaction ("Add send");
            model.addSend (insert, Model::uidOf (target), -6.0, &undo);
        }
        else
        {
            const auto index = (choice - 1000) / 10;
            const auto action = (choice - 1000) % 10;

            int seen = 0;
            for (auto child : insert)
            {
                if (! child.hasType (ids::EFFECT))
                    continue;

                if (seen++ != index)
                    continue;

                if (action == 0)
                {
                    if (auto* track = model.trackFor (Model::insertTrackID (id)))
                        for (auto* plugin : track->pluginList)
                            if (plugin->state[ids::pluginEffect].toString() == Model::uidOf (child))
                                plugin->showWindowExplicitly();
                    return;
                }

                undo.beginNewTransaction ("Edit effect chain");
                const auto position = insert.indexOf (child);

                if (action == 1)      child.setProperty (ids::bypass, ! static_cast<bool> (child[ids::bypass]), &undo);
                else if (action == 2) insert.moveChild (position, std::max (0, position - 1), &undo);
                else if (action == 3) insert.moveChild (position, std::min (insert.getNumChildren() - 1, position + 1), &undo);
                else                  insert.removeChild (child, &undo);
                break;
            }
        }

        model.renderIfNeeded();
        notify();
    }

    void showRoutingMenu()
    {
        auto insert = model.insertFor (id);
        if (! insert.isValid())
            return;

        const auto destination = insert.getProperty (ids::output, masterInsert).toString();

        PopupMenu menu;
        menu.addItem (1, "Master", true, destination == masterInsert);
        for (auto other : model.mixer())
        {
            const auto otherID = Model::uidOf (other);
            if (otherID == id)
                continue;

            menu.addItem (100 + static_cast<int> (other[ids::index]),
                          String (static_cast<int> (other[ids::index])) + " " + other[ids::name].toString(),
                          ! model.wouldFeedBack (id, otherID), destination == otherID);
        }

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (routing),
            [this] (int choice)
            {
                if (choice <= 0)
                    return;

                if (choice == 1)
                {
                    write (ids::output, masterInsert, "Route insert");
                    return;
                }

                if (auto target = model.insertForSlot (choice - 100); target.isValid())
                    write (ids::output, Model::uidOf (target), "Route insert");
            });
    }

    Model& model;
    te::VolumeAndPanPlugin* master;
    std::function<void()> changed;
    Label name, feeds, chain;
    Slider gain, pan;
    TextButton mute { "Mute" }, effects { "FX" }, routing { "> Master" };
    te::LevelMeasurer::Client client;
    te::LevelMeterPlugin* attached = nullptr;
    float peak = -100.0f, held = -100.0f;
    bool meterMoved = true;
    Array<std::pair<String, String>> effectChoices;
};

//==============================================================================
class MixerPanel final : public Component
{
public:
    MixerPanel (Model& m, std::function<void()> onChange) : model (m), changed (std::move (onChange))
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
        StringArray wanted { masterInsert };
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
                    insertID == masterInsert ? model.edit.getMasterVolumePlugin().get() : nullptr, changed));
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
        const auto height = std::max (150, viewport.getHeight() - viewport.getScrollBarThickness());
        strips.setSize (std::max (viewport.getWidth(), mixerStrips.size() * stripWidth), height);
        for (int i = 0; i < mixerStrips.size(); ++i)
            mixerStrips[i]->setBounds (i * stripWidth, 0, stripWidth, height);
    }

    static constexpr int stripWidth = 92;

    Model& model;
    std::function<void()> changed;
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
            g.setColour (theme::selection);
            g.fillRect (0, 0, width, height);
        }

        g.setColour (Colours::white.withAlpha (0.9f));
        g.setFont (Font (FontOptions (13.0f)));
        g.drawText (fields[1], 6, 0, width - 70, height, Justification::centredLeft);
        g.setColour (theme::textDim);
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
        automate.onClick = [this] { showAutomateMenu(); };

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
        addLane.setBounds (bar.removeFromLeft (52).reduced (1));
        removeLane.setBounds (bar.removeFromLeft (52).reduced (1));
        muteLane.setBounds (bar.removeFromLeft (46).reduced (1));
        duplicate.setBounds (bar.removeFromLeft (48).reduced (1));
        makeUnique.setBounds (bar.removeFromLeft (56).reduced (1));
        remove.setBounds (bar.removeFromLeft (48).reduced (1));
        automate.setBounds (bar.removeFromLeft (72).reduced (1));
        snap.setBounds (bar.removeFromLeft (80).reduced (1));
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

    /** Scrolls the arrangement so a beat is on screen, which is what following a chat
        attachment back to the music amounts to. */
    void showBeat (double beat)
    {
        viewport.setViewPosition (jmax (0, grid->xForBeatPublic (beat) - viewport.getWidth() / 3),
                                  viewport.getViewPositionY());
    }

private:
    void notify() { if (changed != nullptr) changed(); }

    /** Offers the parameters worth automating: the selected channel's instrument and
        fader, and the effects on the insert it plays through. Choosing one opens a
        curve for it under the arrangement. */
    void showAutomateMenu()
    {
        auto channel = model.channelFor (selection.channel());
        if (! channel.isValid())
            return;

        automatable.clear();
        PopupMenu menu;

        if (auto* track = model.trackFor (Model::uidOf (channel)))
            addParameters (menu, Model::uidOf (channel), channel[ids::name].toString(), *track);

        auto insert = model.insertForSlot (static_cast<int> (channel[ids::insert]));
        if (insert.isValid())
            if (auto* track = model.trackFor (Model::insertTrackID (Model::uidOf (insert))))
                addParameters (menu, Model::uidOf (insert),
                               String (static_cast<int> (insert[ids::index])) + " " + insert[ids::name].toString(),
                               *track);

        // The same parameters again, this time to be moved by a knob rather than a curve.
        const auto firstLearnItem = 20000;
        PopupMenu learn;
        for (int i = 0; i < automatable.size(); ++i)
            learn.addItem (firstLearnItem + i, automatable.getReference (i).parameter);

        if (learn.containsAnyActiveItems())
        {
            menu.addSeparator();
            menu.addSubMenu ("MIDI learn", learn);
            if (onCancelLearn)
                menu.addItem (30000, "Cancel MIDI learn");
        }

        auto curves = model.automation();
        if (curves.getNumChildren() > 0)
        {
            PopupMenu existing;
            for (int i = 0; i < curves.getNumChildren(); ++i)
                existing.addItem (10000 + i, curves.getChild (i)[ids::parameter].toString());

            menu.addSeparator();
            menu.addSubMenu ("Remove automation", existing);
        }

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (automate),
            [this] (int choice)
            {
                if (choice <= 0)
                    return;

                if (choice == 30000)
                {
                    if (onCancelLearn)
                        onCancelLearn();
                    return;
                }

                if (choice >= 20000)
                {
                    if (onLearn && isPositiveAndBelow (choice - 20000, automatable.size()))
                    {
                        const auto& entry = automatable.getReference (choice - 20000);
                        onLearn (entry.owner, entry.plugin, entry.parameter);
                    }
                    return;
                }

                if (choice >= 10000)
                {
                    grid->removeCurve (choice - 10000);
                    notify();
                    return;
                }

                if (isPositiveAndBelow (choice - 1, automatable.size()))
                {
                    const auto& entry = automatable.getReference (choice - 1);
                    grid->automate (entry.owner, entry.plugin, entry.parameter);
                    notify();
                }
            });
    }

    void addParameters (PopupMenu& menu, const String& ownerID, const String& ownerName, te::AudioTrack& track)
    {
        PopupMenu group;

        for (auto* plugin : track.pluginList)
        {
            PopupMenu perPlugin;

            for (auto* parameter : plugin->getAutomatableParameters())
            {
                automatable.add ({ ownerID, plugin->itemID.toString(), parameter->paramID });
                perPlugin.addItem (automatable.size(), parameter->getParameterName());
            }

            if (perPlugin.containsAnyActiveItems())
                group.addSubMenu (plugin->getName(), perPlugin);
        }

        if (group.containsAnyActiveItems())
            menu.addSubMenu (ownerName, group);
    }

    struct Automatable { String owner, plugin, parameter; };
    Array<Automatable> automatable;

public:
    /** Set by the editor, which owns the engine's controller mappings. */
    std::function<void (const String&, const String&, const String&)> onLearn;
    std::function<void()> onCancelLearn;

private:

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
               duplicate { "Copy" }, makeUnique { "Unique" }, remove { "Delete" },
               automate { "Automate" };
    ComboBox snap;
    Slider zoom;
    Array<double> snapChoices;
};

//==============================================================================
class Workspace final : public Component,
                       public DragAndDropContainer
{
public:
    explicit Workspace (Model& m)
        : model (m),
          layout (m.edit.state.getOrCreateChildWithName (layoutIds::LAYOUT, nullptr)),
          selection (layout)
    {
        auto onChange = [this] { refresh(); };

        browser = std::make_unique<Browser> (model, selection, onChange);
        rack = std::make_unique<ChannelRack> (model, selection, onChange, [this] { openPianoRoll(); });
        mixer = std::make_unique<MixerPanel> (model, onChange);
        picker = std::make_unique<PatternPicker> (model, selection, onChange);
        playlist = std::make_unique<PlaylistPanel> (model, selection, onChange);
        chat = std::make_unique<ChatPanel> (model, selection,
                                            [this] (const Attachment& a) { revealAttachment (a); });
        chat->onRefresh = [this] (int index) { refreshAttachment (index); };

        Component* contents[numPanels] = { browser.get(), rack.get(), mixer.get(), picker.get(),
                                           playlist.get(), chat.get() };
        for (int i = 0; i < numPanels; ++i)
        {
            panels[i] = std::make_unique<Panel> (panelName (i), *contents[i]);
            panels[i]->onCollapse = [this, i] { toggleCollapsed (i); };
            panels[i]->onMaximise = [this, i] { toggleMaximised (i); };
            panels[i]->onClose    = [this, i] { setPanelVisible (i, false); };
            addAndMakeVisible (*panels[i]);
        }

        const auto stored = StringArray::fromTokens (layout[layoutIds::sizes].toString(), " ", "");
        const double fallbacks[5] = { 190.0, 460.0, 290.0, 170.0, 300.0 };
        for (int i = 0; i < 5; ++i)
        {
            const auto value = i < stored.size() ? stored[i].getDoubleValue() : 0.0;
            desired[i] = value > 1.0 ? value : fallbacks[i];
        }

        columns.setItemLayout (2, 260, 4000, -1.0);
        centre.setItemLayout (2, 100, 3000, -1.0);
        right.setItemLayout (2, 100, 3000, -1.0);

        columnBars[0] = std::make_unique<StretchableLayoutResizerBar> (&columns, 1, true);
        columnBars[1] = std::make_unique<StretchableLayoutResizerBar> (&columns, 3, true);
        columnBars[2] = std::make_unique<StretchableLayoutResizerBar> (&columns, 5, true);
        centreBar = std::make_unique<StretchableLayoutResizerBar> (&centre, 1, false);
        rightBar = std::make_unique<StretchableLayoutResizerBar> (&right, 1, false);

        for (auto* bar : std::initializer_list<Component*> { columnBars[0].get(), columnBars[1].get(),
                                                            columnBars[2].get() })
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

    //==========================================================================
    // Panel window controls. All three are layout, not content: a panel that is put
    // away keeps everything it was holding, which matters most for the chat panel -
    // closing it must never be read as abandoning a conversation.

    bool isPanelCollapsed (int index) const
    {
        const auto collapsedFlags = layout[layoutIds::collapsed].toString();
        return index < collapsedFlags.length() && collapsedFlags[index] == '1';
    }

    void setPanelCollapsed (int index, bool shouldBeCollapsed)
    {
        rememberSizes();
        auto collapsedFlags = layout[layoutIds::collapsed].toString().paddedRight ('0', numPanels);
        layout.setProperty (layoutIds::collapsed,
                            collapsedFlags.replaceSection (index, 1, shouldBeCollapsed ? "1" : "0"), nullptr);
        // A collapsed panel cannot also be the one filling the surface.
        if (shouldBeCollapsed && maximisedPanel() == index)
            layout.setProperty (layoutIds::maximised, -1, nullptr);
        resized();
    }

    void toggleCollapsed (int index) { setPanelCollapsed (index, ! isPanelCollapsed (index)); }

    int maximisedPanel() const { return static_cast<int> (layout.getProperty (layoutIds::maximised, -1)); }

    /** Fills the surface with one panel, or puts everything back. The split the person
        chose is remembered first, so restoring returns to it rather than to a default. */
    void setMaximisedPanel (int index)
    {
        if (maximisedPanel() < 0)
            rememberSizes();

        layout.setProperty (layoutIds::maximised, index, nullptr);

        if (index >= 0)
        {
            setPanelVisible (index, true);
            setPanelCollapsed (index, false);
        }

        resized();
    }

    void toggleMaximised (int index)
    {
        setMaximisedPanel (maximisedPanel() == index ? -1 : index);
    }

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

        for (int i = 0; i < numPanels; ++i)
        {
            panels[i]->setCollapsed (isPanelCollapsed (i));
            panels[i]->setMaximised (maximisedPanel() == i);
        }

        // One panel filling the surface is its own layout: everything else steps aside
        // without losing the split it had, because rememberSizes ran before this.
        if (const auto only = maximisedPanel(); only >= 0 && only < numPanels)
        {
            for (int i = 0; i < numPanels; ++i)
                panels[i]->setVisible (i == only);

            for (auto* bar : { columnBars[0].get(), columnBars[1].get(), centreBar.get(), rightBar.get() })
                bar->setVisible (false);

            centreHolder.setVisible (only == panelChannelRack || only == panelMixer);
            rightHolder.setVisible (only == panelPatternPicker || only == panelPlaylist);

            if (centreHolder.isVisible())      centreHolder.setBounds (getLocalBounds());
            else if (rightHolder.isVisible())  rightHolder.setBounds (getLocalBounds());

            panels[only]->setBounds (panels[only]->getParentComponent() == this
                                       ? getLocalBounds()
                                       : panels[only]->getParentComponent()->getLocalBounds());
            return;
        }

        centreHolder.setVisible (true);
        rightHolder.setVisible (true);

        // A hidden panel keeps its remembered size but takes no space, and its resizer
        // bar goes with it, so restoring puts the surface back where it was.
        applyVisibility (getWidth(), getHeight());

        auto r = getLocalBounds();
        Component* columnComponents[7] = { panels[panelBrowser].get(), columnBars[0].get(),
                                           &centreHolder, columnBars[1].get(), &rightHolder,
                                           columnBars[2].get(), panels[panelChat].get() };
        columns.layOutComponents (columnComponents, 7, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);

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

    //==========================================================================
    // Attaching. One path for all three kinds, so a right-click, a shortcut and a
    // check all produce exactly the same attachment. The chat panel is opened when
    // something is attached, because an attachment nobody can see is not much use -
    // but attaching never changes the music, and none of this touches the revision.

    ChatPanel& chatPanel() const { return *chat; }

    /** The bars a person has picked out in the arrangement. When clips are selected the
        region is the span of those clips; otherwise it is the loop the transport is
        showing, which is what "here" means when nothing is selected. */
    bool attachRegion (int revision)
    {
        auto attachment = regionAttachment (revision);
        if (attachment.endBeat <= attachment.startBeat)
            return false;

        openChat();
        chat->attach (std::move (attachment));
        return true;
    }

    /** The notes selected in the piano roll. */
    bool attachNotes (int revision)
    {
        auto attachment = noteAttachment (revision);
        if (attachment.noteIDs.isEmpty())
            return false;

        openChat();
        chat->attach (std::move (attachment));
        return true;
    }

    /** The mixer insert the person is pointing at. */
    bool attachInsert (const String& insertID, int revision)
    {
        const auto wanted = insertID.isNotEmpty() ? insertID : selection.insert();
        if (! model.insertFor (wanted).isValid())
            return false;

        selection.setInsert (wanted);
        openChat();
        chat->attach (Attachments (model).fromInsert (wanted, revision));
        return true;
    }

    /** Takes the card's attachment again from wherever the person is looking now. Only
        ever reached by pressing Update on the card. */
    void refreshAttachment (int index)
    {
        const auto& held = chat->current();
        if (! isPositiveAndBelow (index, static_cast<int> (held.size())))
            return;

        const auto revision = held[static_cast<size_t> (index)].takenAtRevision;

        switch (held[static_cast<size_t> (index)].kind)
        {
            case Attachment::Kind::notes:
                chat->refreshFromSelection (index, noteAttachment (revision));
                break;
            case Attachment::Kind::insert:
                chat->refreshFromSelection (index, Attachments (model).fromInsert (selection.insert(), revision));
                break;
            case Attachment::Kind::region:
            default:
                chat->refreshFromSelection (index, regionAttachment (revision));
                break;
        }
    }

    /** Puts the panels on what an attachment refers to, so a card is a way back to the
        music rather than just a label. */
    void revealAttachment (const Attachment& a)
    {
        switch (a.kind)
        {
            case Attachment::Kind::notes:
                if (a.patternID.isNotEmpty())      selection.setPattern (a.patternID);
                if (a.noteChannelID.isNotEmpty())  selection.setChannel (a.noteChannelID);
                break;

            case Attachment::Kind::insert:
                selection.setInsert (a.insertID);
                break;

            case Attachment::Kind::region:
            default:
                if (! a.laneIDs.isEmpty())
                    selection.setLane (a.laneIDs[0]);
                        playlist->showBeat (a.startBeat);
                break;
        }

        refresh();
    }

    void openChat()
    {
        if (! isPanelVisible (panelChat))
            setPanelVisible (panelChat, true);
        if (isPanelCollapsed (panelChat))
            setPanelCollapsed (panelChat, false);
    }

    /** The editor owns the engine's controller mappings, so the menu asks it to learn. */
    void setMidiLearnHandlers (std::function<void (const String&, const String&, const String&)> learn,
                               std::function<void()> cancel)
    {
        playlist->onLearn = std::move (learn);
        playlist->onCancelLearn = std::move (cancel);
    }

    bool openInstrumentWindow() const { return rack->openSelectedPlugin(); }
    bool closePluginWindows() const { return rack->closePluginWindows(); }
    bool saveInstrumentPreset() const { return rack->saveSelectedPreset(); }
    String loadInstrumentPreset() const { return rack->loadNewestPresetOnSelected(); }

    /** The open note editor, so --screenshots can capture it too. */
    /** The open note editor, when there is one. A question about notes needs the real
        selection, not a copy of it made somewhere else. */
    PianoRollEditor* pianoRollEditor() const
    {
        return pianoRollWindow != nullptr
                 ? dynamic_cast<PianoRollEditor*> (pianoRollWindow->getContentComponent())
                 : nullptr;
    }

    Component* pianoRollContent() const
    {
        return pianoRollWindow != nullptr && pianoRollWindow->isVisible()
                 ? pianoRollWindow->getContentComponent() : nullptr;
    }

    bool addAudioClip (int laneIndex, const String& path, double beat)
    {
        return playlist->getGrid().addAudio (laneIndex, path, beat);
    }

    bool shapeAudioClip (const Identifier& property, double value)
    {
        return playlist->getGrid().shapeSelection (property, value);
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

    Attachment regionAttachment (int revision) const
    {
        Attachments builder (model);
        auto& grid = playlist->getGrid();

        StringArray lanes;
        double from = 0.0, to = 0.0;
        bool any = false;

        for (const auto& clipID : grid.selectedClips())
        {
            auto clip = model.instanceFor (clipID);
            if (! clip.isValid())
                continue;

            lanes.addIfNotAlreadyThere (clip[ids::lane].toString());
            const auto start = static_cast<double> (clip[ids::start]);
            const auto end = start + static_cast<double> (clip[ids::length]);
            from = any ? std::min (from, start) : start;
            to = any ? std::max (to, end) : end;
            any = true;
        }

        if (! any)
        {
            // Nothing picked out, so "here" is the range the transport is looping.
            const auto loop = model.edit.getTransport().getLoopRange();
            from = model.edit.tempoSequence.toBeats (loop.getStart()).inBeats();
            to = model.edit.tempoSequence.toBeats (loop.getEnd()).inBeats();
            if (selection.lane().isNotEmpty())
                lanes.add (selection.lane());
        }

        return builder.fromRegion (lanes, from, to, revision);
    }

    Attachment noteAttachment (int revision) const
    {
        StringArray notes;

        if (auto* editor = pianoRollEditor())
            notes = editor->selectedNotes();

        // With the note editor closed, or open with nothing picked out, the question is
        // about this channel's whole part in this pattern. That is what a person means
        // by "these notes" when they have not narrowed it down.
        if (notes.isEmpty())
            if (auto sequence = Model::findSequence (model.patternFor (selection.pattern()),
                                                     selection.channel());
                sequence.isValid())
                for (auto note : sequence)
                    if (note.hasType (ids::NOTE))
                        notes.add (Model::uidOf (note));

        return Attachments (model).fromNotes (selection.pattern(), selection.channel(), notes, revision);
    }

    void store()
    {
        rememberSizes();
        layout.setProperty (layoutIds::sizes, String (desired[0]) + " " + String (desired[1]) + " "
                                                  + String (desired[2]) + " " + String (desired[3]) + " "
                                                  + String (desired[4]), nullptr);
    }

    Model& model;
    ValueTree layout;
    Selection selection;

private:
    struct PianoRollWindow final : DocumentWindow
    {
        PianoRollWindow (const String& windowTitle, std::unique_ptr<PianoRollEditor> editor)
            : DocumentWindow (windowTitle, theme::panel, DocumentWindow::allButtons)
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
        const bool shared[5] = { isPanelVisible (panelBrowser),
                                 isPanelVisible (panelPatternPicker) || isPanelVisible (panelPlaylist),
                                 isPanelVisible (panelChannelRack) && isPanelVisible (panelMixer),
                                 isPanelVisible (panelPatternPicker) && isPanelVisible (panelPlaylist),
                                 isPanelVisible (panelChat) };
        const double current[5] = { static_cast<double> (columns.getItemCurrentAbsoluteSize (0)),
                                    static_cast<double> (columns.getItemCurrentAbsoluteSize (4)),
                                    static_cast<double> (centre.getItemCurrentAbsoluteSize (2)),
                                    static_cast<double> (right.getItemCurrentAbsoluteSize (0)),
                                    static_cast<double> (columns.getItemCurrentAbsoluteSize (6)) };
        for (int i = 0; i < 5; ++i)
            if (shared[i] && current[i] > 1.0)
                desired[i] = current[i];
    }

    void applyVisibility (int width, int height)
    {
        const auto browserOn = isPanelVisible (panelBrowser);
        const auto chatOn = isPanelVisible (panelChat);
        const auto rackOn = isPanelVisible (panelChannelRack);
        const auto mixerOn = isPanelVisible (panelMixer);
        const auto pickerOn = isPanelVisible (panelPatternPicker);
        const auto playlistOn = isPanelVisible (panelPlaylist);

        panels[panelBrowser]->setVisible (browserOn);
        panels[panelChannelRack]->setVisible (rackOn);
        panels[panelMixer]->setVisible (mixerOn);
        panels[panelPatternPicker]->setVisible (pickerOn);
        panels[panelPlaylist]->setVisible (playlistOn);
        panels[panelChat]->setVisible (chatOn);

        columnBars[0]->setVisible (browserOn);
        columnBars[1]->setVisible (pickerOn || playlistOn);
        columnBars[2]->setVisible (chatOn);
        centreBar->setVisible (rackOn && mixerOn);
        rightBar->setVisible (pickerOn && playlistOn);

        const auto rightOn = pickerOn || playlistOn;
        const auto browserWidth = browserOn ? (isPanelCollapsed (panelBrowser)
                                                 ? static_cast<double> (3 * (Panel::headerHeight - 4) + 18)
                                                 : desired[0])
                                            : 0.0;
        const auto rightWidth = rightOn ? desired[1] : 0.0;
        const auto chatCollapsed = isPanelCollapsed (panelChat);
        const auto chatWidth = chatOn ? (chatCollapsed ? narrowColumn() : desired[4]) : 0.0;
        const auto sideBars = (browserOn ? barSize : 0) + (rightOn ? barSize : 0)
                                + (chatOn ? barSize : 0);

        const auto browserCollapsed = isPanelCollapsed (panelBrowser);
        const auto narrow = static_cast<double> (3 * (Panel::headerHeight - 4) + 18);

        if (browserOn && browserCollapsed) columns.setItemLayout (0, narrow, narrow, narrow);
        else if (browserOn)                columns.setItemLayout (0, 140, 480, browserWidth);
        else                               columns.setItemLayout (0, 0, 0, 0);
        setBar (columns, 1, browserOn);

        if (rightOn) columns.setItemLayout (4, 200, 900, rightWidth);
        else         columns.setItemLayout (4, 0, 0, 0);
        setBar (columns, 3, rightOn);

        // A negative preferred size is a fraction of the whole area, which would starve
        // the fixed columns, so the stretchy item gets what is actually left over.
        if (chatOn && chatCollapsed) columns.setItemLayout (6, chatWidth, chatWidth, chatWidth);
        else if (chatOn)             columns.setItemLayout (6, 220, 620, chatWidth);
        else                         columns.setItemLayout (6, 0, 0, 0);
        setBar (columns, 5, chatOn);

        columns.setItemLayout (2, 260, 4000,
                               std::max (260.0, width - browserWidth - rightWidth - chatWidth - sideBars));

        // The Channel Rack grows with the window and the Mixer keeps a fixed height;
        // on the right it is the other way round, so the Playlist gets the space.
        setStack (centre, rackOn, mixerOn, desired[2], false, height,
                  isPanelCollapsed (panelChannelRack), isPanelCollapsed (panelMixer));
        setStack (right, pickerOn, playlistOn, desired[3], true, height,
                  isPanelCollapsed (panelPatternPicker), isPanelCollapsed (panelPlaylist));
    }

    static void setBar (StretchableLayoutManager& manager, int index, bool present)
    {
        const auto size = present ? barSize : 0;
        manager.setItemLayout (index, size, size, size);
    }

    void setStack (StretchableLayoutManager& manager, bool firstOn, bool secondOn,
                          double fixedSize, bool fixedIsFirst, int available,
                          bool firstCollapsed = false, bool secondCollapsed = false)
    {
        const auto fixIndex = fixedIsFirst ? 0 : 2;
        const bool bothOn = firstOn && secondOn;

        // A collapsed panel is still in the layout, but only its header is: the person
        // can see where it went and click the same button to bring it back.
        const auto headerOnly = static_cast<double> (Panel::headerHeight + 4);
        const auto firstSize = firstCollapsed ? headerOnly : 0.0;
        const auto secondSize = secondCollapsed ? headerOnly : 0.0;

        auto fixed = bothOn ? fixedSize : 0.0;
        if (fixIndex == 0 && firstCollapsed)  fixed = headerOnly;
        if (fixIndex == 2 && secondCollapsed) fixed = headerOnly;

        const auto taken = (firstOn && firstCollapsed ? headerOnly : 0.0)
                             + (secondOn && secondCollapsed ? headerOnly : 0.0);
        const auto stretchy = std::max (40.0, available - std::max (fixed, taken)
                                                - (bothOn ? barSize : 0));

        for (int item : { 0, 2 })
        {
            const auto on = item == 0 ? firstOn : secondOn;
            const auto isCollapsed = item == 0 ? firstCollapsed : secondCollapsed;
            const auto collapsedSize = item == 0 ? firstSize : secondSize;

            if (! on)
                manager.setItemLayout (item, 0, 0, 0);
            else if (isCollapsed)
                manager.setItemLayout (item, collapsedSize, collapsedSize, collapsedSize);
            else if (item == fixIndex && bothOn)
                manager.setItemLayout (item, 80, 3000, fixed);
            else
                manager.setItemLayout (item, 40, 4000, stretchy);
        }

        setBar (manager, 1, bothOn && ! (firstCollapsed && secondCollapsed));
    }

    double desired[5] = { 190.0, 460.0, 290.0, 170.0, 300.0 };

    static double narrowColumn() { return static_cast<double> (3 * (Panel::headerHeight - 4) + 18); }

    std::unique_ptr<Browser> browser;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<MixerPanel> mixer;
    std::unique_ptr<PatternPicker> picker;
    std::unique_ptr<PlaylistPanel> playlist;
    std::unique_ptr<ChatPanel> chat;
    std::unique_ptr<Panel> panels[numPanels];

    std::unique_ptr<PianoRollWindow> pianoRollWindow;
    Component centreHolder, rightHolder;
    StretchableLayoutManager columns, centre, right;
    std::unique_ptr<StretchableLayoutResizerBar> columnBars[3], centreBar, rightBar;
};
}
