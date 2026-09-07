#include <JuceHeader.h>
#include "../common/Utilities.h"
#include "../common/Components.h"
#include "../common/PluginWindow.h"
#include "LiveProject.h"

using namespace juce;
namespace te = tracktion;
using namespace tracktion::literals;

class Editor final : public Component, private Timer
{
public:
    Editor (const File& file, bool playOnStart, bool snapshots)
        : engine ("CoCompose", std::make_unique<ExtendedUIBehaviour>(), nullptr),
          project (engine, file), selection (engine), timeline (*project.edit, selection),
          saveSnapshots (snapshots), startPlayback (playOnStart)
    {
        Helpers::addAndMakeVisible (*this, { &title, &path, &status, &play, &undo, &addTrack,
            &transposeDown, &transposeUp, &folder, &plugins, &audio, &tempo, &timeline });
        title.setText ("CoCompose  /  Live session", dontSendNotification);
        title.setFont (Font (FontOptions (22.0f, Font::bold)));
        path.setText (file.getFullPathName(), dontSendNotification);
        path.setColour (Label::textColourId, Colours::lightgrey);
        tempo.setRange (30, 300, 1);
        tempo.setSliderStyle (Slider::IncDecButtons);
        tempo.setTextBoxStyle (Slider::TextBoxLeft, false, 70, 28);
        tempo.setTextValueSuffix (" BPM");
        tempo.onValueChange = [this]
        {
            project.edit->getUndoManager().beginNewTransaction ("Change tempo");
            project.edit->tempoSequence.getTempo (0)->setBpm (tempo.getValue());
        };
        play.onClick = [this]
        {
            startPlayback = ! startPlayback;
            if (startPlayback) project.edit->getTransport().play (false);
            else project.edit->getTransport().stop (false, false);
        };
        undo.onClick = [this] { project.undo(); };
        addTrack.onClick = [this]
        {
            auto& edit = *project.edit;
            edit.getUndoManager().beginNewTransaction ("Add track");
            edit.ensureNumberOfAudioTracks (te::getAudioTracks (edit).size() + 1);
            auto* track = te::getAudioTracks (edit).getLast();
            auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
            if (synth != nullptr) track->pluginList.insertPlugin (*synth, 0, nullptr);
            track->getVolumePlugin()->setVolumeDb (-12);
        };
        transposeDown.onClick = [this] { transpose (-1); };
        transposeUp.onClick = [this] { transpose (1); };
        folder.onClick = [file] { file.revealToUser(); };
        audio.onClick = [this] { EngineHelpers::showAudioDeviceSettings (engine); };
        plugins.onClick = [this]
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
        };
        auto& view = timeline.getEditViewState();
        view.showFooters = true;
        view.showMidiDevices = false;
        view.showWaveDevices = false;
        view.viewX1 = 0s;
        view.viewX2 = 20s;
        setSize (1180, 720);
        lastControl = project.source.getSiblingFile ("control.json").loadFileAsString();
        startTimer (250);
    }

    ~Editor() override
    {
        stopTimer();
        project.edit->getTransport().stop (false, false);
        project.poll();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff171d28));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        title.setBounds (r.removeFromTop (34));
        path.setBounds (r.removeFromTop (25));
        r.removeFromTop (8);
        auto toolbar = r.removeFromTop (34);
        for (auto* button : { &play, &undo, &addTrack, &transposeDown, &transposeUp, &plugins, &audio, &folder })
            button->setBounds (toolbar.removeFromLeft (108).reduced (2));
        tempo.setBounds (toolbar.reduced (2));
        status.setBounds (r.removeFromTop (38));
        timeline.setBounds (r);
    }

