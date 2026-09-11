#pragma once

#include <JuceHeader.h>

namespace live
{

/** One place for every colour, spacing and size the work surface uses.

    Before this, thirty-odd hex values were spread through four files and the same
    idea - a panel, a selected row, a dimmed label - was written slightly differently
    in each. Naming them is what makes them consistent; a token that is wrong is wrong
    once rather than in nine places.

    The palette is charcoal with a restrained teal accent, per docs/ui-polish-assets.ko.md.
*/
namespace theme
{
    // Grounds, darkest first.
    const juce::Colour window      { 0xff11151d };
    const juce::Colour panel       { 0xff161c26 };
    const juce::Colour panelHeader { 0xff1b2230 };
    const juce::Colour row         { 0xff1e2532 };
    const juce::Colour rowAlt      { 0xff222a38 };
    const juce::Colour sunken      { 0xff0e131b };

    // Lines and edges.
    const juce::Colour edge        { 0xff2a3242 };
    const juce::Colour edgeStrong  { 0xff3a4557 };

    // Text.
    const juce::Colour text        { 0xffd8dee9 };
    const juce::Colour textDim     { 0xff8698b6 };
    const juce::Colour textFaint   { 0xff5c6a83 };

    // The accent. Selection and focus are the same family so a person reads them as
    // "this is the one", and only one of them is ever a ring.
    const juce::Colour accent      { 0xff4fc3b0 };
    const juce::Colour accentDim   { 0xff2f6f66 };
    const juce::Colour selection   { 0xff27414a };

    // Meaning, not decoration.
    const juce::Colour good        { 0xff6fd39a };
    const juce::Colour warn        { 0xffffd479 };
    const juce::Colour danger      { 0xffff9a8c };
    const juce::Colour automation  { 0xffffd479 };
    const juce::Colour clipFill    { 0xff3f7f6f };

    // Spacing, in the window's own units.
    constexpr int gap        = 6;
    constexpr int pad        = 8;
    constexpr int rowHeight  = 26;
    constexpr int headerHeight = 18;

