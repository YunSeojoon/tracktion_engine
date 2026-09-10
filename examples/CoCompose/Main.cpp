#include <JuceHeader.h>
#include "../common/Utilities.h"
#include "../common/Components.h"
#include "../common/PluginWindow.h"
#include "Theme.h"
#include "LiveProject.h"
#include "Recording.h"
#include "Workspace.h"

using namespace juce;
namespace te = tracktion;
using namespace tracktion::literals;

namespace commands
{
enum
{
    playStop = 0x2000, songMode, save, saveCopy, collectSamples, exportMix, exportStems,
    revealFolder, restoreBackup, quitApp, armChannel, recordToggle, countIn,
    undo, redo, addChannel, newPattern, placePattern, makeUnique, splitClip, duplicateClip,
    transposeUp, transposeDown,
    metronome, focusNextPanel, scanPlugins, audioSettings, savePreset, loadPreset, about,
    togglePanelBase // + panel index
};
}

class Editor;

/** The engine hands incoming MIDI controllers to whichever Edit it believes has focus,
    and the default answer is none, which is why nothing was ever learnable. This app has
    exactly one project open, so the answer is always that one. */
struct CoComposeUIBehaviour final : ExtendedUIBehaviour
{
    explicit CoComposeUIBehaviour (Editor& e) : owner (e) {}
    te::Edit* getLastFocusedEdit() override;
    Editor& owner;
};

