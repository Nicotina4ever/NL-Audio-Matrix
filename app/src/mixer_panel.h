#pragma once

// Compact mixer panel used by the GUI.  It is deliberately independent from
// MainComponent: strips only expose gain/mute/level through callbacks, so the
// audio model remains the single source of truth.
struct MixerStripInfo
{
    juce::String id, name, kind;
    float gainDb = 0.0f;
    bool muted = false;
    bool solo = false;
    bool enabled = true;
};

class MixerStrip final : public juce::Component
{
public:
    MixerStrip (MixerStripInfo strip,
                std::function<void (const juce::String&, const juce::String&, float)> gainCallback,
                std::function<void (const juce::String&, const juce::String&, bool)> muteCallback,
                std::function<void (const juce::String&, const juce::String&, bool)> soloCallback)
        : info (std::move (strip)), onGain (std::move (gainCallback)), onMute (std::move (muteCallback)), onSolo (std::move (soloCallback))
    {
        addAndMakeVisible (fader);
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 58, 18);
        fader.setRange (-60.0, 12.0, 0.1);
        fader.setSkewFactorFromMidPoint (-12.0);
        fader.setValue (info.gainDb, juce::dontSendNotification);
        // C9: semantic fader colours; the same tokens are re-applied by the
        // window palette pass on every theme change.
        fader.setColour (juce::Slider::thumbColourId, Palette::cyan);
        fader.setColour (juce::Slider::trackColourId,
                         Palette::isLight() ? Palette::violet.withAlpha (0.75f) : Palette::violet);
        fader.setColour (juce::Slider::backgroundColourId, Palette::panelRaised);
        fader.onValueChange = [this]
        {
            info.gainDb = (float) fader.getValue();
            if (onGain) onGain (info.kind, info.id, info.gainDb);
        };