    // Type. Three sizes, used for three jobs, and nothing in between.
    inline juce::Font body()    { return juce::Font (juce::FontOptions (12.0f)); }
    inline juce::Font small_()  { return juce::Font (juce::FontOptions (10.0f)); }
    inline juce::Font heading() { return juce::Font (juce::FontOptions (11.0f, juce::Font::bold)); }
}

//==============================================================================
/** Draws the controls, so a knob, a fader and a button look like they came from the
    same place and say the same things about their state.

    Everything is drawn as vectors: nothing here is a bitmap, so it stays sharp at any
    display scale. State is never carried by colour alone - hover changes the edge,
    focus adds a ring, disabled drops contrast and the pointer stays where it was.
*/
class CoComposeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    CoComposeLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, theme::window);
        setColour (juce::Label::textColourId,                 theme::text);
        setColour (juce::TextEditor::backgroundColourId,      theme::sunken);
        setColour (juce::TextEditor::textColourId,            theme::text);
        setColour (juce::TextEditor::outlineColourId,         theme::edge);
        setColour (juce::TextEditor::focusedOutlineColourId,  theme::accent);
        setColour (juce::TextButton::buttonColourId,          theme::row);
        setColour (juce::TextButton::buttonOnColourId,        theme::selection);
        setColour (juce::TextButton::textColourOffId,         theme::text);
        setColour (juce::TextButton::textColourOnId,          theme::text);
        setColour (juce::ComboBox::backgroundColourId,        theme::row);
        setColour (juce::ComboBox::textColourId,              theme::text);
        setColour (juce::ComboBox::outlineColourId,           theme::edge);
        setColour (juce::ComboBox::arrowColourId,             theme::textDim);
        setColour (juce::PopupMenu::backgroundColourId,       theme::panel);
        setColour (juce::PopupMenu::textColourId,             theme::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::selection);
        setColour (juce::PopupMenu::highlightedTextColourId,  theme::text);
        setColour (juce::ScrollBar::thumbColourId,            theme::edgeStrong);
        setColour (juce::Slider::textBoxTextColourId,         theme::text);
        setColour (juce::Slider::textBoxBackgroundColourId,   theme::sunken);
        setColour (juce::Slider::textBoxOutlineColourId,      theme::edge);
    }

    // getLabelFont is deliberately not overridden: a label that asks for a size gets it.
    juce::Font getComboBoxFont (juce::ComboBox&) override           { return theme::body(); }
    juce::Font getPopupMenuFont() override                          { return theme::body(); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override  { return theme::body(); }

    //==============================================================================
    /** A 270 degree sweep with the gap at the bottom, drawn in layers: face, edge, the
        track the value runs along, the value itself, then the pointer. A knob whose
        range is symmetric about zero - pan, and anything else bipolar - fills from the
        centre towards the value instead of from the left, and shows nothing at all when
        it is centred. */
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float position, float startAngle, float endAngle,
                           juce::Slider& slider) override
    {
        const auto area = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
        const auto centre = area.getCentre();
        const auto enabled = slider.isEnabled();
        const auto over = slider.isMouseOverOrDragging();

        const auto faceRadius = radius * 0.68f;
        const auto arcRadius = radius * 0.92f;
        const auto angle = startAngle + position * (endAngle - startAngle);

        g.setColour (enabled ? theme::row : theme::row.withAlpha (0.5f));
        g.fillEllipse (juce::Rectangle<float> (faceRadius * 2.0f, faceRadius * 2.0f).withCentre (centre));

        g.setColour (over && enabled ? theme::edgeStrong : theme::edge);
        g.drawEllipse (juce::Rectangle<float> (faceRadius * 2.0f, faceRadius * 2.0f).withCentre (centre), 1.0f);

        const auto thickness = juce::jmax (1.5f, radius * 0.16f);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (theme::sunken);
        g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        // Bipolar when the range is symmetric: a pan knob at centre has no value to show.
        const auto range = slider.getRange();
        const auto bipolar = range.getStart() < 0.0
                               && std::abs (range.getStart() + range.getEnd()) < 1.0e-6;
        const auto from = bipolar ? startAngle + 0.5f * (endAngle - startAngle) : startAngle;

        if (std::abs (angle - from) > 1.0e-3f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (enabled ? theme::accent : theme::accentDim);
            g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }

        // The pointer always agrees with the end of the value arc, at every value.
        juce::Path pointer;
        pointer.addLineSegment ({ centre.x, centre.y - faceRadius * 0.25f,
                                  centre.x, centre.y - faceRadius * 0.92f }, 0.0f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle, centre.x, centre.y));
        g.setColour (enabled ? theme::text : theme::textFaint);
        g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.2f, radius * 0.11f),
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));

        if (slider.hasKeyboardFocus (false))
        {
            g.setColour (theme::accent.withAlpha (0.7f));
            g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre), 1.2f);
        }
    }

    //==============================================================================
    /** A fader with a mark at 0 dB, so the resting point of a channel is visible without
        reading the number underneath it. */
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        const auto enabled = slider.isEnabled();
        const auto over = slider.isMouseOverOrDragging();
        const auto vertical = style == juce::Slider::LinearVertical
                                || style == juce::Slider::LinearBarVertical;

        auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
        const auto trackWidth = juce::jlimit (3.0f, 8.0f,
                                              (vertical ? area.getWidth() : area.getHeight()) * 0.16f);

        auto track = vertical ? area.withSizeKeepingCentre (trackWidth, area.getHeight())
                              : area.withSizeKeepingCentre (area.getWidth(), trackWidth);
        g.setColour (theme::sunken);
        g.fillRoundedRectangle (track, trackWidth * 0.5f);

        // Where zero sits, when the range crosses it.
        const auto range = slider.getRange();
        if (range.getStart() < 0.0 && range.getEnd() > 0.0)
        {
            const auto zero = static_cast<float> ((0.0 - range.getStart()) / range.getLength());
            const auto at = vertical ? juce::jmap (zero, area.getBottom(), area.getY())
                                     : juce::jmap (zero, area.getX(), area.getRight());
            g.setColour (theme::edgeStrong);
            if (vertical) g.fillRect (area.getX(), at - 0.5f, area.getWidth(), 1.0f);
            else          g.fillRect (at - 0.5f, area.getY(), 1.0f, area.getHeight());
        }

        auto filled = track;
        if (vertical) filled = filled.withTop (sliderPos);
        else          filled = filled.withRight (sliderPos);

        g.setColour (enabled ? theme::accent.withAlpha (0.55f) : theme::accentDim.withAlpha (0.4f));
        g.fillRoundedRectangle (filled, trackWidth * 0.5f);

        // A fader handle is wide across the track and short along it, so the line it
        // sits on stays readable. Fourteen to eighteen units, per the asset notes, with
        // a lower bound so it does not vanish on a small control.
        const auto across = juce::jlimit (10.0f, 26.0f,
                                          (vertical ? area.getWidth() : area.getHeight()) * 0.62f);
        const auto along = juce::jlimit (8.0f, 14.0f, across * 0.55f);
        auto handle = vertical
                        ? juce::Rectangle<float> (across, along).withCentre ({ area.getCentreX(), sliderPos })
                        : juce::Rectangle<float> (along, across).withCentre ({ sliderPos, area.getCentreY() });

        // Dark body, bright line across the middle: the line is what a person reads the
        // position from, so the handle itself does not need to be the brightest thing.
        g.setColour (enabled ? theme::rowAlt : theme::rowAlt.withAlpha (0.5f));
        g.fillRoundedRectangle (handle, 2.0f);
        g.setColour (over && enabled ? theme::accent : theme::edgeStrong);
        g.drawRoundedRectangle (handle, 2.0f, 1.0f);

        g.setColour (enabled ? theme::text : theme::textFaint);
        if (vertical) g.fillRect (handle.getX() + 2.0f, handle.getCentreY() - 0.5f, handle.getWidth() - 4.0f, 1.0f);
        else          g.fillRect (handle.getCentreX() - 0.5f, handle.getY() + 2.0f, 1.0f, handle.getHeight() - 4.0f);

        if (slider.hasKeyboardFocus (false))
        {
            g.setColour (theme::accent.withAlpha (0.7f));
            g.drawRoundedRectangle (area.reduced (1.0f), 3.0f, 1.2f);
        }

        juce::ignoreUnused (minPos, maxPos);
    }

    //==============================================================================
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                               bool over, bool down) override
    {
        const auto area = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto enabled = button.isEnabled();
        const auto on = button.getToggleState();

        auto fill = on ? theme::selection : theme::row;
        if (! enabled)   fill = fill.withAlpha (0.45f);
        else if (down)   fill = fill.darker (0.35f);
        else if (over)   fill = fill.brighter (0.18f);

        g.setColour (fill);
        g.fillRoundedRectangle (area, 3.0f);

        g.setColour (! enabled ? theme::edge.withAlpha (0.5f)
                               : on ? theme::accent : (over ? theme::edgeStrong : theme::edge));
        g.drawRoundedRectangle (area, 3.0f, 1.0f);

        if (button.hasKeyboardFocus (false))
        {
            g.setColour (theme::accent.withAlpha (0.8f));
            g.drawRoundedRectangle (area.reduced (1.5f), 2.0f, 1.0f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        g.setFont (theme::body());
        g.setColour (button.isEnabled() ? theme::text : theme::textFaint);
        g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (4, 0),
                          juce::Justification::centred, 1);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool,
                       int, int, int, int, juce::ComboBox& box) override
    {
        const auto area = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
        g.setColour (box.isEnabled() ? theme::row : theme::row.withAlpha (0.45f));
        g.fillRoundedRectangle (area, 3.0f);
        g.setColour (box.isMouseOver() && box.isEnabled() ? theme::edgeStrong : theme::edge);
        g.drawRoundedRectangle (area, 3.0f, 1.0f);

        juce::Path arrow;
        const auto centre = juce::Point<float> (area.getRight() - 10.0f, area.getCentreY());
        arrow.addTriangle (centre.x - 4.0f, centre.y - 2.0f,
                           centre.x + 4.0f, centre.y - 2.0f,
                           centre.x,        centre.y + 3.0f);
        g.setColour (box.isEnabled() ? theme::textDim : theme::textFaint);
        g.fillPath (arrow);

        if (box.hasKeyboardFocus (false))
        {
            g.setColour (theme::accent.withAlpha (0.8f));
            g.drawRoundedRectangle (area.reduced (1.5f), 2.0f, 1.0f);
        }
    }
};

