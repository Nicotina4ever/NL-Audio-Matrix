#pragma once

#include "build_info.h"

// About / credits window.  Kept compact and layout-driven: contact rows stay
// readable at every supported size, the licence text scrolls rather than
// overlapping the contacts, and the build identity is always visible.
class AboutComponent final : public juce::Component
{
public:
    AboutComponent()
    {
        addAndMakeVisible (closeButton);
        L10n::bindButtonText (closeButton, "Chiudi");
        closeButton.onClick = [this]
        {
            if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
                window->closeButtonPressed();
        };

        addAndMakeVisible (copyButton);
        L10n::bindButtonText (copyButton, "Copia diagnostica");
        copyButton.onClick = [this]
        {
            juce::String text;
            text << "NL Audio Matrix\n"
                 << "Version: " << nlmatrix::buildVersion() << "\n"
                 << "Build ID: " << nlmatrix::buildId() << "\n"
                 << "Architecture: " << nlmatrix::buildArch() << "\n"
                 << "Compiler: " << nlmatrix::buildCompiler() << "\n"
                 << "Developer: Nicola Lanci\n"
                 << "Website: https://www.nicolalanci.com\n"
                 << "Email: info@nicolalanci.com\n"
                 << "GitHub: https://github.com/Nicotina4ever\n"
                 << "Repository: https://github.com/Nicotina4ever/NL-Audio-Matrix\n";
            juce::SystemClipboard::copyTextToClipboard (text);
            copyButton.setButtonText (L10n::tr ("Copiato"));
            juce::Timer::callAfterDelay (1200, [safe = juce::Component::SafePointer<AboutComponent> (this)]
            {
                if (safe != nullptr)
                    safe->copyButton.setButtonText (L10n::tr ("Copia diagnostica"));
            });
        };

        addAndMakeVisible (licence);
        licence.setMultiLine (true);
        licence.setReadOnly (true);
        licence.setScrollbarsShown (true);
        licence.setCaretVisible (false);
        licence.setPopupMenuEnabled (false);
        licence.setColour (juce::TextEditor::backgroundColourId, Palette::panel);
        licence.setColour (juce::TextEditor::textColourId, Palette::muted);
        licence.setColour (juce::TextEditor::outlineColourId, Palette::outline);
        licence.setFont (juce::Font (juce::FontOptions (13.5f)));
        licence.setText (
            L10n::tr ("COMPONENTI DI TERZE PARTI E LICENZE", "THIRD-PARTY COMPONENTS AND LICENCES")
            + L10n::utf8 ("\n\n"
                        "JUCE 9.0.4 — AGPLv3 / commercial licence (Roli/JUCE).\n"
                        "ASIO SDK — Steinberg ASIO licensing terms.\n"
                        "Microsoft SysVAD / Micy / Sweetly — MS-PL. The NL virtual driver is experimental and VM-only.\n"
                        "Windows SDK and Microsoft Visual C++ runtime — Microsoft terms.\n\n"
                        "No UAD, VB-Audio, ReaRoute or OBS code is redistributed.\n"
                        "The final licence for the NL Audio Matrix original code has not been chosen yet; this About window does not imply one."),
            false);

        addAndMakeVisible (repo);
        repo.setButtonText ("Repository");
        repo.setURL (juce::URL ("https://github.com/Nicotina4ever/NL-Audio-Matrix"));
        repo.setColour (juce::HyperlinkButton::textColourId, Palette::cyan);

        addAndMakeVisible (website);
        website.setButtonText ("nicolalanci.com");
        website.setURL (juce::URL ("https://www.nicolalanci.com"));
        website.setColour (juce::HyperlinkButton::textColourId, Palette::cyan);

        addAndMakeVisible (email);
        email.setButtonText ("info@nicolalanci.com");
        email.setURL (juce::URL ("mailto:info@nicolalanci.com"));
        email.setColour (juce::HyperlinkButton::textColourId, Palette::cyan);

        addAndMakeVisible (github);
        github.setButtonText ("GitHub");
        github.setURL (juce::URL ("https://github.com/Nicotina4ever"));
        github.setColour (juce::HyperlinkButton::textColourId, Palette::cyan);

        setSize (820, 620);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);
        auto bounds = getLocalBounds().toFloat().reduced (18.0f);

