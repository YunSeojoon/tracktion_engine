#pragma once

#include "Model.h"

namespace live
{
/** The Browser: the project's own objects, the samples on disk, and the plugins that
    have been scanned.

    Folders, favourites and the recent list are stored in the Edit, so a reopened
    session offers the same places. A sample can be previewed here and dragged onto a
    playlist lane; the project section also reports files an audio clip can no longer
    find, and offers to look for them.
*/
namespace browserIds
{
    const Identifier BROWSER ("COCOMPOSEBROWSER"), FOLDER ("FOLDER"), RECENT ("RECENT"), FAVOURITE ("FAVOURITE");
    const Identifier path ("path"), search ("search");
}

class Browser final : public Component,
                      private ListBoxModel,
                      private ChangeListener
{
public:
    Browser (Model& m, Selection& s, std::function<void()> onChange)
        : model (m), selection (s), changed (std::move (onChange)),
          state (m.edit.state.getOrCreateChildWithName (browserIds::BROWSER, nullptr))
    {
        if (state.getChildWithName (browserIds::FOLDER).isValid() == false)
            addFolder (File::getSpecialLocation (File::userMusicDirectory));

        formats.registerBasicFormats();
        transport.addChangeListener (this);

        list.setModel (this);
        list.setRowHeight (19);
        list.setColour (ListBox::backgroundColourId, Colours::transparentBlack);

        search.setTextToShowWhenEmpty ("Search samples", theme::textFaint);
        search.onTextChange = [this] { rebuild(); };
        search.onReturnKey = [this] { rebuild(); };

        addPlace.setTooltip ("Add a folder to the browser");
        addPlace.onClick = [this] { chooseFolder(); };

        favourite.setTooltip ("Keep the selected sample in Favourites");
        favourite.onClick = [this] { toggleFavourite(); };

        preview.setTooltip ("Preview the selected sample");
        preview.setClickingTogglesState (true);
        preview.onClick = [this] { preview.getToggleState() ? startPreview() : stopPreview(); };

        locate.setTooltip ("Find a missing sample");
        locate.onClick = [this] { locateMissing(); };

        addAndMakeVisible (list);
        for (auto* child : std::initializer_list<Component*> { &search, &addPlace, &favourite, &preview, &locate })
            addAndMakeVisible (*child);
    }

    ~Browser() override
    {
        stopPreview();
        transport.removeChangeListener (this);
        transport.setSource (nullptr);
        device.removeAudioCallback (&player);
        device.closeAudioDevice();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        search.setBounds (r.removeFromTop (22).reduced (0, 1));

        auto bar = r.removeFromBottom (24);
        addPlace.setBounds (bar.removeFromLeft (30).reduced (1));
        favourite.setBounds (bar.removeFromLeft (30).reduced (1));
        preview.setBounds (bar.removeFromLeft (36).reduced (1));
        locate.setBounds (bar.removeFromLeft (52).reduced (1));

        list.setBounds (r);
    }

    void refresh()
    {
        rebuild();
        locate.setEnabled (firstMissingClip().isValid());
        favourite.setEnabled (File (selectedPath).existsAsFile());
        preview.setEnabled (favourite.isEnabled());
        if (! transport.isPlaying() && preview.getToggleState())
            preview.setToggleState (false, dontSendNotification);
    }

private:
    struct Row
    {
        String label, kind, id, path;
        bool header = false;
        int depth = 0;
        bool expandable = false, expanded = false, missing = false;
    };

    //==========================================================================
    void addFolder (const File& folder)
    {
        if (! folder.isDirectory())
            return;

        for (auto child : state)
            if (child.hasType (browserIds::FOLDER) && child[browserIds::path].toString() == folder.getFullPathName())
                return;

        ValueTree entry (browserIds::FOLDER);
        entry.setProperty (browserIds::path, folder.getFullPathName(), nullptr);
        state.appendChild (entry, nullptr);
    }

    void remember (const Identifier& type, const File& source, int limit)
    {
        for (int i = state.getNumChildren(); --i >= 0;)
        {
            auto child = state.getChild (i);
            if (child.hasType (type) && child[browserIds::path].toString() == source.getFullPathName())
                state.removeChild (i, nullptr);
        }

        ValueTree entry (type);
        entry.setProperty (browserIds::path, source.getFullPathName(), nullptr);
        state.appendChild (entry, nullptr);

        int seen = 0;
        for (int i = state.getNumChildren(); --i >= 0;)
            if (state.getChild (i).hasType (type) && ++seen > limit)
                state.removeChild (i, nullptr);
    }

    bool isFavourite (const File& source) const
    {
        for (auto child : state)
            if (child.hasType (browserIds::FAVOURITE) && child[browserIds::path].toString() == source.getFullPathName())
                return true;
        return false;
    }

    void toggleFavourite()
    {
        const File source (selectedPath);
        if (! source.existsAsFile())
            return;

        for (int i = state.getNumChildren(); --i >= 0;)
        {
            auto child = state.getChild (i);
            if (child.hasType (browserIds::FAVOURITE) && child[browserIds::path].toString() == source.getFullPathName())
            {
                state.removeChild (i, nullptr);
                rebuild();
                return;
            }
        }

        remember (browserIds::FAVOURITE, source, 64);
        rebuild();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<FileChooser> ("Add a folder to the browser",
                                                 File::getSpecialLocation (File::userMusicDirectory));
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
            [this] (const FileChooser& result)
            {
                addFolder (result.getResult());
                rebuild();
            });
    }