class Editor final : public Component,
                     public MenuBarModel,
                     public ApplicationCommandTarget,
                     private Timer
{
public:
    Editor (const File& file, bool playOnStart, bool snapshots, const File& uiScript)
        : engine ("CoCompose", std::make_unique<CoComposeUIBehaviour> (*this), nullptr),
          project (engine, file), workspace (*project.model),
          saveSnapshots (snapshots), startPlayback (playOnStart)
    {
        scriptFile = uiScript;
        loadScript();

        commandManager.registerAllCommandsForTarget (this);
        // Without an explicit target the manager looks for one via keyboard focus, which
        // a plugin window or a hidden window can take away.
        commandManager.setFirstCommandTarget (this);
        addKeyListener (commandManager.getKeyMappings());
        setWantsKeyboardFocus (true);

        menuBar.setModel (this);
        title.setText ("CoCompose", dontSendNotification);
        title.setFont (Font (FontOptions (18.0f, Font::bold)));
        title.setColour (Label::textColourId, live::theme::text);
        path.setText (file.getFullPathName(), dontSendNotification);
        path.setColour (Label::textColourId, Colours::lightgrey);
        path.setFont (Font (FontOptions (11.0f)));
        path.setJustificationType (Justification::centredRight);

        tempo.setRange (30, 300, 1);
        tempo.setSliderStyle (Slider::IncDecButtons);
        tempo.setTextBoxStyle (Slider::TextBoxLeft, false, 64, 26);
        tempo.setTextValueSuffix (" BPM");
        tempo.onValueChange = [this]
        {
            project.edit->getUndoManager().beginNewTransaction ("Change tempo");
            project.edit->tempoSequence.getTempo (0)->setBpm (tempo.getValue());
        };

        play.onClick = [this] { commandManager.invokeDirectly (commands::playStop, false); };
        song.onClick = [this] { commandManager.invokeDirectly (commands::songMode, false); };
        click.onClick = [this] { commandManager.invokeDirectly (commands::metronome, false); };
        song.setClickingTogglesState (true);
        click.setClickingTogglesState (true);

        for (auto* label : std::initializer_list<Label*> { &position, &load, &focus })
        {
            label->setColour (Label::textColourId, live::theme::textDim);
            label->setFont (Font (FontOptions (12.0f)));
        }
        position.setJustificationType (Justification::centred);
        load.setJustificationType (Justification::centred);

        Helpers::addAndMakeVisible (*this, { &menuBar, &title, &path, &play, &song, &tempo, &position,
                                             &click, &load, &focus, &status, &workspace });

        // One look for the whole surface. Set before anything is laid out so every panel
        // gets the same fonts and metrics from the start.
        LookAndFeel::setDefaultLookAndFeel (&look);

        setSize (1420, 860);
        workspace.setMidiLearnHandlers ([this] (const String& source, const String& plugin, const String& parameter)
                                        { startMidiLearn (source, plugin, parameter); },
                                        [this] { cancelMidiLearn(); });
        lastControl = project.source.getSiblingFile ("control.json").loadFileAsString();
        applyTransportMode();

        if (project.recoveredFrom.isNotEmpty())
            say ("Recovered from " + project.recoveredFrom);
        else if (const auto missing = project.missingAssets(); ! missing.isEmpty())
            say ("Cannot find " + String (missing.size())
                              + (missing.size() == 1 ? " sample: " : " samples: ")
                              + missing.joinIntoString (", ") + " - use Find in the Browser");
        startTimer (250);
    }

    ~Editor() override
    {
        // Before anything else: the child components are destroyed after this object's
        // own members, so a look and feel that has already gone would be a dangling one.
        LookAndFeel::setDefaultLookAndFeel (nullptr);
        stopTimer();
        menuBar.setModel (nullptr);
        // Closing during a recording still keeps the take rather than dropping it.
        if (project.edit->getTransport().isRecording())
            recorder.stopRecording();
        else
            project.edit->getTransport().stop (false, false);
        workspace.store();
        // Layout is not part of the published model, so a session that only changed the
        // work surface still has to be written out before the app closes.
        project.save();
        project.writeBackup();
    }

    void paint (Graphics& g) override { g.fillAll (live::theme::window); }

    void resized() override
    {
        auto r = getLocalBounds();
        menuBar.setBounds (r.removeFromTop (24));

        auto header = r.removeFromTop (28).reduced (10, 0);
        title.setBounds (header.removeFromLeft (140));
        path.setBounds (header);

        auto bar = r.removeFromTop (34).reduced (10, 2);
        play.setBounds (bar.removeFromLeft (96).reduced (2));
        song.setBounds (bar.removeFromLeft (96).reduced (2));
        tempo.setBounds (bar.removeFromLeft (150).reduced (2));
        position.setBounds (bar.removeFromLeft (120));
        click.setBounds (bar.removeFromLeft (100).reduced (2));
        load.setBounds (bar.removeFromLeft (110));
        focus.setBounds (bar.removeFromLeft (150));

        status.setBounds (r.removeFromTop (26).reduced (10, 0));
        workspace.setBounds (r.reduced (8, 4));
    }

    //==============================================================================
    StringArray getMenuBarNames() override { return { "File", "Edit", "View", "Tools", "Help" }; }

    PopupMenu getMenuForIndex (int index, const String&) override
    {
        PopupMenu menu;

        if (index == 0)
        {
            for (auto id : { commands::save, commands::saveCopy, commands::collectSamples,
                             commands::exportMix, commands::exportStems, commands::restoreBackup,
                             commands::revealFolder })
                menu.addCommandItem (&commandManager, id);
            menu.addSeparator();
            menu.addCommandItem (&commandManager, commands::quitApp);
        }
        else if (index == 1)
        {
            for (auto id : { commands::undo, commands::redo })
                menu.addCommandItem (&commandManager, id);
            menu.addSeparator();
            for (auto id : { commands::addChannel, commands::newPattern, commands::placePattern,
                             commands::makeUnique, commands::splitClip, commands::duplicateClip,
                             commands::transposeUp, commands::transposeDown })
                menu.addCommandItem (&commandManager, id);
        }
        else if (index == 2)
        {
            for (int panel = 0; panel < live::numPanels; ++panel)
                menu.addCommandItem (&commandManager, commands::togglePanelBase + panel);
            menu.addSeparator();
            menu.addCommandItem (&commandManager, commands::focusNextPanel);
        }
        else if (index == 3)
        {
            for (auto id : { commands::armChannel, commands::recordToggle, commands::countIn })
                menu.addCommandItem (&commandManager, id);
            menu.addSeparator();
            for (auto id : { commands::savePreset, commands::loadPreset,
                             commands::scanPlugins, commands::audioSettings })
                menu.addCommandItem (&commandManager, id);
        }
        else
        {
            menu.addCommandItem (&commandManager, commands::about);
        }

        return menu;
    }

    void menuItemSelected (int, int) override {}

    //==============================================================================
    ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }

    void getAllCommands (Array<CommandID>& ids) override
    {
        ids.addArray ({ commands::playStop, commands::songMode, commands::save, commands::saveCopy,
                        commands::collectSamples, commands::exportMix, commands::exportStems,
                        commands::revealFolder, commands::restoreBackup, commands::quitApp,
                        commands::armChannel, commands::recordToggle, commands::countIn,
                        commands::undo, commands::redo,
                        commands::addChannel, commands::newPattern, commands::placePattern,
                        commands::makeUnique, commands::splitClip, commands::duplicateClip,
                        commands::transposeUp, commands::transposeDown,
                        commands::metronome, commands::focusNextPanel, commands::scanPlugins,
                        commands::audioSettings, commands::savePreset, commands::loadPreset,
                        commands::about });
        for (int panel = 0; panel < live::numPanels; ++panel)
            ids.add (commands::togglePanelBase + panel);
    }

    void getCommandInfo (CommandID id, ApplicationCommandInfo& info) override
    {
        const auto panelIndex = static_cast<int> (id) - commands::togglePanelBase;
        if (panelIndex >= 0 && panelIndex < live::numPanels)
        {
            info.setInfo ("Show " + String (live::panelName (panelIndex)), "Show or hide the panel", "View", 0);
            info.addDefaultKeypress ('1' + panelIndex, ModifierKeys::altModifier);
            info.setTicked (workspace.isPanelVisible (panelIndex));
            return;
        }

        switch (id)
        {
            case commands::playStop:
                info.setInfo ("Play / Stop", "Start or stop the transport", "Transport", 0);
                info.addDefaultKeypress (KeyPress::spaceKey, ModifierKeys::noModifiers);
                break;
            case commands::songMode:
                info.setInfo ("Song mode", "Loop the whole arrangement instead of the selected pattern", "Transport", 0);
                info.addDefaultKeypress ('l', ModifierKeys::ctrlModifier);
                info.setTicked (isSongMode());
                break;
            case commands::metronome:
                info.setInfo ("Metronome", "Toggle the click track", "Transport", 0);
                info.addDefaultKeypress ('m', ModifierKeys::ctrlModifier);
                info.setTicked (project.edit->clickTrackEnabled);
                break;
            case commands::save:
                info.setInfo ("Save now", "Write the session and state.json", "File", 0);
                info.addDefaultKeypress ('s', ModifierKeys::ctrlModifier);
                break;
            case commands::saveCopy:
                info.setInfo ("Save a copy...", "Copy the session into another folder", "File", 0);
                break;
            case commands::collectSamples:
                info.setInfo ("Collect samples", "Copy every sample the project uses into its own folder",
                              "File", 0);
                break;
            case commands::exportMix:
                info.setInfo ("Export WAV", "Render the arrangement, or the loop range, to a WAV file", "File", 0);
                break;
            case commands::exportStems:
                info.setInfo ("Export stems", "Render one WAV per channel", "File", 0);
                break;
            case commands::armChannel:
                info.setInfo ("Arm channel for recording", "Point the enabled inputs at the selected channel",
                              "Record", 0);
                info.setTicked (recorder.isArmed (workspace.selection.channel()));
                info.setActive (project.model->channelFor (workspace.selection.channel()).isValid());
                break;
            case commands::recordToggle:
                info.setInfo ("Record", "Start or stop recording into the armed channels", "Record", 0);
                info.addDefaultKeypress ('r', ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier);
                info.setTicked (project.edit->getTransport().isRecording());
                break;
            case commands::countIn:
                info.setInfo ("Count in one bar", "Click a bar before recording starts", "Record", 0);
                info.setTicked (recorder.getCountIn() > 0);
                break;
            case commands::restoreBackup:
                info.setInfo ("Restore a backup...", "Choose one of the automatic backups to open next time",
                              "File", 0);
                info.setActive (! project.backupsNewestFirst().isEmpty());
                break;
            case commands::revealFolder:
                info.setInfo ("Open project folder", "Show the project folder in Explorer", "File", 0);
                break;
            case commands::quitApp:
                info.setInfo ("Exit", "Close CoCompose", "File", 0);
                info.addDefaultKeypress ('q', ModifierKeys::ctrlModifier);
                break;
            case commands::undo:
            {
                const auto what = project.edit->getUndoManager().getUndoDescription();
                info.setInfo (what.isEmpty() ? "Undo" : "Undo " + what.toLowerCase(),
                              "Undo the last edit", "Edit", 0);
                info.addDefaultKeypress ('z', ModifierKeys::ctrlModifier);
                info.setActive (project.edit->getUndoManager().canUndo());
                break;
            }
            case commands::redo:
            {
                const auto what = project.edit->getUndoManager().getRedoDescription();
                info.setInfo (what.isEmpty() ? "Redo" : "Redo " + what.toLowerCase(),
                              "Redo the last undone edit", "Edit", 0);
                info.addDefaultKeypress ('z', ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier);
                info.setActive (project.edit->getUndoManager().canRedo());
                break;
            }
            case commands::addChannel:
                info.setInfo ("Add channel", "Add an instrument channel", "Edit", 0);
                info.addDefaultKeypress ('t', ModifierKeys::ctrlModifier);
                break;
            case commands::newPattern:
                info.setInfo ("New pattern", "Create an empty pattern", "Edit", 0);
                info.addDefaultKeypress ('p', ModifierKeys::ctrlModifier);
                break;
            case commands::placePattern:
                info.setInfo ("Place pattern", "Add the selected pattern to the selected lane", "Edit", 0);
                info.addDefaultKeypress ('b', ModifierKeys::ctrlModifier);
                info.setActive (canPlace());
                break;
            case commands::makeUnique:
                info.setInfo ("Make placement unique", "Give the selected clip its own copy of the pattern", "Edit", 0);
                info.addDefaultKeypress ('u', ModifierKeys::ctrlModifier);
                info.setActive (selectedInstance().isValid());
                break;
            case commands::splitClip:
                info.setInfo ("Split clip at playhead", "Cut the selected clips where the playhead is", "Edit", 0);
                info.addDefaultKeypress ('e', ModifierKeys::ctrlModifier);
                info.setActive (selectedInstance().isValid());
                break;
            case commands::duplicateClip:
                info.setInfo ("Duplicate clip", "Repeat the selected clips after themselves", "Edit", 0);
                info.addDefaultKeypress ('r', ModifierKeys::ctrlModifier);
                info.setActive (selectedInstance().isValid());
                break;
            case commands::transposeUp:
                info.setInfo ("Transpose pattern up", "Move every note in the selected pattern up a semitone", "Edit", 0);
                info.addDefaultKeypress (KeyPress::upKey, ModifierKeys::ctrlModifier);
                info.setActive (selectedSequence().isValid());
                break;
            case commands::transposeDown:
                info.setInfo ("Transpose pattern down", "Move every note in the selected pattern down a semitone", "Edit", 0);
                info.addDefaultKeypress (KeyPress::downKey, ModifierKeys::ctrlModifier);
                info.setActive (selectedSequence().isValid());
                break;
            case commands::focusNextPanel:
                info.setInfo ("Focus next panel", "Move keyboard focus to the next visible panel", "View", 0);
                info.addDefaultKeypress (KeyPress::F6Key, ModifierKeys::noModifiers);
                break;
            case commands::savePreset:
                info.setInfo ("Save instrument preset", "Keep the selected channel's instrument settings",
                              "Tools", 0);
                info.setActive (project.model->channelFor (workspace.selection.channel()).isValid());
                break;
            case commands::loadPreset:
                info.setInfo ("Load latest instrument preset",
                              "Put the most recently saved settings on the selected channel", "Tools", 0);
                info.setActive (project.model->channelFor (workspace.selection.channel()).isValid());
                break;
            case commands::scanPlugins:
                info.setInfo ("Scan plugins...", "Find installed VST3 plugins", "Tools", 0);
                break;
            case commands::audioSettings:
                info.setInfo ("Audio settings...", "Choose the output device", "Tools", 0);
                break;
            default:
                info.setInfo ("About CoCompose", "Version and documentation", "Help", 0);
                break;
        }
    }

    bool perform (const InvocationInfo& invocation) override
    {
        const auto panelIndex = static_cast<int> (invocation.commandID) - commands::togglePanelBase;
        if (panelIndex >= 0 && panelIndex < live::numPanels)
        {
            workspace.togglePanel (panelIndex);
            menuItemsChanged();
            return true;
        }

        auto& undoManager = project.edit->getUndoManager();

        switch (invocation.commandID)
        {
            case commands::playStop:
                if (startPlayback || project.edit->getTransport().isRecording())
                {
                    stopTransport();
                }
                else
                {
                    startPlayback = true;
                    project.edit->getTransport().play (false);
                }
                return true;

            case commands::songMode:
                workspace.layout.setProperty (live::layoutIds::patternMode, isSongMode(), nullptr);
                applyTransportMode();
                menuItemsChanged();
                return true;

            case commands::metronome:
                project.edit->clickTrackEnabled = ! project.edit->clickTrackEnabled;
                menuItemsChanged();
                return true;

            case commands::save:
                project.save();
                return true;

            case commands::saveCopy:
                saveCopyAsync();
                return true;

            case commands::collectSamples:
                say (project.collectSamples());
                workspace.refresh();
                return true;

            case commands::exportMix:
                say (exporter.startMix (project.source.getSiblingFile ("mix.wav"), renderRange(), project.revision)
                                  ? "Rendering the mix..." : "A render is already running");
                return true;

            case commands::exportStems:
                say (exporter.startStems (project.source.getSiblingFile ("stems"), renderRange(), project.revision)
                       ? "Rendering stems..." : "A render is already running");
                return true;

            case commands::armChannel:
                recorder.arm (workspace.selection.channel(), ! recorder.isArmed (workspace.selection.channel()));
                project.model->renderIfNeeded();
                workspace.refresh();
                return true;

            case commands::recordToggle:
                if (project.edit->getTransport().isRecording())
                    stopTransport();
                else if (! recorder.startRecording())
                    say ("Arm a channel before recording");

                workspace.refresh();
                return true;

            case commands::countIn:
                recorder.setCountIn (recorder.getCountIn() > 0 ? 0 : 1);
                menuItemsChanged();
                return true;

            case commands::restoreBackup:
                showBackups();
                return true;

            case commands::revealFolder:
                project.source.revealToUser();
                return true;

            case commands::quitApp:
                JUCEApplication::getInstance()->systemRequestedQuit();
                return true;

            case commands::undo:
                project.undo();
                return true;

            case commands::redo:
                project.redo();
                return true;

            case commands::addChannel:
            {
                undoManager.beginNewTransaction ("Add channel");
                auto channel = project.model->addChannel (
                    "Channel " + String (project.model->channels().getNumChildren() + 1), &undoManager);
                project.model->renderIfNeeded();
                workspace.selection.setChannel (live::Model::uidOf (channel));
                workspace.refresh();
                return true;
            }

            case commands::newPattern:
            {
                undoManager.beginNewTransaction ("New pattern");
                auto pattern = project.model->addPattern (
                    "Pattern " + String (project.model->patterns().getNumChildren() + 1),
                    live::defaultPatternBeats, &undoManager);
                project.model->renderIfNeeded();
                workspace.selection.setPattern (live::Model::uidOf (pattern));
                workspace.refresh();
                return true;
            }

            case commands::placePattern:
            {
                if (! canPlace())
                    return true;

                const auto lane = workspace.selection.lane();
                undoManager.beginNewTransaction ("Place pattern");
                project.model->addInstance (lane, workspace.selection.pattern(),
                                            project.model->laneEndBeat (lane), &undoManager);
                project.model->renderIfNeeded();
                workspace.refresh();
                return true;
            }

            case commands::splitClip:
                workspace.playlistGrid().splitSelectionAt (
                    project.edit->tempoSequence.toBeats (project.edit->getTransport().getPosition()).inBeats());
                workspace.refresh();
                return true;

            case commands::duplicateClip:
                workspace.playlistGrid().duplicateSelection();
                workspace.refresh();
                return true;

            case commands::makeUnique:
            {
                auto instance = selectedInstance();
                if (! instance.isValid())
                    return true;

                workspace.playlistGrid().makeSelectionUnique();
                workspace.refresh();
                return true;
            }

            case commands::transposeUp:   transpose (1); return true;
            case commands::transposeDown: transpose (-1); return true;

            case commands::focusNextPanel:
                workspace.focusNextPanel();
                return true;

            case commands::savePreset:
                say (workspace.saveInstrumentPreset() ? "Saved the instrument preset"
                                                                 : "Could not save a preset for this channel");
                return true;

            case commands::loadPreset:
            {
                // Says what went wrong rather than only that something did.
                const auto reason = workspace.loadInstrumentPreset();
                say (reason.isEmpty() ? "Loaded the latest instrument preset" : reason);
                return true;
            }

            case commands::scanPlugins:
                showPluginScanner();
                return true;

            case commands::audioSettings:
                EngineHelpers::showAudioDeviceSettings (engine);
                return true;

            default:
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "CoCompose 0.1.0",
                    "Composing with an external AI on Tracktion Engine.\n\n"
                    "The running app applies edits written to project.json and reports the live "
                    "result in state.json.\n\nGuide: docs/windows-guide.ko.md");
                return true;
        }
    }

