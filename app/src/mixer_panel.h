#pragma once

// Compact mixer panel used by the GUI.  It is deliberately independent from
// MainComponent: strips only expose gain/pan/polarity/enable/monitor/mix-minus
// through callbacks, so the audio model remains the single source of truth.
struct MixerStripInfo
{
    juce::String id, name, kind; // kind: "source" | "bus" | "destination"
    float gainDb = 0.0f;
    float pan = 0.0f;         // sources (0 = centre, -1 = left, +1 = right)
    bool muted = false;
    bool solo = false;
    bool enabled = true;
    bool polarity = false;    // sources
    bool monitor = false;     // destinations
    int mixMinusCount = 0;    // destinations: excluded sources
    bool hasPan = false;
    bool hasPolarity = false;
    bool hasMonitor = false;
    bool hasMixMinus = false;
};

// Fixed metrics: controls never shrink below these values.  When the dock is too
// small the mixer scrolls (horizontal for many strips, vertical for a short
// dock) instead of compressing buttons and labels into unusable slivers.
namespace MixerMetrics
{
    constexpr int minStripWidth = 136;
    constexpr int maxStripWidth = 208;
    constexpr int padding = 8;
    constexpr int headerHeight = 40;
    constexpr int panHeight = 22;
    constexpr int faderMinHeight = 96;
    constexpr int buttonHeight = 26;
    constexpr int rowGap = 4;
    constexpr int buttonRows = 3; // solo / mute+polarity / enable+monitor|mix-minus
}

class MixerStrip final : public juce::Component
{
public:
    std::function<void (const juce::String&, const juce::String&, float)> onGain;
    std::function<void (const juce::String&, const juce::String&, float)> onPan;
    std::function<void (const juce::String&, const juce::String&, bool)> onMute;
    std::function<void (const juce::String&, const juce::String&, bool)> onSolo;
    std::function<void (const juce::String&, const juce::String&, bool)> onPolarity;
    std::function<void (const juce::String&, const juce::String&, bool)> onEnabled;
    std::function<void (const juce::String&, const juce::String&, bool)> onMonitor;
    // (kind, id, anchor component) -> MainComponent shows the mix-minus source
    // picker anchored to the strip; it owns the model update.
    std::function<void (const juce::String&, const juce::String&, juce::Component*)> onMixMinus;

    explicit MixerStrip (MixerStripInfo strip) : info (std::move (strip))
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

        addAndMakeVisible (pan);
        pan.setSliderStyle (juce::Slider::LinearHorizontal);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setValue (info.pan, juce::dontSendNotification);
        pan.setColour (juce::Slider::thumbColourId, Palette::cyan);
        pan.setColour (juce::Slider::trackColourId, Palette::violet.withAlpha (0.6f));
        pan.setTooltip (L10n::tr ("Pan (doppio clic = centro)", "Pan (double click = centre)"));
        pan.onValueChange = [this]
        {
            info.pan = (float) pan.getValue();
            if (onPan) onPan (info.kind, info.id, info.pan);
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

        addAndMakeVisible (polarity);
        polarity.setButtonText (juce::String::fromUTF8 ("\xc3\x98")); // Ø
        polarity.setColour (juce::TextButton::buttonOnColourId, Palette::warning);
        polarity.setClickingTogglesState (true);
        polarity.setTooltip (L10n::tr ("Inverti polarità", "Invert polarity"));
        polarity.setToggleState (info.polarity, juce::dontSendNotification);
        polarity.onClick = [this]
        {
            info.polarity = polarity.getToggleState();
            if (onPolarity) onPolarity (info.kind, info.id, info.polarity);
        };

        addAndMakeVisible (enableButton);
        enableButton.setColour (juce::TextButton::buttonOnColourId, Palette::success);
        enableButton.setClickingTogglesState (true);
        enableButton.setTooltip (L10n::tr ("Canale attivo", "Channel enabled"));
        enableButton.setToggleState (info.enabled, juce::dontSendNotification);
        enableButton.onClick = [this]
        {
            info.enabled = enableButton.getToggleState();
            updateButtonTexts();
            if (onEnabled) onEnabled (info.kind, info.id, info.enabled);
        };

        addAndMakeVisible (monitor);
        monitor.setButtonText ("MON");
        monitor.setColour (juce::TextButton::buttonOnColourId, Palette::cyan);
        monitor.setClickingTogglesState (true);
        monitor.setTooltip (L10n::tr ("Monitora questa destinazione",
                                      "Monitor this destination"));
        monitor.setToggleState (info.monitor, juce::dontSendNotification);
        monitor.onClick = [this]
        {
            info.monitor = monitor.getToggleState();
            if (onMonitor) onMonitor (info.kind, info.id, info.monitor);
        };

        addAndMakeVisible (mixMinusButton);
        mixMinusButton.setButtonText ("MM");
        mixMinusButton.setColour (juce::TextButton::buttonOnColourId, Palette::magenta);
        mixMinusButton.setTooltip (L10n::tr ("Mix-minus: escludi sorgenti da questa destinazione",
                                             "Mix-minus: exclude sources from this destination"));
        mixMinusButton.onClick = [this]
        {
            if (onMixMinus) onMixMinus (info.kind, info.id, &mixMinusButton);
        };

        updateButtonTexts();
        setInfo (info);
        setSize (MixerMetrics::minStripWidth, preferredHeight());
    }

