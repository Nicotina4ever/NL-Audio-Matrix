#pragma once

class DiagnosticsComponent final : public juce::Component, private juce::Timer
{
public:
    std::function<juce::String()> getReport;
    std::function<void()> onClose;

    DiagnosticsComponent()
    {
        addAndMakeVisible (title);
        L10n::bindLabelText (title, "DIAGNOSTICA / LATENZA E TELEMETRIA");
        title.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, Palette::text);
        addAndMakeVisible (report);
        report.setMultiLine (true);
        report.setReadOnly (true);
        report.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                                      juce::Font::plain)));
        report.setColour (juce::TextEditor::backgroundColourId, Palette::panel);
        report.setColour (juce::TextEditor::textColourId, Palette::text);
        report.setColour (juce::TextEditor::outlineColourId, Palette::outline);
        addAndMakeVisible (refresh);
        L10n::bindButtonText (refresh, "Aggiorna");
        refresh.onClick = [this] { updateText(); };
        addAndMakeVisible (closeButton);
        L10n::bindButtonText (closeButton, "Chiudi");
        closeButton.onClick = [this] { if (onClose) onClose(); };
        setSize (760, 520);
        updateText();
        startTimerHz (2);
    }

    // Force an immediate report refresh (the GUI verification mode uses it to
    // capture a populated panel without waiting for the 2 Hz timer).
    void refreshNow() { updateText(); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        title.setBounds (area.removeFromTop (36));
        auto footer = area.removeFromBottom (34);
        refresh.setBounds (footer.removeFromLeft (100));
        footer.removeFromLeft (8);
        closeButton.setBounds (footer.removeFromLeft (80));
        report.setBounds (area.reduced (2));
    }

private:
    void timerCallback() override { updateText(); }
    void updateText()
    {
        if (getReport)
            report.setText (getReport(), false);
    }

    juce::Label title;
    juce::TextEditor report;
    juce::TextButton refresh, closeButton;
};

class DiagnosticsWindow final : public juce::DocumentWindow
{
public:
    explicit DiagnosticsWindow (DiagnosticsComponent* content)
        : juce::DocumentWindow (L10n::tr ("NL Audio Matrix - Diagnostica"), Palette::background,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, true);
        setResizeLimits (480, 320, 1600, 1200);
        centreWithSize (760, 520);
        setVisible (true);
    }
    void closeButtonPressed() override { delete this; }
};