private:
    te::Engine engine;
    live::Project project;
    live::Recorder recorder { *project.model };
    live::Exporter exporter { *project.model };
    live::Workspace workspace;
    ApplicationCommandManager commandManager;
    MenuBarComponent menuBar;
    TooltipWindow tooltips { this, 700 };
    Label title, path, position, load, focus, status;
    TextButton play { "Play / Stop" }, song { "Song" }, click { "Metronome" };
    Slider tempo;
    bool saveSnapshots;
    bool startPlayback;
    int startupTicks = 0;
    int lastSnapshotRevision = -1;
    String lastControl, lastLabels, scriptError, lastScript;
    File scriptFile;
    var script;
    int scriptStep = 0, scriptRound = 0;
    bool renderWasBusy = false;

    bool isSongMode() const
    {
        return ! static_cast<bool> (workspace.layout.getProperty (live::layoutIds::patternMode, false));
    }

    /** The loop range when one covers part of the arrangement, otherwise the whole
        thing, which is what "render the selected range" means here. */
    te::TimeRange renderRange() const
    {
        const auto whole = exporter.arrangementRange();
        const auto loop = project.edit->getTransport().getLoopRange();

        return loop.getLength().inSeconds() > 0.1 && loop.getEnd() <= whole.getEnd()
                ? loop : whole;
    }

    /** Lets a person pick which automatic backup to go back to. The open project is
        never swapped underneath them; the choice is what opens next time. */
    void showBackups()
    {
        const auto backups = project.backupsNewestFirst();
        if (backups.isEmpty())
            return;

        PopupMenu menu;
        for (int i = 0; i < backups.size(); ++i)
            menu.addItem (i + 1, backups[i].getFileName() + "   ("
                                   + File::descriptionOfSizeInBytes (backups[i].getSize()) + ")");

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (status),
            [this, backups] (int choice)
            {
                if (choice > 0 && choice <= backups.size())
                    say (project.restoreBackup (backups[choice - 1]));
            });
    }

    /** Every way of stopping goes through here, so a take is kept whether the app,
        a shortcut or an outside tool ended the recording. */
    void stopTransport()
    {
        startPlayback = false;

        if (! project.edit->getTransport().isRecording())
        {
            project.edit->getTransport().stop (false, false);
            return;
        }

        say (recorder.stopRecording());
        keepWhatWasRecorded();
    }

    /** A take is expensive to lose, so it is written and backed up straight away
        rather than waiting for the next autosave. Every path that folds a take into
        the model comes through here; one that did not would leave a take that only
        exists in memory. */
    void keepWhatWasRecorded()
    {
        project.save();
        project.writeBackup();
    }

    bool canPlace() const
    {
        return project.model->laneFor (workspace.selection.lane()).isValid()
                && project.model->patternFor (workspace.selection.pattern()).isValid();
    }

    /** Follows the selection and the arrangement. Pattern mode loops the pattern that
        is selected now, at the length it is now, and an export renders the loop, so a
        loop left behind by an earlier selection would render the wrong thing. Cheap
        enough to ask every tick, and it only writes when the answer changed. */
    void updateTransportModeIfNeeded()
    {
        const auto key = workspace.selection.pattern() + "|" + String (project.revision)
                           + (isSongMode() ? "|song" : "|pattern");

        if (key == lastTransportKey)
            return;

        lastTransportKey = key;
        applyTransportMode();
    }

    /** Song mode loops the arrangement; pattern mode loops the selected pattern's first
        placement, which is what makes the two transport modes audibly different. */
    void applyTransportMode()
    {
        auto& transport = project.edit->getTransport();
        auto range = te::TimeRange (te::TimePosition(),
                                    std::max (te::TimePosition::fromSeconds (2.0),
                                              te::TimePosition::fromSeconds (project.edit->getLength().inSeconds())));

        if (! isSongMode())
            for (auto instance : project.model->instances())
                if (instance[live::ids::pattern].toString() == workspace.selection.pattern())
                {
                    const auto start = static_cast<double> (instance[live::ids::start]);
                    const auto length = static_cast<double> (instance[live::ids::length]);
                    range = { project.edit->tempoSequence.toTime (te::BeatPosition::fromBeats (start)),
                              project.edit->tempoSequence.toTime (te::BeatPosition::fromBeats (start + length)) };
                    break;
                }

        // Moving the loop under a running transport would jump the playhead, so it is
        // only written when it is actually different.
        if (transport.getLoopRange() != range)
            transport.setLoopRange (range);

        transport.looping = true;
    }

    ValueTree selectedInstance() const
    {
        const auto clips = workspace.playlistGrid().selectedClips();
        return clips.isEmpty() ? ValueTree() : project.model->instanceFor (clips[0]);
    }

    ValueTree selectedSequence() const
    {
        return live::Model::findSequence (project.model->patternFor (workspace.selection.pattern()),
                                          workspace.selection.channel());
    }

    // A clip is a placement of a pattern, so transposing one transposes the pattern and
    // therefore every other placement of it. Make unique first to detach one.
    void transpose (int semitones)
    {
        auto sequence = selectedSequence();

        if (! sequence.isValid() || sequence.getNumChildren() == 0)
        {
            say ("Select a pattern with notes first");
            return;
        }

        auto& undoManager = project.edit->getUndoManager();
        undoManager.beginNewTransaction ("Transpose pattern");
        for (auto note : sequence)
            note.setProperty (live::ids::pitch,
                              jlimit (0, 127, static_cast<int> (note[live::ids::pitch]) + semitones),
                              &undoManager);
        project.model->renderIfNeeded();
    }

    friend struct CoComposeUIBehaviour;

    te::Edit* focusedEdit() const { return project.edit.get(); }

    /** Everything the app says to the person goes through here, so the same words also
        reach sync-status.json and an outside tool can read what happened. */
    void say (const String& message)
    {
        status.setText (message, dontSendNotification);
        project.uiMessage = message;
        messageAt = Time::getMillisecondCounter();
    }

    /** MIDI learn. The engine already keeps controller mappings in the Edit and already
        listens for a controller once a row is armed, but a row only gets its parameter
        through a menu it draws itself. So the mapping is written into the Edit first,
        with a placeholder controller, and the row that produces is the one armed. When a
        controller arrives it replaces the placeholder; if nothing arrives, the mapping
        is taken away again so an armed learn never leaves a wrong one behind. */
    bool startMidiLearn (const String& source, const String& pluginID, const String& parameterID)
    {
        auto* parameter = project.model->automatableParameter (source, pluginID, parameterID);
        if (parameter == nullptr)
            return false;

        cancelMidiLearn();

        auto& mappings = project.edit->getParameterControlMappings();
        mappings.removeParameterMapping (*parameter);

        auto state = project.edit->state.getOrCreateChildWithName (te::IDs::CONTROLLERMAPPINGS, nullptr);
        ValueTree entry (te::IDs::MAP);
        // 1 is a placeholder: loadFromEdit drops a mapping whose controller is zero, and
        // the first controller the person moves overwrites it.
        entry.setProperty (te::IDs::id, 1, nullptr);
        entry.setProperty (te::IDs::channel, 1, nullptr);
        entry.setProperty (te::IDs::param, parameter->getFullName(), nullptr);
        parameter->getOwnerID().setProperty (entry, te::IDs::pluginID, nullptr);
        state.appendChild (entry, nullptr);

        mappings.loadFromEdit();

        for (int row = 0; row < mappings.getNumControllerIDs(); ++row)
            if (mappings.getMappingForRow (row).parameter == parameter)
            {
                learningRow = row;
                learningParameter = parameter;
                learningName = parameter->getFullName();
                mappings.listenToRow (row);
                say ("MIDI learn: move a control for " + learningName);
                return true;
            }

        return false;
    }

    /** Polled while a learn is armed. The engine records the controller as it arrives;
        this is what commits it and stops listening. */
    void continueMidiLearn()
    {
        if (learningRow < 0 || project.edit == nullptr)
            return;

        auto& mappings = project.edit->getParameterControlMappings();

        if (mappings.getRowBeingListenedTo() != learningRow)
        {
            learningRow = -1;
            return;
        }

        // The row's own text says what it has heard so far, and says nothing until a
        // controller has actually moved.
        if (mappings.getTextForRow (learningRow).first.contains (":"))
        {
            mappings.setLearntParam (false);
            mappings.saveToEdit();
            const auto mapping = mappings.getMappingForRow (learningRow);
            say ("MIDI learn: CC " + String (mapping.controllerID)
                              + " on channel " + String (mapping.channelID)
                              + " now moves " + learningName);
            learningRow = -1;
            learningParameter = nullptr;
            workspace.refresh();
        }
    }

    /** Stops an armed learn and takes the placeholder mapping away with it. */
    bool cancelMidiLearn()
    {
        if (learningRow < 0 || project.edit == nullptr)
            return false;

        auto& mappings = project.edit->getParameterControlMappings();
        mappings.listenToRow (-1);

        if (learningParameter != nullptr)
        {
            mappings.removeParameterMapping (*learningParameter);
            mappings.saveToEdit();
        }

        learningRow = -1;
        learningParameter = nullptr;
        say ("MIDI learn cancelled");
        return true;
    }

    bool forgetMidiMapping (const String& source, const String& pluginID, const String& parameterID)
    {
        auto* parameter = project.model->automatableParameter (source, pluginID, parameterID);
        if (parameter == nullptr || project.edit == nullptr)
            return false;

        auto& mappings = project.edit->getParameterControlMappings();
        const auto removed = mappings.removeParameterMapping (*parameter);
        mappings.saveToEdit();
        workspace.refresh();
        return removed;
    }

    /** Scans for plugins one file per tick, so the app stays alive and a check can
        watch it happen. A plugin that takes the app down leaves its name in the
        dead man's pedal file, which is how the next run skips it. */
    bool startPluginScan()
    {
        if (scanner != nullptr)
            return false;

        auto* format = engine.getPluginManager().pluginFormatManager.getFormat (0);
        for (int i = 0; i < engine.getPluginManager().pluginFormatManager.getNumFormats(); ++i)
            if (auto* candidate = engine.getPluginManager().pluginFormatManager.getFormat (i))
                if (candidate->getName().containsIgnoreCase ("VST3"))
                    format = candidate;

        if (format == nullptr)
            return false;

        scanner = std::make_unique<PluginDirectoryScanner> (
            engine.getPluginManager().knownPluginList, *format,
            format->getDefaultLocationsToSearch(), true,
            engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"), false);

        scanned = 0;
        writeScanStatus (true, {});
        return true;
    }

    void continuePluginScan()
    {
        if (scanner == nullptr)
            return;

        String beingScanned;
        const auto more = scanner->scanNextFile (true, beingScanned);
        ++scanned;

        if (more)
        {
            writeScanStatus (true, beingScanned);
            return;
        }

        scanner.reset();
        writeScanStatus (false, {});
        workspace.refresh();
    }

    void writeScanStatus (bool running, const String& current)
    {
        Array<var> found;
        for (const auto& type : engine.getPluginManager().knownPluginList.getTypes())
            found.add (live::object ({ { "name", type.name }, { "format", type.pluginFormatName },
                                       { "manufacturer", type.manufacturerName },
                                       { "version", type.version },
                                       { "instrument", type.isInstrument },
                                       { "identifier", type.createIdentifierString() },
                                       { "file", type.fileOrIdentifier } }));

        live::atomicWrite (project.source.getSiblingFile ("plugin-scan.json"),
                           JSON::toString (live::object ({ { "running", running },
                                                           { "scanned", scanned },
                                                           { "current", current },
                                                           { "found", found } }), false));
    }

    void showPluginScanner()
    {
        DialogWindow::LaunchOptions options;
        options.dialogTitle = "Scan VST3 plugins";
        options.dialogBackgroundColour = Colours::black;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.escapeKeyTriggersCloseButton = true;
        auto* list = new PluginListComponent (engine.getPluginManager().pluginFormatManager,
            engine.getPluginManager().knownPluginList,
            engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
            std::addressof (engine.getPropertyStorage().getPropertiesFile()), true);
        list->setSize (800, 600);
        options.content.setOwned (list);
        options.launchAsync();
    }

    /** Copies the session and its published state into another folder. The running
        session keeps working on the original folder. */
    void saveCopyAsync()
    {
        project.save();
        chooser = std::make_unique<FileChooser> ("Choose an empty folder for the copy",
                                                 project.source.getParentDirectory());
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
            [this] (const FileChooser& result)
            {
                const auto target = result.getResult();
                if (target == File() || ! target.isDirectory())
                    return;

                String failed;
                for (auto* source : { &project.nativeFile, &project.stateFile })
                    if (! source->copyFileTo (target.getChildFile (source->getFileName())))
                        failed = source->getFileName();

                // The copy needs an input file too, seeded from the state it was saved with.
                if (failed.isEmpty() && ! project.stateFile.copyFileTo (target.getChildFile ("project.json")))
                    failed = "project.json";

                say (failed.isEmpty() ? "Saved a copy to " + target.getFullPathName()
                                                 : "Could not write " + failed);
            });
    }

    void pollControl()
    {
        const auto contents = project.source.getSiblingFile ("control.json").loadFileAsString();
        if (contents == lastControl || contents.isEmpty()) return;
        lastControl = contents;
        auto request = JSON::parse (contents);
        String failure;
        try
        {
            live::require (request.isObject() && request["id"].isString(), "Invalid control request");
            live::require (request["session_id"].toString() == project.sessionID, "Wrong session");
            live::require (static_cast<int> (live::number (request, "revision", 0, 2000000000, true)) == project.revision,
                           "Revision conflict");
            const auto action = request["action"].toString();
            if (action == "undo") project.undo();
            else if (action == "redo") project.redo();
            else if (action == "play") { startPlayback = true; project.edit->getTransport().play (false); }
            else if (action == "stop") stopTransport();
            else if (action == "record")
            {
                if (project.edit->getTransport().isRecording())
                    stopTransport();
                else
                    live::require (recorder.startRecording(), "Arm a channel before recording");

                workspace.refresh();
            }
            else if (action == "quit") JUCEApplication::getInstance()->systemRequestedQuit();
            else live::require (false, "Unknown control action");
            project.writeStatus();
        }
        catch (const std::exception& e) { failure = e.what(); }
        live::atomicWrite (project.source.getSiblingFile ("control-status.json"), JSON::toString (live::object ({
            { "id", request["id"] }, { "session_id", project.sessionID }, { "revision", project.revision },
            { "error", failure }, { "status", failure.isEmpty() ? "applied" : "rejected" } }), false));
    }

    void timerCallback() override
    {
        try
        {
        project.poll();
        pollControl();
        workspace.refresh();
        workspace.store();
        // Some graph rebuilds briefly clear the engine's playing flag. Preserve the
        // user's transport intent while retaining the same Edit and play position.
        if (++startupTicks >= 4 && startPlayback && ! project.edit->getTransport().isPlaying())
            project.edit->getTransport().play (false);
        if (startupTicks == 2)
            workspace.focusFirstPanel();

        tempo.setValue (project.edit->tempoSequence.getTempo (0)->getBpm());
        play.setButtonText (project.edit->getTransport().isPlaying() ? "Stop" : "Play");
        song.setToggleState (isSongMode(), dontSendNotification);
        song.setButtonText (isSongMode() ? "Song" : "Pattern");
        click.setToggleState (project.edit->clickTrackEnabled, dontSendNotification);

        const auto bars = project.edit->tempoSequence.toBarsAndBeats (project.edit->getTransport().getPosition());
        position.setText (String (bars.bars + 1) + " : " + String (bars.beats.inBeats() + 1.0, 2),
                          dontSendNotification);
        load.setText ("CPU " + String (roundToInt (engine.getDeviceManager().getCpuUsage() * 100.0f)) + "%",
                      dontSendNotification);
        focus.setText ("Focus: " + workspace.focusedPanelName(), dontSendNotification);

        status.setColour (Label::textColourId, project.error.isEmpty() ? live::theme::accent : live::theme::warn);
        if (project.error.isNotEmpty())
            say ((project.syncState == "applied_unpersisted" ? "Applied; save pending: " : "Sync rejected: ")
                                + project.error);
        else if (Time::getMillisecondCounter() - messageAt > 8000)
            // The idle line, once whatever was last said has had time to be read. It is
            // not itself a message, so it does not go through say() and does not become
            // the last thing an outside tool sees the app report.
            status.setText ("Live sync  |  Revision " + String (project.revision)
                              + "  |  Edit project.json externally; changes appear here automatically",
                            dontSendNotification);

        collectRenderResult();
        continuePluginScan();
        continueMidiLearn();
        updateTransportModeIfNeeded();
        project.writeBackupIfDue();
        project.writeStatus();
        commandManager.commandStatusChanged();
        runScriptStep();
        writeSnapshot();
        }
        catch (const std::exception& e)
        {
            project.error = e.what();
            say ("I/O error: " + project.error);
        }
    }

    /** A render runs on its own thread; this picks up the result and reports it,
        also to a file so an external tool can wait for it. */
    void collectRenderResult()
    {
        if (auto result = exporter.takeResult())
        {
            say (result->message);
            Array<var> files;
            for (const auto& file : result->files)
                files.add (file);

            live::atomicWrite (project.source.getSiblingFile ("render-status.json"),
                               JSON::toString (live::object ({ { "running", false },
                                                               { "message", result->message },
                                                               { "revision", result->revision },
                                                               { "complete", result->complete },
                                                               { "files", files } }), false));
        }
        else if (exporter.isBusy() && ! renderWasBusy)
        {
            live::atomicWrite (project.source.getSiblingFile ("render-status.json"),
                               JSON::toString (live::object ({ { "running", true }, { "message", "Rendering" },
                                                               { "revision", project.revision },
                                                               { "complete", false },
                                                               { "files", Array<var>() } }), false));
        }

        renderWasBusy = exporter.isBusy();
    }

    /** Replays recorded work-surface actions against the real panels, one per tick, so
        a check can drive the UI instead of writing to the model behind its back. It is
        a diagnostic like --screenshots, not part of the live sync contract. */
    /** Reloads when the file changes, so one session can be driven through several
        stages instead of restarting for each one. */
    void loadScript()
    {
        if (! scriptFile.existsAsFile())
            return;

        const auto contents = scriptFile.loadFileAsString();
        if (contents == lastScript || contents.isEmpty())
            return;

        auto parsed = JSON::parse (contents);
        if (! parsed.isArray() || parsed.size() == 0)
            return;

        lastScript = contents;
        script = parsed;
        scriptStep = 0;
        scriptError.clear();
        ++scriptRound;
    }

    void runScriptStep()
    {
        if (! script.isArray() || scriptStep > script.size())
            loadScript();

        if (! script.isArray() || scriptStep > script.size())
            return;

        String failure;

        if (scriptStep < script.size())
        {
            const auto action = script[scriptStep];
            try
            {
                live::require (performScriptAction (action), "Action failed: " + JSON::toString (action, true));
            }
            catch (const std::exception& e) { failure = e.what(); }
        }

        if (failure.isNotEmpty())
            scriptError = failure;

        live::atomicWrite (project.source.getSiblingFile ("ui-script-status.json"), JSON::toString (live::object ({
            { "done", scriptStep }, { "total", script.size() }, { "error", scriptError },
            { "round", scriptRound }, { "finished", scriptStep >= script.size() } }), false));

        if (failure.isEmpty())
            ++scriptStep;
        else
            scriptStep = script.size() + 1; // Stop rather than run the rest on a broken state.
    }

    bool performScriptAction (const var& action)
    {
        if (action.hasProperty ("command"))
        {
            // Some menu items say what they would do — "Undo place pattern" — so a
            // name that starts the label counts as a match.
            const auto wanted = action["command"].toString();
            CommandID exact = 0, prefixed = 0;

            for (auto id : allCommands())
            {
                ApplicationCommandInfo info (id);
                getCommandInfo (id, info);

                if (info.shortName == wanted)
                    exact = id;
                else if (prefixed == 0 && info.shortName.startsWith (wanted))
                    prefixed = id;
            }

            const auto found = exact != 0 ? exact : prefixed;
            return found != 0 && commandManager.invokeDirectly (found, false);
        }

        if (action.hasProperty ("select_channel"))
            return workspace.selectByIndex (live::ids::CHANNEL, static_cast<int> (action["select_channel"]));
        if (action.hasProperty ("select_pattern"))
            return workspace.selectByIndex (live::ids::PATTERN, static_cast<int> (action["select_pattern"]));
        if (action.hasProperty ("select_lane"))
            return workspace.selectByIndex (live::ids::LANE, static_cast<int> (action["select_lane"]));

        if (action.hasProperty ("step"))
        {
            const auto step = action["step"];
            return step.isArray() && step.size() == 2
                    && workspace.toggleStep (static_cast<int> (step[0]), static_cast<int> (step[1]));
        }

        if (action.hasProperty ("place"))
        {
            const auto place = action["place"];
            return place.isArray() && place.size() == 2
                    && workspace.playlistGrid().placeAt (static_cast<int> (place[0]),
                                                         static_cast<double> (place[1]));
        }

        if (action.hasProperty ("pick_clip"))
        {
            const auto pick = action["pick_clip"];
            return pick.isArray() && pick.size() == 2
                    && workspace.playlistGrid().selectClipAt (static_cast<int> (pick[0]),
                                                              static_cast<double> (pick[1]));
        }

        if (action.hasProperty ("move_clip"))
        {
            const auto move = action["move_clip"];
            return move.isArray() && move.size() == 2
                    && workspace.playlistGrid().moveSelection (static_cast<double> (move[0]),
                                                               static_cast<int> (move[1]));
        }

        if (action.hasProperty ("split_clip"))
            return workspace.playlistGrid().splitSelectionAt (static_cast<double> (action["split_clip"]));

        // A marker so a script that repeats the same actions still reads as new work.
        if (action.hasProperty ("comment"))
            return true;

        if (action.hasProperty ("arm"))
        {
            const auto arm = action["arm"];
            if (! arm.isArray() || arm.size() != 2)
                return false;

            auto channel = project.model->channels().getChild (static_cast<int> (arm[0]));
            return channel.isValid()
                    && recorder.arm (live::Model::uidOf (channel), static_cast<bool> (arm[1]));
        }

        if (action.hasProperty ("scan"))
            return startPluginScan();

        if (action.hasProperty ("open_plugin"))
            return workspace.openInstrumentWindow();

        if (action.hasProperty ("close_plugins"))
            return workspace.closePluginWindows();

        if (action.hasProperty ("automate"))
        {
            const auto request = action["automate"];
            if (! request.isArray() || request.size() != 3)
                return false;

            auto channel = project.model->channels().getChild (static_cast<int> (request[0]));
            return channel.isValid()
                    && workspace.playlistGrid().automate (live::Model::uidOf (channel),
                                                          request[1].toString(), request[2].toString());
        }

        if (action.hasProperty ("curve_click"))
        {
            const auto at = action["curve_click"];
            return at.isArray() && at.size() == 3
                    && workspace.playlistGrid().clickCurve (static_cast<int> (at[0]),
                                                            static_cast<double> (at[1]),
                                                            static_cast<double> (at[2]));
        }

        if (action.hasProperty ("curve_drag"))
        {
            const auto drag = action["curve_drag"];
            return drag.isArray() && drag.size() == 3
                    && workspace.playlistGrid().dragCurvePoint (static_cast<int> (drag[0]),
                                                                static_cast<double> (drag[1]),
                                                                static_cast<double> (drag[2]));
        }

        if (action.hasProperty ("midi_learn"))
        {
            const auto learn = action["midi_learn"];
            return learn.isArray() && learn.size() == 3
                    && startMidiLearn (learn[0].toString(), learn[1].toString(), learn[2].toString());
        }

        if (action.hasProperty ("midi_learn_cancel"))
            return cancelMidiLearn();

        if (action.hasProperty ("midi_cc"))
        {
            // Enters where a MIDI device's controller messages enter, so a check drives
            // the same path a knob does rather than a path of its own.
            const auto cc = action["midi_cc"];
            if (! cc.isArray() || cc.size() != 3)
                return false;

            project.edit->getParameterControlMappings()
                .sendChange (static_cast<int> (cc[0]),
                             static_cast<float> (static_cast<double> (cc[2])),
                             static_cast<int> (cc[1]));
            return true;
        }

        if (action.hasProperty ("midi_forget"))
        {
            const auto forget = action["midi_forget"];
            return forget.isArray() && forget.size() == 3
                    && forgetMidiMapping (forget[0].toString(), forget[1].toString(), forget[2].toString());
        }

        if (action.hasProperty ("curve_bend"))
        {
            const auto bend = action["curve_bend"];
            return bend.isArray() && bend.size() == 3
                    && workspace.playlistGrid().bendCurve (static_cast<int> (bend[0]),
                                                           static_cast<double> (bend[1]),
                                                           static_cast<double> (bend[2]));
        }

        if (action.hasProperty ("curve_remove"))
        {
            const auto request = action["curve_remove"];
            return request.isArray() && request.size() == 2
                    && workspace.playlistGrid().removeCurvePoint (static_cast<int> (request[0]),
                                                                  static_cast<double> (request[1]));
        }

        if (action.hasProperty ("take"))
        {
            const auto take = action["take"];
            if (! take.isArray() || take.size() < 4)
                return false;

            auto channel = project.model->channels().getChild (static_cast<int> (take[0]));
            Array<int> pitches;
            for (int i = 3; i < take.size(); ++i)
                pitches.add (static_cast<int> (take[i]));

            return channel.isValid()
                    && recorder.simulateTake (live::Model::uidOf (channel), static_cast<double> (take[1]),
                                              static_cast<double> (take[2]), pitches);
        }

        if (action.hasProperty ("keep_takes"))
        {
            say (recorder.keepTakes());
            keepWhatWasRecorded();
            workspace.refresh();
            return true;
        }

        if (action.hasProperty ("automation"))
        {
            const auto curve = action["automation"];
            if (! curve.isArray() || curve.size() < 4)
                return false;

            auto channel = project.model->channels().getChild (static_cast<int> (curve[0]));
            if (! channel.isValid())
                return false;

            auto& undoManager = project.edit->getUndoManager();
            undoManager.beginNewTransaction ("Edit automation");
            auto lane = project.model->curveFor (live::Model::uidOf (channel), curve[1].toString(),
                                                 curve[2].toString(), &undoManager);

            for (int i = 3; i + 1 < curve.size(); i += 2)
                project.model->addAutomationPoint (lane, static_cast<double> (curve[i]),
                                                   static_cast<double> (curve[i + 1]), 0.0, &undoManager);

            project.model->renderIfNeeded();
            workspace.refresh();
            return true;
        }

        if (action.hasProperty ("export"))
            return action["export"].toString() == "stems"
                     ? exporter.startStems (project.source.getSiblingFile ("stems"), renderRange(), project.revision)
                     : exporter.startMix (project.source.getSiblingFile ("mix.wav"), renderRange(), project.revision);

        if (action.hasProperty ("audio"))
        {
            const auto audio = action["audio"];
            return audio.isArray() && audio.size() == 3
                    && workspace.addAudioClip (static_cast<int> (audio[0]), audio[1].toString(),
                                               static_cast<double> (audio[2]));
        }

        if (action.hasProperty ("shape"))
        {
            const auto shape = action["shape"];
            if (! shape.isArray() || shape.size() != 2)
                return false;

            const auto what = shape[0].toString();
            const Identifier property = what == "gain" ? live::ids::gainDb
                                      : what == "fade_in" ? live::ids::fadeIn
                                      : what == "fade_out" ? live::ids::fadeOut
                                      : what == "speed" ? live::ids::speed
                                      : what == "offset" ? live::ids::offset
                                      : live::ids::length;
            return workspace.shapeAudioClip (property, static_cast<double> (shape[1]));
        }

        if (action.hasProperty ("note"))
        {
            const auto note = action["note"];
            return note.isArray() && note.size() == 4
                    && workspace.addPianoRollNote (static_cast<int> (note[0]), static_cast<double> (note[1]),
                                                   static_cast<double> (note[2]), static_cast<int> (note[3]));
        }

        return false;
    }

    Array<CommandID> allCommands()
    {
        Array<CommandID> ids;
        getAllCommands (ids);
        return ids;
    }

    /** Captures the finished frame, after the panels have been refreshed, whenever the
        project or anything the panels display has changed. */
    void writeSnapshot()
    {
        if (! saveSnapshots)
            return;

        Array<var> labels;
        collectLabels (*this, labels);
        const auto signature = JSON::toString (var (labels), true);
        if (lastSnapshotRevision == project.revision && signature == lastLabels)
            return;

        lastSnapshotRevision = project.revision;
        lastLabels = signature;

        writeImage ("ui.png", *this);
        if (auto* roll = workspace.pianoRollContent())
            writeImage ("piano-roll.png", *roll);

        live::atomicWrite (project.source.getSiblingFile ("ui-state.json"), JSON::toString (live::object ({
            { "revision", project.revision }, { "labels", labels } }), false));
    }

    /** Written through a temporary file so a tool never reads a half-finished image. */
    void writeImage (const String& fileName, Component& component)
    {
        auto image = component.createComponentSnapshot (component.getLocalBounds());
        const auto target = project.source.getSiblingFile (fileName);
        TemporaryFile temporary (target);

        if (auto output = temporary.getFile().createOutputStream())
        {
            PNGImageFormat().writeImageToStream (image, *output);
            output.reset();
            temporary.overwriteTargetFileWithTemporary();
        }
    }

    static void collectLabels (Component& component, Array<var>& labels)
    {
        if (auto* label = dynamic_cast<Label*> (&component)) labels.add (label->getText());
        for (auto* child : component.getChildren()) collectLabels (*child, labels);
    }

    std::unique_ptr<FileChooser> chooser;
    std::unique_ptr<PluginDirectoryScanner> scanner;
    int scanned = 0;
    String lastTransportKey;
    live::CoComposeLookAndFeel look;
    uint32 messageAt = 0;
    int learningRow = -1;
    te::AutomatableParameter* learningParameter = nullptr;
    String learningName;
};