    // Minimum height at which every control keeps its usable size.  The dock may
    // never hand a strip less than this: the mixer grows its content instead.
    static int preferredHeight()
    {
        return MixerMetrics::padding * 2
             + MixerMetrics::headerHeight
             + MixerMetrics::panHeight
             + MixerMetrics::faderMinHeight
             + MixerMetrics::buttonRows * (MixerMetrics::buttonHeight + MixerMetrics::rowGap);
    }

    // Deterministic layout checks used by the GUI test: no control clipped, no
    // button below the minimum height, nothing outside the strip.
    bool layoutIsUsable() const
    {
        if (getWidth() < MixerMetrics::minStripWidth || getHeight() < preferredHeight())
            return false;
        const auto bounds = getLocalBounds();
        for (const juce::Component* c : { (const juce::Component*) &fader,
                                          (const juce::Component*) &pan,
                                          (const juce::Component*) &mute,
                                          (const juce::Component*) &solo,
                                          (const juce::Component*) &polarity,
                                          (const juce::Component*) &enableButton,
                                          (const juce::Component*) &monitor,
                                          (const juce::Component*) &mixMinusButton })
        {
            if (c == nullptr || ! c->isVisible())
                continue;
            if (c->getHeight() < 18 || ! bounds.contains (c->getBounds()))
                return false;
        }
        return true;
    }

    void setInfo (const MixerStripInfo& next)
    {
        info = next;
        if (! fader.isMouseButtonDown())
            fader.setValue (info.gainDb, juce::dontSendNotification);
        if (! pan.isMouseButtonDown())
            pan.setValue (info.pan, juce::dontSendNotification);
        mute.setToggleState (info.muted, juce::dontSendNotification);
        solo.setToggleState (info.solo, juce::dontSendNotification);
        polarity.setToggleState (info.polarity, juce::dontSendNotification);
        enableButton.setToggleState (info.enabled, juce::dontSendNotification);
        monitor.setToggleState (info.monitor, juce::dontSendNotification);
        mute.setEnabled (info.enabled);
        solo.setEnabled (info.enabled);
        pan.setEnabled (info.enabled);
        polarity.setEnabled (info.enabled);
        pan.setVisible (info.hasPan);
        polarity.setVisible (info.hasPolarity);
        monitor.setVisible (info.hasMonitor);
        mixMinusButton.setVisible (info.hasMixMinus);
        updateButtonTexts();
        resized();
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
        juce::String subtitle = info.kind;
        if (info.hasPan)
            subtitle << "  " << (std::abs (info.pan) < 0.01f
                                     ? juce::String ("C")
                                     : (info.pan < 0.0f ? "L" + juce::String ((int) std::abs (info.pan * 100.0f))
                                                        : "R" + juce::String ((int) (info.pan * 100.0f))));
        if (info.hasMixMinus && info.mixMinusCount > 0)
            subtitle << "  MM-" << juce::String (info.mixMinusCount);
        g.drawFittedText (subtitle, label.toNearestInt(), juce::Justification::centred, 1, 0.8f);

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
        auto area = getLocalBounds().reduced (MixerMetrics::padding);
        area.removeFromTop (MixerMetrics::headerHeight);
        area.removeFromRight (18); // level meter column

        if (info.hasPan)
            pan.setBounds (area.removeFromTop (MixerMetrics::panHeight));

        // Rows are fixed-size: they never shrink with the dock.  If the dock is
        // shorter than preferredHeight() the content is taller and scrolls.
        const int fixedRows = MixerMetrics::buttonRows
                              * (MixerMetrics::buttonHeight + MixerMetrics::rowGap);
        fader.setBounds (area.removeFromTop (juce::jmax (MixerMetrics::faderMinHeight,
                                                         area.getHeight() - fixedRows)));

        auto nextRow = [&area]() -> juce::Rectangle<int>
        {
            area.removeFromTop (MixerMetrics::rowGap);
            return area.removeFromTop (MixerMetrics::buttonHeight);
        };

        solo.setBounds (nextRow().reduced (2, 0));

        auto muteRow = nextRow();
        if (info.hasPolarity)
        {
            mute.setBounds (muteRow.removeFromLeft (muteRow.getWidth() / 2 - 2));
            muteRow.removeFromLeft (4);
            polarity.setBounds (muteRow);
        }
        else
        {
            mute.setBounds (muteRow.reduced (2, 0));
        }

        auto lastRow = nextRow();
        const bool destinationExtras = info.hasMonitor || info.hasMixMinus;
        if (destinationExtras)
        {
            // Three equal buttons: ENABLE | MM | MON (no half-size slivers).
            const int third = juce::jmax (34, (lastRow.getWidth() - 4) / 3);
            enableButton.setBounds (lastRow.removeFromLeft (third));
            lastRow.removeFromLeft (2);
            if (info.hasMixMinus && info.hasMonitor)
            {
                mixMinusButton.setBounds (lastRow.removeFromLeft (third));
                lastRow.removeFromLeft (2);
                monitor.setBounds (lastRow);
            }
            else if (info.hasMixMinus)
            {
                mixMinusButton.setBounds (lastRow);
            }
            else
            {
                monitor.setBounds (lastRow);
            }
        }
        else
        {
            enableButton.setBounds (lastRow.reduced (2, 0));
        }
    }

private:
    void updateButtonTexts()
    {
        enableButton.setButtonText (info.enabled ? "ON" : "OFF");
        mixMinusButton.setButtonText (info.mixMinusCount > 0
                                          ? "MM-" + juce::String (info.mixMinusCount) : "MM");
    }