private:
    te::Engine engine;
    live::Project project;
    te::SelectionManager selection;
    EditComponent timeline;
    Label title, path, status;
    TextButton play { "Play / Stop" }, undo { "Undo" }, addTrack { "+ Track" },
        transposeDown { "Notes -1" }, transposeUp { "Notes +1" }, folder { "Project folder" },
        plugins { "Scan plugins" }, audio { "Audio settings" };
    Slider tempo;
    bool saveSnapshots;
    bool startPlayback;
    int startupTicks = 0;
    int lastSnapshotRevision = -1;
    String lastControl;

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
            else if (action == "redo") { project.edit->getUndoManager().redo(); project.poll(); }
            else if (action == "play") { startPlayback = true; project.edit->getTransport().play (false); }
            else if (action == "stop") { startPlayback = false; project.edit->getTransport().stop (false, false); }
            else if (action == "quit") JUCEApplication::getInstance()->systemRequestedQuit();
            else live::require (false, "Unknown control action");
            project.writeStatus();
        }
        catch (const std::exception& e) { failure = e.what(); }
        live::atomicWrite (project.source.getSiblingFile ("control-status.json"), JSON::toString (live::object ({
            { "id", request["id"] }, { "session_id", project.sessionID }, { "revision", project.revision },
            { "error", failure }, { "status", failure.isEmpty() ? "applied" : "rejected" } }), false));
    }

    void transpose (int semitones)
    {
        if (auto* clip = dynamic_cast<te::MidiClip*> (selection.getSelectedObject (0)))
        {
            auto& undoManager = project.edit->getUndoManager();
            undoManager.beginNewTransaction ("Transpose selected clip");
            for (auto* note : clip->getSequence().getNotes())
                note->setNoteNumber (jlimit (0, 127, note->getNoteNumber() + semitones), &undoManager);
        }
        else
            status.setText ("Select a MIDI clip first", dontSendNotification);
    }

    void timerCallback() override
    {
        try
        {
        // Snapshot the previous completed UI update, after ValueTree listeners have run.
        if (saveSnapshots && lastSnapshotRevision != project.revision)
        {
            auto image = createComponentSnapshot (getLocalBounds());
            auto output = project.source.getSiblingFile ("ui.png").createOutputStream();
            if (output != nullptr) { output->setPosition (0); output->truncate(); PNGImageFormat().writeImageToStream (image, *output); }
            Array<var> labels;
            collectLabels (*this, labels);
            live::atomicWrite (project.source.getSiblingFile ("ui-state.json"), JSON::toString (live::object ({
                { "revision", project.revision }, { "labels", labels } }), false));
            lastSnapshotRevision = project.revision;
        }
        project.poll();
        pollControl();
        // Some graph rebuilds briefly clear the engine's playing flag. Preserve the
        // user's transport intent while retaining the same Edit and play position.
        if (++startupTicks >= 4 && startPlayback && ! project.edit->getTransport().isPlaying())
            project.edit->getTransport().play (false);
        tempo.setValue (project.edit->tempoSequence.getTempo (0)->getBpm(), dontSendNotification);
        play.setButtonText (project.edit->getTransport().isPlaying() ? "Stop" : "Play");
        undo.setEnabled (project.edit->getUndoManager().canUndo());
        status.setColour (Label::textColourId, project.error.isEmpty() ? Colour (0xff83dec0) : Colour (0xffffad83));
        status.setText (project.error.isEmpty() ? "Live sync  |  Revision " + String (project.revision)
            + "  |  Edit project.json externally; changes appear here automatically"
            : (project.syncState == "applied_unpersisted" ? "Applied; save pending: " : "Sync rejected: ")
                + project.error, dontSendNotification);
        project.writeStatus();
        timeline.repaint();
        }
        catch (const std::exception& e)
        {
            project.error = e.what();
            status.setText ("I/O error: " + project.error, dontSendNotification);
        }
    }

    static void collectLabels (Component& component, Array<var>& labels)
    {
        if (auto* label = dynamic_cast<Label*> (&component)) labels.add (label->getText());
        for (auto* child : component.getChildren()) collectLabels (*child, labels);
    }
};

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
            auto editor = std::make_unique<Editor> (file, args.contains ("--play"), args.contains ("--screenshots"));
            window = std::make_unique<Window> (std::move (editor), ! args.contains ("--headless"));
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

private:
    struct Window final : DocumentWindow
    {
        Window (std::unique_ptr<Editor> editor, bool visible)
            : DocumentWindow ("CoCompose", Colour (0xff171d28), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (editor.release(), true);
            setResizable (true, false);
            setResizeLimits (1100, 500, 4000, 2400);
            centreWithSize (1180, 720);
            setVisible (visible);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (Application)