te::Edit* CoComposeUIBehaviour::getLastFocusedEdit()
{
    return owner.focusedEdit();
}


class Application final : public JUCEApplication
{
public:
    const String getApplicationName() override { return "CoCompose"; }
    const String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const String& commandLine) override
    {
        auto args = StringArray::fromTokens (commandLine, true);
        args.trim();
        File file = File::getSpecialLocation (File::userDocumentsDirectory)
                        .getChildFile ("CoCompose/project.json");
        const auto projectOption = args.indexOf ("--project");
        if (projectOption >= 0 && projectOption + 1 < args.size())
            file = File::getCurrentWorkingDirectory().getChildFile (args[projectOption + 1].unquoted());
        try
        {
            const auto scriptOption = args.indexOf ("--ui-script");
            const auto uiScript = scriptOption >= 0 && scriptOption + 1 < args.size()
                                      ? File::getCurrentWorkingDirectory().getChildFile (args[scriptOption + 1].unquoted())
                                      : File();
            // Lets a check see the work surface at the scalings Windows actually uses.
            const auto scaleOption = args.indexOf ("--scale");
            if (scaleOption >= 0 && scaleOption + 1 < args.size())
                Desktop::getInstance().setGlobalScaleFactor (
                    jlimit (0.5f, 4.0f, args[scaleOption + 1].getFloatValue()));

            openedProject = file;
            auto editor = std::make_unique<Editor> (file, args.contains ("--play"),
                                                    args.contains ("--screenshots"), uiScript);
            // Lets a check look at the surface at the sizes people actually have, rather
            // than only at whatever this screen happens to allow.
            auto wanted = Rectangle<int> (1420, 860);
            if (const auto sizeOption = args.indexOf ("--size");
                sizeOption >= 0 && sizeOption + 1 < args.size())
            {
                const auto parts = StringArray::fromTokens (args[sizeOption + 1], "x", {});
                if (parts.size() == 2)
                    wanted = { jlimit (640, 4000, parts[0].getIntValue()),
                               jlimit (400, 2400, parts[1].getIntValue()) };
            }

            window = std::make_unique<Window> (std::move (editor), ! args.contains ("--headless"), wanted);
        }
        catch (const std::exception& e)
        {
            file.getSiblingFile ("startup-error.txt").replaceWithText (e.what());
            setApplicationReturnValue (1);
            quit();
        }
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

    /** One project at a time: the app owns a folder's live-sync files while it is open,
        so a second copy would fight the first over them. Opening another project used
        to do nothing at all, which looked like a failed launch. */
    void anotherInstanceStarted (const String& commandLine) override
    {
        if (window == nullptr)
            return;

        window->toFront (true);

        auto args = StringArray::fromTokens (commandLine, true);
        args.trim();
        const auto projectOption = args.indexOf ("--project");
        const auto wanted = projectOption >= 0 && projectOption + 1 < args.size()
                              ? args[projectOption + 1].unquoted() : String();

        if (wanted.isEmpty())
            return;

        AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "CoCompose is already open",
                                          "CoCompose opens one project at a time, and it is already working on\n"
                                          + openedProject.getFullPathName()
                                          + "\n\nClose it first to open\n" + wanted);
    }