//==============================================================================
/** A slider with the two gestures people arrive expecting, whatever shape it is.

    Double-click puts it back where it started. Right-click asks for a number, because
    dragging is hopeless when you want exactly -6 dB and the control is twenty pixels
    across. Neither is a preference: they are what every mixer does, and a control
    without them reads as unfinished no matter how well it is drawn.

    Knobs and faders both get it, which is why this is not called a knob. The default
    is whatever the control was set up with, so "back to default" means what the
    project means by it rather than wherever the slider's range happens to begin.
*/
class ValueSlider final : public juce::Slider
{
public:
    explicit ValueSlider (juce::Slider::SliderStyle style = juce::Slider::RotaryVerticalDrag)
        : juce::Slider (style, juce::Slider::NoTextBox) {}

    /** Sets the value and remembers it as the one to come back to. */
    void setDefaultValue (double value)
    {
        defaultValue = value;
        setDoubleClickReturnValue (true, value);
    }

    /** How a number should read once typed - "dB", "Hz", or empty. Display only. */
    juce::String units;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isRightButtonDown())
        {
            juce::Slider::mouseDown (e);
            return;
        }

        juce::PopupMenu menu;
        menu.addItem (1, "Type a value...");
        menu.addItem (2, "Back to " + juce::String (defaultValue, 2) + units);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this] (int chosen)
        {
            if (chosen == 2)
                setValue (defaultValue, juce::sendNotificationSync);
            else if (chosen == 1)
                askForANumber();
        });
    }

private:
    void askForANumber()
    {
        auto* box = new juce::AlertWindow (getName().isNotEmpty() ? getName() : "Value",
                                           "Type a value" + (units.isNotEmpty() ? " in " + units : juce::String()),
                                           juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("value", juce::String (getValue(), 3));
        box->addButton ("Set", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        box->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, box] (int result)
            {
                std::unique_ptr<juce::AlertWindow> owned (box);

                if (result != 1)
                    return;

                const auto typed = owned->getTextEditorContents ("value").trim();

                // A box that was left empty, or filled with something that is not a
                // number, means "never mind" - not zero, which would be an edit nobody
                // asked for and, on a gain control, a silent one.
                if (typed.isEmpty() || ! typed.containsOnly ("0123456789.,-+eE"))
                    return;

                setValue (juce::jlimit (getMinimum(), getMaximum(), typed.getDoubleValue()),
                          juce::sendNotificationSync);
            }), false);
    }

    double defaultValue = 0.0;
};


} // namespace live
