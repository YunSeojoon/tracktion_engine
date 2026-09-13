#pragma once

#include "Support.h"

namespace live
{

/** Plays one of the two comparison files, through the device the song is already using.

    A comparison that leaves two WAVs beside the project and no way to hear them is a
    comparison a person finishes in Explorer. The files were being made and the listening
    - the one part of this that is actually a judgement - was the part with no button.

    It hangs a second audio callback on the engine's own device manager rather than
    opening a device of its own. Two programmes fighting over one sound card is a
    problem nobody should have to think about while deciding whether a bassline is
    better, and the engine is already holding the card.

    The song keeps playing if it was playing. That is deliberate and it is also a thing
    to be careful about: listening to A over the top of the song is not listening to A.
    The panel stops the transport before it starts one of these, and says so.
*/
class Listening final : private juce::Timer
{
public:
    explicit Listening (juce::AudioDeviceManager& sharedDevice) : device (sharedDevice) {}

    ~Listening() override
    {
        stop();
    }

    /** Starts one of the two files. Returns why not, or nothing if it started.

        A file that is not there is the ordinary case rather than an error: a person can
        press this before a comparison has been made, and being told "there is nothing
        to listen to yet" is the answer to that. */
    juce::String play (const juce::File& file, const juce::String& whichHalf)
    {
        stop();

        if (! file.existsAsFile())
            return "There is nothing to listen to yet - render a comparison first";

        formats.registerBasicFormats();

        auto* made = formats.createReaderFor (file);
        if (made == nullptr)
            return "That file could not be read: " + file.getFileName();

        source = std::make_unique<juce::AudioFormatReaderSource> (made, true);
        transport.setSource (source.get(), 0, nullptr, made->sampleRate);
        player.setSource (&transport);
        device.addAudioCallback (&player);

        playing = whichHalf;
        transport.setPosition (0.0);
        transport.start();
        startTimer (100);
        return {};
    }

    void stop()
    {
        stopTimer();
        transport.stop();
        transport.setSource (nullptr);
        player.setSource (nullptr);
        device.removeAudioCallback (&player);
        source.reset();
        playing.clear();
    }

    /** Which half is playing, or empty. What the panel shows, so a person listening to
        two nearly identical files knows which one is in front of them - without that,
        an A/B is two sounds and a guess. */
    juce::String nowPlaying() const { return playing; }

    bool isPlaying() const { return playing.isNotEmpty(); }

    /** Called when a file reaches its end, so the panel stops saying it is playing. */
    std::function<void()> onFinished;

private:
    void timerCallback() override
    {
        if (transport.isPlaying())
            return;

        // It ran out. Stopping here rather than leaving the callback attached means the
        // device is not carrying a silent source around for the rest of the session.
        stop();

        if (onFinished != nullptr)
            onFinished();
    }

    juce::AudioDeviceManager& device;
    juce::AudioFormatManager formats;
    juce::AudioSourcePlayer player;
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> source;
    juce::String playing;
};

} // namespace live