private:
    struct Window final : DocumentWindow
    {
        Window (std::unique_ptr<Editor> editor, bool visible, Rectangle<int> wanted)
            : DocumentWindow ("CoCompose", live::theme::window, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (editor.release(), true);
            setResizable (true, false);

            // At 150% or 200% a fixed size is bigger than the screen, and a minimum
            // bigger than the screen cannot be shrunk to fit, so both are capped by
            // what this display actually has room for.
            const auto room = availableSize();
            setResizeLimits (jmin (1180, room.getWidth()), jmin (620, room.getHeight()), 4000, 2400);
            centreWithSize (jmin (wanted.getWidth(), room.getWidth()),
                            jmin (wanted.getHeight(), room.getHeight()));
            setVisible (visible);
        }
        /** The room a window has, in the units a window is sized in. userBounds is
            already divided by the scaling in force, so nothing else has to be. A
            little is left over for the title bar and the screen edge. */
        static Rectangle<int> availableSize()
        {
            if (auto* display = Desktop::getInstance().getDisplays().getPrimaryDisplay())
                return display->userBounds.withTrimmedBottom (40.0f).withTrimmedRight (8.0f).toNearestInt();

            return { 1420, 860 };
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
    File openedProject;
};

START_JUCE_APPLICATION (Application)