        addAndMakeVisible (mute);
        mute.setButtonText ("M");
        mute.setColour (juce::TextButton::buttonOnColourId, Palette::magenta);
        mute.setClickingTogglesState (true);
        mute.setToggleState (info.muted, juce::dontSendNotification);
        mute.onClick = [this]
        {
            info.muted = mute.getToggleState();
            if (onMute) onMute (info.kind, info.id, info.muted);
        };
        addAndMakeVisible (solo);
        solo.setButtonText ("S");
        solo.setColour (juce::TextButton::buttonOnColourId, Palette::cyan);
        solo.setClickingTogglesState (true);
        solo.setToggleState (info.solo, juce::dontSendNotification);
        solo.onClick = [this]
        {
            info.solo = solo.getToggleState();
            if (onSolo) onSolo (info.kind, info.id, info.solo);
        };
        setSize (112, 312);
    }

    void setInfo (const MixerStripInfo& next)
    {
        info = next;
        if (! fader.isMouseButtonDown())
            fader.setValue (info.gainDb, juce::dontSendNotification);
        mute.setToggleState (info.muted, juce::dontSendNotification);
        solo.setToggleState (info.solo, juce::dontSendNotification);
        mute.setEnabled (info.enabled);
        solo.setEnabled (info.enabled);
        repaint();
    }

    void setLevels (float l, float r)
    {
        if (std::abs (levelL - l) < 0.01f && std::abs (levelR - r) < 0.01f)
            return;
        levelL = l;
        levelR = r;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Palette::panel.brighter (0.03f));
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (info.enabled ? Palette::outline : Palette::magenta.withAlpha (0.7f));
        g.drawRoundedRectangle (bounds, 8.0f, 1.0f);

        auto label = bounds.removeFromTop (36.0f).reduced (6.0f, 2.0f);
        g.setColour (info.enabled ? Palette::text : Palette::text.withAlpha (0.5f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (info.name, label.removeFromTop (17.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.8f);
        g.setColour (Palette::text.withAlpha (0.55f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText (info.kind, label.toNearestInt(), juce::Justification::centred, 1, 0.8f);

        auto meter = juce::Rectangle<float> (bounds.getRight() - 14.0f, bounds.getY() + 6.0f,
                                             8.0f, bounds.getHeight() - 58.0f);
        g.setColour (Palette::background.darker (0.4f));
        g.fillRoundedRectangle (meter, 3.0f);
        auto drawLevel = [&g, meter] (float db, float x)
        {
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto bar = meter.withTrimmedTop (meter.getHeight() * (1.0f - norm))
                            .withX (x).withWidth (3.0f);
            g.setColour (db > -3.0f ? Palette::magenta : db > -12.0f ? Palette::cyan : Palette::violet);
            g.fillRoundedRectangle (bar, 2.0f);
        };
        drawLevel (levelL, meter.getX());
        drawLevel (levelR, meter.getX() + 5.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        area.removeFromTop (38);
        area.removeFromRight (18);
        // Leave exactly enough room for the 4 px gap plus solo/mute rows so the
        // dB box is never cut by the pane bottom on shorter docks.
        fader.setBounds (area.removeFromTop (juce::jmax (80, area.getHeight() - 59)));
        area.removeFromTop (4);
        solo.setBounds (area.removeFromTop (24).reduced (4, 0));
        area.removeFromTop (3);
        mute.setBounds (area.removeFromTop (28).reduced (4, 0));
    }

private:
    MixerStripInfo info;
    std::function<void (const juce::String&, const juce::String&, float)> onGain;
    std::function<void (const juce::String&, const juce::String&, bool)> onMute;
    std::function<void (const juce::String&, const juce::String&, bool)> onSolo;
    juce::Slider fader;
    juce::TextButton mute, solo;
    float levelL = -100.0f, levelR = -100.0f;
};

class MixerComponent final : public juce::Component, private juce::Timer
{
public:
    std::function<std::vector<MixerStripInfo>()> getStrips;
    std::function<juce::Point<float> (const juce::String&)> getLevels;
    std::function<void (const juce::String&, const juce::String&, float)> onGain;
    std::function<void (const juce::String&, const juce::String&, bool)> onMute;
    std::function<void (const juce::String&, const juce::String&, bool)> onSolo;
    std::function<void()> onClose;

    MixerComponent()
    {
        addAndMakeVisible (viewport);
        addAndMakeVisible (close);
        close.setButtonText ("X");
        close.setTooltip (L10n::tr ("Chiudi mixer", "Close mixer"));
        close.onClick = [this] { if (onClose) onClose(); };
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (false, true);
        startTimerHz (30);
    }

    void resized() override
    {
        auto header = getLocalBounds().reduced (8).removeFromTop (24);
        close.setBounds (header.removeFromRight (28));
        viewport.setBounds (getLocalBounds().reduced (8).withTrimmedTop (28));
        layoutStrips();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);
        g.setColour (Palette::text);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText ("MIXER", getLocalBounds().reduced (12).removeFromTop (22),
                    juce::Justification::centredLeft);
        if (strips.empty())
        {
            g.setColour (Palette::muted);
            g.setFont (juce::FontOptions (14.0f));
            g.drawText (L10n::tr ("Nessun canale — aggiungi una sorgente o una destinazione al canvas.",
                                  "No channels yet — add a source or destination to the canvas."),
                        getLocalBounds().reduced (24).withTrimmedTop (32),
                        juce::Justification::centredLeft, true);
        }
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        if (! getStrips)
            return;
        const auto infos = getStrips();
        while ((int) strips.size() < (int) infos.size())
        {
            auto strip = std::make_unique<MixerStrip> (infos[(size_t) strips.size()], onGain, onMute, onSolo);
            content.addAndMakeVisible (*strip);
            strips.push_back (std::move (strip));
        }
        while ((int) strips.size() > (int) infos.size())
        {
            content.removeChildComponent (strips.back().get());
            strips.pop_back();
        }
        for (int i = 0; i < (int) infos.size(); ++i)
        {
            strips[(size_t) i]->setInfo (infos[(size_t) i]);
            if (getLevels)
            {
                const auto levels = getLevels (infos[(size_t) i].id);
                strips[(size_t) i]->setLevels (levels.x, levels.y);
            }
        }
        layoutStrips();
    }

    void layoutStrips()
    {
        const int stripWidth = 112;
        const int viewportWidth = juce::jmax (1, viewport.getWidth() - 12);
        const int count = juce::jmax (1, (int) strips.size());
        const int width = juce::jmax (viewportWidth, count * stripWidth + 8);
        // Fit strips to the visible pane: the old fixed 320/330 px design height
        // was taller than the dock at 1366x768, clipping fader/dB/buttons.
        const int stripHeight = juce::jmax (240, viewport.getHeight() - 8);
        content.setSize (width, stripHeight + 8);
        for (int i = 0; i < (int) strips.size(); ++i)
            strips[(size_t) i]->setBounds (4 + i * stripWidth, 4, stripWidth - 4, stripHeight);
    }

    juce::Viewport viewport;
    juce::Component content;
    juce::TextButton close;
    std::vector<std::unique_ptr<MixerStrip>> strips;
};

class MixerResizer final : public juce::Component
{
public:
    std::function<void (int)> onDragDelta;
    MixerResizer() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
    void mouseDown (const juce::MouseEvent& e) override { dragStart = e.getPosition().y; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int delta = e.getPosition().y - dragStart;
        dragStart = e.getPosition().y;
        if (onDragDelta) onDragDelta (-delta);
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (Palette::outline.withAlpha (0.6f));
        g.fillRect (getLocalBounds().reduced (4, 2));
    }
private:
    int dragStart = 0;
};

class MixerWindow final : public juce::DocumentWindow
{
public:
    explicit MixerWindow (MixerComponent* mixerContent)
        : juce::DocumentWindow ("NL Audio Matrix - Mixer", Palette::background,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (mixerContent, true);
        setResizable (true, true);
        setResizeLimits (420, 340, 2400, 1400);
        centreWithSize (900, 420);
        setVisible (true);
    }

    void closeButtonPressed() override { delete this; }
};