        auto hero = bounds.removeFromTop (112.0f);
        juce::ColourGradient gradient (Palette::panelRaised, hero.getTopLeft(),
                                       Palette::panel, hero.getBottomRight(), false);
        g.setGradientFill (gradient);
        g.fillRoundedRectangle (hero, 12.0f);
        g.setColour (Palette::violet.withAlpha (0.8f));
        g.drawRoundedRectangle (hero.reduced (0.5f), 12.0f, 1.0f);
        g.setColour (Palette::cyan);
        g.fillRoundedRectangle (hero.removeFromLeft (5.0f).reduced (10.0f, 22.0f), 2.5f);
        auto heroText = hero.reduced (26.0f, 16.0f).withTrimmedLeft (10.0f);
        g.setColour (Palette::text);
        g.setFont (juce::FontOptions (32.0f, juce::Font::bold));
        g.drawText ("NL AUDIO MATRIX", heroText.removeFromTop (40.0f), juce::Justification::centredLeft);
        g.setColour (Palette::muted);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (L10n::tr ("Routing e aggregazione audio professionale per Windows"),
                    heroText.removeFromTop (22.0f), juce::Justification::centredLeft);
        heroText.removeFromTop (4.0f);
        g.setColour (Palette::cyan);
        g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        g.drawText ("VERSION " + juce::String (nlmatrix::buildVersion())
                    + L10n::utf8 ("   •   BUILD ") + juce::String (nlmatrix::buildId()),
                    heroText.removeFromTop (20.0f), juce::Justification::centredLeft);

        auto lower = bounds.withTrimmedTop (12.0f);
        auto left = lower.removeFromLeft (juce::jmax (300.0f, lower.getWidth() * 0.42f));
        auto right = lower.withTrimmedLeft (12.0f);
        drawInfoBlock (g, left, L10n::tr ("SVILUPPATORE"),
                       L10n::tr ("Nicola Lanci\nVoice-over, tecnico del suono e autore software.\n"
                                 "Design, motore, GUI e integrazione di NL Audio Matrix.",
                                 "Nicola Lanci\nVoice-over artist, audio engineer and software author.\n"
                                 "Design, engine, GUI and integration of NL Audio Matrix."));
        drawInfoBlock (g, right, L10n::tr ("BUILD E RUNTIME"),
                       L10n::tr ("Architettura: ", "Architecture: ")
                           + juce::String (nlmatrix::buildArch()) + "\n"
                           + L10n::tr ("Compilatore: ", "Compiler: ")
                           + juce::String (nlmatrix::buildCompiler()) + "\n"
                           + L10n::tr ("Build: ", "Build timestamp: ")
                           + juce::String (nlmatrix::buildId()) + "\n"
                           + L10n::tr ("Driver: NLMatrix virtual audio (build sperimentale solo VM)\n",
                                       "Driver: NLMatrix virtual audio (VM-only experimental build)\n")
                           + L10n::tr ("Compatibilità: verifica Diagnostica per la versione del driver caricata.",
                                       "Compatibility: verify Diagnostics for the loaded driver version."));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);
        area.removeFromTop (112);
        auto footer = area.removeFromBottom (34);
        closeButton.setBounds (footer.removeFromRight (86).reduced (0, 2));
        footer.removeFromRight (8);
        copyButton.setBounds (footer.removeFromRight (132).reduced (0, 2));

        auto links = area.removeFromBottom (32);
        auto linkRow = links;
        website.setBounds (linkRow.removeFromLeft (juce::jmax (130, linkRow.getWidth() * 18 / 100)));
        email.setBounds (linkRow.removeFromLeft (juce::jmax (200, linkRow.getWidth() * 29 / 100)));
        github.setBounds (linkRow.removeFromLeft (juce::jmax (90, linkRow.getWidth() * 12 / 100)));
        repo.setBounds (linkRow);

        area.removeFromTop (220);
        licence.setBounds (area.reduced (0, 8));
    }

private:
    void drawInfoBlock (juce::Graphics& g, juce::Rectangle<float> area,
                        const juce::String& title, const juce::String& body)
    {
        g.setColour (Palette::panel);
        g.fillRoundedRectangle (area, 9.0f);
        g.setColour (Palette::outline.withAlpha (0.8f));
        g.drawRoundedRectangle (area, 9.0f, 1.0f);
        g.setColour (Palette::violet);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (title, area.reduced (16.0f, 10.0f).removeFromTop (20.0f).toNearestInt(),
                    juce::Justification::centredLeft);
        g.setColour (Palette::text);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText (body, area.reduced (16.0f, 10.0f).withTrimmedTop (24.0f).toNearestInt(),
                          juce::Justification::topLeft, 8, 0.9f);
    }

    juce::HyperlinkButton website, github, email, repo;
    juce::TextButton closeButton, copyButton;
    juce::TextEditor licence;
};

class AboutWindow final : public juce::DocumentWindow
{
public:
    AboutWindow()
        : juce::DocumentWindow (L10n::tr ("Info su NL Audio Matrix"), Palette::background,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AboutComponent(), true);
        setResizable (true, true);
        setResizeLimits (700, 540, 1200, 900);
        centreWithSize (820, 620);
        setVisible (true);
    }

    void closeButtonPressed() override { delete this; }
};