    MixerStripInfo info;
    juce::Slider fader, pan;
    juce::TextButton mute, solo, polarity, enableButton, monitor, mixMinusButton;
    float levelL = -100.0f, levelR = -100.0f;
};

class MixerComponent final : public juce::Component, private juce::Timer
{
public:
    std::function<std::vector<MixerStripInfo>()> getStrips;
    std::function<juce::Point<float> (const juce::String&)> getLevels;
    std::function<void (const juce::String&, const juce::String&, float)> onGain;
    std::function<void (const juce::String&, const juce::String&, float)> onPan;
    std::function<void (const juce::String&, const juce::String&, bool)> onMute;
    std::function<void (const juce::String&, const juce::String&, bool)> onSolo;
    std::function<void (const juce::String&, const juce::String&, bool)> onPolarity;
    std::function<void (const juce::String&, const juce::String&, bool)> onEnabled;
    std::function<void (const juce::String&, const juce::String&, bool)> onMonitor;
    std::function<void (const juce::String&, const juce::String&, juce::Component*)> onMixMinus;
    std::function<void()> onClose;

    MixerComponent()
    {
        addAndMakeVisible (viewport);
        addAndMakeVisible (close);
        close.setButtonText ("X");
        close.setTooltip (L10n::tr ("Chiudi mixer", "Close mixer"));
        close.onClick = [this] { if (onClose) onClose(); };
        viewport.setViewedComponent (&content, false);
        // Both directions: many strips scroll horizontally, a short dock scrolls
        // vertically instead of compressing the strip controls.
        viewport.setScrollBarsShown (true, true);
        startTimerHz (30);
    }

    // --- deterministic layout checks used by the GUI mixer test ---------------
    void refreshForTest() { refresh(); }
    int stripCount() const { return (int) strips.size(); }
    int stripWidthForTest() const
    {
        return strips.empty() ? 0 : strips.front()->getWidth();
    }
    int contentWidth() const { return content.getWidth(); }
    int contentHeight() const { return content.getHeight(); }
    juce::Rectangle<int> stripBounds (int index) const
    {
        if (index < 0 || index >= (int) strips.size())
            return {};
        return strips[(size_t) index]->getBounds();
    }
    bool stripsLayoutIsUsable() const
    {
        for (const auto& strip : strips)
            if (! strip->layoutIsUsable())
                return false;
        return true;
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

    void makeStrip (const MixerStripInfo& info)
    {
        auto strip = std::make_unique<MixerStrip> (info);
        strip->onGain = onGain;
        strip->onPan = onPan;
        strip->onMute = onMute;
        strip->onSolo = onSolo;
        strip->onPolarity = onPolarity;
        strip->onEnabled = onEnabled;
        strip->onMonitor = onMonitor;
        strip->onMixMinus = onMixMinus;
        content.addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }

    void refresh()
    {
        if (! getStrips)
            return;
        const auto infos = getStrips();
        while ((int) strips.size() < (int) infos.size())
            makeStrip (infos[(size_t) strips.size()]);
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
        const int count = juce::jmax (1, (int) strips.size());
        const int availableWidth = juce::jmax (1, viewport.getWidth() - 12);
        // Spread the strips over the dock up to a comfortable maximum; below the
        // minimum width the viewport scrolls horizontally instead of shrinking.
        const int stripWidth = juce::jlimit (MixerMetrics::minStripWidth,
                                             MixerMetrics::maxStripWidth,
                                             availableWidth / count);
        const int stripHeight = juce::jmax (viewport.getHeight() - 8,
                                            MixerStrip::preferredHeight());
        const int width = juce::jmax (availableWidth, count * stripWidth + 8);
        content.setSize (width, stripHeight + 8);
        for (int i = 0; i < (int) strips.size(); ++i)
            strips[(size_t) i]->setBounds (4 + i * stripWidth, 4, stripWidth, stripHeight);
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