    //==========================================================================
    ValueTree firstMissingClip() const
    {
        for (auto clip : model.instances())
            if (clip.hasType (ids::AUDIO) && ! File (clip[ids::file].toString()).existsAsFile())
                return clip;
        return {};
    }

    /** Points every clip that referred to a lost file at the folder it was found in,
        which is what usually happens when a project moves between machines. */
    void locateMissing()
    {
        auto missing = firstMissingClip();
        if (! missing.isValid())
            return;

        const auto wanted = File (missing[ids::file].toString()).getFileName();
        chooser = std::make_unique<FileChooser> ("Find " + wanted, File(),
                                                 model.edit.engine.getAudioFileFormatManager()
                                                      .readFormatManager.getWildcardForAllFormats());
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
            [this] (const FileChooser& result)
            {
                const auto found = result.getResult();
                if (! found.existsAsFile())
                    return;

                auto& undo = model.edit.getUndoManager();
                undo.beginNewTransaction ("Find missing samples");

                for (auto clip : model.instances())
                {
                    if (! clip.hasType (ids::AUDIO))
                        continue;

                    const File current (clip[ids::file].toString());
                    if (current.existsAsFile())
                        continue;

                    auto candidate = found.getParentDirectory().getChildFile (current.getFileName());
                    if (current.getFileName() == found.getFileName())
                        candidate = found;

                    if (candidate.existsAsFile())
                        clip.setProperty (ids::file, candidate.getFullPathName(), &undo);
                }

                model.renderIfNeeded();
                addFolder (found.getParentDirectory());
                notify();
            });
    }

    //==========================================================================
    void startPreview()
    {
        const File source (selectedPath);
        if (! source.existsAsFile())
        {
            preview.setToggleState (false, dontSendNotification);
            return;
        }

        if (! deviceReady)
        {
            device.initialiseWithDefaultDevices (0, 2);
            device.addAudioCallback (&player);
            player.setSource (&transport);
            deviceReady = true;
        }

        stopPreview();

        if (auto* format = formats.findFormatForFileExtension (source.getFileExtension()))
            if (auto* reader = format->createReaderFor (source.createInputStream().release(), true))
            {
                readerSource = std::make_unique<AudioFormatReaderSource> (reader, true);
                transport.setSource (readerSource.get(), 0, nullptr, reader->sampleRate);
                transport.setPosition (0.0);
                transport.start();
                remember (browserIds::RECENT, source, 16);
                rebuild();
                return;
            }

        preview.setToggleState (false, dontSendNotification);
    }

    void stopPreview()
    {
        transport.stop();
        transport.setSource (nullptr);
        readerSource.reset();
    }

    void changeListenerCallback (ChangeBroadcaster*) override
    {
        if (! transport.isPlaying())
            preview.setToggleState (false, dontSendNotification);
    }

    //==========================================================================
    void rebuild()
    {
        Array<Row> rebuilt;

        rebuilt.add ({ "Project", {}, {}, {}, true });
        addSection (rebuilt, "Channels", ids::CHANNEL, model.channels(), "channel");
        addSection (rebuilt, "Patterns", ids::PATTERN, model.patterns(), "pattern");
        addSection (rebuilt, "Playlist lanes", ids::LANE, model.lanes(), "lane");
        addSection (rebuilt, "Mixer inserts", ids::INSERT, model.mixer(), "insert");

        Array<var> assets;
        for (auto clip : model.instances())
            if (clip.hasType (ids::AUDIO))
                assets.addIfNotAlreadyThere (clip[ids::file]);

        if (! assets.isEmpty())
        {
            rebuilt.add ({ "  Project samples", {}, {}, {}, true, 0 });
            for (const auto& asset : assets)
            {
                const File source (asset.toString());
                rebuilt.add ({ "    " + source.getFileName(), "sample", {}, source.getFullPathName(),
                               false, 0, false, false, ! source.existsAsFile() });
            }
        }

        addPathSection (rebuilt, "Favourites", browserIds::FAVOURITE);
        addPathSection (rebuilt, "Recent", browserIds::RECENT);

        rebuilt.add ({ "Samples", {}, {}, {}, true });
        for (auto child : state)
            if (child.hasType (browserIds::FOLDER))
                addFolderRows (rebuilt, File (child[browserIds::path].toString()), 1);

        rebuilt.add ({ "Scanned plugins", {}, {}, {}, true });
        auto& known = model.edit.engine.getPluginManager().knownPluginList;
        if (known.getNumTypes() == 0)
            rebuilt.add ({ "  (none scanned yet)", {}, {}, {}, false });
        else
            for (const auto& type : known.getTypes())
                rebuilt.add ({ "  " + type.name, {}, {}, {}, false });

        if (! sameAs (rebuilt))
        {
            rows = std::move (rebuilt);
            list.updateContent();
        }
        list.repaint();
    }

    void addSection (Array<Row>& into, const String& title, const Identifier& type,
                     ValueTree parent, const String& kind)
    {
        into.add ({ "  " + title, {}, {}, {}, true });
        for (auto child : parent)
            if (child.hasType (type))
                into.add ({ "    " + child[ids::name].toString(), kind, Model::uidOf (child), {}, false });
    }

    void addPathSection (Array<Row>& into, const String& title, const Identifier& type)
    {
        Array<File> found;
        for (auto child : state)
            if (child.hasType (type))
                found.insert (0, File (child[browserIds::path].toString()));

        if (found.isEmpty())
            return;

        into.add ({ title, {}, {}, {}, true });
        for (const auto& source : found)
            if (matchesSearch (source))
                into.add ({ "  " + source.getFileName(), "sample", {}, source.getFullPathName(),
                            false, 0, false, false, ! source.existsAsFile() });
    }

    bool matchesSearch (const File& source) const
    {
        const auto term = search.getText().trim();
        return term.isEmpty() || source.getFileName().containsIgnoreCase (term);
    }

    bool isOpen (const File& folder) const { return openFolders.contains (folder.getFullPathName()); }

    /** Only opened folders are listed, so a big sample library costs nothing until it
        is actually looked into. A search looks one level deeper. */
    void addFolderRows (Array<Row>& into, const File& folder, int depth)
    {
        if (! folder.isDirectory() || depth > 8)
            return;

        into.add ({ String::repeatedString ("  ", depth) + folder.getFileName(), "folder", {},
                    folder.getFullPathName(), false, depth, true, isOpen (folder) });

        if (! isOpen (folder))
            return;

        for (const auto& child : folder.findChildFiles (File::findDirectories, false, "*",
                                                        File::FollowSymlinks::no))
            addFolderRows (into, child, depth + 1);

        for (const auto& child : folder.findChildFiles (File::findFiles, false, "*",
                                                        File::FollowSymlinks::no))
            if (isAudioFile (child) && matchesSearch (child))
                into.add ({ String::repeatedString ("  ", depth + 1) + child.getFileName(), "sample", {},
                            child.getFullPathName(), false, depth + 1 });
    }

    bool isAudioFile (const File& source) const
    {
        return formats.findFormatForFileExtension (source.getFileExtension()) != nullptr;
    }

    bool sameAs (const Array<Row>& other) const
    {
        if (other.size() != rows.size())
            return false;
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i].label != other[i].label || rows[i].path != other[i].path
                 || rows[i].expanded != other[i].expanded || rows[i].missing != other[i].missing)
                return false;
        return true;
    }

    //==========================================================================
    int getNumRows() override { return rows.size(); }

    /** A sample dragged out of here becomes an audio clip on the lane it is dropped on. */
    var getDragSourceDescription (const SparseSet<int>& selectedRows) override
    {
        if (selectedRows.isEmpty() || ! isPositiveAndBelow (selectedRows[0], rows.size()))
            return {};

        const auto& row = rows.getReference (selectedRows[0]);
        return row.kind == "sample" ? var ("sample:" + row.path) : var();
    }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool) override
    {
        if (! isPositiveAndBelow (row, rows.size()))
            return;

        const auto& item = rows.getReference (row);
        const auto picked = (! item.id.isEmpty()
                              && ((item.kind == "channel" && item.id == selection.channel())
                               || (item.kind == "pattern" && item.id == selection.pattern())
                               || (item.kind == "lane" && item.id == selection.lane())))
                            || (item.kind == "sample" && item.path == selectedPath);

        if (picked)
        {
            g.setColour (theme::selection);
            g.fillRect (0, 0, width, height);
        }

        if (item.expandable)
        {
            g.setColour (theme::textDim);
            g.drawText (item.expanded ? "-" : "+", 2 + item.depth * 10, 0, 10, height, Justification::centredLeft);
        }

        g.setColour (item.missing ? theme::danger
                   : item.header ? theme::textDim : Colours::white.withAlpha (0.88f));
        g.setFont (Font (FontOptions (item.header ? 12.0f : 13.0f, item.header ? Font::bold : Font::plain)));
        g.drawText (item.label + (item.missing ? "  (missing)" : ""), 12, 0, width - 16, height,
                    Justification::centredLeft);
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (! isPositiveAndBelow (row, rows.size()))
            return;

        const auto& item = rows.getReference (row);

        if (item.kind == "folder")
        {
            if (isOpen (File (item.path))) openFolders.removeString (item.path);
            else openFolders.add (item.path);
            rebuild();
            return;
        }

        if (item.kind == "sample")
        {
            selectedPath = item.path;
            refresh();
            return;
        }

        if (item.kind == "channel") selection.setChannel (item.id);
        else if (item.kind == "pattern") selection.setPattern (item.id);
        else if (item.kind == "lane") selection.setLane (item.id);
        else return;

        notify();
    }

    void listBoxItemDoubleClicked (int row, const MouseEvent&) override
    {
        if (isPositiveAndBelow (row, rows.size()) && rows.getReference (row).kind == "sample")
        {
            selectedPath = rows.getReference (row).path;
            preview.setToggleState (true, dontSendNotification);
            startPreview();
        }
    }

    void notify() { if (changed != nullptr) changed(); }

    Model& model;
    Selection& selection;
    std::function<void()> changed;
    ValueTree state;
    ListBox list;
    Array<Row> rows;
    StringArray openFolders;
    String selectedPath;
    TextEditor search;
    TextButton addPlace { "+" }, favourite { "*" }, preview { "Play" }, locate { "Find" };
    std::unique_ptr<FileChooser> chooser;

    // A preview plays straight to the default device, so it never touches the Edit.
    AudioFormatManager formats;
    AudioDeviceManager device;
    AudioSourcePlayer player;
    AudioTransportSource transport;
    std::unique_ptr<AudioFormatReaderSource> readerSource;
    bool deviceReady = false;
};
}
