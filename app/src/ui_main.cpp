/*
    NLMatrixUI — first graphical patchbay for NL Audio Matrix.

    The GUI edits a profile in memory, writes the same JSON format consumed by
    NLMatrixRouter, launches the router as a child process and displays its
    per-second meter output. This keeps the audio engine independent from the
    UI and gives us a usable graphical workflow without waiting for a full
    in-process refactor.

    Virtual endpoint creation (NL Mic / NL Return) still requires the kernel
    driver; the GUI can already reference those endpoints when they exist.
*/

#include <juce_audio_devices/juce_audio_devices.h>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <tlhelp32.h>
 #include <shlobj.h>
 #include <shellapi.h>
 #include <mmdeviceapi.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <propsys.h>
 #include <propvarutil.h>
#endif

#include "build_info.h"
#include "../../driver/sysvad-nl/nlmatrix_ioctl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace
{
    // Visual language derived from the Nicola Lanci Voice Over mark: deep
    // violet, neon fuchsia and cyan, with restrained glow rather than a gaming
    // look. All colours are centralised here so future views stay coherent.
    static void fitComponentToWorkArea (juce::Component& component, int width, int height)
    {
        int w = width;
        int h = height;
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = display->userArea;
            w = juce::jmin (w, juce::jmax (360, area.getWidth() - 60));
            h = juce::jmin (h, juce::jmax (240, area.getHeight() - 80));
        }
        component.setSize (juce::jmax (360, w), juce::jmax (240, h));
    }
    enum class ThemeMode { dark, system, light };

    namespace Palette
    {
        // Semantic tokens.  They are mutable so the whole application can
        // switch theme at runtime without reconstructing panels.
        inline juce::Colour background   { 0xff0b0714 };
        inline juce::Colour panel        { 0xff171027 };
        inline juce::Colour panelRaised  { 0xff211536 };
        inline juce::Colour outline      { 0xff5b3b8c };
        inline juce::Colour violet       { 0xff9b5cff };
        inline juce::Colour magenta      { 0xffec4899 };
        inline juce::Colour cyan         { 0xff22d3ee };
        inline juce::Colour text         { 0xfffdfbff };
        inline juce::Colour muted        { 0xffccbfe8 };
        inline juce::Colour disabled     { 0xff8d80a6 };
        inline juce::Colour canvas       { 0xff0a0712 };
        inline juce::Colour cable        { 0xff8b5cf6 };
        inline juce::Colour success      { 0xff34d399 };
        inline juce::Colour warning      { 0xfffbbf24 };
        inline juce::Colour danger       { 0xfffb7185 };

        // True for the Light palette; used where a token needs an alpha tweak to
        // stay visible on a bright background (e.g. slider fader fill).
        inline bool isLight() { return background.getPerceivedBrightness() > 0.5f; }
    }

    inline bool isSystemLightTheme()
    {
       #if JUCE_WINDOWS
        HKEY key = nullptr;
        if (RegOpenKeyExW (HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                           0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            DWORD value = 1, size = sizeof (value), type = 0;
            const auto result = RegQueryValueExW (key, L"AppsUseLightTheme", nullptr, &type,
                                                  reinterpret_cast<LPBYTE> (&value), &size);
            RegCloseKey (key);
            if (result == ERROR_SUCCESS) return value != 0;
        }
       #endif
        return false;
    }

    inline void applyPalette (ThemeMode mode)
    {
        const bool light = mode == ThemeMode::light
                           || (mode == ThemeMode::system && isSystemLightTheme());
        if (light)
        {
            Palette::background  = juce::Colour (0xfff5f3fa);
            Palette::panel       = juce::Colour (0xffffffff);
            Palette::panelRaised = juce::Colour (0xffeeeaf8);
            Palette::outline     = juce::Colour (0xffcbc2e2);
            Palette::violet      = juce::Colour (0xff6d28d9);
            Palette::magenta     = juce::Colour (0xffc026d3);
            Palette::cyan        = juce::Colour (0xff0891b2);
            Palette::text        = juce::Colour (0xff141021);
            Palette::muted       = juce::Colour (0xff4a4159);
            Palette::disabled    = juce::Colour (0xff7b7290);
            Palette::canvas      = juce::Colour (0xfff1eef8);
            Palette::cable       = juce::Colour (0xff7c3aed);
            Palette::success     = juce::Colour (0xff047857);
            Palette::warning     = juce::Colour (0xffb45309);
            Palette::danger      = juce::Colour (0xffbe123c);
        }
        else
        {
            Palette::background  = juce::Colour (0xff0b0714);
            Palette::panel       = juce::Colour (0xff171027);
            Palette::panelRaised = juce::Colour (0xff211536);
            Palette::outline     = juce::Colour (0xff5b3b8c);
            Palette::violet      = juce::Colour (0xff9b5cff);
            Palette::magenta     = juce::Colour (0xffec4899);
            Palette::cyan        = juce::Colour (0xff22d3ee);
            Palette::text        = juce::Colour (0xfffdfbff);
            Palette::muted       = juce::Colour (0xffccbfe8);
            Palette::disabled    = juce::Colour (0xff8d80a6);
            Palette::canvas      = juce::Colour (0xff0a0712);
            Palette::cable       = juce::Colour (0xff8b5cf6);
            Palette::success     = juce::Colour (0xff34d399);
            Palette::warning     = juce::Colour (0xfffbbf24);
            Palette::danger      = juce::Colour (0xfffb7185);
        }
    }

    //==============================================================================
    // Localization (C8).
    //
    // Italian is the product default; English is switched at runtime from the
    // View menu or Preferences and persisted in preferences.json.  Every user
    // visible string is looked up through L10n::tr(): the Italian source text is
    // the key, so an entry that has no English translation yet falls back to
    // readable Italian instead of a raw identifier.
    //
    // Widgets register their text once through the bind helpers below.  A
    // language switch re-applies every live binding, so menus, top bar, section
    // titles and open panels refresh without recreating windows.  Bindings hold
    // juce::Component::SafePointer, so a panel that has been closed simply drops
    // out instead of dangling.
    namespace L10n
    {
        enum class Language { italian, english };

        inline Language& currentLanguageVar() { static Language language = Language::italian; return language; }
        inline Language currentLanguage() { return currentLanguageVar(); }
        inline void setLanguage (Language language) { currentLanguageVar() = language; }
        inline juce::String languageCode() { return currentLanguage() == Language::english ? "en" : "it"; }
        inline Language languageFromCode (const juce::String& code)
        {
            return code.trim().toLowerCase().startsWith ("en") ? Language::english
                                                              : Language::italian;
        }

        struct Translation
        {
            const char* italian;
            const char* english;
        };

        inline const Translation* translationTable (int& count)
        {
            static const Translation entries[] =
            {
                // ---- application menu ----
                { "File", "File" },
                { "Routing", "Routing" },
                { "Dispositivi", "Devices" },
                { "Strumenti", "Tools" },
                { "Vista", "View" },
                { "Aiuto", "Help" },
                { "Nuovo progetto", "New project" },
                { "Apri profilo...", "Open profile..." },
                { "Salva profilo...", "Save profile..." },
                { "Esci", "Exit" },
                { "Aggiungi sorgente", "Add source" },
                { "Aggiungi destinazione", "Add destination" },
                { "Aggiungi bus", "Add bus" },
                { "Avvia / ferma motore", "Start / stop engine" },
                { "Applica live", "Apply live" },
                { "Aggiorna dispositivi", "Refresh devices" },
                { "Dispositivi virtuali...", "Virtual devices..." },
                { "Info motore ASIO", "ASIO engine info" },
                { "Browser dispositivi...", "Device browser..." },
                { "Diagnostica / telemetria...", "Diagnostics / telemetry..." },
                { "Preferenze...", "Preferences..." },
                { "Adatta canvas", "Fit routing canvas" },
                { "Salva layout...", "Save layout..." },
                { "Carica layout...", "Load layout..." },
                { "Ripristina pannelli", "Reset panels" },
                { "Tema: Scuro", "Theme: Dark" },
                { "Tema: Sistema", "Theme: System" },
                { "Tema: Chiaro", "Theme: Light" },
                { "Lingua: Italiano", "Language: Italian" },
                { "Lingua: English", "Language: English" },
                { "Scorciatoie da tastiera", "Keyboard shortcuts" },
                { "Mostra NL Audio Matrix", "Show NL Audio Matrix" },
                { "Nascondi finestra", "Hide window" },
                { "Avvia engine audio", "Start audio engine" },
                { "Ferma engine audio", "Stop audio engine" },
                { "Info su NL Audio Matrix", "About NL Audio Matrix" },
                // ---- main window ----
                { "Motore ASIO principale", "Main ASIO engine" },
                { "Pronto", "Ready" },
                { "Avvia", "Start" },
                { "Ferma", "Stop" },
                { "Carica profilo...", "Load profile..." },
                { "Aggiorna", "Refresh" },
                { "Nome profilo", "Profile name" },
                { "Nuovo profilo", "New profile" },
                { "Sorgenti (ingressi)", "Sources (inputs)" },
                { "Destinazioni (uscite)", "Destinations (outputs)" },
                { "Bus interni (nessun device Windows)", "Internal buses (no Windows device)" },
                { "Rotte", "Routes" },
                { "Misuratori", "Meters" },
                { "Connessioni", "Connections" },
                { "Rimuovi", "Remove" },
                { "Aggiorna selezionato", "Update" },
                { "Rimuovi selezionato", "Remove selected" },
                { "Rimuovi rotta", "Remove selected" },
                { "Scegli canali...", "Pick channels..." },
                { "Scegli app...", "Pick app..." },
                { "Processi figli inclusi (API Windows)", "Child processes included (Windows API)" },
                { "Sorgente attiva", "Source enabled" },
                { "Aggiungi rotta", "Add route" },
                { "Rotta sorgente", "Route source" },
                { "Rotta destinazione", "Route destination" },
                { "Guadagno dB", "Gain dB" },
                { "Attiva", "Enabled" },
                { "Ingresso ASIO", "ASIO input" },
                { "WASAPI loopback (audio di sistema)", "WASAPI loopback (system playback)" },
                { "WASAPI capture (dispositivo di ingresso)", "WASAPI capture (input device)" },
                { "Cattura processo (per app)", "Process capture (per app)" },
                { "Bus interno", "Internal bus" },
                { "Uscita ASIO", "ASIO output" },
                { "Endpoint render Windows", "Windows render endpoint" },
                { "Endpoint virtuale NL (driver)", "NL virtual endpoint (driver)" },
                // ---- editor placeholders ----
                { "nome sorgente", "source name" },
                { "canali ASIO, es. 1,2", "ASIO channels, e.g. 1,2" },
                { "pan -1..1", "pan -1..1" },
                { "processo.exe o PID", "process.exe or PID" },
                { "guadagno sorgente dB", "source gain dB" },
                { "nome destinazione", "destination name" },
                { "guadagno destinazione dB", "destination gain dB" },
                { "nome bus (es. NL Mic Marco)", "bus name (e.g. NL Mic Marco)" },
                { "guadagno bus dB", "bus gain dB" },
                { "guadagno dB", "gain dB" },
                // ---- preferences ----
                { "PREFERENZE", "PREFERENCES" },
                { "Modalità avanzata (sample rate e buffer manuali)", "Advanced mode (manual sample rate and buffer)" },
                { "Profilo latenza", "Latency profile" },
                { "Bassa latenza", "Low latency" },
                { "Buffer del motore", "Engine buffer" },
                { "Avanzate", "Advanced" },
                { "Sample rate del motore master", "Master engine sample rate" },
                { "Buffer ASIO (campioni)", "ASIO buffer (samples)" },
                { "Ring minimo (ms)", "Ring minimum (ms)" },
                { "Ring massimo (ms)", "Ring maximum (ms)" },
                { "Driver minimo (ms)", "Driver minimum (ms)" },
                { "Driver massimo (ms)", "Driver maximum (ms)" },
                { "Chunk scrittura (ms)", "Write chunk (ms)" },
                { "Intervallo servo (ms)", "Servo interval (ms)" },
                { "Applica", "Apply" },
                { "Annulla", "Cancel" },
                { "Chiudi", "Close" },
                { "Lingua", "Language" },
                { "Il profilo latenza consiglia il buffer ASIO; il buffer del motore è indipendente. "
                  "Le modifiche al buffer del motore riavviano il router; sample rate/buffer ASIO riaprono il device.",
                  "The latency profile only recommends the ASIO buffer; the engine buffer is independent. "
                  "Engine buffer changes restart the router; ASIO sample rate/buffer changes reopen the device." },
                // ---- about ----
                { "NL Audio Matrix - Preferenze", "NL Audio Matrix - Preferences" },
                { "NL Audio Matrix - Diagnostica", "NL Audio Matrix - Diagnostics" },
                { "DIAGNOSTICA / LATENZA E TELEMETRIA", "DIAGNOSTICS / LATENCY & TELEMETRY" },
                { "Copia diagnostica", "Copy diagnostics" },
                { "Copiato", "Copied" },
                { "Routing e aggregazione audio professionale per Windows",
                  "Professional audio routing & aggregation for Windows" },
                { "SVILUPPATORE", "DEVELOPER" },
                { "BUILD E RUNTIME", "BUILD & RUNTIME" },
            };
            count = (int) (sizeof (entries) / sizeof (entries[0]));
            return entries;
        }

        inline juce::String tr (const char* italian)
        {
            if (italian == nullptr)
                return {};
            if (currentLanguage() == Language::italian)
                return juce::String::fromUTF8 (italian);
            int count = 0;
            const auto* entries = translationTable (count);
            for (int i = 0; i < count; ++i)
                if (std::strcmp (entries[i].italian, italian) == 0)
                    return juce::String::fromUTF8 (entries[i].english);
            return juce::String::fromUTF8 (italian);
        }

        inline juce::String tr (const juce::String& italian)
        {
            return tr (italian.toRawUTF8());
        }

        // JUCE's String(const char*) treats 8-bit data as ASCII/Latin-1, so any
        // literal containing UTF-8 glyphs (accents, middle dot, em dash) must be
        // decoded explicitly.  L10n::tr() already does this for translations;
        // this helper covers the remaining direct literals.
        inline juce::String utf8 (const char* text)
        {
            return juce::String::fromUTF8 (text);
        }

        // Inline two-argument form for prose that is not a stable key: keeps the
        // Italian and English text side by side at the call site.
        inline juce::String tr (const char* italian, const char* english)
        {
            return currentLanguage() == Language::english ? juce::String::fromUTF8 (english)
                                                          : juce::String::fromUTF8 (italian);
        }

        // ---- live rebinding -------------------------------------------------
        inline std::vector<std::function<void()>>& bindings()
        {
            static std::vector<std::function<void()>> list;
            return list;
        }

        inline void applyBindings()
        {
            for (auto& binding : bindings())
                if (binding != nullptr)
                    binding();
        }

        inline std::function<void()>& languageChangedHook()
        {
            static std::function<void()> hook;
            return hook;
        }

        inline void notifyLanguageChanged()
        {
            applyBindings();
            if (languageChangedHook() != nullptr)
                languageChangedHook()();
        }

        template <typename ComponentType>
        inline void bindLabelText (ComponentType& label, const char* italian)
        {
            juce::Component::SafePointer<ComponentType> safe (&label);
            bindings().push_back ([safe, italian]
            {
                if (safe != nullptr)
                    safe->setText (tr (italian), juce::dontSendNotification);
            });
            label.setText (tr (italian), juce::dontSendNotification);
        }

        template <typename ComponentType>
        inline void bindButtonText (ComponentType& button, const char* italian)
        {
            juce::Component::SafePointer<ComponentType> safe (&button);
            bindings().push_back ([safe, italian]
            {
                if (safe != nullptr)
                    safe->setButtonText (tr (italian));
            });
            button.setButtonText (tr (italian));
        }

        template <typename ComponentType>
        inline void bindPlaceholder (ComponentType& editor, const char* italian)
        {
            juce::Component::SafePointer<ComponentType> safe (&editor);
            bindings().push_back ([safe, italian]
            {
                if (safe != nullptr)
                    safe->setTextToShowWhenEmpty (tr (italian), Palette::muted);
            });
            editor.setTextToShowWhenEmpty (tr (italian), Palette::muted);
        }

        inline void bindAction (std::function<void()> action)
        {
            bindings().push_back (std::move (action));
        }
    }

    class NeonLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        NeonLookAndFeel() { refreshColours(); }

        void refreshColours()
        {
            setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
            setColour (juce::DocumentWindow::textColourId, Palette::text);
            setColour (juce::Label::textColourId, Palette::text);
            setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::TextButton::buttonColourId, Palette::panelRaised);
            setColour (juce::TextButton::buttonOnColourId, Palette::violet);
            setColour (juce::TextButton::textColourOffId, Palette::text);
            setColour (juce::TextButton::textColourOnId, Palette::background);
            setColour (juce::ComboBox::backgroundColourId, Palette::panel);
            setColour (juce::ComboBox::outlineColourId, Palette::outline);
            setColour (juce::ComboBox::textColourId, Palette::text);
            setColour (juce::ComboBox::arrowColourId, Palette::cyan);
            setColour (juce::ComboBox::focusedOutlineColourId, Palette::magenta);
            setColour (juce::TextEditor::backgroundColourId, Palette::panel);
            setColour (juce::TextEditor::textColourId, Palette::text);
            setColour (juce::TextEditor::outlineColourId, Palette::outline);
            setColour (juce::TextEditor::focusedOutlineColourId, Palette::cyan);
            setColour (juce::TextEditor::highlightColourId, Palette::violet.withAlpha (0.45f));
            setColour (juce::ListBox::backgroundColourId, Palette::panel);
            setColour (juce::ListBox::outlineColourId, Palette::outline);
            setColour (juce::ListBox::textColourId, Palette::text);
            setColour (juce::PopupMenu::backgroundColourId, Palette::panelRaised);
            setColour (juce::PopupMenu::textColourId, Palette::text);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::violet.withAlpha (0.35f));
            setColour (juce::PopupMenu::highlightedTextColourId, Palette::text);
            setColour (juce::ScrollBar::thumbColourId, Palette::violet.withAlpha (0.65f));
            setColour (juce::ToggleButton::textColourId, Palette::text);
            setColour (juce::ToggleButton::tickColourId, Palette::cyan);
            setColour (juce::ToggleButton::tickDisabledColourId, Palette::outline);
        }
        // User request: slightly larger, more legible base text everywhere.
        juce::Font getLabelFont (juce::Label&) override
        {
            return juce::Font (juce::FontOptions (15.5f));
        }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return juce::Font (juce::FontOptions (juce::jlimit (14.0f, 17.0f,
                                                                (float) buttonHeight * 0.5f)));
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override
        {
            return juce::Font (juce::FontOptions (15.0f));
        }

        juce::Font getPopupMenuFont() override
        {
            return juce::Font (juce::FontOptions (15.0f));
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                   const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override
        {
            auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
            auto base = backgroundColour;
            if (shouldDrawButtonAsDown) base = Palette::magenta;
            else if (shouldDrawButtonAsHighlighted) base = base.brighter (0.18f);
            g.setColour (base);
            g.fillRoundedRectangle (bounds, 7.0f);
            g.setColour (shouldDrawButtonAsHighlighted ? Palette::cyan.withAlpha (0.8f)
                                                       : Palette::outline);
            g.drawRoundedRectangle (bounds, 7.0f, 1.0f);
        }
    };

    struct Source
    {
        juce::String id, name, type, deviceId, endpoint, process, processTitle, bus;
        int pid = 0;
        int driverEndpoint = 0;
        bool includeProcessTree = true;
        bool enabled = true;
        bool muted = false;
        float gainDb = 0.0f;
        float pan = 0.0f;
        bool polarity = false;
        bool solo = false;
        float x = -1.0f, y = -1.0f;
        double baseFrequency = 300.0;
        double testLevel = 0.2;
        std::vector<int> channels;
    };

    struct Destination
    {
        juce::String id, name, type, endpoint, deviceId;
        int driverEndpoint = 0;
        bool enabled = true;
        bool muted = false;
        bool solo = false;
        bool monitor = false;
        float gainDb = 0.0f;
        float x = -1.0f, y = -1.0f;
        std::vector<int> channels;
        std::vector<juce::String> mixMinus;
        // E6: per-driver-destination ring target override in ms (0 = global
        // engine tuning). Ignored for non-driver destinations.
        int driverTargetMs = 0;
    };

    // Human-readable type selectors.  The internal keys are what the router
    // and profile format use; users never have to type "asio-in" etc.
    static juce::String sourceTypeKey (int id)
    {
        switch (id)
        {
            case 2: return "loopback";
            case 3: return "capture";
            case 4: return "process";
            case 5: return "bus";
            case 6: return "driver";
            default: return "asio-in";
        }
    }

    static int sourceTypeId (const juce::String& key)
    {
        if (key == "loopback") return 2;
        if (key == "capture") return 3;
        if (key == "process") return 4;
        if (key == "bus") return 5;
        if (key == "driver") return 6;
        return 1;
    }

    static juce::String destinationTypeKey (int id)
    {
        switch (id)
        {
            case 2: return "render";
            case 3: return "driver";
            default: return "asio-out";
        }
    }

    static int destinationTypeId (const juce::String& key)
    {
        if (key == "render") return 2;
        if (key == "driver") return 3;
        return 1;
    }

    struct Bus
    {
        juce::String id, name;
        bool enabled = true;
        bool muted = false;
        bool solo = false;
        float gainDb = 0.0f;
        int channels = 2;
        float x = -1.0f, y = -1.0f;
    };

    struct Route
    {
        juce::String sourceId, destinationId;
        double gainDb = 0.0;
        std::vector<int> channelMap;
        bool enabled = true;
    };

    // The UI presents ASIO channels exactly as the driver does: 1-based.
    // The profile/config model stays 0-based (current engine ABI); these two
    // helpers are the only conversion boundary, so the Channel Picker and the
    // text field can never silently select the wrong physical channel.
    std::vector<int> parseChannels (const juce::String& text)
    {
        std::vector<int> result;
        juce::StringArray parts;
        parts.addTokens (text, ",", "");
        for (const auto& p : parts)
        {
            if (p.trim().isEmpty())
                continue;
            const int oneBased = p.trim().getIntValue();
            if (oneBased >= 1)
                result.push_back (oneBased - 1);
        }
        return result;
    }

    juce::String channelsToText (const std::vector<int>& channels)
    {
        juce::StringArray parts;
        for (int ch : channels)
            parts.add (juce::String (ch + 1));
        return parts.joinIntoString (",");
    }

    juce::Array<juce::var> channelsToVar (const std::vector<int>& channels)
    {
        juce::Array<juce::var> result;
        for (int ch : channels)
            result.add (ch);
        return result;
    }

    //==============================================================================
    // Disclosure arrow drawn with juce::Path: the UI font does not reliably
    // provide the unicode triangle glyphs on every Windows install, so arrows
    // are painted geometry instead of text (this was visible as empty boxes).
    class DisclosureButton final : public juce::Button
    {
    public:
        enum class Direction { left, right, down, up };

        explicit DisclosureButton (Direction d = Direction::down)
            : juce::Button ("disclosure"), direction (d)
        {
            setTooltip (L10n::tr ("Comprimi / espandi", "Collapse / expand"));
        }

        void setDirection (Direction d)
        {
            if (direction != d)
            {
                direction = d;
                repaint();
            }
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto area = getLocalBounds().toFloat().reduced (7.0f);
            const float size = juce::jmin (area.getWidth(), area.getHeight());
            juce::Path triangle;
            switch (direction)
            {
                case Direction::down:  triangle.addTriangle (0.0f, 0.0f, size, 0.0f, size * 0.5f, size); break;
                case Direction::up:    triangle.addTriangle (size * 0.5f, 0.0f, size, size, 0.0f, size); break;
                case Direction::right: triangle.addTriangle (0.0f, 0.0f, size, size * 0.5f, 0.0f, size); break;
                case Direction::left:
                default:               triangle.addTriangle (size, 0.0f, 0.0f, size * 0.5f, size, size); break;
            }
            triangle.applyTransform (juce::AffineTransform::translation (
                area.getCentreX() - size * 0.5f, area.getCentreY() - size * 0.5f));
            g.setColour (down || highlighted ? Palette::cyan : Palette::text);
            g.fillPath (triangle);
        }

    private:
        Direction direction;
    };

    // Visual patchbay: drag from a source card to a destination card to create a
    // route. Cables are drawn from the real in-memory profile, not a mockup.
    class PatchbayView final : public juce::Component
    {
    public:
        std::function<void (const juce::String&, const juce::String&)> onConnect;

        void setData (const std::vector<Source>& s, const std::vector<Destination>& d,
                      const std::vector<Bus>& b, const std::vector<Route>& r)
        {
            sources = s;
            destinations = d;
            buses = b;
            routes = r;
            rebuildCards();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::canvas);
            auto area = getLocalBounds().reduced (10);
            auto header = area.removeFromTop (24);
            g.setColour (Palette::muted);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (L10n::tr ("SORGENTI", "SOURCES"), header.removeFromLeft (getWidth() / 2 - 20),
                        juce::Justification::centredLeft);
            g.drawText (L10n::tr ("DESTINAZIONI", "DESTINATIONS"), header, juce::Justification::centredLeft);

            for (const auto& route : routes)
            {
                if (! route.enabled)
                    continue;
                const auto* a = findCard (sourceCards, route.sourceId);
                const auto* b = findCard (destCards, route.destinationId);
                if (a == nullptr || b == nullptr)
                    continue;
                drawCable (g, { a->area.getRight(), a->area.getCentreY() },
                           { b->area.getX(), b->area.getCentreY() },
                           colourFor (a->subtitle, false), 2.2f);
            }

            if (dragSource >= 0 && dragSource < (int) sourceCards.size())
            {
                const auto& card = sourceCards[(size_t) dragSource].area;
                drawCable (g, { card.getRight(), card.getCentreY() },
                           dragPoint, Palette::cyan, 2.6f);
            }

            for (const auto& card : sourceCards)
                drawCard (g, card, false);
            for (const auto& card : destCards)
                drawCard (g, card, true);

            if (sourceCards.empty() && destCards.empty())
            {
                g.setColour (Palette::muted);
                g.setFont (15.0f);
                g.drawText (L10n::tr ("Aggiungi una sorgente e una destinazione per iniziare a patchare",
                                      "Add a source and a destination to start patching"),
                            getLocalBounds(), juce::Justification::centred);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragSource = -1;
            for (int i = 0; i < (int) sourceCards.size(); ++i)
                if (sourceCards[(size_t) i].area.contains (e.getPosition()))
                {
                    dragSource = i;
                    dragPoint = e.getPosition();
                    break;
                }
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragSource >= 0)
            {
                dragPoint = e.getPosition();
                repaint();
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (dragSource >= 0)
            {
                const auto sourceId = sourceCards[(size_t) dragSource].id;
                for (const auto& card : destCards)
                    if (card.area.contains (e.getPosition()))
                    {
                        if (onConnect)
                            onConnect (sourceId, card.id);
                        break;
                    }
            }
            dragSource = -1;
            repaint();
        }

    private:
        struct Card
        {
            juce::String id, title, subtitle;
            juce::Rectangle<int> area;
        };

        static const Card* findCard (const std::vector<Card>& cards, const juce::String& id)
        {
            for (const auto& c : cards)
                if (c.id == id)
                    return &c;
            return nullptr;
        }

        static juce::Colour colourFor (const juce::String& id, bool destination)
        {
            const auto text = id.toLowerCase();
            if (text.contains ("driver") || text.contains ("nl-")) return Palette::magenta;
            if (text.contains ("bus") || text.contains ("render")) return Palette::violet;
            if (text.contains ("process") || text.contains ("app")) return Palette::magenta.brighter (0.15f);
            juce::ignoreUnused (destination);
            return Palette::cyan;
        }

        void rebuildCards()
        {
            sourceCards.clear();
            destCards.clear();
            for (const auto& s : sources)
            {
                Card c;
                c.id = s.id;
                c.title = s.name.isNotEmpty() ? s.name : s.id;
                c.subtitle = s.type;
                sourceCards.push_back (c);
            }
            for (const auto& d : destinations)
            {
                Card c;
                c.id = d.id;
                c.title = d.name.isNotEmpty() ? d.name : d.id;
                c.subtitle = d.type;
                destCards.push_back (c);
            }
            for (const auto& b : buses)
            {
                Card c;
                c.id = b.id;
                c.title = b.name.isNotEmpty() ? b.name : b.id;
                c.subtitle = "bus";
                destCards.push_back (c);
            }
            layoutCards();
        }

        void layoutCards()
        {
            if (patchArea.isEmpty())
                return;
            const int half = patchArea.getWidth() / 2;
            const auto left = patchArea.withWidth (juce::jmax (0, half - 34));
            const auto right = patchArea.withTrimmedLeft (half + 34);
            auto place = [] (std::vector<Card>& cards, juce::Rectangle<int> area, bool rightSide)
            {
                if (cards.empty())
                    return;
                const int gap = 8;
                const int cardH = juce::jlimit (34, 58,
                    (area.getHeight() - gap * ((int) cards.size() - 1))
                        / juce::jmax (1, (int) cards.size()));
                int y = area.getY();
                for (auto& c : cards)
                {
                    const int w = juce::jmin (area.getWidth(), 220);
                    c.area = rightSide ? juce::Rectangle<int> (area.getRight() - w, y, w, cardH)
                                       : juce::Rectangle<int> (area.getX(), y, w, cardH);
                    y += cardH + gap;
                }
            };
            place (sourceCards, left, false);
            place (destCards, right, true);
        }

        void drawCard (juce::Graphics& g, const Card& card, bool destination) const
        {
            const auto colour = colourFor (card.subtitle, destination);
            g.setColour (colour.withAlpha (0.16f));
            g.fillRoundedRectangle (card.area.toFloat(), 7.0f);
            g.setColour (colour.withAlpha (0.65f));
            g.drawRoundedRectangle (card.area.toFloat(), 7.0f, 1.0f);
            g.setColour (Palette::text);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            auto r = card.area.reduced (10, 5);
            g.drawText (card.title, r.removeFromTop (r.getHeight() / 2),
                        juce::Justification::centredLeft);
            g.setColour (Palette::muted);
            g.setFont (11.0f);
            g.drawText (card.subtitle, r, juce::Justification::centredLeft);
        }

        static juce::Path makeCablePath (juce::Point<float> a, juce::Point<float> b)
        {
            juce::Path path;
            const float dx = juce::jmax (40.0f, std::abs (b.x - a.x) / 2.0f);
            path.startNewSubPath (a);
            path.cubicTo ({ a.x + dx, a.y }, { b.x - dx, b.y }, b);
            return path;
        }

        static void drawCable (juce::Graphics& g, juce::Point<int> a, juce::Point<int> b,
                               juce::Colour colour, float thickness)
        {
            const auto path = makeCablePath (a.toFloat(), b.toFloat());
            g.setColour (colour.withAlpha (0.9f));
            g.strokePath (path, juce::PathStrokeType (thickness,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        void resized() override
        {
            patchArea = getLocalBounds().reduced (10);
            patchArea.removeFromTop (26);
            layoutCards();
        }

        std::vector<Source> sources;
        std::vector<Destination> destinations;
        std::vector<Bus> buses;
        std::vector<Route> routes;
        std::vector<Card> sourceCards, destCards;
        juce::Rectangle<int> patchArea;
        juce::Point<int> dragPoint;
        int dragSource = -1;
    };

    // Fully interactive canvas: free node placement, multi-selection, cable
    // creation/reconnection, route context actions, zoom and pan.
    class PatchbayCanvas final : public juce::Component
    {
    public:
        PatchbayCanvas()
        {
            setWantsKeyboardFocus (true);
        }
        std::function<void (const juce::String& kind, const juce::String& id,
                            float x, float y, bool finished)> onNodeMoved;
        std::function<void (const juce::String& source, const juce::String& destination)> onConnect;
        std::function<void (const juce::String& source, const juce::String& oldDest,
                            const juce::String& newDest)> onReconnect;
        std::function<void (int routeIndex)> onDeleteRoute;
        std::function<void (int routeIndex, float gainDb, bool enabled)> onRouteChanged;
        std::function<void (int routeIndex)> onEditRouteGain;
        std::function<void (const juce::String& kind, const juce::String& id)> onEditNode;
        std::function<void (const juce::String& kind, const juce::String& id)> onRemoveNode;
        std::function<void (const juce::String& kind, const juce::String& id, bool enabled)> onNodeEnabled;
        std::function<void (const juce::String& kind, const juce::String& id, bool muted)> onNodeMuted;
        std::function<void (const juce::String& kind, const juce::String& id,
                            const juce::String& name)> onRenameNode;
        std::function<void (const juce::String& kind)> onAddNode;
        // G3: bidirectional selection sync with the left panel.
        std::function<void (const juce::String& kind, const juce::String& id)> onSelectionChanged;
        std::function<void (int routeIndex)> onCableSelected;

        void setOnlineStates (const std::map<juce::String, bool>& states)
        {
            if (onlineStates == states)
                return;
            onlineStates = states;
            repaint();
        }

        void setData (const std::vector<Source>& s, const std::vector<Destination>& d,
                      const std::vector<Bus>& b, const std::vector<Route>& r)
        {
            sources = s;
            destinations = d;
            buses = b;
            routes = r;
            if (selectedCable >= (int) routes.size())
                selectedCable = -1;
            rebuild();
            needsFit = true;
            if (getWidth() > 0 && getHeight() > 0)
                fitContent();
            else
                repaint();
        }

        void resized() override
        {
            if (needsFit)
                fitContent();
        }

        void fitContent()
        {
            if (nodes.empty()) return;
            juce::Rectangle<float> bounds;
            bool first = true;
            for (const auto& n : nodes)
            {
                const auto r = juce::Rectangle<float> (n.world.x, n.world.y, 200.0f, 66.0f);
                bounds = first ? r : bounds.getUnion (r);
                first = false;
            }
            if (bounds.getWidth() <= 0.0f || getWidth() <= 0 || getHeight() <= 0)
                return;
            const float targetW = juce::jmax (1.0f, (float) getWidth() - 36.0f);
            const float targetH = juce::jmax (1.0f, (float) getHeight() - 36.0f);
            zoom = juce::jlimit (0.55f, 1.2f, juce::jmin (targetW / bounds.getWidth(),
                                                          targetH / bounds.getHeight(),
                                                          1.2f));
            pan = { 18.0f - bounds.getX() * zoom, 18.0f - bounds.getY() * zoom };
            needsFit = false;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::canvas);
            drawGrid (g);

            for (const auto& s : sources)
                if (s.type != "bus")
                    if (const auto* r = nodeRect ("source", s.id))
                        drawNode (g, *r, "S:" + s.id, s.name, s.type, s.enabled, true, false);
            for (const auto& b : buses)
                if (const auto* r = nodeRect ("bus", b.id))
                    drawNode (g, *r, "B:" + b.id, b.name, "bus", b.enabled, true, true);
            for (const auto& d : destinations)
                if (const auto* r = nodeRect ("destination", d.id))
                    drawNode (g, *r, "D:" + d.id, d.name, d.type, d.enabled, false, true);

            for (int i = 0; i < (int) routes.size(); ++i)
            {
                const auto& route = routes[(size_t) i];
                const auto* a = sourceNodeRect (route.sourceId);
                const auto* b = destinationNodeRect (route.destinationId);
                if (a == nullptr || b == nullptr)
                    continue;
                const bool selected = i == selectedCable;
                const bool hovered = i == hoveredCable;
                if (selected || hovered)
                {
                    g.setColour ((selected ? Palette::cyan : Palette::magenta).withAlpha (0.18f));
                    const auto glow = makeCablePath ({ (float) a->getRight(), (float) a->getCentreY() },
                                                     { (float) b->getX(), (float) b->getCentreY() });
                    g.strokePath (glow, juce::PathStrokeType (selected ? 12.0f : 8.0f,
                                                              juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
                }
                drawCable (g, { a->getRight(), a->getCentreY() },
                           { b->getX(), b->getCentreY() },
                           route.enabled ? (selected ? Palette::cyan
                                                     : hovered ? Palette::cyan.brighter (0.2f)
                                                               : Palette::magenta)
                                         : Palette::outline,
                           selected ? 4.5f : hovered ? 3.5f : 2.2f);

                if (selected)
                {
                    const auto mid = cableMidpoint (i);
                    disconnectButton = { (int) mid.x - 11, (int) mid.y - 11, 22, 22 };
                    g.setColour (Palette::panel);
                    g.fillEllipse (disconnectButton.toFloat());
                    g.setColour (Palette::danger);
                    g.drawEllipse (disconnectButton.toFloat(), 1.5f);
                    g.setColour (Palette::text);
                    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
                    g.drawText ("x", disconnectButton, juce::Justification::centred);
                }
            }

            if (draftMode != DraftMode::None)
            {
                juce::Point<int> start;
                if (draftMode == DraftMode::Create && draftSource.isNotEmpty())
                    if (const auto* src = sourceNodeRect (draftSource))
                        start = { src->getRight(), src->getCentreY() };
                if (draftMode == DraftMode::Reconnect)
                    start = draftStartScreen;
                drawCable (g, start, draftPoint, Palette::cyan, 3.0f);
            }

            g.setColour (Palette::muted.withAlpha (0.8f));
            g.setFont (11.5f);
            g.drawText (L10n::tr ("Trascina nodi | trascina dai connettori | clic sul cavo + Canc | rotella zoom | centrale pan",
                                  "Drag nodes | drag ports | click cable + Del | wheel zoom | middle pan"),
                        getLocalBounds().removeFromTop (18).reduced (8, 0),
                        juce::Justification::centredLeft);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            grabKeyboardFocus();
            if (e.mods.isPopupMenu())
            {
                showContextMenu (e);
                return;
            }
            lastMouse = e.getPosition();
            dragStartWorld = screenToWorld (e.getPosition().toFloat());

            if (e.mods.isMiddleButtonDown())
            {
                panning = true;
                panStart = pan;
                return;
            }

            if (selectedCable >= 0 && disconnectButton.contains (e.getPosition()))
            {
                if (onDeleteRoute)
                    onDeleteRoute (selectedCable);
                selectedCable = -1;
                return;
            }

            // Destination input port: reconnect the cable ending there.
            const int reconnect = cableAtDestinationPort (e.getPosition());
            if (reconnect >= 0)
            {
                draftMode = DraftMode::Reconnect;
                reconnectRoute = reconnect;
                draftPoint = e.getPosition();
                if (const auto* r = destinationNodeRect (routes[(size_t) reconnect].destinationId))
                    draftStartScreen = { r->getX(), r->getCentreY() };
                repaint();
                return;
            }

            // Source output port: start a new cable. Buses expose the same
            // output port; the model creates/uses their bus-source on connect.
            for (const auto& s : sources)
                if (s.type != "bus")
                    if (const auto* r = nodeRect ("source", s.id))
                    {
                        juce::Rectangle<float> port ((float) r->getRight() - 7.0f,
                                                     (float) r->getCentreY() - 7.0f, 14.0f, 14.0f);
                        if (port.contains (e.position))
                        {
                            draftMode = DraftMode::Create;
                            draftSource = s.id;
                            draftPoint = e.getPosition();
                            repaint();
                            return;
                        }
                    }
            for (const auto& b : buses)
                if (const auto* r = nodeRect ("bus", b.id))
                {
                    juce::Rectangle<float> port ((float) r->getRight() - 7.0f,
                                                 (float) r->getCentreY() - 7.0f, 14.0f, 14.0f);
                    if (port.contains (e.position))
                    {
                        draftMode = DraftMode::Create;
                        draftSource = b.id;
                        draftPoint = e.getPosition();
                        repaint();
                        return;
                    }
                }

            const int cableHit = cableAt (e.getPosition());
            if (cableHit >= 0)
            {
                selectedCable = cableHit;
                selectedNodes.clear();
                hoveredCable = cableHit;
                repaint();
                if (onCableSelected)
                    onCableSelected (cableHit);
                return;
            }

            const auto hit = nodeAt (e.getPosition());
            if (hit.isNotEmpty())
            {
                if (e.mods.isShiftDown() || e.mods.isCommandDown())
                {
                    if (selectedNodes.count (hit) != 0) selectedNodes.erase (hit);
                    else selectedNodes.insert (hit);
                }
                else if (selectedNodes.count (hit) == 0)
                {
                    selectedNodes.clear();
                    selectedNodes.insert (hit);
                }
                dragNodeKey = hit;
                dragStartPositions.clear();
                for (const auto& key : selectedNodes)
                    if (const auto* n = findNode (kindOf (key), idOf (key)))
                        dragStartPositions[key] = n->world;
                selectedCable = -1;
                repaint();
                if (onSelectionChanged)
                    onSelectionChanged (kindOf (hit), idOf (hit));
                return;
            }

            selectedCable = cableAt (e.getPosition());
            if (selectedCable < 0 && ! (e.mods.isShiftDown() || e.mods.isCommandDown()))
                selectedNodes.clear();
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (panning)
            {
                pan = panStart + (e.getPosition() - lastMouse).toFloat();
                repaint();
                return;
            }
            if (draftMode != DraftMode::None)
            {
                draftPoint = e.getPosition();
                repaint();
                return;
            }
            if (dragNodeKey.isNotEmpty())
            {
                const auto world = screenToWorld (e.getPosition().toFloat());
                const auto delta = world - dragStartWorld;
                for (const auto& [key, start] : dragStartPositions)
                {
                    const auto newPos = start + delta;
                    applyNodePosition (key, newPos);
                    notifyMove (key, newPos, false);
                }
                repaint();
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (panning)
            {
                panning = false;
                return;
            }
            if (draftMode == DraftMode::Create)
            {
                for (const auto& d : destinations)
                    if (const auto* r = nodeRect ("destination", d.id))
                        if (juce::Rectangle<float> ((float) r->getX() - 8.0f,
                                                    (float) r->getCentreY() - 8.0f,
                                                    16.0f, 16.0f).contains (e.position))
                        {
                            if (onConnect)
                            {
                                ++testConnectCount;
                                onConnect (draftSource, d.id);
                            }
                            break;
                        }
                for (const auto& b : buses)
                    if (const auto* r = nodeRect ("bus", b.id))
                        if (juce::Rectangle<float> ((float) r->getX() - 8.0f,
                                                    (float) r->getCentreY() - 8.0f,
                                                    16.0f, 16.0f).contains (e.position))
                        {
                            if (onConnect)
                            {
                                ++testConnectCount;
                                onConnect (draftSource, b.id);
                            }
                            break;
                        }
            }
            else if (draftMode == DraftMode::Reconnect && reconnectRoute >= 0)
            {
                for (const auto& d : destinations)
                    if (const auto* r = nodeRect ("destination", d.id))
                        if (juce::Rectangle<float> ((float) r->getX() - 8.0f,
                                                    (float) r->getCentreY() - 8.0f,
                                                    16.0f, 16.0f).contains (e.position))
                        {
                            if (onReconnect)
                                onReconnect (routes[(size_t) reconnectRoute].sourceId,
                                             routes[(size_t) reconnectRoute].destinationId, d.id);
                            break;
                        }
                for (const auto& b : buses)
                    if (const auto* r = nodeRect ("bus", b.id))
                        if (juce::Rectangle<float> ((float) r->getX() - 8.0f,
                                                    (float) r->getCentreY() - 8.0f,
                                                    16.0f, 16.0f).contains (e.position))
                        {
                            if (onReconnect)
                                onReconnect (routes[(size_t) reconnectRoute].sourceId,
                                             routes[(size_t) reconnectRoute].destinationId, b.id);
                            break;
                        }
            }
            else if (dragNodeKey.isNotEmpty())
            {
                for (const auto& key : selectedNodes)
                    if (const auto* n = findNode (kindOf (key), idOf (key)))
                    {
                        ++testMoveCount;
                        notifyMove (key, n->world, true);
                    }
                dragNodeKey.clear();
            }
            draftMode = DraftMode::None;
            reconnectRoute = -1;
            repaint();
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            {
                if (selectedCable >= 0 && onDeleteRoute)
                {
                    onDeleteRoute (selectedCable);
                    ++testDeleteCount;
                    selectedCable = -1;
                    return true;
                }
                if (! selectedNodes.empty() && onRemoveNode)
                {
                    const auto copy = selectedNodes;
                    selectedNodes.clear();
                    for (const auto& key : copy)
                        onRemoveNode (kindOf (key), idOf (key));
                    return true;
                }
            }
            return false;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            if (panning || draftMode != DraftMode::None || dragNodeKey.isNotEmpty())
                return;
            const int cable = cableAt (e.getPosition());
            if (cable != hoveredCable)
            {
                hoveredCable = cable;
                setMouseCursor (cable >= 0 ? juce::MouseCursor::PointingHandCursor
                                           : juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            if (hoveredCable >= 0)
            {
                hoveredCable = -1;
                setMouseCursor (juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            const auto before = screenToWorld (e.getPosition().toFloat());
            zoom = juce::jlimit (0.35f, 2.5f, zoom * (wheel.deltaY > 0 ? 1.1f : 0.9f));
            const auto after = screenToWorld (e.getPosition().toFloat());
            pan += (after - before) * zoom;

            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const auto key = nodeAt (e.getPosition());
            if (key.isNotEmpty() && onEditNode)
                onEditNode (kindOf (key), idOf (key));
        }

        void showContextMenu (const juce::MouseEvent& e)
        {
            // A cable is a first-class patch object: right-clicking its visible
            // stroke must offer cable actions even where it crosses a node.
            const auto key = cableAt (e.getPosition()) >= 0
                                 ? juce::String()
                                 : nodeAt (e.getPosition());
            juce::PopupMenu menu;
            if (key.isNotEmpty())
            {
                const auto kind = kindOf (key);
                const auto id = idOf (key);
                menu.addItem (1, L10n::tr ("Rinomina", "Rename"));
                menu.addItem (4, L10n::tr ("Proprietà", "Properties"));
                bool enabled = true;
                if (kind == "source")
                    for (const auto& s : sources) if (s.id == id) enabled = s.enabled;
                if (kind == "destination")
                    for (const auto& d : destinations) if (d.id == id) enabled = d.enabled;
                if (kind == "bus")
                    for (const auto& b : buses) if (b.id == id) enabled = b.enabled;
                menu.addItem (2, enabled ? L10n::tr ("Disattiva", "Disable")
                                         : L10n::tr ("Attiva", "Enable"));
                bool muted = false;
                if (kind == "source") for (const auto& s : sources) if (s.id == id) muted = s.muted;
                if (kind == "destination") for (const auto& d : destinations) if (d.id == id) muted = d.muted;
                if (kind == "bus") for (const auto& b : buses) if (b.id == id) muted = b.muted;
                menu.addItem (5, muted ? L10n::tr ("Togli mute", "Unmute")
                                       : L10n::tr ("Muta", "Mute"));
                menu.addSeparator();
                menu.addItem (3, L10n::tr ("Rimuovi", "Remove"));
            }
            else
            {
                const int cable = cableAt (e.getPosition());
                if (cable >= 0)
                {
                    selectedCable = cable;
                    repaint();
                    menu.addItem (10, L10n::tr ("Guadagno 0 dB", "Gain 0 dB"));
                    menu.addItem (11, L10n::tr ("Guadagno -6 dB", "Gain -6 dB"));
                    menu.addItem (12, L10n::tr ("Guadagno -12 dB", "Gain -12 dB"));
                    menu.addItem (13, L10n::tr ("Guadagno -24 dB", "Gain -24 dB"));
                    const bool on = routes[(size_t) cable].enabled;
                    menu.addItem (14, on ? L10n::tr ("Muta", "Mute")
                                         : L10n::tr ("Togli mute", "Unmute"));
                    menu.addSeparator();
                    menu.addItem (16, L10n::tr ("Guadagno...", "Gain..."));
                    menu.addItem (15, L10n::tr ("Scollega cavo", "Disconnect cable"));
                }
                else
                {
                    menu.addItem (20, L10n::tr ("Aggiungi sorgente", "Add source"));
                    menu.addItem (21, L10n::tr ("Aggiungi destinazione", "Add destination"));
                    menu.addItem (22, L10n::tr ("Aggiungi bus", "Add bus"));
                }
            }

            const auto screen = localPointToGlobal (e.getPosition());
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetScreenArea ({ screen.x, screen.y, 1, 1 })
                                    .withParentComponent (this),
                                [this, key] (int result)
                                {
                                    if (result >= 1 && result <= 4 && key.isNotEmpty())
                                    {
                                        const auto kind = kindOf (key);
                                        const auto id = idOf (key);
                                        if (result == 1) renameNode (kind, id);
                                        else if (result == 4 && onEditNode) onEditNode (kind, id);
                                        else if (result == 5 && onNodeMuted)
                                        {
                                            bool muted = false;
                                            if (kind == "source") for (const auto& s : sources) if (s.id == id) muted = s.muted;
                                            if (kind == "destination") for (const auto& d : destinations) if (d.id == id) muted = d.muted;
                                            if (kind == "bus") for (const auto& b : buses) if (b.id == id) muted = b.muted;
                                            onNodeMuted (kind, id, ! muted);
                                        }
                                        else if (result == 2)
                                        {
                                            bool enabled = true;
                                            if (kind == "source")
                                                for (const auto& s : sources) if (s.id == id) enabled = s.enabled;
                                            if (kind == "destination")
                                                for (const auto& d : destinations) if (d.id == id) enabled = d.enabled;
                                            if (kind == "bus")
                                                for (const auto& b : buses) if (b.id == id) enabled = b.enabled;
                                            if (onNodeEnabled) onNodeEnabled (kind, id, ! enabled);
                                        }
                                        else if (onRemoveNode) onRemoveNode (kind, id);
                                    }
                                    else if (result == 16 && selectedCable >= 0 && onEditRouteGain)
                                    {
                                        onEditRouteGain (selectedCable);
                                    }
                                    else if (result >= 10 && result <= 15 && selectedCable >= 0
                                             && selectedCable < (int) routes.size())
                                    {
                                        const auto route = routes[(size_t) selectedCable];
                                        if (result == 15 && onDeleteRoute) onDeleteRoute (selectedCable);
                                        else if (result == 14 && onRouteChanged)
                                            onRouteChanged (selectedCable, (float) route.gainDb, ! route.enabled);
                                        else if (result >= 10 && result <= 13 && onRouteChanged)
                                        {
                                            const float gain = result == 10 ? 0.0f
                                                                  : result == 11 ? -6.0f
                                                                  : result == 12 ? -12.0f : -24.0f;
                                            onRouteChanged (selectedCable, gain, true);
                                        }
                                    }
                                    else if (result >= 20 && result <= 22 && onAddNode)
                                    {
                                        onAddNode (result == 20 ? "source"
                                                              : result == 21 ? "destination" : "bus");
                                    }
                                });
        }

        int selectedCableIndex() const { return selectedCable; }
        int selectedNodeCount() const { return (int) selectedNodes.size(); }
        int hoveredCableIndex() const { return hoveredCable; }
        int routeCount() const { return (int) routes.size(); }
        void selectCable (int index) { selectedCable = index; repaint(); }

        // G3: highlight one node from the left list without firing callbacks
        // (prevents a selection-feedback loop with MainComponent).
        void selectNode (const juce::String& kind, const juce::String& id)
        {
            selectedNodes.clear();
            if (kind.isNotEmpty() && id.isNotEmpty())
            {
                const juce::String prefix = kind == "source" ? "S" : kind == "destination" ? "D" : "B";
                selectedNodes.insert (prefix + ":" + id);
            }
            selectedCable = -1;
            repaint();
        }

        void clearSelection()
        {
            selectedNodes.clear();
            selectedCable = -1;
            repaint();
        }

        juce::Point<float> cableScreenMidpoint (int index) const
        {
            return localPointToGlobal (cableMidpoint (index));
        }

        juce::Point<float> nodeScreenCentre (const juce::String& kind, const juce::String& id,
                                             bool inputPort) const
        {
            if (const auto* r = nodeRect (kind, id))
                return localPointToGlobal (juce::Point<float> (inputPort ? (float) r->getX()
                                                                           : (float) r->getRight(),
                                                               (float) r->getCentreY()));
            return {};
        }

        juce::String runInteractionTest()
        {
            juce::StringArray report;
            auto event = [this] (juce::Point<float> p, bool dragged)
            {
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                                         p, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                         this, this, juce::Time::getCurrentTime(), p,
                                         juce::Time::getCurrentTime(), 1, dragged);
            };
            if (sources.empty() || destinations.empty())
            {
                return "FAIL canvas interaction: nodes missing\n";
            }
            const auto* a = nodeRect ("source", sources.front().id);
            const auto* b = nodeRect ("destination", destinations.front().id);
            if (a == nullptr || b == nullptr)
                return "FAIL canvas interaction: node rects missing\n";
            const juce::Point<float> out ((float) a->getRight(), (float) a->getCentreY());
            const juce::Point<float> in ((float) b->getX(), (float) b->getCentreY());
            const int connectsBefore = testConnectCount;
            mouseDown (event (out, false));
            mouseDrag (event (in, true));
            mouseUp (event (in, true));
            report.add ((testConnectCount > connectsBefore ? "PASS " : "FAIL ")
                        + juce::String ("canvas mouse connect"));

            if (! nodes.empty())
            {
                const auto* fresh = nodeRect ("source", sources.front().id);
                if (fresh == nullptr)
                    return report.joinIntoString ("\n") + "\nFAIL canvas interaction: node rect lost\n";
                const auto center = juce::Point<float> (fresh->getCentreX(), fresh->getBottom() - 12.0f);
                const int movesBefore = testMoveCount;
                mouseDown (event (center, false));
                mouseDrag (event (center + juce::Point<float> (36.0f, 18.0f), true));
                mouseUp (event (center + juce::Point<float> (36.0f, 18.0f), true));
                report.add ((testMoveCount > movesBefore ? "PASS " : "FAIL ")
                            + juce::String ("canvas mouse node drag"));
            }

            // Hit-test the actual Bézier curve, then Del.
            if (! routes.empty())
            {
                const auto midpoint = cableMidpoint (0);
                mouseDown (event (midpoint, false));
                report.add ((selectedCable == 0 ? "PASS " : "FAIL ")
                            + juce::String ("canvas mouse selects cable"));
                mouseUp (event (midpoint, false));
                const int deletesBefore = testDeleteCount;
                keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
                report.add ((testDeleteCount > deletesBefore ? "PASS " : "FAIL ")
                            + juce::String ("canvas Del removes selected cable"));
            }

            if (! buses.empty() && ! destinations.empty())
            {
                const auto* busRect = nodeRect ("bus", buses.front().id);
                const auto* destRect = nodeRect ("destination", destinations.front().id);
                if (busRect != nullptr && destRect != nullptr)
                {
                    const juce::Point<float> busOut ((float) busRect->getRight(),
                                                     (float) busRect->getCentreY());
                    const juce::Point<float> destIn ((float) destRect->getX(),
                                                     (float) destRect->getCentreY());
                    const int before = testConnectCount;
                    mouseDown (event (busOut, false));
                    mouseDrag (event (destIn, true));
                    mouseUp (event (destIn, true));
                    bool busRoute = false;
                    for (const auto& r : routes)
                        if (r.destinationId == destinations.front().id)
                            busRoute = true;
                    report.add ((testConnectCount > before && busRoute ? "PASS " : "FAIL ")
                                + juce::String ("canvas bus output creates route"));
                }
            }
            return report.joinIntoString ("\n") + "\n";
        }

    private:
        enum class DraftMode { None, Create, Reconnect };
        struct Node
        {
            juce::String key, kind, id, title, subtitle;
            juce::Point<float> world;
            bool enabled = true, hasIn = false, hasOut = false;
        };

        void rebuild()
        {
            nodes.clear();
            int i = 0;
            for (const auto& s : sources)
                if (s.type != "bus")
                    nodes.push_back ({ "S:" + s.id, "source", s.id, s.name, s.type,
                                       { s.x >= 0 ? s.x : 40.0f, s.y >= 0 ? s.y : 40.0f + 82.0f * i++ },
                                       s.enabled, false, true });
            i = 0;
            for (const auto& b : buses)
                nodes.push_back ({ "B:" + b.id, "bus", b.id, b.name, "bus",
                                   { b.x >= 0 ? b.x : 360.0f, b.y >= 0 ? b.y : 40.0f + 82.0f * i++ },
                                   b.enabled, true, true });
            i = 0;
            for (const auto& d : destinations)
                nodes.push_back ({ "D:" + d.id, "destination", d.id, d.name, d.type,
                                   { d.x >= 0 ? d.x : 680.0f, d.y >= 0 ? d.y : 40.0f + 82.0f * i++ },
                                   d.enabled, true, false });
        }

        juce::Rectangle<int> toScreen (const Node& node) const
        {
            const int w = (int) (200.0f * zoom);
            const int h = (int) (66.0f * zoom);
            const auto p = node.world * zoom + pan;
            return { (int) p.x, (int) p.y, w, h };
        }

        juce::Point<float> screenToWorld (juce::Point<float> p) const
        {
            return (p - pan) / zoom;
        }

        const Node* findNode (const juce::String& kind, const juce::String& id) const
        {
            for (const auto& n : nodes)
                if (n.kind == kind && n.id == id)
                    return &n;
            return nullptr;
        }

        const juce::Rectangle<int>* nodeRect (const juce::String& kind, const juce::String& id) const
        {
            if (const auto* n = findNode (kind, id))
            {
                const int w = (int) (200.0f * zoom);
                const int h = (int) (66.0f * zoom);
                const auto p = n->world * zoom + pan;
                auto& slot = screenCache[kind + ":" + id];
                slot = { (int) p.x, (int) p.y, w, h };
                return &slot;
            }
            return nullptr;
        }

        const juce::Rectangle<int>* nodeRect (const juce::String& key) const
        {
            return nodeRect (kindOf (key), idOf (key));
        }

        const juce::Rectangle<int>* sourceNodeRect (const juce::String& sourceId) const
        {
            for (const auto& s : sources)
                if (s.id == sourceId)
                    return s.type == "bus" ? nodeRect ("bus", s.bus)
                                           : nodeRect ("source", s.id);
            // A route created from a bus before its bus-source exists uses the
            // bus id as source id.
            for (const auto& b : buses)
                if (b.id == sourceId)
                    return nodeRect ("bus", b.id);
            return nullptr;
        }

        const juce::Rectangle<int>* destinationNodeRect (const juce::String& destinationId) const
        {
            if (const auto* r = nodeRect ("destination", destinationId))
                return r;
            return nodeRect ("bus", destinationId);
        }

        juce::String nodeAt (juce::Point<int> p) const
        {
            for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
                if (toScreen (*it).contains (p))
                    return it->key;
            return {};
        }

        int cableAtDestinationPort (juce::Point<int> p) const
        {
            for (int i = 0; i < (int) routes.size(); ++i)
            {
                const auto& r = routes[(size_t) i];
                const auto* rect = destinationNodeRect (r.destinationId);
                if (rect == nullptr) continue;
                juce::Rectangle<float> port ((float) rect->getX() - 9.0f,
                                             (float) rect->getCentreY() - 9.0f, 18.0f, 18.0f);
                if (port.contains (p.toFloat())) return i;
            }
            return -1;
        }

        juce::Point<float> cableMidpoint (int index) const
        {
            if (index < 0 || index >= (int) routes.size())
                return { (float) getWidth() / 2.0f, (float) getHeight() / 2.0f };
            const auto& r = routes[(size_t) index];
            const auto* a = sourceNodeRect (r.sourceId);
            const auto* b = destinationNodeRect (r.destinationId);
            if (a == nullptr || b == nullptr)
                return { (float) getWidth() / 2.0f, (float) getHeight() / 2.0f };
            const auto path = makeCablePath ({ (float) a->getRight(), (float) a->getCentreY() },
                                             { (float) b->getX(), (float) b->getCentreY() });
            juce::Point<float> mid;
            path.getNearestPoint (path.getPointAlongPath (path.getLength() * 0.5f), mid);
            return mid;
        }

        int cableAt (juce::Point<int> p) const
        {
            for (int i = 0; i < (int) routes.size(); ++i)
            {
                const auto& r = routes[(size_t) i];
                const auto* a = sourceNodeRect (r.sourceId);
                const auto* b = destinationNodeRect (r.destinationId);
                if (a == nullptr || b == nullptr) continue;
                const auto start = juce::Point<float> ((float) a->getRight(), (float) a->getCentreY());
                const auto end = juce::Point<float> ((float) b->getX(), (float) b->getCentreY());
                juce::Path path = makeCablePath (start, end);
                juce::Point<float> nearest;
                path.getNearestPoint (p.toFloat(), nearest);
                const float threshold = juce::jlimit (10.0f, 16.0f, 12.0f * zoom);
                if (nearest.getDistanceFrom (p.toFloat()) <= threshold)
                    return i;
            }
            return -1;
        }

        static juce::String kindOf (const juce::String& key)
        {
            const auto prefix = key.upToFirstOccurrenceOf (":", false, false);
            if (prefix == "S") return "source";
            if (prefix == "D") return "destination";
            if (prefix == "B") return "bus";
            return prefix;
        }
        static juce::String idOf (const juce::String& key) { return key.fromFirstOccurrenceOf (":", false, false); }

        void applyNodePosition (const juce::String& key, juce::Point<float> world)
        {
            for (auto& n : nodes)
                if (n.key == key)
                {
                    n.world = world;
                    break;
                }
        }

        void notifyMove (const juce::String& key, juce::Point<float> world, bool finished)
        {
            if (onNodeMoved)
                onNodeMoved (kindOf (key), idOf (key), world.x, world.y, finished);
        }

        void renameNode (const juce::String& kind, const juce::String& id)
        {
            juce::String oldName = id;
            for (const auto& n : nodes)
                if (n.kind == kind && n.id == id) oldName = n.title;
            auto* aw = new juce::AlertWindow (L10n::tr ("Rinomina nodo", "Rename node"),
                                              L10n::tr ("Nuovo nome", "New name"),
                                              juce::MessageBoxIconType::NoIcon, this);
            aw->addTextEditor ("name", oldName);
            aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw, kind, id] (int result)
            {
                if (result == 1 && onRenameNode)
                    onRenameNode (kind, id, aw->getTextEditorContents ("name"));
                delete aw;
            }), true);
        }

        void drawGrid (juce::Graphics& g) const
        {
            const int grid = (int) (44.0f * zoom);
            if (grid <= 4) return;
            g.setColour (Palette::outline.withAlpha (0.12f));
            for (int x = (int) std::fmod (pan.x, (float) grid); x < getWidth(); x += grid)
                g.drawVerticalLine (x, 0.0f, (float) getHeight());
            for (int y = (int) std::fmod (pan.y, (float) grid); y < getHeight(); y += grid)
                g.drawHorizontalLine (y, 0.0f, (float) getWidth());
        }

        void drawNode (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& key,
                       const juce::String& title, const juce::String& subtitle,
                       bool enabled, bool hasOut, bool hasIn) const
        {
            const bool selected = selectedNodes.count (key) != 0;
            const bool online = onlineStates.count (idOf (key)) == 0
                                    || onlineStates.find (idOf (key))->second;
            g.setColour (enabled ? Palette::panelRaised : Palette::panel);
            g.fillRoundedRectangle (r.toFloat(), 8.0f);
            g.setColour (selected ? Palette::cyan
                                  : ! online ? Palette::danger
                                  : enabled ? (hasOut && ! hasIn ? Palette::magenta
                                                               : hasIn && ! hasOut ? Palette::cyan
                                                                                   : Palette::violet)
                                            : Palette::outline);
            g.drawRoundedRectangle (r.toFloat(), 8.0f, selected ? 3.0f : 1.5f);
            if (! online)
            {
                g.setColour (Palette::danger);
                g.setFont (juce::FontOptions (juce::jlimit (7.5f, 10.0f, 9.0f * zoom),
                                              juce::Font::bold));
                g.drawText ("OFFLINE", r.removeFromTop (juce::jmax (10, (int) (14.0f * zoom)))
                                           .removeFromRight (r.getWidth() / 2),
                            juce::Justification::centredRight);
            }
            g.setColour (enabled ? Palette::text : Palette::muted);
            const float titleFont = juce::jlimit (11.0f, 16.0f, 15.0f * juce::jmax (zoom, 0.75f));
            g.setFont (juce::FontOptions (titleFont, juce::Font::bold));
            const bool showSubtitle = r.getWidth() >= 145;
            auto textArea = r.reduced (juce::jmax (7, (int) (14.0f * zoom)), 3);
            g.drawText (title, showSubtitle ? textArea.removeFromTop (r.getHeight() / 2) : textArea,
                        juce::Justification::centredLeft);
            if (showSubtitle)
            {
                g.setColour (Palette::muted);
                g.setFont (juce::FontOptions (juce::jlimit (9.5f, 12.0f, 11.5f * juce::jmax (zoom, 0.8f))));
                g.drawText (subtitle, textArea.removeFromBottom (r.getHeight() / 2),
                            juce::Justification::centredLeft);
            }
            if (hasOut)
            {
                g.setColour (Palette::magenta);
                g.fillEllipse ((float) r.getRight() - 7.0f, (float) r.getCentreY() - 6.0f, 12.0f, 12.0f);
            }
            if (hasIn)
            {
                g.setColour (Palette::cyan);
                g.fillEllipse ((float) r.getX() - 5.0f, (float) r.getCentreY() - 6.0f, 12.0f, 12.0f);
            }
        }

        static juce::Path makeCablePath (juce::Point<float> a, juce::Point<float> b)
        {
            juce::Path path;
            const float dx = juce::jmax (40.0f, std::abs (b.x - a.x) / 2.0f);
            path.startNewSubPath (a);
            path.cubicTo ({ a.x + dx, a.y }, { b.x - dx, b.y }, b);
            return path;
        }

        static void drawCable (juce::Graphics& g, juce::Point<int> a, juce::Point<int> b,
                               juce::Colour colour, float thickness)
        {
            const auto path = makeCablePath (a.toFloat(), b.toFloat());
            g.setColour (colour.withAlpha (0.9f));
            g.strokePath (path, juce::PathStrokeType (thickness,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        std::vector<Source> sources;
        std::vector<Destination> destinations;
        std::vector<Bus> buses;
        std::vector<Route> routes;
        std::vector<Node> nodes;
        std::set<juce::String> selectedNodes;
        std::map<juce::String, juce::Point<float>> dragStartPositions;
        std::map<juce::String, bool> onlineStates;
        juce::String dragNodeKey, draftSource;
        mutable std::map<juce::String, juce::Rectangle<int>> screenCache;
        juce::Point<int> lastMouse, draftPoint, draftStartScreen;
        juce::Point<float> dragStartWorld, pan { 0.0f, 0.0f }, panStart;
        float zoom = 1.0f;
        bool panning = false;
        bool needsFit = true;
        DraftMode draftMode = DraftMode::None;
        int reconnectRoute = -1;
        int selectedCable = -1;
        int hoveredCable = -1;
        juce::Rectangle<int> disconnectButton;
        int testConnectCount = 0, testMoveCount = 0, testDeleteCount = 0;
    };

    struct MeterState
    {
        float l = -100.0f, r = -100.0f;
        bool clip = false, online = true;
        juce::int64 dropped = 0, underruns = 0;
        juce::String kind, type;

        // Engine v2 per-destination buffering telemetry.  These fields are
        // absent from older routers, so they are parsed shallowly and shown as
        // "n/d" in the diagnostics view when missing.
        bool hasBufferTelemetry = false;
        double ringFillMs = -1.0, ringTargetMs = -1.0;
        double driverUsedMs = -1.0, driverTargetMs = -1.0;
        double maxWakeMs = -1.0;
        juce::int64 writerStarved = 0, writes = 0;
        juce::int64 driverDroppedBytes = 0, driverUnderrunBytes = 0;
    };

    struct MeterRow
    {
        juce::String id, name, kind;
        float l = -100.0f, r = -100.0f;
        bool clip = false, online = true;
    };

    class MeterPanel final : public juce::Component
    {
    public:
        void setRows (std::vector<MeterRow> newRows)
        {
            rows = std::move (newRows);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::panel);
            auto area = getLocalBounds().reduced (8, 5);
            if (rows.empty())
            {
                g.setColour (Palette::muted);
                g.setFont (14.0f);
                g.drawText (L10n::tr ("I misuratori compaiono quando il motore è attivo",
                                      "Meters appear when the engine is running"),
                            area, juce::Justification::centred);
                return;
            }
            const int rowH = juce::jlimit (36, 50, area.getHeight() / juce::jmax (1, (int) rows.size()));
            for (const auto& row : rows)
            {
                auto r = area.removeFromTop (rowH).reduced (0, 2);
                auto label = r.removeFromLeft (juce::jmin (132, r.getWidth() / 2));
                g.setColour (row.online ? Palette::text : Palette::disabled);
                g.setFont (14.0f);
                g.drawText (row.name, label.removeFromTop (label.getHeight() / 2),
                            juce::Justification::centredLeft);
                g.setColour (row.online ? Palette::muted : Palette::disabled);
                g.setFont (12.0f);
                g.drawText (row.kind + (row.online ? juce::String() : L10n::utf8 (" · OFFLINE")),
                            label, juce::Justification::centredLeft);
                drawBar (g, r.removeFromTop (r.getHeight() / 2), row.l, row.clip && row.l >= row.r);
                drawBar (g, r, row.r, row.clip);
            }
        }

    private:
        static void drawBar (juce::Graphics& g, juce::Rectangle<int> r, float db, bool clip)
        {
            r = r.reduced (2, 1);
            g.setColour (Palette::panelRaised);
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto filled = r.toFloat().withWidth (r.toFloat().getWidth() * norm);
            g.setGradientFill (juce::ColourGradient (Palette::cyan, (float) r.getX(), 0.0f,
                                                     Palette::magenta, (float) r.getRight(), 0.0f,
                                                     false));
            g.fillRoundedRectangle (filled, 3.0f);
            if (clip)
            {
                g.setColour (Palette::danger);
                g.fillRoundedRectangle (r.toFloat(), 3.0f);
            }
            g.setColour (Palette::text.withAlpha (0.85f));
            g.setFont (11.0f);
            g.drawText (juce::String (db, 1), r, juce::Justification::centredRight);
        }

        std::vector<MeterRow> rows;
    };

    struct RowModel final : public juce::ListBoxModel
    {
        juce::StringArray rows;
        std::function<void (int)> onSelected, onDoubleClicked;

        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            if (onDoubleClicked) onDoubleClicked (row);
        }

        void selectedRowsChanged (int lastRowSelected) override
        {
            if (onSelected)
                onSelected (lastRowSelected);
        }

        int getNumRows() override { return rows.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                               bool rowIsSelected) override
        {
            if (row < 0 || row >= rows.size())
                return;
            if (rowIsSelected)
                g.fillAll (Palette::violet.withAlpha (0.40f));
            g.setColour (Palette::text);
            g.setFont (14.0f);
            g.drawText (rows[row], 6, 0, width - 12, height, juce::Justification::centredLeft, true);
        }
    };

    struct SourceDialog : public juce::Component
    {
        SourceDialog (const juce::StringArray& endpoints, const juce::StringArray& busNames)
        {
            type.addItemList ({ "asio-in", "loopback", "capture", "process", "bus" }, 1);
            type.setSelectedId (1);
            endpoint.addItemList (endpoints, 1);
            bus.addItemList (busNames, 1);
            addAndMakeVisible (type);
            addAndMakeVisible (endpoint);
            addAndMakeVisible (bus);
            addAndMakeVisible (name);
            addAndMakeVisible (channelsEditor);
            addAndMakeVisible (processEditor);
            name.setTextToShowWhenEmpty ("Source name", Palette::muted);
            channelsEditor.setTextToShowWhenEmpty ("Channels, e.g. 1,2", Palette::muted);
            processEditor.setTextToShowWhenEmpty ("process.exe or leave empty for PID", Palette::muted);
            type.onChange = [this] { updateVisibility(); };
            updateVisibility();
            setSize (460, 260);
        }

        void updateVisibility()
        {
            const auto t = type.getText();
            endpoint.setVisible (t == "loopback" || t == "capture");
            bus.setVisible (t == "bus");
            processEditor.setVisible (t == "process");
            channelsEditor.setVisible (t == "asio-in");
            resized();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12);
            type.setBounds (area.removeFromTop (28));
            area.removeFromTop (6);
            name.setBounds (area.removeFromTop (28));
            area.removeFromTop (6);
            if (endpoint.isVisible()) endpoint.setBounds (area.removeFromTop (28));
            if (bus.isVisible()) bus.setBounds (area.removeFromTop (28));
            if (processEditor.isVisible()) processEditor.setBounds (area.removeFromTop (28));
            if (channelsEditor.isVisible()) channelsEditor.setBounds (area.removeFromTop (28));
        }

        juce::ComboBox type, endpoint, bus;
        juce::TextEditor name, channelsEditor, processEditor;
    };

    // Visual channel selector: shows the channel names actually reported by
    // the selected driver, with quick mono/stereo/all choices.  Manual channel
    // text remains available in the main panel for advanced cases.
    class ChannelPickerComponent final : public juce::Component
    {
    public:
        ChannelPickerComponent (const juce::String& device, const juce::StringArray& names,
                                const std::vector<int>& selected)
            : channelNames (names)
        {
            title.setText (device.isNotEmpty() ? device : "Canali disponibili",
                           juce::dontSendNotification);
            title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            addAndMakeVisible (title);

            viewport = new juce::Viewport();
            auto* content = new juce::Component();
            viewport->setViewedComponent (content, true);
            viewport->setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            for (int i = 0; i < names.size(); ++i)
            {
                auto* toggle = new juce::ToggleButton (juce::String (i + 1) + L10n::utf8 ("  ·  ") + names[i]);
                toggle->setToggleState (std::find (selected.begin(), selected.end(), i) != selected.end(),
                                        juce::dontSendNotification);
                toggles.push_back (toggle);
                content->addAndMakeVisible (toggle);
            }
            content->setSize (520, juce::jmax (30, names.size() * 26));
            content->setBounds (0, 0, 520, juce::jmax (30, names.size() * 26));

            stereo.setButtonText ("Stereo 1/2");
            mono.setButtonText ("Mono 1");
            all.setButtonText ("Tutti i canali");
            clear.setButtonText ("Nessuno");
            addAndMakeVisible (stereo); addAndMakeVisible (mono);
            addAndMakeVisible (all); addAndMakeVisible (clear);
            stereo.onClick = [this] { selectRange ({ 0, 1 }); };
            mono.onClick = [this] { selectRange ({ 0 }); };
            all.onClick = [this] { selectRange (allIndices()); };
            clear.onClick = [this] { selectRange ({}); };
            setSize (560, 520);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (10);
            title.setBounds (area.removeFromTop (26));
            auto quick = area.removeFromTop (30);
            stereo.setBounds (quick.removeFromLeft (100)); quick.removeFromLeft (4);
            mono.setBounds (quick.removeFromLeft (80)); quick.removeFromLeft (4);
            all.setBounds (quick.removeFromLeft (110)); quick.removeFromLeft (4);
            clear.setBounds (quick.removeFromLeft (90));
            viewport->setBounds (area.reduced (0, 6));
            if (auto* content = viewport->getViewedComponent())
                content->setBounds (0, 0, juce::jmax (200, viewport->getWidth() - 18),
                                    juce::jmax (30, (int) toggles.size() * 26));
            for (int i = 0; i < toggles.size(); ++i)
                toggles[i]->setBounds (4, i * 26, juce::jmax (200, viewport->getWidth() - 26), 24);
        }

        std::vector<int> selectedChannels() const
        {
            std::vector<int> result;
            for (int i = 0; i < toggles.size(); ++i)
                if (toggles[i]->getToggleState())
                    result.push_back (i);
            return result;
        }

    private:
        std::vector<int> allIndices() const
        {
            std::vector<int> result;
            for (int i = 0; i < channelNames.size(); ++i) result.push_back (i);
            return result;
        }
        void selectRange (const std::vector<int>& indices)
        {
            for (int i = 0; i < toggles.size(); ++i)
                toggles[i]->setToggleState (std::find (indices.begin(), indices.end(), i) != indices.end(),
                                            juce::dontSendNotification);
        }

        juce::StringArray channelNames;
        juce::Label title;
        juce::Viewport* viewport = nullptr;
        std::vector<juce::ToggleButton*> toggles;
        juce::TextButton stereo, mono, all, clear;
    };
    // Searchable running-application picker for Process Capture. Enumeration
    // happens only while the dialog is visible; there is no background poll.
    struct RunningApp
    {
        juce::String executable, title, path;
        int pid = 0;
        juce::Image icon;
    };

    static juce::Image processIcon (const juce::String& path)
    {
        juce::Image result;
        SHFILEINFOW info {};
        if (SHGetFileInfoW (path.toWideCharPointer(), 0, &info, sizeof (info),
                            SHGFI_ICON | SHGFI_SMALLICON) == 0 || info.hIcon == nullptr)
            return result;
        ICONINFO iconInfo {};
        if (! GetIconInfo (info.hIcon, &iconInfo))
        {
            DestroyIcon (info.hIcon);
            return result;
        }
        BITMAP bitmap {};
        if (iconInfo.hbmColor == nullptr || GetObject (iconInfo.hbmColor, sizeof (bitmap), &bitmap) == 0)
        {
            if (iconInfo.hbmColor != nullptr) DeleteObject (iconInfo.hbmColor);
            if (iconInfo.hbmMask != nullptr) DeleteObject (iconInfo.hbmMask);
            DestroyIcon (info.hIcon);
            return result;
        }
        BITMAPINFO bitmapInfo {};
        bitmapInfo.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = bitmap.bmWidth;
        bitmapInfo.bmiHeader.biHeight = -bitmap.bmHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> pixels ((size_t) bitmap.bmWidth * (size_t) bitmap.bmHeight * 4, 0);
        HDC dc = CreateCompatibleDC (nullptr);
        if (dc != nullptr)
        {
            GetDIBits (dc, iconInfo.hbmColor, 0, (UINT) bitmap.bmHeight,
                       pixels.data(), &bitmapInfo, DIB_RGB_COLORS);
            DeleteDC (dc);
            result = juce::Image (juce::Image::ARGB, bitmap.bmWidth, bitmap.bmHeight, true);
            for (int y = 0; y < bitmap.bmHeight; ++y)
                for (int x = 0; x < bitmap.bmWidth; ++x)
                {
                    const auto* p = pixels.data() + ((size_t) y * (size_t) bitmap.bmWidth + (size_t) x) * 4;
                    result.setPixelAt (x, y, juce::Colour (p[2], p[1], p[0], p[3]));
                }
        }
        if (iconInfo.hbmColor != nullptr) DeleteObject (iconInfo.hbmColor);
        if (iconInfo.hbmMask != nullptr) DeleteObject (iconInfo.hbmMask);
        DestroyIcon (info.hIcon);
        return result;
    }

    static std::vector<RunningApp> enumerateRunningApps (bool includeBackground)
    {
        std::vector<RunningApp> apps;
        struct Context
        {
            std::vector<RunningApp>* apps;
            std::set<int>* pids;
        } context { &apps, new std::set<int>() };
        EnumWindows ([] (HWND hwnd, LPARAM parameter) -> BOOL
        {
            auto* ctx = reinterpret_cast<Context*> (parameter);
            if (ctx == nullptr || ! IsWindowVisible (hwnd))
                return TRUE;
            const int titleLength = GetWindowTextLengthW (hwnd);
            if (titleLength <= 0)
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId (hwnd, &pid);
            if (pid == 0 || ctx->pids->count ((int) pid) != 0)
                return TRUE;
            HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process == nullptr)
                return TRUE;
            wchar_t path[32768] = {};
            DWORD pathLength = (DWORD) std::size (path);
            const bool got = QueryFullProcessImageNameW (process, 0, path, &pathLength) != 0;
            CloseHandle (process);
            if (! got)
                return TRUE;
            wchar_t title[1024] = {};
            GetWindowTextW (hwnd, title, (int) std::size (title));
            RunningApp app;
            app.pid = (int) pid;
            app.path = juce::String (path, (int) pathLength);
            app.executable = app.path.fromLastOccurrenceOf ("\\", false, false);
            app.title = juce::String (title).trim();
            if (app.title.isEmpty())
                return TRUE;
            app.icon = processIcon (app.path);
            ctx->pids->insert (app.pid);
            ctx->apps->push_back (std::move (app));
            return TRUE;
        }, reinterpret_cast<LPARAM> (&context));
        delete context.pids;

        if (includeBackground)
        {
            HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
            if (snapshot != INVALID_HANDLE_VALUE)
            {
                PROCESSENTRY32W entry {};
                entry.dwSize = sizeof (entry);
                if (Process32FirstW (snapshot, &entry))
                    do
                    {
                        if (entry.th32ProcessID == 0 || entry.th32ProcessID == 4) continue;
                        if (std::any_of (apps.begin(), apps.end(),
                                         [&] (const RunningApp& a) { return a.pid == (int) entry.th32ProcessID; }))
                            continue;
                        RunningApp app;
                        app.pid = (int) entry.th32ProcessID;
                        app.executable = juce::String (entry.szExeFile);
                        app.title = app.executable;
                        app.icon = juce::Image();
                        apps.push_back (std::move (app));
                    } while (Process32NextW (snapshot, &entry));
                CloseHandle (snapshot);
            }
        }
        std::sort (apps.begin(), apps.end(), [] (const RunningApp& a, const RunningApp& b)
        {
            return a.title.compareIgnoreCase (b.title) < 0;
        });
        return apps;
    }

    class ProcessPickerComponent final : public juce::Component, private juce::Timer
    {
    public:
        std::function<void (const RunningApp&, bool includeChildren)> onAccept;
        ProcessPickerComponent()
        {
            search.setTextToShowWhenEmpty (L10n::tr ("Cerca applicazione o titolo finestra...",
                                                      "Search application or window title..."),
                                           Palette::muted);
            search.onTextChange = [this] { rebuild(); };
            addAndMakeVisible (search);
            addAndMakeVisible (refresh);
            refresh.setButtonText (L10n::tr ("Aggiorna", "Refresh"));
            refresh.onClick = [this] { refreshList(); };
            addAndMakeVisible (includeChildren);
            includeChildren.setButtonText (L10n::tr ("Processi figli inclusi (API Windows)",
                                                      "Child processes included (Windows API)"));
            includeChildren.setEnabled (false);
            includeChildren.setToggleState (true, juce::dontSendNotification);
            showSystem.setButtonText (L10n::tr ("Mostra processi di sistema (avanzato)",
                                                 "Show system processes (advanced)"));
            showSystem.onClick = [this] { refreshList(); };
            addAndMakeVisible (showSystem);
            list.setModel (&model);
            list.setRowHeight (34);
            addAndMakeVisible (list);
            hint.setText (L10n::tr ("La cattura per processo richiede Windows 10 20348+. "
                                       "Applicazioni protette o senza audio possono restare offline; "
                                       "il nodo mantiene il collegamento e riprova senza usare il PID come chiave.",
                                       "Per-process capture requires Windows 10 20348+. "
                                       "Protected applications or apps without audio may stay offline; "
                                       "the node keeps the binding and retries without using the PID as its key."),
                             juce::dontSendNotification);
            hint.setColour (juce::Label::textColourId, Palette::muted);
            hint.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (hint);
            choose.setButtonText (L10n::tr ("Usa applicazione", "Use application"));
            choose.onClick = [this] { acceptSelected(); };
            addAndMakeVisible (choose);
            model.apps = &filtered;
            model.onChoose = [this] (int row) { selectedRow = row; acceptSelected(); };
            fitComponentToWorkArea (*this, 620, 430);
            startTimer (2500);
            refreshList();
        }
        ~ProcessPickerComponent() override { stopTimer(); }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8);
            auto top = area.removeFromTop (30);
            search.setBounds (top.removeFromLeft (top.getWidth() - 100));
            top.removeFromLeft (4);
            refresh.setBounds (top.removeFromLeft (96));
            auto options = area.removeFromTop (26);
            includeChildren.setBounds (options.removeFromLeft (220));
            options.removeFromLeft (8);
            showSystem.setBounds (options);
            list.setBounds (area.removeFromTop (juce::jmax (120, area.getHeight() - 82)));
            hint.setBounds (area.removeFromTop (42));
            choose.setBounds (area.removeFromTop (30).removeFromRight (150));
        }
        std::function<void()> onCloseRequest;

    private:
        void acceptSelected()
        {
            const int row = selectedRow >= 0 ? selectedRow : list.getSelectedRow();
            if (row < 0 || row >= (int) filtered.size()) return;
            if (onAccept) onAccept (*filtered[(size_t) row], includeChildren.getToggleState());
            if (onCloseRequest) onCloseRequest();
        }
        int selectedRow = -1;
        void timerCallback() override { if (isShowing()) refreshList(); }
        void refreshList()
        {
            apps = enumerateRunningApps (showSystem.getToggleState());
            rebuild();
        }
        void rebuild()
        {
            filtered.clear();
            const auto query = search.getText().trim().toLowerCase();
            for (const auto& app : apps)
                if (query.isEmpty() || app.title.toLowerCase().contains (query)
                    || app.executable.toLowerCase().contains (query))
                    filtered.push_back (&app);
            model.apps = &filtered;
            model.rows = (int) filtered.size();
            list.updateContent();
            list.repaint();
        }
        struct Model final : public juce::ListBoxModel
        {
            int rows = 0;
            const std::vector<const RunningApp*>* apps = nullptr;
            juce::ListBox* list = nullptr;
            std::function<void (int)> onChoose;
            int getNumRows() override { return rows; }
            void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
            {
                if (apps == nullptr || row < 0 || row >= (int) apps->size()) return;
                if (selected) g.fillAll (Palette::violet.withAlpha (0.35f));
                const auto& app = *(*apps)[(size_t) row];
                if (app.icon.isValid())
                    g.drawImageWithin (app.icon, 4, 5, 24, 24, juce::RectanglePlacement::centred);
                g.setColour (Palette::text);
                g.setFont (14.0f);
                g.drawText (app.title, 34, 2, width - 140, 16, juce::Justification::centredLeft, true);
                g.setColour (Palette::muted);
                g.setFont (11.0f);
                g.drawText (app.executable + L10n::utf8 ("  ·  PID ") + juce::String (app.pid),
                            34, 18, width - 140, 14, juce::Justification::centredLeft, true);
            }
            void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
            {
                if (onChoose) onChoose (row);
            }
            void returnKeyPressed (int row) override
            {
                if (onChoose) onChoose (row);
            }
        };
        Model model;
        juce::ListBox list;
        juce::TextEditor search;
        juce::TextButton refresh;
        juce::ToggleButton includeChildren, showSystem;
        juce::Label hint;
        juce::TextButton choose;
        std::vector<RunningApp> apps;
        std::vector<const RunningApp*> filtered;
    };
    //==============================================================================
    struct ComApartment
    {
        HRESULT result = E_FAIL;
        ComApartment() { result = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED); }
        ~ComApartment() { if (SUCCEEDED (result)) CoUninitialize(); }
        bool usable() const { return SUCCEEDED (result) || result == RPC_E_CHANGED_MODE; }
    };

    //==============================================================================
    // Virtual-device manager.  The driver publishes a fixed pool of independent
    // endpoint pairs; this window shows the real Windows endpoints, their
    // state and which patchbay nodes use each pair.  Renaming is done through
    // the Windows audio property store, so it does not require a new INF.
    class VirtualDevicesDialog final : public juce::Component
    {
    public:
        VirtualDevicesDialog()
        {
            addAndMakeVisible (log);
            log.setMultiLine (true);
            log.setReadOnly (true);
            addAndMakeVisible (refreshButton);
            addAndMakeVisible (openSettingsButton);
            addAndMakeVisible (closeButton);
            refreshButton.setButtonText (L10n::tr ("Aggiorna", "Refresh"));
            openSettingsButton.setButtonText (L10n::tr ("Impostazioni audio di Windows...",
                                                        "Windows sound settings..."));
            closeButton.setButtonText (L10n::tr ("Chiudi", "Close"));
            refreshButton.onClick = [this] { refresh(); };
            openSettingsButton.onClick = []
            {
#if JUCE_WINDOWS
                ShellExecuteW (nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
#endif
            };
            closeButton.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };
            setSize (720, 430);
            refresh();
        }

        void setAssignments (const juce::String& text)
        {
            assignments = text;
            refresh();
        }

        static juce::String normalizeWindowsNames()
        {
            juce::String report;
            renameEndpoints (false, report);
            renameEndpoints (true, report);
            return report;
        }

        static juce::String listWindowsEndpoints()
        {
            juce::String text;
            text << "Render:\n";
            for (const auto& ep : enumerate (false))
                text << "  " << ep.name << "\n";
            text << "Capture:\n";
            for (const auto& ep : enumerate (true))
                text << "  " << ep.name << "\n";
            return text;
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::background);
            g.setColour (Palette::outline);
            g.drawRect (getLocalBounds(), 1);
            g.setColour (Palette::text);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText (L10n::tr ("Dispositivi virtuali", "Virtual devices"), 14, 8, getWidth() - 28, 24,
                        juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (14);
            area.removeFromTop (32);
            auto row = area.removeFromBottom (34);
            closeButton.setBounds (row.removeFromRight (90));
            row.removeFromRight (8);
            openSettingsButton.setBounds (row.removeFromRight (210));
            row.removeFromRight (8);
            refreshButton.setBounds (row.removeFromRight (90));
            area.removeFromBottom (8);
            log.setBounds (area);
        }

    private:
#if JUCE_WINDOWS
        struct Endpoint
        {
            juce::String name;
            DWORD state = 0;
        };

        static std::vector<Endpoint> enumerate (bool capture)
        {
            ComApartment com;
            std::vector<Endpoint> result;
            if (! com.usable())
                return result;
            IMMDeviceEnumerator* enumerator = nullptr;
            if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS (&enumerator))) || enumerator == nullptr)
                return result;
            IMMDeviceCollection* collection = nullptr;
            if (SUCCEEDED (enumerator->EnumAudioEndpoints (capture ? eCapture : eRender,
                                                           DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED
                                                               | DEVICE_STATE_UNPLUGGED,
                                                           &collection))
                && collection != nullptr)
            {
                UINT count = 0;
                collection->GetCount (&count);
                for (UINT i = 0; i < count; ++i)
                {
                    IMMDevice* device = nullptr;
                    if (FAILED (collection->Item (i, &device)) || device == nullptr)
                        continue;
                    IPropertyStore* store = nullptr;
                    juce::String friendly, parent;
                    if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &store)) && store != nullptr)
                    {
                        PROPVARIANT value;
                        PropVariantInit (&value);
                        if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &value))
                            && value.vt == VT_LPWSTR)
                            friendly = juce::String (value.pwszVal);
                        PropVariantClear (&value);
                        PropVariantInit (&value);
                        if (SUCCEEDED (store->GetValue (PKEY_DeviceInterface_FriendlyName, &value))
                            && value.vt == VT_LPWSTR)
                            parent = juce::String (value.pwszVal);
                        PropVariantClear (&value);
                        store->Release();
                    }
                    DWORD state = 0;
                    device->GetState (&state);
                    if (parent.containsIgnoreCase ("NLMatrix")
                        || friendly.containsIgnoreCase ("NLMatrix"))
                        result.push_back ({ friendly, state });
                    device->Release();
                }
                collection->Release();
            }
            enumerator->Release();
            return result;
        }

        static void renameEndpoints (bool capture, juce::String& report)
        {
            ComApartment com;
            if (! com.usable())
            {
                report = "COM initialization failed.";
                return;
            }
            IMMDeviceEnumerator* enumerator = nullptr;
            if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS (&enumerator))) || enumerator == nullptr)
            {
                report = "Cannot open the Windows audio endpoint enumerator.";
                return;
            }
            IMMDeviceCollection* collection = nullptr;
            const auto flow = capture ? eCapture : eRender;
            if (FAILED (enumerator->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &collection))
                || collection == nullptr)
            {
                enumerator->Release();
                report = "Cannot enumerate endpoints.";
                return;
            }
            UINT count = 0;
            collection->GetCount (&count);
            int index = 0;
            for (UINT i = 0; i < count && index < NLMATRIX_MAX_ENDPOINTS; ++i)
            {
                IMMDevice* device = nullptr;
                if (FAILED (collection->Item (i, &device)) || device == nullptr)
                    continue;
                IPropertyStore* readStore = nullptr;
                juce::String parent;
                if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &readStore)) && readStore != nullptr)
                {
                    PROPVARIANT value;
                    PropVariantInit (&value);
                    if (SUCCEEDED (readStore->GetValue (PKEY_DeviceInterface_FriendlyName, &value))
                        && value.vt == VT_LPWSTR)
                        parent = juce::String (value.pwszVal);
                    PropVariantClear (&value);
                    readStore->Release();
                }
                if (! parent.containsIgnoreCase ("NLMatrix"))
                {
                    device->Release();
                    continue;
                }
                IPropertyStore* store = nullptr;
                if (SUCCEEDED (device->OpenPropertyStore (STGM_READWRITE, &store)) && store != nullptr)
                {
                    const auto name = (capture ? juce::String ("NL Mic ")
                                               : juce::String ("NL Return "))
                                      + juce::String (index + 1);
                    PROPVARIANT value;
                    PropVariantInit (&value);
                    const HRESULT initResult = InitPropVariantFromString (
                        name.toWideCharPointer(), &value);
                    const HRESULT setResult = SUCCEEDED (initResult)
                                                  ? store->SetValue (PKEY_Device_FriendlyName, value)
                                                  : initResult;
                    const HRESULT commitResult = SUCCEEDED (setResult) ? store->Commit() : setResult;
                    report << name << (SUCCEEDED (commitResult) ? " OK\n" : " FAILED\n");
                    PropVariantClear (&value);
                    store->Release();
                }
                ++index;
                device->Release();
            }
            collection->Release();
            enumerator->Release();
        }
#else
        static std::vector<Endpoint> enumerate (bool) { return {}; }
        static void renameEndpoints (bool, juce::String& report) { report = "Windows only."; }
#endif

        void refresh()
        {
            juce::String text;
            text << "Driver pool: " << NLMATRIX_MAX_ENDPOINTS
                 << " independent render/capture pairs.\n"
                 << "Internal buses do not create Windows devices.\n\n";
            text << "Render endpoints (NL Return):\n";
            int n = 1;
            for (const auto& ep : enumerate (false))
                text << "  " << n++ << ". " << ep.name
                     << (ep.state == DEVICE_STATE_ACTIVE ? "  [online]" : "  [offline]") << "\n";
            text << "\nCapture endpoints (NL Mic):\n";
            n = 1;
            for (const auto& ep : enumerate (true))
                text << "  " << n++ << ". " << ep.name
                     << (ep.state == DEVICE_STATE_ACTIVE ? "  [online]" : "  [offline]") << "\n";
            if (assignments.isNotEmpty())
                text << "\nAssignments in this profile:\n" << assignments;
            text << "\nRename individual endpoints from Windows sound settings; "
                    "NL Audio Matrix only publishes the fixed pool and keeps it assigned here.";
            log.setText (text);
        }

        juce::TextEditor log;
        juce::TextButton refreshButton, openSettingsButton, closeButton;
        juce::String assignments;
    };

    // G4: one ASIO device format choice as edited in Preferences and persisted in
    // the profile devices[] and preferences.json.
    struct DeviceFormatChoice
    {
        juce::String deviceId;   // "asio:<driver>" (same id used in the profile)
        juce::String driver;
        juce::String role;       // e.g. "Motore principale" or "usato da: <nodes>"
        double sampleRate = 0.0;
        int bufferSize = 0;      // 0 = automatic (driver/backend choice)
    };

    // G4: per-device format as reported by the router METERS "devices" array.
    struct RouterDeviceTelemetry
    {
        juce::String id, backend, driver;
        bool clockMaster = false;
        double requestedRate = 0.0, rate = 0.0;
        int requestedBuffer = 0, buffer = 0;
        int inputs = 0, outputs = 0;
    };

    #include "about_panel.h"
    #include "device_browser.h"
    #include "diagnostics_panel.h"
    #include "preferences_panel.h"
    #include "mixer_panel.h"

    // Shared PNG writer for the GUI verification screenshots.
    static void writePngImage (const juce::Image& image, const juce::File& file)
    {
        file.deleteFile();
        if (! image.isValid() || image.getWidth() <= 0)
            return;
        if (auto stream = file.createOutputStream())
        {
            juce::PNGImageFormat png;
            png.writeImageToStream (image, *stream);
        }
    }

    class MainComponent final : public juce::Component, private juce::Timer
    {
    public:
        MainComponent()
            : sourceList ("sources", &sourceModel),
              destinationList ("destinations", &destinationModel),
              busList ("buses", &busModel),
              routeList ("routes", &routeModel)
        {
            setSize (1600, 1000);
            setLookAndFeel (&lookAndFeel);
            // Share the palette-aware look&feel with every other window this app
            // opens (Preferences, Diagnostics, About, Mixer, Device browser), so
            // Light/System themes recolour their controls too instead of leaving
            // construction-time dark colours (C9).
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            setColour (juce::ResizableWindow::backgroundColourId, Palette::background);

            addChildComponent (mixerPanel);
            addChildComponent (mixerResizer);
            addAndMakeVisible (profileName);
            profileName.setTextToShowWhenEmpty ("Profile name", Palette::muted);
            profileName.setText ("New profile");

            addAndMakeVisible (asioBox);
            asioLabel.setText ("Motore ASIO principale", juce::dontSendNotification);
            asioLabel.setColour (juce::Label::textColourId, Palette::text.withAlpha (0.75f));
            asioLabel.setFont (juce::FontOptions (13.0f));
            asioLabel.setTooltip (L10n::utf8 ("Driver ASIO del master clock. Ogni sorgente/destinazione può usare un driver ASIO aggiuntivo nello stesso motore."));
            addAndMakeVisible (asioLabel);
            asioBox.onChange = [this] { setStatus (L10n::tr ("Motore ASIO: ", "ASIO engine: ") + asioBox.getText()); };
            addAndMakeVisible (startStop);
            addAndMakeVisible (applyButton);
            addAndMakeVisible (saveButton);
            addAndMakeVisible (loadButton);
            addAndMakeVisible (refreshButton);
            addAndMakeVisible (leftPanelArrow);
            leftPanelArrow.setTooltip (L10n::tr ("Comprimi / espandi il pannello sinistro",
                                                 "Collapse / expand the left panel"));
            leftPanelArrow.onClick = [this] { setLeftPanelCollapsed (! leftPanelCollapsed); };

            addAndMakeVisible (statusLabel);
            statusLabel.setColour (juce::Label::textColourId, Palette::success);
            statusLabel.setText ("Ready", juce::dontSendNotification);

            startStop.setButtonText ("Start");
            applyButton.setButtonText ("Apply live");
            saveButton.setButtonText ("Save profile...");
            loadButton.setButtonText ("Load profile...");
            refreshButton.setButtonText ("Refresh devices");

            addSection (sourceTitle, "Sources (inputs)");
            configureSectionArrow (sourceSectionArrow);
            sourceSectionArrow.onClick = [this] { setSectionCollapsed (0, ! sourcesCollapsed); };
            sourceList.setRowHeight (22);
            sourceList.setModel (&sourceModel);
            addAndMakeVisible (sourceList);

            sourceType.addItemList ({ "ASIO input", "WASAPI loopback (system playback)",
                                      "WASAPI capture (input device)", "Process capture (per app)",
                                      "Internal bus", "NL virtual endpoint (driver)" }, 1);
            sourceType.setSelectedId (1);
            sourceType.setTooltip (L10n::tr ("Da dove questa sorgente prende l'audio. ASIO usa il dispositivo ASIO del motore; "
                                             "loopback registra ciò che Windows riproduce; capture registra un dispositivo di ingresso; "
                                             "process registra una applicazione; il bus interno resta dentro NL Audio Matrix; "
                                             "l'endpoint virtuale NL usa un NL Return pubblicato.",
                                             "Where this source takes audio from. ASIO uses the engine's ASIO device; "
                                             "loopback records what Windows plays; capture records an input device; "
                                             "process records one application; internal bus stays inside NL Audio Matrix; "
                                             "NL virtual endpoint uses a published NL Return."));
            addAndMakeVisible (sourceType);
            for (int n = 1; n <= NLMATRIX_MAX_ENDPOINTS; ++n)
                sourceDriverEndpoint.addItem ("NL virtual input " + juce::String (n), n);
            sourceDriverEndpoint.setSelectedId (1);
            sourceDriverEndpoint.setTooltip (L10n::tr ("Coppia di endpoint virtuali usata da questa sorgente. "
                                                       "La coppia 1 è NL Return 1; la coppia N è NL Return N.",
                                                       "Independent virtual endpoint pair used by this source. "
                                                       "Pair 1 is NL Return 1; pair N is NL Return N."));
            addAndMakeVisible (sourceDriverEndpoint);
            sourceName.setTextToShowWhenEmpty ("source name", Palette::muted);
            sourceChannels.setTextToShowWhenEmpty ("ASIO channels, e.g. 1,2", Palette::muted);
            sourceChannels.setTooltip (L10n::tr ("Numeri di canale ASIO come mostrati dal driver (1-based), "
                                                 "separati da virgola. Ignorati dalle sorgenti non ASIO.",
                                                 "ASIO channel numbers as shown by the ASIO driver (1-based), "
                                                 "separated by comma. Ignored by non-ASIO sources."));
            sourceProcess.setTextToShowWhenEmpty ("process.exe or PID", Palette::muted);
            sourceProcess.setTooltip (L10n::tr ("Applicazione da catturare: nome eseguibile (es. Source-Connect.exe) "
                                                "o process id. Si può includere l'intero albero dei processi.",
                                                "Application to capture: executable name (e.g. Source-Connect.exe) "
                                                "or process id. The whole process tree can be included."));
            addAndMakeVisible (sourceName);
            sourcePickChannels.setButtonText ("Scegli canali...");
            sourcePickChannels.setTooltip ("Elenca i canali reali del dispositivo selezionato.");
            sourcePickChannels.onClick = [this] { openChannelPicker (true); };
            addAndMakeVisible (sourcePickChannels);
            addAndMakeVisible (sourceChannels);
            addAndMakeVisible (sourceProcess);
            sourcePickProcess.setButtonText ("Scegli app...");
            sourcePickProcess.setTooltip ("Elenco ricercabile delle applicazioni in esecuzione, con icone e titoli.");
            sourcePickProcess.onClick = [this] { openProcessPicker(); };
            addAndMakeVisible (sourcePickProcess);
            sourceProcessTree.setButtonText ("Processi figli inclusi (API Windows)");
            sourceProcessTree.setEnabled (false);
            sourceProcessTree.setToggleState (true, juce::dontSendNotification);
            sourceProcessTree.setTooltip ("Cattura anche l audio dei processi figli dell applicazione scelta.");
            addAndMakeVisible (sourceProcessTree);
            sourceGain.setTextToShowWhenEmpty ("source gain dB", Palette::muted);
            sourceGain.setTooltip (L10n::tr ("Guadagno di ingresso/sorgente in dB (0 = unità). Applicato prima della matrice di routing.",
                                             "Input/source gain in dB (0 = unity). Applied before the routing matrix."));
            sourceGain.setText ("0", juce::dontSendNotification);
            addAndMakeVisible (sourceGain);
            sourcePan.setTextToShowWhenEmpty ("pan -1..1", Palette::muted);
            sourcePan.setText ("0", juce::dontSendNotification);
            sourcePan.setTooltip ("Pan/balance della sorgente; 0 = centro. Applicato prima della matrice.");
            addAndMakeVisible (sourcePan);
            addAndMakeVisible (sourcePolarity);
            sourcePolarity.setTooltip (L10n::utf8 ("Inverte la polarità della sorgente (anti-fase)."));
            addAndMakeVisible (sourceDevice);
            sourceDevice.setTooltip (L10n::tr ("Dispositivo ASIO usato da questo ingresso quando è diverso dal motore principale. "
                                               "Gli adapter nello stesso processo vengono creati solo se referenziati.",
                                               "ASIO device used by this input when device != main engine. Same-process adapters are created only when referenced."));
            addAndMakeVisible (destinationDevice);
            addAndMakeVisible (destinationMonitor);
            addAndMakeVisible (destinationMixMinus);
            destinationMixMinus.setTooltip ("Esclude una sorgente da questa destinazione (mix-minus / anti-feedback).");
            destinationMonitor.setTooltip (L10n::tr ("Il Solo agisce solo sulle destinazioni marcate come monitor; i ritorni normali/FOH non vengono mai mutati dal solo.",
                                                      "Solo affects only destinations marked as monitor; normal returns/FOH are never muted by solo."));
            destinationDevice.setTooltip (L10n::tr ("Dispositivo ASIO usato da questa uscita quando è diverso dal motore principale.",
                                                     "ASIO device used by this output when device != main engine."));
            destinationDriverTarget.setTextToShowWhenEmpty ("target driver ms (0 = auto)", Palette::muted);
            destinationDriverTarget.setTooltip (L10n::tr ("Target del ring del driver NLMatrix per questa uscita, in ms (0 = tuning globale).",
                                                          "NLMatrix driver ring target for this output in ms (0 = global tuning)."));
            destinationDriverTarget.setText ("0", juce::dontSendNotification);
            addAndMakeVisible (destinationDriverTarget);
            destinationGain.setTextToShowWhenEmpty ("destination gain dB", Palette::muted);
            destinationGain.setTooltip (L10n::tr ("Guadagno di destinazione in dB (0 = unità). Applicato dopo il mix della matrice.",
                                                  "Destination gain in dB (0 = unity). Applied after the matrix mix."));
            destinationGain.setText ("0", juce::dontSendNotification);
            addAndMakeVisible (destinationGain);
            busGain.setTextToShowWhenEmpty ("bus gain dB", Palette::muted);
            busGain.setTooltip (L10n::tr ("Guadagno del bus in dB (0 = unità).", "Bus gain in dB (0 = unity)."));
            busGain.setText ("0", juce::dontSendNotification);
            addAndMakeVisible (busGain);
            sourceEnabled.setButtonText ("Sorgente attiva");
            sourceEnabled.setToggleState (true, juce::dontSendNotification);
            sourceEnabled.setTooltip ("Disattiva la sorgente senza rimuovere configurazione o collegamenti.");
            addAndMakeVisible (sourceEnabled);
            addAndMakeVisible (sourceEndpoint);
            addAndMakeVisible (sourceBus);
            addAndMakeVisible (addSourceButton);
            addAndMakeVisible (updateSourceButton);
            addAndMakeVisible (removeSourceButton);
            addSourceButton.setButtonText ("Add source");
            updateSourceButton.setButtonText ("Update");
            removeSourceButton.setButtonText ("Remove");

            addSection (destinationTitle, "Destinations (outputs)");
            configureSectionArrow (destinationSectionArrow);
            destinationSectionArrow.onClick = [this] { setSectionCollapsed (1, ! destinationsCollapsed); };
            destinationList.setRowHeight (22);
            destinationList.setModel (&destinationModel);
            addAndMakeVisible (destinationList);
            destinationType.addItemList ({ "ASIO output", "Windows render endpoint",
                                           "NL virtual endpoint (driver)" }, 1);
            destinationType.setSelectedId (1);
            destinationType.setTooltip (L10n::tr ("Dove questa destinazione invia l'audio. ASIO usa il dispositivo ASIO del motore; "
                                                  "Windows render usa un normale endpoint di riproduzione (es. un virtual cable); "
                                                  "l'endpoint virtuale NL alimenta un NL Mic pubblicato.",
                                                  "Where this destination sends audio. ASIO uses the engine ASIO device; "
                                                  "Windows render uses a normal playback endpoint (e.g. a virtual cable); "
                                                  "NL virtual endpoint feeds a published NL Mic."));
            addAndMakeVisible (destinationType);
            for (int n = 1; n <= NLMATRIX_MAX_ENDPOINTS; ++n)
                destinationDriverEndpoint.addItem ("NL virtual output " + juce::String (n), n);
            destinationDriverEndpoint.setSelectedId (1);
            destinationDriverEndpoint.setTooltip (L10n::tr ("Coppia di endpoint virtuali usata da questa destinazione. "
                                                            "La coppia 1 è NL Mic 1; la coppia N è NL Mic N.",
                                                            "Independent virtual endpoint pair used by this destination. "
                                                            "Pair 1 is NL Mic 1; pair N is NL Mic N."));
            addAndMakeVisible (destinationDriverEndpoint);
            destinationName.setTextToShowWhenEmpty ("destination name", Palette::muted);
            destinationChannels.setTextToShowWhenEmpty ("ASIO channels, e.g. 1,2", Palette::muted);
            destinationChannels.setTooltip (L10n::tr ("Numeri di canale di uscita ASIO (1-based), separati da virgola. "
                                                      "Ignorati dalle destinazioni non ASIO.",
                                                      "ASIO output channel numbers (1-based), comma separated. "
                                                      "Ignored by non-ASIO destinations."));
            addAndMakeVisible (destinationName);
            destinationPickChannels.setButtonText ("Scegli canali...");
            destinationPickChannels.setTooltip ("Elenca i canali reali della destinazione selezionata.");
            destinationPickChannels.onClick = [this] { openChannelPicker (false); };
            addAndMakeVisible (destinationPickChannels);
            addAndMakeVisible (destinationChannels);
            addAndMakeVisible (destinationEndpoint);
            addAndMakeVisible (addDestinationButton);
            addAndMakeVisible (updateDestinationButton);
            addAndMakeVisible (removeDestinationButton);
            addDestinationButton.setButtonText ("Add destination");
            updateDestinationButton.setButtonText ("Update");
            removeDestinationButton.setButtonText ("Remove");

            addSection (busTitle, "Internal buses (no Windows device)");
            configureSectionArrow (busSectionArrow);
            busSectionArrow.onClick = [this] { setSectionCollapsed (2, ! busesCollapsed); };
            busList.setRowHeight (22);
            busList.setModel (&busModel);
            addAndMakeVisible (busList);
            busName.setTextToShowWhenEmpty ("bus name (e.g. NL Mic Marco)", Palette::muted);
            addAndMakeVisible (busName);
            for (int n = 1; n <= 16; ++n) busChannels.addItem (juce::String (n) + (n == 1 ? " canale" : " canali"), n);
            busChannels.setSelectedId (2, juce::dontSendNotification);
            busChannels.setTooltip ("Numero di canali del bus interno.");
            addAndMakeVisible (busChannels);
            addAndMakeVisible (addBusButton);
            addAndMakeVisible (updateBusButton);
            addAndMakeVisible (removeBusButton);
            addBusButton.setButtonText ("Add bus");
            updateBusButton.setButtonText ("Update");
            removeBusButton.setButtonText ("Remove");
            virtualDevicesButton.setButtonText ("Virtual devices...");
            virtualDevicesButton.setTooltip (L10n::tr ("Mostra il pool pubblicato NL Return/NL Mic, lo stato online, "
                                                       "le assegnazioni e i nomi Windows.",
                                                       "Show the published NL Return/NL Mic pool, online state, "
                                                       "assignments and Windows names."));
            virtualDevicesButton.onClick = [this] { showVirtualDevices(); };
            addAndMakeVisible (virtualDevicesButton);

            auto configureHelp = [this] (juce::TextButton& button, const juce::String& title,
                                         const juce::String& body)
            {
                button.setButtonText ("?");
                button.setTooltip (title);
                button.onClick = [title, body]
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::InfoIcon, title, body, "OK");
                };
            };
            configureHelp (sourceHelpButton,
                           L10n::tr ("Cos'è una sorgente?", "What is a source?"),
                           L10n::tr ("Le sorgenti sono gli ingressi audio della matrice.\n\n"
                                     "Ingresso ASIO: canali del dispositivo ASIO selezionato.\n"
                                     "WASAPI loopback: registra ciò che Windows riproduce su una uscita.\n"
                                     "WASAPI capture: registra un normale microfono/dispositivo di ingresso.\n"
                                     "Cattura processo: registra una singola applicazione (nome o PID).\n"
                                     "Bus interno: punto di mix che resta dentro NL Audio Matrix e non crea un dispositivo Windows.\n"
                                     "Endpoint virtuale NL: usa una delle coppie pubblicate NL Return/NL Mic.",
                                     "Sources are audio inputs of the matrix.\n\n"
                                     "ASIO input: channels from the selected ASIO device.\n"
                                     "WASAPI loopback: records what Windows plays on one output.\n"
                                     "WASAPI capture: records a normal microphone/input device.\n"
                                     "Process capture: records one application (name or PID).\n"
                                     "Internal bus: a mix point that stays inside NL Audio Matrix and does not create a Windows device.\n"
                                     "NL virtual endpoint: uses one of the published NL Return/NL Mic pairs."));
            configureHelp (destinationHelpButton,
                           L10n::tr ("Cos'è una destinazione?", "What is a destination?"),
                           L10n::tr ("Le destinazioni sono le uscite audio della matrice.\n\n"
                                     "Uscita ASIO: canali del dispositivo ASIO selezionato.\n"
                                     "Endpoint render Windows: un normale dispositivo di riproduzione Windows (anche virtual cable).\n"
                                     "Endpoint virtuale NL: alimenta una coppia NL Mic pubblicata, così un'altra applicazione può registrarla.\n\n"
                                     "Un cavo del patchbay è la rotta reale: sorgente -> destinazione. Il solo nome di una sorgente o di un bus non crea un dispositivo Windows.",
                                     "Destinations are audio outputs of the matrix.\n\n"
                                     "ASIO output: channels of the selected ASIO device.\n"
                                     "Windows render endpoint: a normal Windows playback device (including virtual cables).\n"
                                     "NL virtual endpoint: feeds one published NL Mic pair so another application can record it.\n\n"
                                     "A patchbay cable is the actual route: source -> destination. A source name or bus name alone does not create a Windows device."));
            configureHelp (busHelpButton,
                           L10n::tr ("Bus interni e dispositivi virtuali", "Internal buses vs virtual devices"),
                           L10n::tr ("Un bus interno è solo un punto di mix dentro NL Audio Matrix: non crea un dispositivo audio Windows e non consuma endpoint.\n\n"
                                     "Gli endpoint virtuali sono un pool separato e fisso pubblicato dal driver (NL Return 1..4 / NL Mic 1..4). "
                                     "Usa il tipo sorgente/destinazione \"Endpoint virtuale NL\" per collegare un nodo a una coppia.\n\n"
                                     "Questa distinzione evita di creare due dispositivi Windows per ogni sorgente o bus interno.",
                                     "An internal bus is only a mix point inside NL Audio Matrix: it does not create a Windows audio device and costs no endpoint.\n\n"
                                     "Virtual endpoints are a separate, fixed pool published by the driver (NL Return 1..4 / NL Mic 1..4). "
                                     "Use the source/destination type \"NL virtual endpoint\" to bind one node to a pair.\n\n"
                                     "This distinction avoids creating two Windows devices for every internal source or bus."));
            configureHelp (asioHelpButton,
                           L10n::tr ("Dispositivo motore ASIO", "ASIO engine device"),
                           L10n::tr ("Il dispositivo ASIO usato dal router per tutti i canali ASIO di ingresso/uscita e come clock audio. "
                                     "Scegli FlexASIO su questa VM di test, o la tua interfaccia reale sul PC principale. "
                                     "Non è un'impostazione per canale: i canali si scelgono in ogni sorgente/destinazione.",
                                     "The ASIO device used by the router for all ASIO input/output channels and as the audio clock. "
                                     "Choose FlexASIO on this test VM, or your real interface on the main PC. "
                                     "It is not a per-channel setting; channels are selected in each source/destination."));
            addAndMakeVisible (sourceHelpButton);
            addAndMakeVisible (destinationHelpButton);
            addAndMakeVisible (busHelpButton);
            addAndMakeVisible (asioHelpButton);

            addAndMakeVisible (patchbay);
            patchbay.onConnect = [this] (const juce::String& sourceId,
                                        const juce::String& destinationId)
            {
                connectFromPatchbay (sourceId, destinationId);
            };
            patchbay.onNodeMoved = [this] (const juce::String& kind, const juce::String& id,
                                           float x, float y, bool finished)
            {
                moveNodeInModel (kind, id, x, y);
                if (finished)
                {
                    juce::String error;
                    saveLiveProfile (error);
                }
            };
            patchbay.onDeleteRoute = [this] (int index) { deleteRouteAt (index); };
            patchbay.onEditRouteGain = [this] (int index)
            {
                if (index < 0 || index >= (int) routes.size()) return;
                auto* aw = new juce::AlertWindow (L10n::tr ("Guadagno connessione", "Connection gain"),
                                                  L10n::tr ("Guadagno in dB (0 = unità)", "Gain in dB (0 = unity)"),
                                                  juce::MessageBoxIconType::NoIcon, this);
                aw->addTextEditor ("gain", juce::String (routes[(size_t) index].gainDb, 2),
                                   L10n::tr ("Guadagno dB", "Gain dB"));
                aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                aw->addButton ("Annulla", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                juce::Component::SafePointer<MainComponent> safe (this);
                aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, index] (int result)
                {
                    if (result == 1 && safe != nullptr && index < (int) safe->routes.size())
                    {
                        safe->routes[(size_t) index].gainDb = aw->getTextEditorContents ("gain").getDoubleValue();
                        safe->refreshAll();
                        safe->applyProfile();
                    }
                    delete aw;
                }), true);
            };
            patchbay.onRouteChanged = [this] (int index, float gain, bool enabled)
            {
                changeRouteAt (index, gain, enabled);
            };
            patchbay.onReconnect = [this] (const juce::String& source,
                                           const juce::String& oldDest,
                                           const juce::String& newDest)
            {
                reconnectRoute (source, oldDest, newDest);
            };
            patchbay.onRemoveNode = [this] (const juce::String& kind, const juce::String& id)
            {
                removeNode (kind, id);
            };
            patchbay.onEditNode = [this] (const juce::String& kind, const juce::String& id)
            {
                editNodeByReference (kind, id);
            };
            patchbay.onNodeMuted = [this] (const juce::String& kind, const juce::String& id, bool muted)
            {
                if (kind == "source") for (auto& s : sources) if (s.id == id) s.muted = muted;
                if (kind == "destination") for (auto& d : destinations) if (d.id == id) d.muted = muted;
                if (kind == "bus") for (auto& b : buses) if (b.id == id) b.muted = muted;
                refreshAll();
                applyProfile();
                setStatus (muted ? L10n::tr ("Nodo mutato", "Node muted")
                                 : L10n::tr ("Nodo riattivato", "Node unmuted"));
            };
            patchbay.onNodeEnabled = [this] (const juce::String& kind,
                                             const juce::String& id, bool enabled)
            {
                setNodeEnabled (kind, id, enabled);
            };
            patchbay.onRenameNode = [this] (const juce::String& kind, const juce::String& id,
                                            const juce::String& name)
            {
                renameNode (kind, id, name);
            };
            patchbay.onAddNode = [this] (const juce::String& kind) { addNodeFromCanvas (kind); };
            // G3: clicking a canvas node shows and scrolls to its editable details
            // in the left panel; clicking a cable selects its Connections row.
            patchbay.onSelectionChanged = [this] (const juce::String& kind, const juce::String& id)
            {
                selectNodeFromCanvas (kind, id);
            };
            patchbay.onCableSelected = [this] (int index)
            {
                if (index >= 0 && index < routeModel.rows.size() - 1)
                    routeList.selectRow (index);
            };

            addSection (routeTitle, "Routes");
            routeList.setRowHeight (28);
            routeList.setModel (&routeModel);
            addAndMakeVisible (routeList);
            addAndMakeVisible (routeSource);
            addAndMakeVisible (routeDestination);
            routeGain.setTextToShowWhenEmpty ("gain dB", Palette::muted);
            routeGain.setText ("0");
            addAndMakeVisible (routeGain);
            for (auto* label : { &routeSourceLabel, &routeDestLabel, &routeGainLabel })
            {
                label->setColour (juce::Label::textColourId, Palette::text);
                label->setFont (juce::FontOptions (13.0f));
                addAndMakeVisible (*label);
            }
            routeSourceLabel.setText ("Route source", juce::dontSendNotification);
            routeDestLabel.setText ("Route destination", juce::dontSendNotification);
            routeGainLabel.setText ("Gain dB", juce::dontSendNotification);
            addAndMakeVisible (routeEnabled);
            addAndMakeVisible (addRouteButton);
            addAndMakeVisible (updateRouteButton);
            addAndMakeVisible (removeRouteButton);
            routeEnabled.setButtonText ("Enabled");
            routeEnabled.setToggleState (true, juce::dontSendNotification);
            addRouteButton.setButtonText ("Add route");
            updateRouteButton.setButtonText ("Update selected");
            removeRouteButton.setButtonText ("Remove selected");

            addSection (metersTitle, "Meters");
            addAndMakeVisible (metersPanel);

            loadPreferences();
            refreshDevices();
            refreshAll();
            registerLocalisedTexts();
            updateDisclosureStates();
            updateSectionVisibility();

            startStop.onClick = [this] { toggleRouter(); };
            applyButton.onClick = [this] { applyProfile(); };
            saveButton.onClick = [this] { saveProfile(); };
            loadButton.onClick = [this] { loadProfile(); };
            refreshButton.onClick = [this] { refreshDevices(); refreshAll(); };
            addSourceButton.onClick = [this] { addSource(); };
            removeSourceButton.onClick = [this] { removeSelectedSource(); };
            addDestinationButton.onClick = [this] { addDestination(); };
            removeDestinationButton.onClick = [this] { removeSelectedDestination(); };
            addBusButton.onClick = [this] { addBus(); };
            removeBusButton.onClick = [this] { removeSelectedBus(); };
            addRouteButton.onClick = [this] { addRoute(); };
            updateRouteButton.onClick = [this] { updateSelectedRoute(); };
            removeRouteButton.onClick = [this] { removeSelectedRoute(); };
            updateSourceButton.onClick = [this] { updateSelectedSource(); };
            updateDestinationButton.onClick = [this] { updateSelectedDestination(); };
            updateBusButton.onClick = [this] { updateSelectedBus(); };

            sourceModel.onSelected = [this] (int row)
            {
                if (row >= 0 && row < (int) sources.size())
                {
                    fillSourceFields (row);
                    patchbay.selectNode ("source", sources[(size_t) row].id);
                }
            };
            sourceModel.onDoubleClicked = [this] (int row)
            {
                if (row >= 0 && row < (int) sources.size())
                {
                    fillSourceFields (row);
                    sourceName.grabKeyboardFocus();
                    sourceName.selectAll();
                }
            };
            destinationModel.onSelected = [this] (int row)
            {
                if (row >= 0 && row < (int) destinations.size())
                {
                    fillDestinationFields (row);
                    patchbay.selectNode ("destination", destinations[(size_t) row].id);
                }
            };
            destinationModel.onDoubleClicked = [this] (int row)
            {
                if (row >= 0 && row < (int) destinations.size())
                {
                    fillDestinationFields (row);
                    destinationName.grabKeyboardFocus();
                    destinationName.selectAll();
                }
            };
            busModel.onSelected = [this] (int row)
            {
                if (row >= 0 && row < (int) buses.size())
                {
                    patchbay.selectNode ("bus", buses[(size_t) row].id);
                    busName.setText (buses[(size_t) row].name, juce::dontSendNotification);
                }
                if (row >= 0 && row < (int) buses.size())
                busGain.setText (juce::String (buses[(size_t) row].gainDb, 1), juce::dontSendNotification);
                busChannels.setSelectedId (juce::jlimit (1, 16, buses[(size_t) row].channels), juce::dontSendNotification);
            };
            routeModel.onSelected = [this] (int row)
            {
                if (row < 0 || row >= (int) routes.size())
                    return;
                patchbay.selectCable (row);
                const auto& r = routes[(size_t) row];
                routeGain.setText (juce::String (r.gainDb, 1), juce::dontSendNotification);
                routeEnabled.setToggleState (r.enabled, juce::dontSendNotification);
                for (int i = 0; i < (int) sources.size(); ++i)
                    if (sources[(size_t) i].id == r.sourceId)
                        routeSource.setSelectedItemIndex (i, juce::dontSendNotification);
                for (int i = 0; i < (int) destinations.size(); ++i)
                    if (destinations[(size_t) i].id == r.destinationId)
                        routeDestination.setSelectedItemIndex (i, juce::dontSendNotification);
                for (int i = 0; i < (int) buses.size(); ++i)
                    if (buses[(size_t) i].id == r.destinationId)
                        routeDestination.setSelectedItemIndex ((int) destinations.size() + i,
                                                               juce::dontSendNotification);
            };
            sourceType.onChange = [this] { updateSourceFields(); if (sourceChannels.getText().isEmpty()) sourceChannels.setText ("1,2"); };
            destinationType.onChange = [this] { updateDestinationFields(); if (destinationChannels.getText().isEmpty()) destinationChannels.setText ("1,2"); };
            sourceEndpoint.onChange = [this] { setStatus (L10n::tr ("Sorgente Windows: ", "Windows source: ")
                                                                  + sourceEndpoint.getText()); };
            destinationEndpoint.onChange = [this] { setStatus (L10n::tr ("Destinazione Windows: ", "Windows destination: ")
                                                                       + destinationEndpoint.getText()); };
            updateSourceFields();
            updateDestinationFields();

            for (juce::Component* c : { (juce::Component*) &sourceTitle,
                                        (juce::Component*) &sourceSectionArrow,
                                        (juce::Component*) &sourceList,
                                        (juce::Component*) &sourceType,
                                        (juce::Component*) &sourceDevice,
                                        (juce::Component*) &sourceName,
                                        (juce::Component*) &sourceGain,
                                        (juce::Component*) &sourceEndpoint,
                                        (juce::Component*) &sourceChannels,
                                        (juce::Component*) &sourcePickChannels,
                                        (juce::Component*) &sourceProcess,
                                        (juce::Component*) &sourcePickProcess,
                                        (juce::Component*) &sourceProcessTree,
                                        (juce::Component*) &sourceBus,
                                        (juce::Component*) &sourceDriverEndpoint,
                                        // These three used to be missing from this list: they
                                        // stayed children of MainComponent and painted at the
                                        // inner-panel coordinates as window-space slivers on the
                                        // left edge.  Reparenting them is the fix for the
                                        // long-standing "left panel clipped at x<0" defect.
                                        (juce::Component*) &sourceEnabled,
                                        (juce::Component*) &sourcePan,
                                        (juce::Component*) &sourcePolarity,
                                        (juce::Component*) &addSourceButton,
                                        (juce::Component*) &updateSourceButton,
                                        (juce::Component*) &removeSourceButton,
                                        (juce::Component*) &sourceHelpButton,
                                        (juce::Component*) &destinationTitle,
                                        (juce::Component*) &destinationSectionArrow,
                                        (juce::Component*) &destinationList,
                                        (juce::Component*) &destinationType,
                                        (juce::Component*) &destinationDevice,
                                        (juce::Component*) &destinationMonitor,
                                        (juce::Component*) &destinationMixMinus,
                                        (juce::Component*) &destinationName,
                                        (juce::Component*) &destinationGain,
                                        (juce::Component*) &destinationEndpoint,
                                        (juce::Component*) &destinationChannels,
                                        (juce::Component*) &destinationPickChannels,
                                        (juce::Component*) &destinationDriverEndpoint,
                                        (juce::Component*) &destinationDriverTarget,
                                        (juce::Component*) &addDestinationButton,
                                        (juce::Component*) &updateDestinationButton,
                                        (juce::Component*) &removeDestinationButton,
                                        (juce::Component*) &destinationHelpButton,
                                        (juce::Component*) &busTitle,
                                        (juce::Component*) &busSectionArrow,
                                        (juce::Component*) &busList,
                                        (juce::Component*) &busName,
                                        (juce::Component*) &busGain,
                                        (juce::Component*) &busChannels,
                                        (juce::Component*) &addBusButton,
                                        (juce::Component*) &updateBusButton,
                                        (juce::Component*) &removeBusButton,
                                        (juce::Component*) &busHelpButton,
                                        (juce::Component*) &virtualDevicesButton })
                leftContent.addAndMakeVisible (c);
            leftViewport.setViewedComponent (&leftContent, false);
            leftViewport.setScrollBarsShown (true, false);
            addAndMakeVisible (leftViewport);

            loadThemePreference();
            setThemeMode (themeMode);
            startTimer (150);
        }

        ~MainComponent() override
        {
            stopTimer();
            stopRouter();
            if (&juce::LookAndFeel::getDefaultLookAndFeel() == &lookAndFeel)
                juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
            setLookAndFeel (nullptr);
        }

        bool loadProfileFile (const juce::File& file)
        {
            juce::String error;
            logLine ("load profile " + file.getFullPathName()
                     + " exists=" + juce::String (file.existsAsFile() ? 1 : 0));
            const bool ok = readProfile (file, error);
            if (ok)
            {
                currentProfileFile = file;
                logLine ("profile loaded: " + juce::String (sources.size()) + " sources, "
                         + juce::String (destinations.size()) + " destinations");
            }
            else
            {
                logLine ("profile error: " + error);
                setStatus (error);
            }
            return ok;
        }

        bool saveLiveProfile (juce::String& error)
        {
            if (! writeProfile (profileTempFile(), error))
                return false;
            if (currentProfileFile != juce::File())
                return writeProfile (currentProfileFile, error);
            return true;
        }

        void startEngine() { startRouter(); }
        void stopEngine() { stopRouter(); }
        bool isEngineRunning() const { return router != nullptr && router->isRunning(); }

        void runRealInputTest()
        {
            auto append = [] (const juce::String& text)
            {
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("nlmatrix_clicktest.log")
                    .appendText (text + "\n");
            };
            auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("nlmatrix_clicktest.log");
            file.deleteFile();
            append ("NL Audio Matrix real-input click test");
            startEngine();

            struct State { int before = 0; int afterDelete = 0; bool selected = false; };
            auto state = std::make_shared<State>();
            state->before = patchbay.routeCount();

            juce::Timer::callAfterDelay (1500, [this, state, append]
            {
                if (auto* top = getTopLevelComponent())
                {
                    top->toFront (true);
#if JUCE_WINDOWS
                    if (auto* hwnd = top->getWindowHandle())
                        SetForegroundWindow ((HWND) hwnd);
#endif
                }
                patchbay.grabKeyboardFocus();
                const auto mid = patchbay.cableScreenMidpoint (0);
                append ("click at " + juce::String (mid.x, 1) + "," + juce::String (mid.y, 1)
                        + " routes=" + juce::String (state->before));
                SetCursorPos ((int) mid.x, (int) mid.y);
                mouse_event (MOUSEEVENTF_MOVE, 0, 0, 0, 0);

                juce::Timer::callAfterDelay (250, [this, state, append, mid]
                {
                append (juce::String (patchbay.hoveredCableIndex() >= 0 ? "PASS " : "FAIL ")
                        + "real mouse hover highlights cable (hover="
                        + juce::String (patchbay.hoveredCableIndex()) + ")");
                mouse_event (MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                mouse_event (MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

                juce::Timer::callAfterDelay (450, [this, state, append]
                {
                    state->selected = patchbay.selectedCableIndex() == 0;
                    append (juce::String (state->selected ? "PASS " : "FAIL ")
                            + "real mouse click selects cable (selected="
                            + juce::String (patchbay.selectedCableIndex()) + ")");
                    keybd_event (VK_DELETE, 0, 0, 0);
                    keybd_event (VK_DELETE, 0, KEYEVENTF_KEYUP, 0);

                    juce::Timer::callAfterDelay (450, [this, state, append]
                    {
                        state->afterDelete = patchbay.routeCount();
                        append (juce::String (state->afterDelete < state->before ? "PASS " : "FAIL ")
                                + "real Delete removes cable (routes "
                                + juce::String (state->before) + " -> "
                                + juce::String (state->afterDelete) + ")");
                        append (juce::String (isEngineRunning() ? "PASS " : "FAIL ")
                                + "router alive after live disconnection");

                        // Real drag from a bus output to a destination input.
                        if (! buses.empty() && ! destinations.empty())
                        {
                            const auto out = patchbay.nodeScreenCentre ("bus", buses.front().id, false);
                            const auto in = patchbay.nodeScreenCentre ("destination", destinations.front().id, true);
                            const int beforeBus = patchbay.routeCount();
                            append ("bus drag " + juce::String (out.x, 1) + ","
                                    + juce::String (out.y, 1) + " -> "
                                    + juce::String (in.x, 1) + ","
                                    + juce::String (in.y, 1));
                            SetCursorPos ((int) out.x, (int) out.y);
                            mouse_event (MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                            juce::Timer::callAfterDelay (180, [this, state, append, out, in, beforeBus]
                            {
                                SetCursorPos ((int) in.x, (int) in.y);
                                mouse_event (MOUSEEVENTF_MOVE, 0, 0, 0, 0);
                                juce::Timer::callAfterDelay (180, [this, state, append, beforeBus]
                                {
                                    mouse_event (MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                                    juce::Timer::callAfterDelay (450, [this, state, append, beforeBus]
                                    {
                                        append (juce::String (patchbay.routeCount() > beforeBus ? "PASS " : "FAIL ")
                                                + "real bus-output drag creates route (routes "
                                                + juce::String (patchbay.routeCount()) + ")");
                                        const int beforeListDelete = patchbay.routeCount();
                                        const auto rowPoint = routeList.localPointToGlobal (juce::Point<int> (12, 8));
                                        const auto buttonPoint = removeRouteButton.localPointToGlobal (
                                            removeRouteButton.getLocalBounds().getCentre());
                                        SetCursorPos ((int) rowPoint.x, (int) rowPoint.y);
                                        mouse_event (MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                                        mouse_event (MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                                        juce::Timer::callAfterDelay (300, [this, append, beforeListDelete, buttonPoint]
                                        {
                                            SetCursorPos ((int) buttonPoint.x, (int) buttonPoint.y);
                                            mouse_event (MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                                            mouse_event (MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                                            juce::Timer::callAfterDelay (400, [this, append, beforeListDelete]
                                            {
                                                append (juce::String (patchbay.routeCount() < beforeListDelete
                                                                          ? "PASS " : "FAIL ")
                                                        + "real Connections list Disconnect button (routes "
                                                        + juce::String (beforeListDelete) + " -> "
                                                        + juce::String (patchbay.routeCount()) + ")");
                                                if (patchbay.routeCount() > 0)
                                                {
                                                    const int beforeContext = patchbay.routeCount();
                                                    const auto mid = patchbay.cableScreenMidpoint (0);
                                                    SetCursorPos ((int) mid.x, (int) mid.y);
                                                    mouse_event (MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
                                                    mouse_event (MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
                                                    juce::Timer::callAfterDelay (450, [this, append, beforeContext]
                                                    {
                                                        for (int n = 0; n < 6; ++n)
                                                        {
                                                            keybd_event (VK_DOWN, 0, 0, 0);
                                                            keybd_event (VK_DOWN, 0, KEYEVENTF_KEYUP, 0);
                                                        }
                                                        keybd_event (VK_RETURN, 0, 0, 0);
                                                        keybd_event (VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
                                                        juce::Timer::callAfterDelay (450, [this, append, beforeContext]
                                                        {
                                                            append (juce::String (patchbay.routeCount() < beforeContext
                                                                                      ? "PASS " : "FAIL ")
                                                                    + "real right-click cable menu Disconnect (routes "
                                                                    + juce::String (beforeContext) + " -> "
                                                                    + juce::String (patchbay.routeCount()) + ")");
                                                            saveSnapshot();
                                                            stopEngine();
                                                            append ("END");
                                                            juce::JUCEApplication::getInstance()->systemRequestedQuit();
                                                        });
                                                    });
                                                }
                                                else
                                                {
                                                    saveSnapshot();
                                                    stopEngine();
                                                    append ("END");
                                                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                                                }
                                            });
                                        });
                                    });
                                });
                            });
                        }
                        else
                        {
                            saveSnapshot();
                            stopEngine();
                            append ("SKIP real bus drag (no bus/destination)");
                            append ("END");
                            juce::JUCEApplication::getInstance()->systemRequestedQuit();
                        }
                    });
                });
                });
            });
        }

        void saveSnapshot()
        {
            const auto snapshot = createComponentSnapshot (getLocalBounds());
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("nlmatrix_snapshot.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (snapshot, *stream);
            }
        }

        //==========================================================================
        // GUI verification (--guiverify <dir>): saves IT/EN x Dark/Light/System
        // screenshots of the main window, Preferences and Diagnostics, plus the
        // 1600x900 and 1366x768 layouts used for the C6-lite clipping check.
        void runGuiVerify (const juce::File& outputDirectory)
        {
            outputDirectory.createDirectory();

            // Representative project so the screenshots show a real patchbay.
            // Verify mode is test-only: session persistence is disabled.
            if (sources.empty()) addNodeFromCanvas ("source");
            if (destinations.empty()) addNodeFromCanvas ("destination");
            if (buses.empty()) addNodeFromCanvas ("bus");
            refreshAll();
            if (! sources.empty() && ! destinations.empty())
                connectFromPatchbay (sources.front().id, destinations.front().id);
            if (! sources.empty() && ! buses.empty())
                connectFromPatchbay (sources.front().id, buses.front().id);
            profileName.setText ("Focusrite - NL Mic 1 (demo)", juce::dontSendNotification);
            profileName.setCaretPosition (0);
            refreshAll();

            struct VerifyJob
            {
                L10n::Language language = L10n::Language::italian;
                ThemeMode theme = ThemeMode::dark;
                int width = 1900, height = 1020;
                bool panels = true;
                juce::String sizeTag;
            };

            std::vector<VerifyJob> jobs;
            for (auto language : { L10n::Language::italian, L10n::Language::english })
                for (auto theme : { ThemeMode::dark, ThemeMode::system, ThemeMode::light })
                    jobs.push_back ({ language, theme, 1900, 1020, true, "1920x1080" });
            jobs.push_back ({ L10n::Language::italian, ThemeMode::dark, 1600, 900, false, "1600x900" });
            jobs.push_back ({ L10n::Language::italian, ThemeMode::dark, 1366, 768, false, "1366x768" });

            auto pending = std::make_shared<std::vector<VerifyJob>> (std::move (jobs));
            auto index = std::make_shared<int> (0);
            auto step = std::make_shared<std::function<void()>>();
            auto layoutLines = std::make_shared<juce::StringArray>();
            juce::Component::SafePointer<MainComponent> safe (this);
            const auto directory = outputDirectory;

            *step = [safe, pending, index, step, directory, layoutLines]
            {
                if (safe == nullptr)
                    return;
                if (*index >= (int) pending->size())
                {
                    // Geometry evidence for the C6-lite check.
                    juce::StringArray report;
                    report.add ("NL Audio Matrix - GUI verification layout report");
                    report.add ("window size | theme_language | result");
                    report.addArray (*layoutLines);
                    const auto failures = [&layoutLines]
                    {
                        int failed = 0;
                        for (const auto& line : *layoutLines)
                            if (line.contains ("FAIL")) ++failed;
                        return failed;
                    }();
                    report.add ("SUMMARY " + juce::String ((int) layoutLines->size() - failures)
                                + "/" + juce::String ((int) layoutLines->size())
                                + " layout checks passed");
                    directory.getChildFile ("gui_verify_layout.log").replaceWithText (
                        report.joinIntoString ("\n") + "\n");

                    safe->setLanguageMode (L10n::Language::italian);
                    safe->setThemeMode (ThemeMode::dark);
                    safe->setStatus (L10n::tr ("Verifica GUI completata", "GUI verification complete"));
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                    return;
                }
                const auto job = (*pending)[(*index)++];
                safe->setLanguageMode (job.language);
                safe->setThemeMode (job.theme);
                if (auto* top = safe->getTopLevelComponent())
                    top->setBounds (10, 10, job.width, job.height);

                juce::Timer::callAfterDelay (320, [safe, job, directory, step, layoutLines]
                {
                    if (safe == nullptr) return;
                    const juce::String prefix = juce::String (job.theme == ThemeMode::dark ? "dark"
                                                                 : job.theme == ThemeMode::light ? "light" : "system")
                                                + "_" + (job.language == L10n::Language::english ? "en" : "it")
                                                + "_" + job.sizeTag;
                    const auto violations = safe->layoutViolations();
                    layoutLines->add (juce::String (job.width) + "x" + juce::String (job.height)
                                      + " | " + prefix + " | "
                                      + (violations.isEmpty() ? "PASS"
                                                              : "FAIL: " + violations.joinIntoString ("; ")));
                    if (auto* top = safe->getTopLevelComponent())
                        writePngImage (top->createComponentSnapshot (top->getLocalBounds(), false, 1.0f),
                                       directory.getChildFile (prefix + "_main.png"));
                    if (job.panels)
                    {
                        // Use the real windows (not bare components) so the shot
                        // includes the window background and the app look&feel.
                        auto* prefsWindow = new PreferencesWindow (
                            new PreferencesComponent (48000.0, 256, EngineUiSettings::preset ("balanced"),
                                                     {}, {}, {}));
                        prefsWindow->setBounds (60, 60, 560, 700);
                        writePngImage (prefsWindow->createComponentSnapshot (prefsWindow->getLocalBounds(),
                                                                             false, 1.0f),
                                       directory.getChildFile (prefix + "_preferences.png"));
                        delete prefsWindow;

                        auto* diagContent = new DiagnosticsComponent();
                        diagContent->getReport = []
                        {
                            return L10n::tr (
                                "NL Audio Matrix - diagnostica\n"
                                "Motore ASIO: Focusrite USB ASIO\n"
                                "Sample rate motore: 48000.0 Hz\n"
                                "Buffer motore: 256 campioni\n"
                                "Callbacks: 12345\n\n"
                                "== BUFFERING / SALUTE MOTORE ==\n"
                                "Modalità buffer: Balanced (balanced)\n"
                                "Stato motore: running   sano: sì   riavvii ASIO: 0\n"
                                "Età callback: 12.4 ms   gap massimo callback (1 s): 18.7 ms\n"
                                "Tuning attivo: ring 60-200 ms   driver 50-250 ms   chunk 5 ms   servo 2 ms\n",
                                "NL Audio Matrix - diagnostics\n"
                                "ASIO engine: Focusrite USB ASIO\n"
                                "Engine rate: 48000.0 Hz\n"
                                "Engine buffer: 256 samples\n"
                                "Callbacks: 12345\n\n"
                                "== BUFFERING / ENGINE HEALTH ==\n"
                                "Buffer mode: Balanced (balanced)\n"
                                "Engine state: running   healthy: yes   ASIO restarts: 0\n"
                                "Callback age: 12.4 ms   max callback gap (1 s): 18.7 ms\n"
                                "Tuning active: ring 60-200 ms   driver 50-250 ms   chunk 5 ms   servo 2 ms\n");
                        };
                        diagContent->refreshNow();
                        auto* diagWindow = new DiagnosticsWindow (diagContent);
                        diagWindow->setBounds (60, 60, 760, 520);
                        diagContent->refreshNow();
                        writePngImage (diagWindow->createComponentSnapshot (diagWindow->getLocalBounds(),
                                                                           false, 1.0f),
                                       directory.getChildFile (prefix + "_diagnostics.png"));
                        delete diagWindow;
                    }
                    (*step)();
                });
            };
            (*step)();
        }

        void runSelfTest()
        {
            // Whole-run snapshot of the real preferences: the self-test must be
            // non-destructive even though individual checks exercise the real
            // save/load paths (it previously could leave the file with test values).
            const bool selftestHadPrefs = preferencesFile().existsAsFile();
            const auto selftestPrefsSnapshot = selftestHadPrefs
                                                   ? preferencesFile().loadFileAsString() : juce::String();
            const auto selftestEngineSettings = engineSettings;
            const double selftestPreferredRate = preferredSampleRate;
            const int selftestPreferredBuffer = preferredBuffer;
            const auto selftestDeviceFormats = deviceFormats;
            juce::StringArray results;
            auto check = [&results] (bool condition, const juce::String& label)
            {
                results.add ((condition ? "PASS " : "FAIL ") + label);
            };

            const auto previousSourceType = sourceType.getSelectedId();
            sourceType.setSelectedId (1, juce::dontSendNotification);
            updateSourceFields();
            const auto discoveredChannels = discoverChannels (true);
            check (! discoveredChannels.isEmpty(), "ASIO input channel discovery");
            check (! enumerateRunningApps (false).empty(), "running application discovery");
            sourceName.clear();
            check (autoNodeName (true).isNotEmpty(), "automatic node name");
            const auto savedAsio = asioBox.getText();
            asioBox.setText ("ReaRoute ASIO (x64)", juce::dontSendNotification);
            check (discoverChannels (true).size() >= 16, "ReaRoute 16-channel discovery");
            check (channelsToText (std::vector<int> { 0, 1 }) == "1,2",
                   "ASIO channel UI displays 1-based channels");
            check (parseChannels ("1,2") == std::vector<int> { 0, 1 },
                   "ASIO channel UI maps 1-based channels to 0-based model");
            asioBox.setText (savedAsio, juce::dontSendNotification);
            sourceType.setSelectedId (previousSourceType, juce::dontSendNotification);
            const auto previousDestType = destinationType.getSelectedId();
            destinationType.setSelectedId (1, juce::dontSendNotification);
            updateDestinationFields();
            check (! discoverChannels (false).isEmpty(), "ASIO output channel discovery");
            destinationType.setSelectedId (previousDestType, juce::dontSendNotification);
            updateDestinationFields();
            updateSourceFields();

            // Deterministic canvas fixture.  --selftest runs synchronously at
            // startup, before the deferred last-session restore, and test mode
            // disables session persistence: the in-memory model can therefore
            // be legitimately empty here.  The checks below verify how the GUI
            // reacts to real nodes, not what the operator had saved, so build
            // the nodes through the same production actions the UI uses.  A
            // profile passed with --profile is preserved and used as-is.
            if (sources.empty()) addNodeFromCanvas ("source");
            if (destinations.empty()) addNodeFromCanvas ("destination");
            if (buses.empty()) addNodeFromCanvas ("bus");
            refreshAll();
            check (! sources.empty() && ! destinations.empty(),
                   "selftest fixture nodes available");

            // G3: bidirectional selection sync between canvas and left panel.
            if (! sources.empty())
            {
                selectNodeFromCanvas ("source", sources.front().id);
                check (sourceList.getSelectedRow() == 0,
                       "canvas selection shows source details in left panel");
                patchbay.clearSelection();
                sourceList.deselectAllRows();
                sourceList.selectRow (0);
                check (patchbay.selectedNodeCount() > 0,
                       "left list selection highlights the canvas node");
            }

            // C6-lite: the main layout must fit every target window size with no
            // control outside the window and no left-panel control lost outside
            // the scrolling panel (regression guard for the clipped-sliver bug).
            {
                auto* top = getTopLevelComponent();
                const auto savedBounds = top != nullptr ? top->getBounds() : juce::Rectangle<int>();
                auto layoutFitsAt = [this] (int width, int height)
                {
                    if (auto* window = getTopLevelComponent())
                        window->setBounds (window->getX(), window->getY(), width, height);
                    else
                        setSize (width, height);
                    return layoutViolations().isEmpty();
                };
                const bool fits1366 = layoutFitsAt (1366, 768);
                const bool fits1600 = layoutFitsAt (1600, 900);
                const bool fits1920 = layoutFitsAt (1900, 1020);
                if (top != nullptr)
                    top->setBounds (savedBounds);
                else
                    setSize (1600, 1000);
                check (fits1366, "layout fits 1366x768 without clipping");
                check (fits1600, "layout fits 1600x900 without clipping");
                check (fits1920, "layout fits 1920x1080 without clipping");
            }

            const auto originalSources = sources;
            const auto originalDestinations = destinations;
            const auto originalBuses = buses;
            const auto originalRoutes = routes;
            const auto interactionReport = patchbay.runInteractionTest();
            sources = originalSources;
            destinations = originalDestinations;
            buses = originalBuses;
            routes = originalRoutes;
            refreshAll();
            results.add (interactionReport.trimEnd());
            const int s0 = (int) sources.size();
            const int d0 = (int) destinations.size();
            const int b0 = (int) buses.size();
            const int r0 = (int) routes.size();

            addNodeFromCanvas ("source");
            addNodeFromCanvas ("destination");
            addNodeFromCanvas ("bus");
            check ((int) sources.size() == s0 + 1, "canvas add source");
            check ((int) destinations.size() == d0 + 1, "canvas add destination");
            check ((int) buses.size() == b0 + 1, "canvas add bus");

            const auto newSource = sources.back().id;
            const auto newDestination = destinations.back().id;
            connectFromPatchbay (newSource, newDestination);
            check ((int) routes.size() == r0 + 1, "canvas connect creates route");
            bool routeFound = false;
            for (const auto& r : routes)
                if (r.sourceId == newSource && r.destinationId == newDestination)
                    routeFound = true;
            check (routeFound, "canvas route present in model");
            if (! routes.empty())
            {
                const auto oldDestination = routes.back().destinationId;
                const auto otherDestination = destinations.front().id;
                reconnectRoute (newSource, oldDestination, otherDestination);
                bool reconnected = false;
                for (const auto& r : routes)
                    if (r.sourceId == newSource && r.destinationId == otherDestination)
                        reconnected = true;
                check (reconnected, "canvas cable endpoint can be reconnected");
            }

            moveNodeInModel ("source", newSource, 321.0f, 123.0f);
            juce::String positionError;
            saveLiveProfile (positionError);
            const auto parsed = juce::JSON::parse (profileTempFile());
            bool positionSaved = false;
            if (auto* root = parsed.getDynamicObject())
                if (auto* arr = root->getProperty ("sources").getArray())
                    for (const auto& item : *arr)
                        if (auto* o = item.getDynamicObject())
                            if (o->getProperty ("id").toString() == newSource)
                                positionSaved = std::abs ((double) o->getProperty ("x") - 321.0) < 0.5
                                                && std::abs ((double) o->getProperty ("y") - 123.0) < 0.5;
            check (positionSaved, "canvas position persisted");

            juce::String reloadError;
            const bool reloaded = readProfile (profileTempFile(), reloadError);
            bool layoutRestored = false;
            if (reloaded)
                for (const auto& s : sources)
                    if (s.id == newSource)
                        layoutRestored = std::abs (s.x - 321.0f) < 0.5f
                                         && std::abs (s.y - 123.0f) < 0.5f;
            check (layoutRestored, "profile reload restores node layout");

            removeNode ("source", newSource);
            bool removedRoute = true;
            for (const auto& r : routes)
                if (r.sourceId == newSource) removedRoute = false;
            check (removedRoute, "node removal removes its cables");
            check ((int) sources.size() == s0, "node removal updates model");

            //------------------------------------------------------------------
            // Engine v2 buffering persistence.  Every profile writer in the GUI
            // (writeProfile, saveLiveProfile, saveProfile for the file chooser,
            // saveLastSessionProfile and the router config written by
            // startRouter) funnels through writeProfile, so one round-trip
            // covers the whole family.  Advanced values are used so every
            // numeric field differs from the presets: a clamped or dropped
            // field cannot masquerade as a pass.
            const auto savedEngineSettings = engineSettings;
            const auto savedProfileName = profileName.getText();
            const auto savedAsioBoxText = asioBox.getText();
            EngineUiSettings expectedEngine = EngineUiSettings::preset ("advanced");
            expectedEngine.ringTargetMinMs = 77;
            expectedEngine.ringTargetMaxMs = 177;
            expectedEngine.driverTargetMs = 66;
            expectedEngine.driverTargetMaxMs = 166;
            expectedEngine.writeChunkMs = 7;
            expectedEngine.servoIntervalMs = 3;
            engineSettings = expectedEngine;

            juce::String engineError;
            check (writeProfile (profileTempFile(), engineError),
                   "profile written with engine settings");
            auto loadEngineSectionFromDisk = [] (const juce::File& file)
            {
                const auto parsed = juce::JSON::parse (file);
                if (auto* root = parsed.getDynamicObject())
                    return root->getProperty ("engine");
                return juce::var();
            };
            check (EngineUiSettings::fromVar (loadEngineSectionFromDisk (profileTempFile()))
                       == expectedEngine,
                   "profile engine settings persisted");

            engineSettings = EngineUiSettings::preset ("low"); // poison before reload
            juce::String engineReloadError;
            const bool engineProfileReloaded = readProfile (profileTempFile(), engineReloadError);
            check (engineProfileReloaded && engineSettings == expectedEngine,
                   "engine settings round-trip");

            // G4: device formats (sample rate / buffer) must round-trip through the
            // profile devices[] and through preferences.json.
            {
                // Keep the real preferences untouched by this block.
                const auto g4PrefsFile = preferencesFile();
                const bool g4HadPreferences = g4PrefsFile.existsAsFile();
                const auto g4PrefsBackup = g4HadPreferences ? g4PrefsFile.loadFileAsString() : juce::String();

                const double savedRate = preferredSampleRate;
                const int savedBuffer = preferredBuffer;
                const auto savedFormats = deviceFormats;
                preferredSampleRate = 96000.0;
                preferredBuffer = 512;
                juce::String formatError;
                const bool wroteFormat = writeProfile (profileTempFile(), formatError);
                double parsedRate = 0.0;
                int parsedBuffer = 0;
                bool parsedMasterFound = false;
                // NB: keep the parsed var alive: getDynamicObject() returns a raw
                // pointer into it, so parsing into a temporary would dangle.
                const auto parsedProfileVar = juce::JSON::parse (profileTempFile());
                if (auto* root = parsedProfileVar.getDynamicObject())
                    if (auto* devices = root->getProperty ("devices").getArray())
                        for (const auto& item : *devices)
                            if (auto* o = item.getDynamicObject())
                            {
                                const bool master = (bool) o->getProperty ("clockMaster")
                                                    || o->getProperty ("driver").toString()
                                                           == root->getProperty ("asio").toString();
                                if (master)
                                {
                                    parsedMasterFound = true;
                                    parsedRate = (double) o->getProperty ("sampleRate");
                                    parsedBuffer = (int) o->getProperty ("buffer");
                                }
                            }
                const bool reloadedFormat = readProfile (profileTempFile(), formatError);
                const bool formatRoundTrip = wroteFormat && reloadedFormat && parsedMasterFound
                                             && std::abs (parsedRate - 96000.0) < 0.5
                                             && parsedBuffer == 512
                                             && std::abs (preferredSampleRate - 96000.0) < 0.5
                                             && preferredBuffer == 512;
                // Preferences round-trip for an extra device format.
                DeviceFormatChoice extra;
                extra.deviceId = "asio:NL SelfTest Device";
                extra.driver = "NL SelfTest Device";
                extra.sampleRate = 88200.0;
                extra.bufferSize = 128;
                deviceFormats[extra.deviceId] = extra;
                savePreferences();
                deviceFormats.erase (extra.deviceId);
                loadPreferences();
                const auto prefsIt = deviceFormats.find (extra.deviceId);
                const bool prefsFormat = prefsIt != deviceFormats.end()
                                         && std::abs (prefsIt->second.sampleRate - 88200.0) < 0.5
                                         && prefsIt->second.bufferSize == 128;
                deviceFormats = savedFormats;
                preferredSampleRate = savedRate;
                preferredBuffer = savedBuffer;
                readProfile (profileTempFile(), formatError);
                deviceFormats = savedFormats;
                preferredSampleRate = savedRate;
                preferredBuffer = savedBuffer;
                if (g4HadPreferences)
                    g4PrefsFile.replaceWithText (g4PrefsBackup);
                else
                    g4PrefsFile.deleteFile();
                check (formatRoundTrip, "device format persisted in profile and reloaded"
                                            + juce::String (formatRoundTrip
                                                                ? ""
                                                                : " [wrote=" + juce::String ((int) wroteFormat)
                                                                      + " reloaded=" + juce::String ((int) reloadedFormat)
                                                                      + " masterFound=" + juce::String ((int) parsedMasterFound)
                                                                      + " parsed=" + juce::String (parsedRate, 0) + "/" + juce::String (parsedBuffer)
                                                                      + " loaded=" + juce::String (preferredSampleRate, 0) + "/" + juce::String (preferredBuffer)
                                                                      + " asio='" + asioBox.getText() + "']"));
                check (prefsFormat, "device formats persisted in preferences.json");
            }

            // preferences.json must keep the same object between runs.
            const auto prefsFile = preferencesFile();
            const bool hadPreferences = prefsFile.existsAsFile();
            const auto prefsBackup = hadPreferences ? prefsFile.loadFileAsString() : juce::String();
            engineSettings = expectedEngine;
            savePreferences();
            check (EngineUiSettings::fromVar (loadEngineSectionFromDisk (prefsFile))
                       == expectedEngine,
                   "engine settings persisted in preferences.json");
            // G2: collapsible panel state must persist in preferences.json and
            // the collapsed layout must still fit the window.
            {
                const bool wasCollapsed = leftPanelCollapsed;
                const bool wasSources = sourcesCollapsed;
                setLeftPanelCollapsed (true);
                setSectionCollapsed (0, true);
                const bool collapseFits = layoutViolations().isEmpty();
                // Prove the production write path reaches disk, then prove the
                // production loader restores it (round-trip, not a JSON probe).
                savePreferences();
                const auto written = preferencesFile().loadFileAsString();
                const bool fileHasState = written.contains ("\"leftPanelCollapsed\": true")
                                          && written.contains ("\"sourcesCollapsed\": true");
                leftPanelCollapsed = false;
                sourcesCollapsed = false;
                loadPreferences();
                const bool loadedBack = leftPanelCollapsed && sourcesCollapsed;
                leftPanelCollapsed = wasCollapsed;
                sourcesCollapsed = wasSources;
                updateSectionVisibility();
                updateDisclosureStates();
                resized();
                const bool uiPersisted = fileHasState && loadedBack;
                check (uiPersisted, "collapse state persisted in preferences.json"
                                        + juce::String (uiPersisted
                                                            ? ""
                                                            : " [fileHasState=" + juce::String ((int) fileHasState)
                                                                  + " loadedBack=" + juce::String ((int) loadedBack)
                                                                  + " tail=" + written.substring (juce::jmax (0, written.length() - 160)) + "]"));
                check (collapseFits, "collapsed left panel keeps layout valid");
                check (layoutViolations().isEmpty(), "expanded left panel restores layout");
            }

            if (hadPreferences)
            {
                prefsFile.replaceWithText (prefsBackup);
            }
            else
            {
                // Fresh machine: keep the real (non-test) settings instead of
                // leaving no preferences.json at all, so the acceptance harness
                // and the next GUI start see the engine object and language.
                engineSettings = savedEngineSettings;
                savePreferences();
            }
            engineSettings = savedEngineSettings;

            // The acceptance harness verifies %APPDATA%/NL Audio Matrix/
            // last_session_profile.json after --selftest.  Test mode normally
            // keeps session persistence disabled so automation never rewrites
            // operator data, but this check deliberately exercises the real
            // last-session path once, with deterministic settings, so the
            // evidence file always carries the engine object.
            {
                const bool persistenceWasEnabled = sessionPersistenceEnabled;
                sessionPersistenceEnabled = true;
                engineSettings = expectedEngine;
                saveLastSessionProfile();
                sessionPersistenceEnabled = persistenceWasEnabled;
                const auto lastSessionFile = lastSessionProfileFile();
                check (EngineUiSettings::fromVar (loadEngineSectionFromDisk (lastSessionFile))
                           == expectedEngine,
                       "last-session profile engine settings persisted");
                engineSettings = savedEngineSettings;
            }

            // Backward compatibility: a profile saved before engine v2 has no
            // "engine" object and must load without resetting the current
            // choice (missing preferences/defaults fall back to Balanced).
            {
                const auto legacyFile = profileTempFile().getSiblingFile (
                    "nlmatrix_selftest_legacy_profile.json");
                legacyFile.replaceWithText (
                    "{\"version\":3,\"name\":\"legacy\",\"asio\":\"\","
                    "\"sources\":[],\"destinations\":[],\"buses\":[],\"routes\":[]}");
                juce::String legacyError;
                const bool legacyLoaded = readProfile (legacyFile, legacyError);
                asioBox.setText (savedAsioBoxText, juce::dontSendNotification);
                profileName.setText (savedProfileName, juce::dontSendNotification);
                check (legacyLoaded && engineSettings == savedEngineSettings,
                       "legacy profile without engine stays compatible");
                check (EngineUiSettings::fromVar (juce::var()) == EngineUiSettings::preset ("balanced"),
                       "missing engine settings default to Balanced");
                legacyFile.deleteFile();
            }

            sources = originalSources;
            destinations = originalDestinations;
            buses = originalBuses;
            routes = originalRoutes;

            // The engine/meter section needs a live cable so the router has an
            // active graph; meter nodes are reported for every configured node.
            if (routes.empty() && ! sources.empty() && ! destinations.empty())
                connectFromPatchbay (sources.front().id, destinations.front().id);
            // Muting the only active cable intentionally pauses the engine ("no
            // active route; devices released"), so keep a second active route to
            // the internal bus: the behaviour under test is that a live gain/mute
            // edit does not restart the router.  An internal-bus route stays
            // active without requiring another Windows/ASIO destination.
            if ((int) routes.size() < 2 && ! sources.empty() && ! buses.empty())
                connectFromPatchbay (sources.front().id, buses.front().id);
            refreshAll();

            startEngine();
            juce::Thread::sleep (1800);
            check (isEngineRunning(), "router starts from GUI");
            if (! routes.empty())
            {
                changeRouteAt (0, -12.0f, false);
                juce::Thread::sleep (300);
                check (std::abs (routes[0].gainDb + 12.0) < 0.01 && ! routes[0].enabled,
                       "cable gain/mute persisted in model");
                check (isEngineRunning(), "router survives live gain/mute edit without restart");
                changeRouteAt (0, 0.0f, true);
                juce::Thread::sleep (600);
            }
            const auto telemetry = telemetryFile.loadFileAsString();
            check (telemetry.contains ("METERS "), "router telemetry present");
            bool hasNode = telemetry.contains ("\"nodes\"");
            check (hasNode, "router telemetry has meter nodes");
            check (telemetry.contains ("\"devices\""), "router telemetry device formats present");
            // The telemetry file is appended by the router process: walk back
            // from the newest METERS line and stop at the first one that
            // actually yields nodes, so a line caught mid-write (torn read)
            // cannot make the parser look broken.
            juce::StringArray meterLines;
            meterLines.addLines (telemetry);
            for (int i = meterLines.size(); --i >= 0 && meterStates.empty();)
                if (meterLines[i].startsWith ("METERS "))
                    applyMeterJson (meterLines[i].substring (7));
            refreshMeters();
            check (! meterStates.empty(), "GUI parses actual router meter values");
            check (! routerDevices.empty(), "GUI parses router device format telemetry");
            check (! lastFormatReport.isEmpty(), "requested-vs-actual device format report available");
            if (! meterStates.empty())
            {
                bool finiteLevels = true;
                for (const auto& [id, state] : meterStates)
                {
                    juce::ignoreUnused (id);
                    if (! std::isfinite (state.l) || ! std::isfinite (state.r))
                        finiteLevels = false;
                }
                check (finiteLevels, "router meter values are finite numbers");
            }
            if (profileName.getText().containsIgnoreCase ("Loop meter"))
            {
                bool signal = false;
                for (const auto& [id, state] : meterStates)
                {
                    juce::ignoreUnused (id);
                    if (state.l > -60.0f || state.r > -60.0f)
                        signal = true;
                }
                check (signal, "GUI meters show real tone level");
            }
            //==================================================================
            // G4 integration: perform a REAL device-format change through the
            // production apply path while the actual dev router is running:
            // read current format from METERS devices[] -> apply 44100 via the
            // graceful-stop path -> assert the restarted router reports 44100 ->
            // restore 48000 -> assert restoration and no crash.
            {
                double beforeRate = 0.0;
                int beforeBuffer = 0;
                const bool readBefore = readLatestRouterTelemetry();
                for (const auto& d : routerDevices)
                    if (d.clockMaster)
                    {
                        beforeRate = d.rate;
                        beforeBuffer = d.buffer;
                    }
                const double targetRate = std::abs (beforeRate - 44100.0) < 0.5 ? 48000.0 : 44100.0;
                juce::String applyReport;
                const bool applied = applyDeviceFormatChange (targetRate, 256, {}, {},
                                                              engineSettings, {}, applyReport);
                juce::Thread::sleep (1800);
                bool targetSeen = false;
                double targetActualRate = 0.0;
                int targetActualBuffer = 0;
                readLatestRouterTelemetry();
                for (const auto& d : routerDevices)
                    if (d.clockMaster)
                    {
                        targetActualRate = d.rate;
                        targetActualBuffer = d.buffer;
                        targetSeen = std::abs (d.rate - targetRate) < 0.5;
                    }
                const bool changeOk = readBefore && beforeRate > 0.0 && applied
                                      && isEngineRunning() && targetSeen;
                check (changeOk, "real format change applies via graceful stop"
                                     + juce::String (changeOk
                                                         ? ""
                                                         : " [before=" + juce::String (beforeRate, 0)
                                                               + "/" + juce::String (beforeBuffer)
                                                               + " target=" + juce::String (targetRate, 0)
                                                               + " actual=" + juce::String (targetActualRate, 0)
                                                               + "/" + juce::String (targetActualBuffer)
                                                               + " applied=" + juce::String ((int) applied)
                                                               + " running=" + juce::String ((int) isEngineRunning())
                                                               + " report='" + applyReport + "']"));

                juce::String restoreReport;
                const bool restoreCalled = applyDeviceFormatChange (beforeRate > 0.0 ? beforeRate : 48000.0,
                                                                    256, {}, {}, engineSettings, {},
                                                                    restoreReport);
                juce::Thread::sleep (1800);
                bool restoredSeen = false;
                readLatestRouterTelemetry();
                for (const auto& d : routerDevices)
                    if (d.clockMaster && std::abs (d.rate - (beforeRate > 0.0 ? beforeRate : 48000.0)) < 0.5)
                        restoredSeen = true;
                check (restoreCalled && restoredSeen && isEngineRunning(),
                       "format change restored" + juce::String (restoredSeen && restoreCalled
                                                                    ? ""
                                                                    : " [report='" + restoreReport + "']"));
                check (! routerControlFile.existsAsFile(), "format change used graceful control channel");
            }

            {
                const auto snapshot = createComponentSnapshot (getLocalBounds());
                const auto imageFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("nlmatrix_selftest_ui.png");
                imageFile.deleteFile();
                if (auto stream = imageFile.createOutputStream())
                {
                    juce::PNGImageFormat png;
                    png.writeImageToStream (snapshot, *stream);
                }
                check (imageFile.existsAsFile() && imageFile.getSize() > 1000,
                       "GUI rendered snapshot captured");
            }
            stopEngine();
            check (! isEngineRunning(), "router stops from GUI");
            check (! routerControlFile.existsAsFile(), "graceful stop control channel cleaned up");

            // Restore the operator's real preferences (or leave a clean default
            // file on a fresh machine, as the acceptance harness expects).
            if (selftestHadPrefs)
            {
                preferencesFile().replaceWithText (selftestPrefsSnapshot);
            }
            else
            {
                engineSettings = selftestEngineSettings;
                preferredSampleRate = selftestPreferredRate;
                preferredBuffer = selftestPreferredBuffer;
                deviceFormats = selftestDeviceFormats;
                savePreferences();
            }
            engineSettings = selftestEngineSettings;
            preferredSampleRate = selftestPreferredRate;
            preferredBuffer = selftestPreferredBuffer;
            deviceFormats = selftestDeviceFormats;

            results.insert (0, "NL Audio Matrix GUI self-test");
            const auto body = results.joinIntoString ("\n");
            juce::StringArray bodyLines;
            bodyLines.addLines (body);
            int passedChecks = 0, failedChecks = 0;
            for (const auto& line : bodyLines)
            {
                if (line.startsWith ("PASS ")) ++passedChecks;
                else if (line.startsWith ("FAIL ")) ++failedChecks;
            }
            const auto text = body + "\nSUMMARY " + juce::String (passedChecks) + "/"
                              + juce::String (passedChecks + failedChecks) + " checks passed\n";
            const auto log = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("nlmatrix_selftest.log");
            log.replaceWithText (text);
            DBG (text);
        }

        void paint (juce::Graphics& g) override
        {
            juce::ColourGradient gradient (Palette::background, 0.0f, 0.0f,
                                          Palette::panelRaised, 0.0f,
                                          (float) getHeight(), false);
            g.setGradientFill (gradient);
            g.fillAll();
            g.setColour (Palette::magenta.withAlpha (0.08f));
            g.fillRect (0, 0, getWidth(), 2);
        }

        //----------------------------------------------------------------------
        // G2: collapsible left panel and collapsible source/destination/bus
        // sections.  State is persisted in preferences.json (backward compatible)
        // and restored at startup; the whole panel collapses to a slim rail so
        // the canvas can use the width.
        void configureSectionArrow (DisclosureButton& arrow)
        {
            arrow.setTooltip (L10n::tr ("Comprimi / espandi la sezione",
                                        "Collapse / expand the section"));
        }

        void updateDisclosureStates()
        {
            leftPanelArrow.setDirection (leftPanelCollapsed ? DisclosureButton::Direction::right
                                                             : DisclosureButton::Direction::left);
            sourceSectionArrow.setDirection (sourcesCollapsed ? DisclosureButton::Direction::right
                                                              : DisclosureButton::Direction::down);
            destinationSectionArrow.setDirection (destinationsCollapsed
                                                      ? DisclosureButton::Direction::right
                                                      : DisclosureButton::Direction::down);
            busSectionArrow.setDirection (busesCollapsed ? DisclosureButton::Direction::right
                                                         : DisclosureButton::Direction::down);
            leftPanelArrow.setTooltip (L10n::tr (leftPanelCollapsed ? "Espandi il pannello sinistro"
                                                                    : "Comprimi il pannello sinistro",
                                                  leftPanelCollapsed ? "Expand the left panel"
                                                                     : "Collapse the left panel"));
        }

        void setLeftPanelCollapsed (bool collapsed)
        {
            if (leftPanelCollapsed == collapsed)
                return;
            leftPanelCollapsed = collapsed;
            savePreferences();
            updateDisclosureStates();
            resized();
            repaint();
        }

        void setSectionCollapsed (int section, bool collapsed)
        {
            if (section == 0) sourcesCollapsed = collapsed;
            else if (section == 1) destinationsCollapsed = collapsed;
            else busesCollapsed = collapsed;
            updateSectionVisibility();
            savePreferences();
            updateDisclosureStates();
            resized();
        }

        void updateSectionVisibility()
        {
            const bool sourceOpen = ! sourcesCollapsed;
            for (juce::Component* c : { (juce::Component*) &sourceList,
                                        (juce::Component*) &sourceEnabled,
                                        (juce::Component*) &sourceType,
                                        (juce::Component*) &sourceName,
                                        (juce::Component*) &sourceGain,
                                        (juce::Component*) &sourcePan,
                                        (juce::Component*) &sourcePolarity,
                                        (juce::Component*) &sourceDevice,
                                        (juce::Component*) &sourceEndpoint,
                                        (juce::Component*) &sourceChannels,
                                        (juce::Component*) &sourcePickChannels,
                                        (juce::Component*) &sourceProcess,
                                        (juce::Component*) &sourcePickProcess,
                                        (juce::Component*) &sourceProcessTree,
                                        (juce::Component*) &sourceBus,
                                        (juce::Component*) &sourceDriverEndpoint,
                                        (juce::Component*) &addSourceButton,
                                        (juce::Component*) &updateSourceButton,
                                        (juce::Component*) &removeSourceButton })
                c->setVisible (sourceOpen);
            if (sourceOpen)
                updateSourceFields();

            const bool destinationOpen = ! destinationsCollapsed;
            for (juce::Component* c : { (juce::Component*) &destinationList,
                                        (juce::Component*) &destinationType,
                                        (juce::Component*) &destinationDevice,
                                        (juce::Component*) &destinationMonitor,
                                        (juce::Component*) &destinationMixMinus,
                                        (juce::Component*) &destinationName,
                                        (juce::Component*) &destinationGain,
                                        (juce::Component*) &destinationEndpoint,
                                        (juce::Component*) &destinationChannels,
                                        (juce::Component*) &destinationPickChannels,
                                        (juce::Component*) &destinationDriverEndpoint,
                                        (juce::Component*) &destinationDriverTarget,
                                        (juce::Component*) &addDestinationButton,
                                        (juce::Component*) &updateDestinationButton,
                                        (juce::Component*) &removeDestinationButton })
                c->setVisible (destinationOpen);
            if (destinationOpen)
                updateDestinationFields();

            const bool busOpen = ! busesCollapsed;
            for (juce::Component* c : { (juce::Component*) &busList,
                                        (juce::Component*) &busName,
                                        (juce::Component*) &busGain,
                                        (juce::Component*) &busChannels,
                                        (juce::Component*) &addBusButton,
                                        (juce::Component*) &updateBusButton,
                                        (juce::Component*) &removeBusButton,
                                        (juce::Component*) &virtualDevicesButton })
                c->setVisible (busOpen);
        }

        // G3: canvas -> left panel selection.  Expands a collapsed panel or
        // section, selects the matching list row, fills the editable fields and
        // scrolls the left viewport so the details are visible.
        void selectNodeFromCanvas (const juce::String& kind, const juce::String& id)
        {
            if (kind.isEmpty() || id.isEmpty())
                return;
            if (leftPanelCollapsed)
                setLeftPanelCollapsed (false);
            if (kind == "source")
            {
                if (sourcesCollapsed)
                    setSectionCollapsed (0, false);
                for (int i = 0; i < (int) sources.size(); ++i)
                    if (sources[(size_t) i].id == id)
                    {
                        sourceList.selectRow (i);
                        fillSourceFields (i);
                        scrollLeftPanelToSection (0);
                        return;
                    }
            }
            else if (kind == "destination")
            {
                if (destinationsCollapsed)
                    setSectionCollapsed (1, false);
                for (int i = 0; i < (int) destinations.size(); ++i)
                    if (destinations[(size_t) i].id == id)
                    {
                        destinationList.selectRow (i);
                        fillDestinationFields (i);
                        scrollLeftPanelToSection (1);
                        return;
                    }
            }
            else if (kind == "bus")
            {
                if (busesCollapsed)
                    setSectionCollapsed (2, false);
                for (int i = 0; i < (int) buses.size(); ++i)
                    if (buses[(size_t) i].id == id)
                    {
                        busList.selectRow (i);
                        busName.setText (buses[(size_t) i].name, juce::dontSendNotification);
                        busGain.setText (juce::String (buses[(size_t) i].gainDb, 1),
                                         juce::dontSendNotification);
                        busChannels.setSelectedId (juce::jlimit (1, 16, buses[(size_t) i].channels),
                                                   juce::dontSendNotification);
                        scrollLeftPanelToSection (2);
                        return;
                    }
            }
        }

        void scrollLeftPanelToSection (int section)
        {
            if (leftPanelCollapsed)
                return;
            int y = 0;
            if (section == 1)
                y = (sourcesCollapsed ? 32 : 530) + 4;
            else if (section == 2)
                y = (sourcesCollapsed ? 32 : 530) + 4
                    + (destinationsCollapsed ? 32 : 540) + 4;
            leftViewport.setViewPosition (0, y);
        }

        //======================================================================
        // G4: ASIO device formats (sample rate / buffer size).
        DeviceFormatChoice formatForDriver (const juce::String& driver) const
        {
            const auto key = "asio:" + driver;
            const auto it = deviceFormats.find (key);
            if (it != deviceFormats.end())
                return it->second;
            DeviceFormatChoice choice;
            choice.deviceId = key;
            choice.driver = driver;
            choice.sampleRate = driver == asioBox.getText() ? preferredSampleRate : 48000.0;
            choice.bufferSize = driver == asioBox.getText() ? preferredBuffer : 0;
            return choice;
        }

        void setFormatForDriver (const juce::String& driver, double rate, int buffer)
        {
            DeviceFormatChoice choice;
            choice.deviceId = "asio:" + driver;
            choice.driver = driver;
            choice.sampleRate = rate;
            choice.bufferSize = buffer;
            deviceFormats[choice.deviceId] = choice;
            if (driver == asioBox.getText())
            {
                preferredSampleRate = rate;
                preferredBuffer = buffer > 0 ? buffer : preferredBuffer;
            }
        }

        // Formats shown in Preferences: the master device first, then every extra
        // ASIO device referenced by a source or destination.
        std::vector<DeviceFormatChoice> collectDeviceFormatsForPreferences() const
        {
            std::vector<DeviceFormatChoice> result;
            auto master = formatForDriver (asioBox.getText());
            master.role = L10n::tr ("Motore principale", "Main engine");
            result.push_back (master);

            std::map<juce::String, juce::String> used;
            for (const auto& s : sources)
                if (s.deviceId.startsWith ("asio:"))
                    used[s.deviceId] += (used[s.deviceId].isEmpty() ? "" : ", ") + s.name;
            for (const auto& d : destinations)
                if (d.deviceId.startsWith ("asio:"))
                    used[d.deviceId] += (used[d.deviceId].isEmpty() ? "" : ", ") + d.name;
            for (const auto& [deviceId, nodes] : used)
            {
                auto driver = deviceId.fromFirstOccurrenceOf ("asio:", false, false);
                if (driver == asioBox.getText())
                    continue;
                auto choice = formatForDriver (driver);
                choice.role = L10n::tr ("usato da: ", "used by: ") + nodes;
                result.push_back (choice);
            }
            return result;
        }

        // Query the real capabilities of one ASIO driver.  Only safe while the
        // router is stopped: an ASIO driver can normally be opened by one process
        // at a time.  Returns current rate/buffer on success.
        bool probeAsioFormat (const juce::String& driver,
                              std::vector<double>& availableRates,
                              std::vector<int>& availableBuffers,
                              double& currentRate, int& currentBuffer)
        {
            availableRates.clear();
            availableBuffers.clear();
            if (driver.isEmpty() || isEngineRunning())
                return false;
            auto type = std::unique_ptr<juce::AudioIODeviceType> (
                juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
            if (type == nullptr)
                return false;
            type->scanForDevices();
            std::unique_ptr<juce::AudioIODevice> device (type->createDevice (driver, driver));
            if (device == nullptr)
                return false;
            const auto ins = device->getInputChannelNames();
            const auto outs = device->getOutputChannelNames();
            juce::BigInteger inMask, outMask;
            inMask.setRange (0, ins.size(), true);
            outMask.setRange (0, outs.size(), true);
            const auto error = device->open (inMask, outMask, preferredSampleRate, 0);
            if (error.isNotEmpty())
                return false;
            currentRate = device->getCurrentSampleRate();
            currentBuffer = device->getCurrentBufferSizeSamples();
            for (auto r : device->getAvailableSampleRates()) availableRates.push_back (r);
            for (auto b : device->getAvailableBufferSizes()) availableBuffers.push_back (b);
            device->close();
            return ! availableRates.empty();
        }

        //======================================================================
        // E1: graceful router stop through the --control file.  The router fades
        // its destinations out (20 ms), closes devices and exits 0.  Falls back to
        // a hard kill only if the child does not exit in time.
        void stopRouterGracefully()
        {
            if (router == nullptr)
                return;
            if (router->isRunning())
            {
                routerControlFile.replaceWithText ("stop\n");
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 4000.0;
                while (router->isRunning()
                       && juce::Time::getMillisecondCounterHiRes() < deadline)
                    juce::Thread::sleep (20);
            }
            joinRouterReader();
            if (router->isRunning())
                router->kill();
            router->waitForProcessToFinish (500);
            router.reset();
            routerControlFile.deleteFile();
        }

        //======================================================================
        // G4: human readable requested-vs-actual format report for status bar and
        // Diagnostics. We never claim the requested value: only the actual one.
        juce::String buildFormatReport() const
        {
            juce::StringArray lines;
            for (const auto& d : routerDevices)
            {
                const bool mismatch = d.requestedRate > 0.0
                                      && (std::abs (d.requestedRate - d.rate) > 0.5
                                          || (d.requestedBuffer > 0 && d.buffer != d.requestedBuffer));
                lines.add (d.driver
                           + (d.clockMaster ? L10n::tr (" (master)", " (master)") : juce::String())
                           + " — " + L10n::tr ("richiesto ", "requested ")
                           + (d.requestedRate > 0.0 ? juce::String (d.requestedRate, 0) : juce::String ("-"))
                           + " Hz / "
                           + (d.requestedBuffer > 0 ? juce::String (d.requestedBuffer) : juce::String ("-"))
                           + " — " + L10n::tr ("attivo ", "active ")
                           + juce::String (d.rate, 0) + " Hz / " + juce::String (d.buffer)
                           + (mismatch ? L10n::tr ("  [arrotondato dal driver]", "  [rounded by the driver]")
                                       : juce::String()));
            }
            return lines.joinIntoString ("\n");
        }

        //======================================================================
        // C6/C9 geometry regression support.  layoutViolations() reports any
        // visible child that leaves the window (negative offset or overflow) or
        // any left-panel editor that is not parented to the scrolling panel
        // content (the old "clipped sliver at x<0" defect).
        static juce::String describeComponent (juce::Component& c)
        {
            if (dynamic_cast<juce::TextEditor*> (&c) != nullptr) return "TextEditor";
            if (dynamic_cast<juce::ComboBox*> (&c) != nullptr) return "ComboBox";
            if (dynamic_cast<juce::ListBox*> (&c) != nullptr) return "ListBox";
            if (dynamic_cast<juce::Viewport*> (&c) != nullptr) return "Viewport";
            if (dynamic_cast<juce::TextButton*> (&c) != nullptr) return "TextButton";
            if (dynamic_cast<juce::ToggleButton*> (&c) != nullptr) return "ToggleButton";
            if (dynamic_cast<juce::Label*> (&c) != nullptr) return "Label";
            return "Component";
        }

        juce::StringArray layoutViolations() const
        {
            juce::StringArray problems;
            const auto window = getLocalBounds();
            // Direct children are the panels/controls the window layout owns.
            // (Scrollable content is intentionally taller/wider than the window
            // and is checked separately below.)
            for (auto* child : getChildren())
            {
                if (child->isVisible())
                {
                    const auto area = child->getBounds();
                    if (area.getX() < 0 || area.getY() < 0
                        || area.getRight() > window.getRight()
                        || area.getBottom() > window.getBottom())
                        problems.add (describeComponent (*child) + " outside window " + area.toString());
                }
            }
            if (! leftPanelCollapsed && leftContent.getWidth() > leftViewport.getWidth())
                problems.add ("left panel content wider than its viewport");
            // Every visible left-panel control (including the action buttons,
            // which are placed directly under their list) must be fully inside
            // the scroll content - no sliced rows at the viewport fold.
            if (! leftPanelCollapsed)
                for (auto* child : leftContent.getChildren())
                    if (child->isVisible()
                        && (child->getX() < 0 || child->getY() < 0
                            || child->getRight() > leftContent.getWidth()
                            || child->getBottom() > leftContent.getHeight()))
                        problems.add (describeComponent (*child) + " outside left panel content");
            if (! leftPanelArrow.isVisible() || leftPanelArrow.getWidth() <= 0)
                problems.add ("left panel disclosure arrow missing");

            for (juce::Component* c : { (juce::Component*) &sourceEnabled, (juce::Component*) &sourcePan,
                                        (juce::Component*) &sourcePolarity, (juce::Component*) &sourceType,
                                        (juce::Component*) &sourceName, (juce::Component*) &sourceGain,
                                        (juce::Component*) &sourceDevice, (juce::Component*) &sourceChannels,
                                        (juce::Component*) &sourcePickChannels,
                                        (juce::Component*) &destinationType, (juce::Component*) &destinationName,
                                        (juce::Component*) &destinationGain, (juce::Component*) &destinationDevice,
                                        (juce::Component*) &destinationMonitor,
                                        (juce::Component*) &destinationMixMinus,
                                        (juce::Component*) &destinationChannels,
                                        (juce::Component*) &destinationPickChannels,
                                        (juce::Component*) &busName, (juce::Component*) &busGain,
                                        (juce::Component*) &busChannels,
                                        (juce::Component*) &addSourceButton,
                                        (juce::Component*) &addDestinationButton,
                                        (juce::Component*) &addBusButton })
                if (c->isVisible() && ! leftContent.isParentOf (c))
                    problems.add (describeComponent (*c) + " not inside the left panel content");

            // Mixer dock: when visible it must be tall enough for a full strip
            // (name + fader + dB + solo/mute).
            if (mixerVisible && mixerPanel.getHeight() > 0 && mixerPanel.getHeight() < 360)
                problems.add ("mixer dock too short: " + juce::String (mixerPanel.getHeight()) + " px");

            return problems;
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (10);
            auto top = area.removeFromTop (92);
            auto topRow = top.removeFromTop (36);
            const bool compact = area.getWidth() < 1200;
            profileName.setBounds (topRow.removeFromLeft (compact ? 128 : 220));
            topRow.removeFromLeft (compact ? 4 : 6);
            asioLabel.setBounds (topRow.removeFromLeft (compact ? 140 : 175));
            topRow.removeFromLeft (4);
            asioBox.setBounds (topRow.removeFromLeft (compact ? 120 : 220));
            topRow.removeFromLeft (4);
            asioHelpButton.setBounds (topRow.removeFromLeft (28));
            topRow.removeFromLeft (compact ? 4 : 6);
            startStop.setBounds (topRow.removeFromLeft (compact ? 58 : 78));
            topRow.removeFromLeft (4);
            applyButton.setBounds (topRow.removeFromLeft (compact ? 80 : 118));
            topRow.removeFromLeft (4);
            saveButton.setBounds (topRow.removeFromLeft (compact ? 78 : 128));
            topRow.removeFromLeft (4);
            loadButton.setBounds (topRow.removeFromLeft (compact ? 78 : 128));
            topRow.removeFromLeft (4);
            refreshButton.setBounds (topRow.removeFromLeft (compact ? 84 : 130));
            topRow.removeFromLeft (4);
            // G2: obvious collapse/expand arrow for the whole left panel.
            leftPanelArrow.setBounds (topRow.removeFromLeft (compact ? 26 : 28));
            top.removeFromTop (4);
            statusLabel.setBounds (top.removeFromTop (32));

            if (mixerVisible)
            {
                // A mixer strip needs ~324 px for name+fader+dB+buttons; keep the
                // dock tall enough that nothing is cut off at 1366x768 and above,
                // while still leaving the routing area usable on very small windows.
                const int minDock = 380;
                const int dockHeight = juce::jlimit (160, juce::jmax (160, area.getHeight() - 120),
                                                     juce::jmax (minDock, mixerHeight));
                auto mixerArea = area.removeFromBottom (dockHeight);
                mixerResizer.setBounds (mixerArea.removeFromTop (8));
                mixerPanel.setBounds (mixerArea);
                mixerPanel.toFront (false);
                mixerResizer.toFront (false);
            }

            const int rightWidth = juce::jlimit (250, 340, area.getWidth() / 4);
            auto right = area.removeFromRight (rightWidth);
            auto meterArea = right.removeFromTop (juce::jlimit (200, 330, right.getHeight() / 2));
            metersTitle.setBounds (meterArea.removeFromTop (26));
            metersPanel.setBounds (meterArea.reduced (2));
            right.removeFromTop (6);
            routeTitle.setText (L10n::tr ("Connessioni"), juce::dontSendNotification);
            routeTitle.setVisible (true);
            routeTitle.setBounds (right.removeFromTop (26));
            auto disconnectArea = right.removeFromBottom (38);
            removeRouteButton.setBounds (disconnectArea.removeFromLeft (170));
            routeList.setVisible (true);
            routeList.setBounds (right);

            const int expandedLeftWidth = juce::jlimit (310, 450, area.getWidth() * 37 / 100);
            const int leftWidth = leftPanelCollapsed ? 26 : expandedLeftWidth;
            auto leftArea = area.removeFromLeft (leftWidth);
            leftViewport.setVisible (! leftPanelCollapsed);
            if (leftPanelCollapsed)
            {
                // Slim rail: the top-bar arrow is the obvious way back.
                leftPanelArrow.toFront (false);
            }
            else
            {
                leftViewport.setBounds (leftArea);
                const int sourceHeight = sourcesCollapsed ? 32 : 530;
                const int destinationHeight = destinationsCollapsed ? 32 : 540;
                const int busHeight = busesCollapsed ? 32 : 215;
                // Row heights grew with the +1 pt legibility pass; the content is
                // sized to the visible sections so closed sections collapse and the
                // viewport scrolls instead of clipping.
                leftContent.setSize (juce::jmax (250, leftArea.getWidth() - 16),
                                     sourceHeight + destinationHeight + busHeight + 24);
                auto left = leftContent.getLocalBounds().reduced (2, 0);
                auto sourceArea = left.removeFromTop (sourceHeight);
                auto sourceTitleRow = sourceArea.removeFromTop (sourcesCollapsed ? 30 : 28);
                sourceSectionArrow.setBounds (sourceTitleRow.removeFromLeft (28));
                sourceHelpButton.setBounds (sourceTitleRow.removeFromRight (26));
                sourceTitle.setBounds (sourceTitleRow);
                if (! sourcesCollapsed)
                {
                    sourceList.setBounds (sourceArea.removeFromTop (112));
                    sourceArea.removeFromTop (4);
                    // Actions live directly under their list: with the larger
                    // fonts the old position at the section bottom sat exactly on
                    // the viewport fold and looked cut off.
                    {
                        auto sourceButtons = sourceArea.removeFromTop (30);
                        addSourceButton.setBounds (sourceButtons.removeFromLeft (juce::jmin (124, sourceButtons.getWidth())));
                        sourceButtons.removeFromLeft (4);
                        updateSourceButton.setBounds (sourceButtons.removeFromLeft (juce::jmin (92, sourceButtons.getWidth())));
                        sourceButtons.removeFromLeft (4);
                        removeSourceButton.setBounds (sourceButtons.removeFromLeft (juce::jmin (92, sourceButtons.getWidth())));
                    }
                    sourceArea.removeFromTop (4);

                    auto sourceFields = sourceArea.removeFromTop (352);
                    auto placeSource = [&sourceFields] (juce::Component& c)
                    {
                        if (! c.isVisible())
                            return;
                        c.setBounds (sourceFields.removeFromTop (34));
                        sourceFields.removeFromTop (3);
                    };
                    placeSource (sourceEnabled);
                    placeSource (sourceType);
                    placeSource (sourceName);
                    placeSource (sourceGain);
                    {
                        auto panRow = sourceFields.removeFromTop (34);
                        sourcePan.setBounds (panRow.removeFromLeft (124));
                        panRow.removeFromLeft (6);
                        sourcePolarity.setBounds (panRow.removeFromLeft (200));
                        sourceFields.removeFromTop (3);
                    }
                    placeSource (sourceDevice);
                    placeSource (sourceEndpoint);
                    if (sourceChannels.isVisible())
                    {
                        auto channelRow = sourceFields.removeFromTop (34);
                        sourceChannels.setBounds (channelRow.removeFromLeft (juce::jmax (80, channelRow.getWidth() - 124)));
                        channelRow.removeFromLeft (4);
                        sourcePickChannels.setVisible (true);
                        sourcePickChannels.setBounds (channelRow);
                        sourceFields.removeFromTop (3);
                    }
                    if (sourceProcess.isVisible())
                    {
                        auto processRow = sourceFields.removeFromTop (34);
                        sourceProcess.setBounds (processRow.removeFromLeft (juce::jmax (100, processRow.getWidth() - 112)));
                        processRow.removeFromLeft (4);
                        sourcePickProcess.setBounds (processRow);
                        sourceFields.removeFromTop (3);
                        sourceProcessTree.setBounds (sourceFields.removeFromTop (26));
                        sourceFields.removeFromTop (3);
                    }
                    placeSource (sourceBus);
                    placeSource (sourceDriverEndpoint);
                }

                left.removeFromTop (4);
                auto destArea = left.removeFromTop (destinationHeight);
                auto destTitleRow = destArea.removeFromTop (destinationsCollapsed ? 30 : 28);
                destinationSectionArrow.setBounds (destTitleRow.removeFromLeft (28));
                destinationHelpButton.setBounds (destTitleRow.removeFromRight (26));
                destinationTitle.setBounds (destTitleRow);
                if (! destinationsCollapsed)
                {
                    destinationList.setBounds (destArea.removeFromTop (94));
                    destArea.removeFromTop (4);
                    {
                        auto destButtons = destArea.removeFromTop (30);
                        addDestinationButton.setBounds (destButtons.removeFromLeft (juce::jmin (136, destButtons.getWidth())));
                        destButtons.removeFromLeft (4);
                        updateDestinationButton.setBounds (destButtons.removeFromLeft (juce::jmin (92, destButtons.getWidth())));
                        destButtons.removeFromLeft (4);
                        removeDestinationButton.setBounds (destButtons.removeFromLeft (juce::jmin (92, destButtons.getWidth())));
                    }
                    destArea.removeFromTop (4);
                    auto destFields = destArea.removeFromTop (356);
                    auto placeDest = [&destFields] (juce::Component& c)
                    {
                        if (! c.isVisible())
                            return;
                        c.setBounds (destFields.removeFromTop (34));
                        destFields.removeFromTop (3);
                    };
                    placeDest (destinationType);
                    placeDest (destinationName);
                    placeDest (destinationGain);
                    placeDest (destinationDevice);
                    placeDest (destinationMonitor);
                    placeDest (destinationMixMinus);
                    placeDest (destinationEndpoint);
                    if (destinationChannels.isVisible())
                    {
                        auto channelRow = destFields.removeFromTop (34);
                        destinationChannels.setBounds (channelRow.removeFromLeft (juce::jmax (80, channelRow.getWidth() - 124)));
                        channelRow.removeFromLeft (4);
                        destinationPickChannels.setVisible (true);
                        destinationPickChannels.setBounds (channelRow);
                        destFields.removeFromTop (3);
                    }
                    placeDest (destinationDriverEndpoint);
                    placeDest (destinationDriverTarget);
                }

                left.removeFromTop (4);
                auto busArea = left.removeFromTop (busHeight);
                auto busTitleRow = busArea.removeFromTop (busesCollapsed ? 30 : 28);
                busSectionArrow.setBounds (busTitleRow.removeFromLeft (28));
                busHelpButton.setBounds (busTitleRow.removeFromRight (26));
                busTitle.setBounds (busTitleRow);
                if (! busesCollapsed)
                {
                    busList.setBounds (busArea.removeFromTop (58));
                    busArea.removeFromTop (4);
                    {
                        auto busButtons = busArea.removeFromTop (30);
                        addBusButton.setBounds (busButtons.removeFromLeft (juce::jmin (96, busButtons.getWidth())));
                        busButtons.removeFromLeft (4);
                        updateBusButton.setBounds (busButtons.removeFromLeft (juce::jmin (84, busButtons.getWidth())));
                        busButtons.removeFromLeft (4);
                        removeBusButton.setBounds (busButtons.removeFromLeft (juce::jmin (84, busButtons.getWidth())));
                    }
                    busArea.removeFromTop (4);
                    auto busRow = busArea.removeFromTop (34);
                    // Two rows: name/channels/gain first, actions second.  A single
                    // row overflowed at 1366x768 and pushed the buttons out.
                    busName.setBounds (busRow.removeFromLeft (juce::jmax (70, busRow.getWidth() - 132)));
                    busRow.removeFromLeft (4);
                    busChannels.setBounds (busRow.removeFromLeft (juce::jmin (80, busRow.getWidth())));
                    busRow.removeFromLeft (4);
                    busGain.setBounds (busRow.removeFromLeft (juce::jmin (56, busRow.getWidth())));
                    busArea.removeFromTop (4);
                    virtualDevicesButton.setBounds (busArea.removeFromTop (34).removeFromLeft (180));
                }
            }

            // The canvas is the primary workspace. The technical route table is
            // intentionally hidden: cables and their context menu are the
            // editing surface; the model stays the same.
            patchbay.setBounds (area);
            removeRouteButton.setButtonText (L10n::tr ("Rimuovi rotta"));
            removeRouteButton.setVisible (true);
            routeSourceLabel.setVisible (false);
            routeDestLabel.setVisible (false);
            routeGainLabel.setVisible (false);
            routeSource.setVisible (false);
            routeDestination.setVisible (false);
            routeGain.setVisible (false);
            routeEnabled.setVisible (false);
            addRouteButton.setVisible (false);
            updateRouteButton.setVisible (false);
        }

    public:
        void editNodeByReference (const juce::String& kind, const juce::String& id)
        {
            if (kind == "source")
            {
                for (int i = 0; i < (int) sources.size(); ++i)
                    if (sources[(size_t) i].id == id)
                    {
                        sourceList.selectRow (i);
                        fillSourceFields (i);
                        sourceName.grabKeyboardFocus();
                        sourceName.selectAll();
                        return;
                    }
            }
            else if (kind == "destination")
            {
                for (int i = 0; i < (int) destinations.size(); ++i)
                    if (destinations[(size_t) i].id == id)
                    {
                        destinationList.selectRow (i);
                        fillDestinationFields (i);
                        destinationName.grabKeyboardFocus();
                        destinationName.selectAll();
                        return;
                    }
            }
            else if (kind == "bus")
            {
                for (int i = 0; i < (int) buses.size(); ++i)
                    if (buses[(size_t) i].id == id)
                    {
                        busList.selectRow (i);
                        busName.setText (buses[(size_t) i].name, juce::dontSendNotification);
                        busChannels.setSelectedId (juce::jlimit (1, 16, buses[(size_t) i].channels),
                                                   juce::dontSendNotification);
                        busName.grabKeyboardFocus();
                        busName.selectAll();
                        return;
                    }
            }
        }

        void clearProject()
        {
            sources.clear();
            destinations.clear();
            buses.clear();
            routes.clear();
            selectedProcessTitle.clear();
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Nuovo progetto vuoto", "New empty project"));
        }
        juce::File preferencesFile() const
        {
            auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("NL Audio Matrix");
            dir.createDirectory();
            return dir.getChildFile ("preferences.json");
        }

        void loadPreferences()
        {
            const auto file = preferencesFile();
            if (! file.existsAsFile()) return;
            const auto parsed = juce::JSON::parse (file);
            if (auto* o = parsed.getDynamicObject())
            {
                const double rate = (double) o->getProperty ("sampleRate");
                const int buffer = (int) o->getProperty ("buffer");
                if (rate > 0.0) preferredSampleRate = rate;
                if (buffer > 0) preferredBuffer = buffer;
                if (auto* formats = o->getProperty ("deviceFormats").getDynamicObject())
                {
                    for (const auto& property : formats->getProperties())
                    {
                        auto* object = property.value.getDynamicObject();
                        if (object == nullptr)
                            continue;
                        DeviceFormatChoice choice;
                        choice.driver = property.name.toString();
                        choice.deviceId = "asio:" + choice.driver;
                        choice.sampleRate = (double) object->getProperty ("sampleRate");
                        choice.bufferSize = (int) object->getProperty ("buffer");
                        if (choice.sampleRate > 0.0)
                            deviceFormats[choice.deviceId] = choice;
                    }
                }
                // Missing language/engine objects in old preferences.json files
                // keep Italian and Balanced instead of failing to load.
                L10n::setLanguage (L10n::languageFromCode (o->getProperty ("language").toString()));
                engineSettings = EngineUiSettings::fromVar (o->getProperty ("engine"));
                // G2: collapsible-panel state (missing in older files -> expanded).
                if (auto* ui = o->getProperty ("ui").getDynamicObject())
                {
                    leftPanelCollapsed = (bool) ui->getProperty ("leftPanelCollapsed");
                    sourcesCollapsed = (bool) ui->getProperty ("sourcesCollapsed");
                    destinationsCollapsed = (bool) ui->getProperty ("destinationsCollapsed");
                    busesCollapsed = (bool) ui->getProperty ("busesCollapsed");
                }
            }
        }

        void savePreferences()
        {
            auto rootVar = juce::var (new juce::DynamicObject());
            auto* o = rootVar.getDynamicObject();
            o->setProperty ("sampleRate", preferredSampleRate);
            o->setProperty ("buffer", preferredBuffer);
            o->setProperty ("engine", engineSettings.toVar());
            o->setProperty ("language", L10n::languageCode());
            {
                // G4: per-device ASIO formats, keyed by driver name.
                auto* formats = new juce::DynamicObject();
                for (const auto& [deviceId, choice] : deviceFormats)
                {
                    juce::ignoreUnused (deviceId);
                    if (choice.driver.isEmpty())
                        continue;
                    auto* object = new juce::DynamicObject();
                    object->setProperty ("sampleRate", choice.sampleRate);
                    object->setProperty ("buffer", choice.bufferSize);
                    formats->setProperty (juce::Identifier (choice.driver), juce::var (object));
                }
                o->setProperty ("deviceFormats", juce::var (formats));
            }
            {
                auto* ui = new juce::DynamicObject();
                ui->setProperty ("leftPanelCollapsed", leftPanelCollapsed);
                ui->setProperty ("sourcesCollapsed", sourcesCollapsed);
                ui->setProperty ("destinationsCollapsed", destinationsCollapsed);
                ui->setProperty ("busesCollapsed", busesCollapsed);
                o->setProperty ("ui", juce::var (ui));
            }
            preferencesFile().replaceWithText (juce::JSON::toString (rootVar, true));
        }

        juce::File layoutsDirectory() const
        {
            auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("NL Audio Matrix").getChildFile ("layouts");
            dir.createDirectory();
            return dir;
        }

        bool writeLayoutFile (const juce::File& file)
        {
            auto rootVar = juce::var (new juce::DynamicObject());
            auto* o = rootVar.getDynamicObject();
            if (auto* top = getTopLevelComponent())
            {
                const auto b = top->getBounds();
                o->setProperty ("x", b.getX());
                o->setProperty ("y", b.getY());
                o->setProperty ("w", b.getWidth());
                o->setProperty ("h", b.getHeight());
            }
            o->setProperty ("mixerVisible", mixerVisible);
            o->setProperty ("mixerHeight", mixerHeight);
            return file.replaceWithText (juce::JSON::toString (rootVar, true));
        }

        void applyLayoutFile (const juce::File& file)
        {
            const auto parsed = juce::JSON::parse (file);
            auto* o = parsed.getDynamicObject();
            if (o == nullptr) return;
            if (auto* top = getTopLevelComponent())
            {
                const int x = (int) o->getProperty ("x");
                const int y = (int) o->getProperty ("y");
                const int requestedW = juce::jmax (900, (int) o->getProperty ("w"));
                const int requestedH = juce::jmax (520, (int) o->getProperty ("h"));
                if (const auto* display = juce::Desktop::getInstance().getDisplays()
                                              .getDisplayForRect ({ x, y, requestedW, requestedH }))
                {
                    const auto area = display->userArea;
                    const int w = juce::jmin (requestedW, area.getWidth() - 40);
                    const int h = juce::jmin (requestedH, area.getHeight() - 80);
                    const int nx = juce::jlimit (area.getX() + 20, area.getRight() - w - 20, x);
                    const int ny = juce::jlimit (area.getY() + 20, area.getBottom() - h - 20, y);
                    top->setBounds (nx, ny, w, h);
                }
                else
                    top->setBounds (x, y, requestedW, requestedH);
            }
            mixerHeight = juce::jlimit (160, 700, (int) o->getProperty ("mixerHeight"));
            setMixerVisible ((bool) o->getProperty ("mixerVisible"));
            setStatus (L10n::tr ("Layout caricato: ", "Layout loaded: ") + file.getFileNameWithoutExtension());
        }

        void saveLayoutInteractive()
        {
            auto* aw = new juce::AlertWindow ("Salva layout", "Nome del layout",
                                              juce::MessageBoxIconType::NoIcon, this);
            aw->addTextEditor ("name", "Layout 1", "Nome");
            aw->addButton ("Salva", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Annulla", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            juce::Component::SafePointer<MainComponent> safe (this);
            aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int result)
            {
                if (result == 1 && safe != nullptr)
                {
                    auto name = aw->getTextEditorContents ("name").trim();
                    if (name.isEmpty()) name = "Layout";
                    safe->writeLayoutFile (safe->layoutsDirectory().getChildFile (name + ".json"));
                    safe->setStatus (L10n::tr ("Layout salvato: ", "Layout saved: ") + name);
                }
                delete aw;
            }), true);
        }

        void loadLayoutInteractive()
        {
            juce::PopupMenu menu;
            const auto files = layoutsDirectory().findChildFiles (juce::File::findFiles, false, "*.json");
            for (int i = 0; i < files.size(); ++i)
                menu.addItem (i + 1, files[i].getFileNameWithoutExtension());
            if (files.isEmpty())
            {
                setStatus (L10n::tr ("Nessun layout salvato", "No saved layouts"));
                return;
            }
            juce::Component::SafePointer<MainComponent> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                                [safe, files] (int result)
                                {
                                    if (result > 0 && safe != nullptr && result <= files.size())
                                        safe->applyLayoutFile (files[result - 1]);
                                });
        }

        void restoreLastLayout()
        {
            const auto file = layoutsDirectory().getChildFile ("Last session.json");
            if (file.existsAsFile())
                applyLayoutFile (file);
        }

        void saveLastLayout()
        {
            if (! sessionPersistenceEnabled) return;
            writeLayoutFile (layoutsDirectory().getChildFile ("Last session.json"));
        }
        //======================================================================
        // G4 production apply path for a device-format change.  Shared by the
        // Preferences dialog and by the self-test (so the test exercises exactly
        // the code the user runs): validate -> graceful stop via --control ->
        // rewrite profile/preferences -> restart -> status with requested-vs-actual.
        bool applyDeviceFormatChange (double rate, int buffer,
                                      const std::vector<double>& availableRates,
                                      const std::vector<int>& availableBuffers,
                                      EngineUiSettings engine,
                                      std::vector<DeviceFormatChoice> extraFormats,
                                      juce::String& resultText)
        {
            resultText.clear();
            juce::String validationError;
            if (! availableRates.empty())
            {
                bool rateOk = false;
                for (auto r : availableRates)
                    if (std::abs (r - rate) < 0.5) rateOk = true;
                if (! rateOk)
                    validationError = L10n::tr ("Sample rate non supportato dal driver: ",
                                                "Sample rate not supported by the driver: ")
                                      + juce::String (rate, 0) + " Hz";
            }
            if (validationError.isEmpty() && ! availableBuffers.empty() && buffer > 0)
            {
                bool bufferOk = false;
                for (auto b : availableBuffers)
                    if (b == buffer) bufferOk = true;
                if (! bufferOk)
                    validationError = L10n::tr ("Buffer non supportato dal driver: ",
                                                "Buffer size not supported by the driver: ")
                                      + juce::String (buffer);
            }
            if (validationError.isNotEmpty())
            {
                setStatus (validationError);
                logLine ("apply rejected: " + validationError);
                resultText = validationError;
                return false;
            }

            const bool wasRunning = isEngineRunning();
            // E1: graceful stop (fade-out, devices closed, exit 0) then reopen with
            // the new format.  No hard kill, no click at the change.
            if (wasRunning)
                stopRouter();
            setFormatForDriver (asioBox.getText(), rate, buffer);
            for (auto& choice : extraFormats)
            {
                choice.deviceId = choice.deviceId.isNotEmpty() ? choice.deviceId
                                                               : "asio:" + choice.driver;
                deviceFormats[choice.deviceId] = choice;
            }
            preferredSampleRate = rate;
            if (buffer > 0) preferredBuffer = buffer;
            if (engine.mode.isNotEmpty()) engineSettings = engine;
            savePreferences();
            juce::String err;
            if (! saveLiveProfile (err))
                setStatus (err);
            if (wasRunning)
                startRouter();
            resultText = L10n::tr ("Formato applicato: richiesto ", "Format applied: requested ")
                         + juce::String (rate, 0) + " Hz / " + juce::String (buffer);
            if (lastFormatReport.isNotEmpty())
                resultText << " — " << lastFormatReport;
            else if (wasRunning)
                resultText << " — " << L10n::tr ("verifica in Diagnostica", "check Diagnostics");
            setStatus (resultText);
            return true;
        }

        // Read the newest METERS line directly from the telemetry file and apply
        // it now.  Used by the self-test because its synchronous run blocks the UI
        // timer that normally consumes telemetry.
        bool readLatestRouterTelemetry()
        {
            const auto text = telemetryFile.loadFileAsString();
            if (text.isEmpty())
                return false;
            juce::StringArray lines;
            lines.addLines (text);
            for (int i = lines.size(); --i >= 0;)
                if (lines[i].startsWith ("METERS "))
                {
                    applyMeterJson (lines[i].substring (7));
                    refreshMeters();
                    return true;
                }
            return false;
        }

        void openPreferences()
        {
            // G4: probe the real ASIO driver capabilities.  An ASIO driver can be
            // opened by one process at a time, so this is only possible while the
            // router is stopped; otherwise Preferences shows the fallback lists and
            // says that the value is verified on apply.
            std::vector<double> availableRates;
            std::vector<int> availableBuffers;
            double probedRate = 0.0;
            int probedBuffer = 0;
            const bool probed = probeAsioFormat (asioBox.getText(), availableRates,
                                                 availableBuffers, probedRate, probedBuffer);
            juce::ignoreUnused (probed, probedRate, probedBuffer);
            auto extras = collectDeviceFormatsForPreferences();
            if (! extras.empty())
                extras.erase (extras.begin()); // master device is edited by the main controls
            auto* content = new PreferencesComponent (preferredSampleRate, preferredBuffer,
                                                      engineSettings, availableRates,
                                                      availableBuffers, extras);
            auto* win = new PreferencesWindow (content);
            juce::Component::SafePointer<MainComponent> safe (this);
            content->onClose = [win] { win->closeButtonPressed(); };
            content->onApply = [safe, win, availableRates, availableBuffers]
                (double rate, int buffer, EngineUiSettings engine,
                 std::vector<DeviceFormatChoice> extraFormats)
            {
                if (safe == nullptr) return;
                juce::String applyReport;
                safe->applyDeviceFormatChange (rate, buffer, availableRates, availableBuffers,
                                               engine, std::move (extraFormats), applyReport);
                win->closeButtonPressed();
            };
            content->onLanguageChanged = [safe] (const juce::String& code)
            {
                if (safe == nullptr) return;
                safe->setLanguageMode (L10n::languageFromCode (code));
            };
        }
        void openDiagnostics()
        {
            auto* content = new DiagnosticsComponent();
            auto* win = new DiagnosticsWindow (content);
            juce::Component::SafePointer<MainComponent> safe (this);
            content->onClose = [win] { win->closeButtonPressed(); };
            content->getReport = [safe]() -> juce::String
            {
                if (safe == nullptr) return {};
                auto fmt = [] (double value, int decimals) { return juce::String (value, decimals); };
                auto pairMs = [] (double target, double actual) -> juce::String
                {
                    if (target < 0.0 && actual < 0.0) return "-";
                    return (target >= 0.0 ? juce::String (target, 1) : juce::String ("n/d"))
                           + "/" + (actual >= 0.0 ? juce::String (actual, 1) : juce::String ("n/d"));
                };
                auto counter = [] (bool available, juce::int64 value) -> juce::String
                {
                    return available ? juce::String (value) : juce::String ("-");
                };

                // C8: diagnostics prose is generated in the current language at
                // every refresh (the panel re-reads this report twice a second).
                auto txt = [] (const char* italian, const char* english)
                {
                    return L10n::tr (italian, english);
                };
                juce::String t;
                t << txt ("NL Audio Matrix - diagnostica\n", "NL Audio Matrix - diagnostics\n");
                t << txt ("Motore ASIO: ", "ASIO engine: ")
                  << (safe->asioBox.getText().isEmpty() ? txt ("(nessuno)", "(none)")
                                                        : safe->asioBox.getText()) << "\n";
                t << txt ("Sample rate motore: ", "Engine rate: ")
                  << juce::String (safe->engineRate, 1) << " Hz\n";
                t << txt ("Buffer motore: ", "Engine buffer: ") << safe->engineBuffer
                  << txt (" campioni\n", " samples\n");
                t << "Callbacks: " << safe->engineCallbacks << "\n\n";

                // ---- engine v2 buffering / health block ---------------------
                t << txt ("== BUFFERING / SALUTE MOTORE ==\n", "== BUFFERING / ENGINE HEALTH ==\n");
                if (! safe->hasRouterBufferTelemetry)
                {
                    t << txt ("Telemetria buffer: non riportata da questa build del router; i valori sotto sono n/d.\n",
                              "Buffer telemetry: not reported by this router build; values below are n/d.\n");
                }
                else
                {
                    const auto mode = safe->routerBufferMode.isNotEmpty()
                                          ? safe->routerBufferMode
                                          : (safe->hasRouterTuning ? safe->routerTuning.mode
                                                                   : juce::String());
                    t << txt ("Modalità buffer: ", "Buffer mode: ")
                      << (mode.isEmpty()
                              ? juce::String ("n/d")
                              : EngineUiSettings::labelForMode (mode) + " (" + mode + ")")
                      << "\n";
                    t << txt ("Stato motore: ", "Engine state: ")
                      << (safe->routerEngineState.isEmpty() ? "n/d" : safe->routerEngineState)
                      << txt ("   sano: ", "   healthy: ")
                      << (safe->engineHealthy ? txt ("sì", "yes") : "NO")
                      << txt ("   riavvii ASIO: ", "   ASIO restarts: ")
                      << safe->routerAsioRestarts << "\n";
                    t << txt ("Età callback: ", "Callback age: ")
                      << fmt (safe->lastCallbackAgeMs, 1) << " ms"
                      << txt ("   gap massimo callback (1 s): ", "   max callback gap (1 s): ")
                      << (safe->hasRouterCallbackGap
                              ? fmt (safe->routerCallbackGapMaxMs, 1) + " ms"
                              : juce::String ("n/d"))
                      << "\n";
                    if (safe->hasRouterFrameStats)
                    {
                        const double expected = (double) safe->routerExpectedFrames;
                        const double deficit = (double) safe->routerFrameDeficit;
                        const double pct = expected > 0.0 ? 100.0 * deficit / expected : 0.0;
                        t << txt ("Frame: attesi ", "Frames: expected ") << safe->routerExpectedFrames
                          << txt ("   deficit ", "   deficit ") << safe->routerFrameDeficit
                          << " (" << fmt (pct, 3) << " %)" << "\n";
                    }
                    if (safe->hasRouterTuning)
                    {
                        const auto& tune = safe->routerTuning;
                        t << txt ("Tuning attivo: ring ", "Tuning active: ring ")
                          << tune.ringTargetMinMs << "-" << tune.ringTargetMaxMs
                          << " ms   driver " << tune.driverTargetMs << "-" << tune.driverTargetMaxMs
                          << " ms   extra ASIO " << tune.extraAsioTargetMs
                          << " ms   render " << tune.renderTargetMs
                          << " ms   chunk " << tune.writeChunkMs
                          << " ms   servo " << tune.servoIntervalMs << " ms\n";
                    }
                }

                // ---- G4: device formats (requested vs actual) --------------
                t << txt ("\n== FORMATO DISPOSITIVI ASIO ==\n", "\n== ASIO DEVICE FORMATS ==\n");
                if (safe->routerDevices.empty())
                {
                    t << txt ("Nessuna telemetria formato (router precedente o motor spento).\n",
                              "No device-format telemetry (older router or engine stopped).\n");
                }
                else
                {
                    for (const auto& d : safe->routerDevices)
                    {
                        const bool mismatch = d.requestedRate > 0.0
                                              && std::abs (d.requestedRate - d.rate) > 0.5;
                        const bool bufferRounded = d.requestedBuffer > 0 && d.buffer != d.requestedBuffer;
                        t << d.driver << (d.clockMaster ? txt (" [master]", " [master]") : juce::String())
                          << "   " << txt ("richiesto ", "requested ")
                          << (d.requestedRate > 0.0 ? juce::String (d.requestedRate, 0) : juce::String ("n/d"))
                          << " Hz / " << (d.requestedBuffer > 0 ? juce::String (d.requestedBuffer) : juce::String ("n/d"))
                          << "   " << txt ("attivo ", "active ")
                          << juce::String (d.rate, 0) << " Hz / " << d.buffer
                          << "   I/O " << d.inputs << "/" << d.outputs
                          << (mismatch ? txt ("  [NON SUPPORTATO]", "  [NOT SUPPORTED]")
                                       : bufferRounded ? txt ("  [buffer arrotondato]", "  [buffer rounded]")
                                                       : juce::String())
                          << "\n";
                    }
                }

                // ---- per-destination ring/driver/xrun block ----------------
                t << txt ("\n== DESTINAZIONI: RING / DRIVER / XRUN ==\n",
                          "\n== DESTINATIONS: RING / DRIVER / XRUN ==\n");
                bool anyDestination = false;
                for (const auto& entry : safe->meterStates)
                {
                    const auto& s = entry.second;
                    if (s.kind != "destination") continue;
                    anyDestination = true;
                    const bool buffer = s.hasBufferTelemetry;
                    t << entry.first
                      << (s.type.isNotEmpty() ? " [" + s.type + "]" : "")
                      << (s.online ? txt (" online", " online") : " OFFLINE")
                      << " - ring " << (buffer ? pairMs (s.ringTargetMs, s.ringFillMs)
                                               : juce::String ("n/d"))
                      << " ms - driver " << (buffer ? pairMs (s.driverTargetMs, s.driverUsedMs)
                                                    : juce::String ("n/d"))
                      << " ms\n"
                      << "    xruns " << s.underruns
                      << " | starved " << counter (buffer, s.writerStarved)
                      << txt (" | scritture ", " | writes ") << counter (buffer, s.writes)
                      << " | dropB " << counter (buffer, s.driverDroppedBytes)
                      << " | unrB " << counter (buffer, s.driverUnderrunBytes)
                      << " | wake " << (buffer && s.maxWakeMs >= 0.0
                                            ? juce::String (s.maxWakeMs, 1) + " ms"
                                            : juce::String ("-"))
                      << "\n";
                }
                if (! anyDestination)
                    t << txt ("(nessuna destinazione nel profilo attivo)\n",
                              "(no destination in the active profile)\n");
                t << txt ("Cambio modalità: Preferenze > Buffer del motore "
                          "(Low/Balanced/Safe/Avanzate; il router si riavvia automaticamente).\n\n",
                          "Changing mode: Preferences > Engine buffer "
                          "(Low/Balanced/Safe/Advanced; the router restarts automatically).\n\n");

                // ---- existing per-node meters ------------------------------
                t << txt ("id | tipo | L/R dBFS | persi | underrun | online | clip\n",
                          "id | kind | L/R dBFS | dropped | underruns | online | clip\n");
                t << "--------------------------------------------------------------\n";
                for (const auto& entry : safe->meterStates)
                {
                    const auto& s = entry.second;
                    t << entry.first << " | " << s.kind << " | "
                      << juce::String (s.l, 1) << "/" << juce::String (s.r, 1) << " | "
                      << s.dropped << " | " << s.underruns << " | "
                      << (s.online ? txt ("sì", "yes") : txt ("no", "no")) << " | "
                      << (s.clip ? "CLIP" : "-") << "\n";
                }
                t << txt ("\nLatenza/drift sono stime finché non viene eseguita la calibrazione a impulsi.\n",
                          "\nLatency/drift are estimates until impulse calibration is performed.\n");
                return t;
            };
        }
        void openDeviceBrowser()
        {
            std::vector<DeviceBrowserItem> items;
            for (const auto& driver : asioDrivers)
                items.push_back ({ "ASIO", driver, L10n::tr ("driver ASIO", "ASIO driver"), "asio:" + driver,
                                   "asio-in", "asio-out", {}, 0 });
            for (const auto& endpoint : wasapiEndpoints)
            {
                items.push_back ({ "WASAPI", endpoint,
                                   L10n::tr ("endpoint Windows / loopback", "Windows endpoint / loopback"),
                                   {}, "loopback", "render", endpoint, 0 });
            }
            for (int pair = 1; pair <= NLMATRIX_MAX_ENDPOINTS; ++pair)
            {
                const auto name = "NL Virtual " + juce::String (pair);
                items.push_back ({ "NL Matrix", name,
                                   L10n::tr ("coppia NL Return/NL Mic ", "NL Return/NL Mic pair ")
                                       + juce::String (pair),
                                   {}, "driver", "driver", {}, pair - 1 });
            }

            auto* browser = new DeviceBrowserComponent (std::move (items));
            auto* window = new DeviceBrowserWindow (browser);
            juce::Component::SafePointer<MainComponent> safe (this);
            browser->onClose = [window] { window->closeButtonPressed(); };
            browser->onChoose = [safe, browser] (const DeviceBrowserItem& item, bool asSource)
            {
                if (safe == nullptr) return;
                if (asSource)
                {
                    Source s;
                    s.id = safe->uniqueId ("src", safe->sources, safe->destinations, safe->buses);
                    s.name = item.name;
                    s.type = item.sourceType;
                    s.deviceId = item.deviceId;
                    s.endpoint = item.endpoint;
                    s.driverEndpoint = item.driverEndpoint;
                    s.channels = { 0, 1 };
                    s.x = 40.0f;
                    s.y = 40.0f + 82.0f * (float) safe->sources.size();
                    safe->sources.push_back (s);
                    safe->sourceList.selectRow ((int) safe->sources.size() - 1);
                    safe->fillSourceFields ((int) safe->sources.size() - 1);
                }
                else
                {
                    Destination d;
                    d.id = safe->uniqueId ("dst", safe->sources, safe->destinations, safe->buses);
                    d.name = item.name;
                    d.type = item.destinationType;
                    d.deviceId = item.deviceId;
                    d.endpoint = item.endpoint;
                    d.driverEndpoint = item.driverEndpoint;
                    d.channels = { 0, 1 };
                    d.x = 680.0f;
                    d.y = 40.0f + 82.0f * (float) safe->destinations.size();
                    safe->destinations.push_back (d);
                    safe->destinationList.selectRow ((int) safe->destinations.size() - 1);
                    safe->fillDestinationFields ((int) safe->destinations.size() - 1);
                }
                safe->refreshAll();
                safe->applyProfile();
                safe->setStatus (L10n::tr ("Dispositivo aggiunto: ", "Device added: ") + item.name);
            };
        }
        void configureMixer()
        {
            if (mixerConfigured) return;
            mixerConfigured = true;
            mixerPanel.getStrips = [this]() -> std::vector<MixerStripInfo>
            {
                std::vector<MixerStripInfo> infos;
                for (const auto& s : sources)
                    if (s.type != "bus")
                        infos.push_back ({ s.id, s.name, "source", s.gainDb, s.muted, s.solo, s.enabled });
                for (const auto& b : buses)
                    infos.push_back ({ b.id, b.name, "bus", b.gainDb, b.muted, b.solo, b.enabled });
                for (const auto& d : destinations)
                    infos.push_back ({ d.id, d.name, "destination", d.gainDb, d.muted, d.solo, d.enabled });
                return infos;
            };
            mixerPanel.getLevels = [this] (const juce::String& id)
            {
                const auto it = meterStates.find (id);
                if (it == meterStates.end()) return juce::Point<float> (-100.0f, -100.0f);
                return juce::Point<float> (it->second.l, it->second.r);
            };
            mixerPanel.onGain = [this] (const juce::String& kind, const juce::String& id, float gain)
            {
                if (kind == "source") for (auto& s : sources) if (s.id == id) s.gainDb = gain;
                if (kind == "bus") for (auto& b : buses) if (b.id == id) b.gainDb = gain;
                if (kind == "destination") for (auto& d : destinations) if (d.id == id) d.gainDb = gain;
                refreshAll();
                applyProfile();
            };
            mixerPanel.onMute = [this] (const juce::String& kind, const juce::String& id, bool muted)
            {
                if (kind == "source") for (auto& s : sources) if (s.id == id) s.muted = muted;
                if (kind == "bus") for (auto& b : buses) if (b.id == id) b.muted = muted;
                if (kind == "destination") for (auto& d : destinations) if (d.id == id) d.muted = muted;
                refreshAll();
                applyProfile();
            };
            mixerPanel.onSolo = [this] (const juce::String& kind, const juce::String& id, bool solo)
            {
                if (kind == "source") for (auto& s : sources) if (s.id == id) s.solo = solo;
                if (kind == "bus") for (auto& b : buses) if (b.id == id) b.solo = solo;
                if (kind == "destination") for (auto& d : destinations) if (d.id == id) d.solo = solo;
                refreshAll();
                applyProfile();
            };
            mixerPanel.onClose = [this] { setMixerVisible (false); };
            mixerResizer.onDragDelta = [this] (int delta)
            {
                mixerHeight = juce::jlimit (160, juce::jmax (220, getHeight() / 2), mixerHeight + delta);
                resized();
            };
        }

        void setMixerVisible (bool visible)
        {
            configureMixer();
            mixerVisible = visible;
            mixerPanel.setVisible (visible);
            mixerResizer.setVisible (visible);
            resized();
            setStatus (visible ? L10n::tr ("Mixer visibile", "Mixer visible")
                             : L10n::tr ("Mixer nascosto", "Mixer hidden"));
        }

        void openMixer() { setMixerVisible (true); }
        void toggleMixer() { setMixerVisible (! mixerVisible); }
        void runLayoutTest()
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("nlmatrix_layout_test.json");
            const bool saved = writeLayoutFile (file);
            const auto parsed = juce::JSON::parse (file);
            bool valid = false;
            if (auto* o = parsed.getDynamicObject())
                valid = (int) o->getProperty ("w") >= 900
                        && (int) o->getProperty ("h") >= 520
                        && o->hasProperty ("mixerVisible")
                        && (int) o->getProperty ("mixerHeight") >= 160;
            const auto log = file.getSiblingFile ("nlmatrix_layouttest.log");
            log.replaceWithText (juce::String (saved && valid ? "PASS" : "FAIL")
                                 + " layout roundtrip\n");
        }
        juce::File lastSessionProfileFile() const
        {
            auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("NL Audio Matrix");
            dir.createDirectory();
            return dir.getChildFile ("last_session_profile.json");
        }

        void saveLastSessionProfile()
        {
            if (! sessionPersistenceEnabled) return;
            juce::String error;
            writeProfile (lastSessionProfileFile(), error);
        }

        void restoreLastSessionProfile()
        {
            if (! sessionPersistenceEnabled) return;
            const auto file = lastSessionProfileFile();
            if (! file.existsAsFile()) return;
            stopRouter();
            juce::String error;
            if (readProfile (file, error))
            {
                refreshAll();
                setStatus (L10n::tr ("Ultima sessione ripristinata", "Restored last session profile"));
            }
        }

        void setSessionPersistenceEnabled (bool enabled) { sessionPersistenceEnabled = enabled; }
        void restoreLastLayoutOnStartup() { restoreLastLayout(); }
        void saveLastLayoutOnClose() { saveLastLayout(); }

        juce::File themePreferenceFile() const
        {
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                .getChildFile ("NL Audio Matrix").getChildFile ("theme.txt");
        }

        void saveThemePreference() const
        {
            auto file = themePreferenceFile();
            file.getParentDirectory().createDirectory();
            file.replaceWithText (themeMode == ThemeMode::light ? "light"
                                  : themeMode == ThemeMode::system ? "system" : "dark");
        }

        void loadThemePreference()
        {
            const auto text = themePreferenceFile().loadFileAsString().trim().toLowerCase();
            themeMode = text == "light" ? ThemeMode::light
                       : text == "system" ? ThemeMode::system : ThemeMode::dark;
            lastSystemLight = isSystemLightTheme();
            applyPalette (themeMode);
        }

        void setThemeMode (ThemeMode mode)
        {
            themeMode = mode;
            lastSystemLight = isSystemLightTheme();
            applyPalette (mode);
            lookAndFeel.refreshColours();
            sendLookAndFeelChange();
            applyPaletteToAllWindows();
            saveThemePreference();
            setStatus (mode == ThemeMode::light ? L10n::tr ("Tema: Chiaro", "Theme: Light")
                      : mode == ThemeMode::system ? L10n::tr ("Tema: Sistema", "Theme: System")
                                                  : L10n::tr ("Tema: Scuro", "Theme: Dark"));
        }

        // C9: re-colour a component tree from the semantic Palette tokens.  Works
        // for the main window and for separately created windows (Preferences,
        // Diagnostics, About, Mixer, Device browser), which inherit the shared
        // NeonLookAndFeel as the application default look&feel.
        static void applyPaletteToTree (juce::Component& root)
        {
            std::function<void (juce::Component&)> applyColours = [&] (juce::Component& c)
            {
                if (auto* label = dynamic_cast<juce::Label*> (&c))
                    label->setColour (juce::Label::textColourId, Palette::text);
                if (auto* editor = dynamic_cast<juce::TextEditor*> (&c))
                {
                    editor->setColour (juce::TextEditor::backgroundColourId, Palette::panel);
                    editor->setColour (juce::TextEditor::textColourId, Palette::text);
                    editor->setColour (juce::TextEditor::outlineColourId, Palette::outline);
                    editor->setColour (juce::TextEditor::focusedOutlineColourId, Palette::cyan);
                    editor->setColour (juce::TextEditor::highlightColourId, Palette::violet.withAlpha (0.45f));
                    editor->setColour (juce::CaretComponent::caretColourId, Palette::text);
                    // TextEditor caches the colour of existing text at insertion
                    // time, so setting textColourId alone leaves old-theme text
                    // (e.g. near-white on the Light background).  Recount it.
                    editor->applyColourToAllText (Palette::text, false);
                }
                if (auto* list = dynamic_cast<juce::ListBox*> (&c))
                {
                    list->setColour (juce::ListBox::backgroundColourId, Palette::panel);
                    list->setColour (juce::ListBox::outlineColourId, Palette::outline);
                    list->setColour (juce::ListBox::textColourId, Palette::text);
                }
                if (auto* combo = dynamic_cast<juce::ComboBox*> (&c))
                {
                    combo->setColour (juce::ComboBox::backgroundColourId, Palette::panel);
                    combo->setColour (juce::ComboBox::outlineColourId, Palette::outline);
                    combo->setColour (juce::ComboBox::textColourId, Palette::text);
                    combo->setColour (juce::ComboBox::arrowColourId, Palette::cyan);
                    combo->setColour (juce::ComboBox::focusedOutlineColourId, Palette::magenta);
                    combo->setColour (juce::ComboBox::buttonColourId, Palette::panelRaised);
                }
                if (auto* button = dynamic_cast<juce::TextButton*> (&c))
                {
                    button->setColour (juce::TextButton::buttonColourId, Palette::panelRaised);
                    button->setColour (juce::TextButton::buttonOnColourId, Palette::violet);
                    button->setColour (juce::TextButton::textColourOffId, Palette::text);
                    button->setColour (juce::TextButton::textColourOnId, Palette::background);
                }
                if (auto* toggle = dynamic_cast<juce::ToggleButton*> (&c))
                {
                    toggle->setColour (juce::ToggleButton::textColourId, Palette::text);
                    toggle->setColour (juce::ToggleButton::tickColourId, Palette::cyan);
                    toggle->setColour (juce::ToggleButton::tickDisabledColourId, Palette::outline);
                }
                if (auto* slider = dynamic_cast<juce::Slider*> (&c))
                {
                    // C9: the fader needs a visible filled track in both themes.
                    // Light: translucent deep violet on the light pane; Dark: the
                    // same token reads as a saturated violet on the dark pane.
                    slider->setColour (juce::Slider::thumbColourId, Palette::cyan);
                    slider->setColour (juce::Slider::trackColourId,
                                       Palette::isLight() ? Palette::violet.withAlpha (0.75f)
                                                          : Palette::violet);
                    slider->setColour (juce::Slider::backgroundColourId, Palette::panelRaised);
                    slider->setColour (juce::Slider::textBoxTextColourId, Palette::text);
                    slider->setColour (juce::Slider::textBoxBackgroundColourId, Palette::panel);
                    slider->setColour (juce::Slider::textBoxOutlineColourId, Palette::outline);
                }
                if (auto* link = dynamic_cast<juce::HyperlinkButton*> (&c))
                    link->setColour (juce::HyperlinkButton::textColourId, Palette::cyan);
                for (auto* child : c.getChildren())
                    applyColours (*child);
            };
            if (auto* window = dynamic_cast<juce::ResizableWindow*> (&root))
                window->setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
            applyColours (root);
        }

        // Apply the palette to the main tree and to every other top-level window
        // so no component keeps construction-time dark colours after a switch.
        void applyPaletteToAllWindows()
        {
            // Placeholder colours are set through the text bindings, so re-apply
            // them on a theme change too (a plain theme switch does not change
            // the language); this keeps "nome sorgente"/"0,1" hints readable.
            L10n::applyBindings();
            applyPaletteToTree (*this);
            // Status keeps its semantic accent instead of the generic text token.
            statusLabel.setColour (juce::Label::textColourId, Palette::success);
            for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
                if (auto* top = juce::Desktop::getInstance().getComponent (i))
                {
                    applyPaletteToTree (*top);
                    top->sendLookAndFeelChange();
                    top->repaint();
                }
        }

        static void repaintAllTopLevelWindows()
        {
            for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
                if (auto* component = juce::Desktop::getInstance().getComponent (i))
                    component->repaint();
        }

        void pollSystemTheme()
        {
            if (themeMode != ThemeMode::system) return;
            if (++themePollTicks < 20) return;
            themePollTicks = 0;
            const bool light = isSystemLightTheme();
            if (light != lastSystemLight)
            {
                lastSystemLight = light;
                applyPalette (ThemeMode::system);
                lookAndFeel.refreshColours();
                sendLookAndFeelChange();
                applyPaletteToAllWindows();
                setStatus (L10n::tr ("Tema di sistema aggiornato", "System theme updated"));
            }
        }
        // C8: runtime language switch.  Persisted in preferences.json; live
        // bindings and the menu-bar hook refresh every registered text.
        void setLanguageMode (L10n::Language language)
        {
            if (L10n::currentLanguage() != language)
            {
                L10n::setLanguage (language);
                savePreferences();
            }
            L10n::notifyLanguageChanged();
            setStatus (language == L10n::Language::english ? "Language: English"
                                                           : "Lingua: Italiano");
            repaintAllTopLevelWindows();
        }

        L10n::Language getLanguageMode() const { return L10n::currentLanguage(); }

        void handleMenuCommand (int command)
        {
            switch (command)
            {
                case 1001: // New project
                {
                    juce::Component::SafePointer<MainComponent> safe (this);
                    juce::AlertWindow::showOkCancelBox (
                        juce::MessageBoxIconType::QuestionIcon,
                        L10n::tr ("Nuovo progetto", "New project"),
                        L10n::tr ("Cancellare il progetto corrente? Salvalo prima se serve.",
                                  "Clear the current project? Save it first if needed."),
                        L10n::tr ("Nuovo progetto", "New project"),
                        L10n::tr ("Annulla", "Cancel"), nullptr,
                        juce::ModalCallbackFunction::create ([safe] (int result)
                        {
                            if (result == 1 && safe != nullptr)
                                safe->clearProject();
                        }));
                    break;
                }
                case 1002: loadProfile(); break;
                case 1003:
                case 1004: saveProfile(); break;
                case 1005:
                    if (auto* app = juce::JUCEApplication::getInstance())
                        app->systemRequestedQuit();
                    break;
                case 2001: addNodeFromCanvas ("source"); break;
                case 2002: addNodeFromCanvas ("destination"); break;
                case 2003: addNodeFromCanvas ("bus"); break;
                case 2004: toggleRouter(); break;
                case 2005: applyProfile(); break;
                case 3001: refreshDevices(); refreshAll(); break;
                case 3002: showVirtualDevices(); break;
                case 3004: openDeviceBrowser(); break;
                case 6001: openDiagnostics(); break;
                case 6002: openPreferences(); break;
                case 4003: saveLayoutInteractive(); break;
                case 4004: loadLayoutInteractive(); break;
                case 4005: setMixerVisible (false); break;
                case 3003:
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::InfoIcon,
                        L10n::tr ("Motore ASIO", "ASIO engine"),
                        L10n::tr ("Dispositivo motore corrente: ", "Current engine device: ")
                            + asioBox.getText() + "\n\n"
                            + L10n::tr ("Questa build apre un solo dispositivo motore ASIO alla volta; i canali di "
                                        "sorgenti e destinazioni sono indipendenti dal clock del motore.",
                                        "This build opens one ASIO engine device at a time; source and destination "
                                        "channel bindings are independent of the engine clock."));
                    break;
                case 4001: patchbay.fitContent(); break;
                case 4002: toggleMixer(); break;
                case 7001: setThemeMode (ThemeMode::dark); break;
                case 7002: setThemeMode (ThemeMode::system); break;
                case 7003: setThemeMode (ThemeMode::light); break;
                case 7004: setLanguageMode (L10n::Language::italian); break;
                case 7005: setLanguageMode (L10n::Language::english); break;
                case 5001:
                    new AboutWindow();
                    break;
                case 5002:
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::InfoIcon,
                        L10n::tr ("Scorciatoie da tastiera", "Keyboard shortcuts"),
                        L10n::tr ("Doppio clic su un nodo: proprietà\n"
                                  "Clic destro su nodo/cavo/canvas: menu contestuale\n"
                                  "Canc: rimuove il cavo selezionato\n"
                                  "Rotella: zoom; trascinamento centrale: pan\n"
                                  "Ctrl+S: salva profilo (quando il menu ha il focus)",
                                  "Double-click a node: properties\n"
                                  "Right-click node/cable/canvas: context menu\n"
                                  "Delete: remove selected cable\n"
                                  "Mouse wheel: zoom; middle drag: pan\n"
                                  "Ctrl+S: save profile (when menu focus)"));
                    break;
                default: break;
            }
        }
    private:
        // C8: every main-window string is registered once for live IT/EN
        // rebinding.  Called after preferences are loaded so a persisted English
        // choice is applied at startup, then re-applied by setLanguageMode().
        void registerLocalisedTexts()
        {
            L10n::bindPlaceholder (profileName, "Nome profilo");
            L10n::bindLabelText (asioLabel, "Motore ASIO principale");
            // The status line is transient: translate the initial text once instead
            // of rebinding it, so a language switch never wipes the last message.
            statusLabel.setText (L10n::tr ("Pronto"), juce::dontSendNotification);
            L10n::bindButtonText (applyButton, "Applica live");
            L10n::bindButtonText (saveButton, "Salva profilo...");
            L10n::bindButtonText (loadButton, "Carica profilo...");
            L10n::bindButtonText (refreshButton, "Aggiorna");
            L10n::bindLabelText (sourceTitle, "Sorgenti (ingressi)");
            L10n::bindLabelText (destinationTitle, "Destinazioni (uscite)");
            L10n::bindLabelText (busTitle, "Bus interni (nessun device Windows)");
            L10n::bindLabelText (routeTitle, "Connessioni");
            L10n::bindLabelText (metersTitle, "Misuratori");
            L10n::bindButtonText (sourcePickChannels, "Scegli canali...");
            L10n::bindButtonText (sourcePickProcess, "Scegli app...");
            L10n::bindButtonText (sourceProcessTree, "Processi figli inclusi (API Windows)");
            L10n::bindButtonText (sourceEnabled, "Sorgente attiva");
            L10n::bindButtonText (addSourceButton, "Aggiungi sorgente");
            L10n::bindButtonText (updateSourceButton, "Aggiorna selezionato");
            L10n::bindButtonText (removeSourceButton, "Rimuovi");
            L10n::bindButtonText (destinationPickChannels, "Scegli canali...");
            L10n::bindButtonText (addDestinationButton, "Aggiungi destinazione");
            L10n::bindButtonText (updateDestinationButton, "Aggiorna selezionato");
            L10n::bindButtonText (removeDestinationButton, "Rimuovi");
            L10n::bindButtonText (addBusButton, "Aggiungi bus");
            L10n::bindButtonText (updateBusButton, "Aggiorna selezionato");
            L10n::bindButtonText (removeBusButton, "Rimuovi");
            L10n::bindButtonText (virtualDevicesButton, "Dispositivi virtuali...");
            L10n::bindButtonText (routeEnabled, "Attiva");
            L10n::bindButtonText (addRouteButton, "Aggiungi rotta");
            L10n::bindButtonText (updateRouteButton, "Aggiorna selezionato");
            L10n::bindButtonText (removeRouteButton, "Rimuovi rotta");
            L10n::bindLabelText (routeSourceLabel, "Rotta sorgente");
            L10n::bindLabelText (routeDestLabel, "Rotta destinazione");
            L10n::bindLabelText (routeGainLabel, "Guadagno dB");
            L10n::bindPlaceholder (sourceName, "nome sorgente");
            L10n::bindPlaceholder (sourceChannels, "canali ASIO, es. 1,2");
            L10n::bindPlaceholder (sourceProcess, "processo.exe o PID");
            L10n::bindPlaceholder (sourceGain, "guadagno sorgente dB");
            L10n::bindPlaceholder (sourcePan, "pan -1..1");
            L10n::bindPlaceholder (destinationName, "nome destinazione");
            L10n::bindPlaceholder (destinationChannels, "canali ASIO, es. 1,2");
            L10n::bindPlaceholder (destinationGain, "guadagno destinazione dB");
            L10n::bindPlaceholder (busName, "nome bus (es. NL Mic Marco)");
            L10n::bindPlaceholder (busGain, "guadagno bus dB");
            L10n::bindPlaceholder (routeGain, "guadagno dB");
            L10n::bindAction ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->startStop.setButtonText (
                        L10n::tr (safe->isEngineRunning() ? "Ferma" : "Avvia"));
            });
            refreshMainComboItems();
            L10n::bindAction ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr) safe->refreshMainComboItems();
            });
            if (profileName.getText().isEmpty() || profileName.getText() == "New profile"
                || profileName.getText() == L10n::tr ("Nuovo profilo"))
                profileName.setText (L10n::tr ("Nuovo profilo"), juce::dontSendNotification);
            L10n::applyBindings();
        }

        // C8: source/destination type selectors follow the interface language.
        // Item ids are independent of the text, so the current selection is
        // preserved across a rebuild.
        void refreshMainComboItems()
        {
            const int sourceTypeId = sourceType.getSelectedId();
            juce::StringArray sourceItems;
            sourceItems.add (L10n::tr ("Ingresso ASIO"));
            sourceItems.add (L10n::tr ("WASAPI loopback (audio di sistema)"));
            sourceItems.add (L10n::tr ("WASAPI capture (dispositivo di ingresso)"));
            sourceItems.add (L10n::tr ("Cattura processo (per app)"));
            sourceItems.add (L10n::tr ("Bus interno"));
            sourceItems.add (L10n::tr ("Endpoint virtuale NL (driver)"));
            sourceType.clear (juce::dontSendNotification);
            sourceType.addItemList (sourceItems, 1);
            sourceType.setSelectedId (sourceTypeId > 0 ? sourceTypeId : 1,
                                      juce::dontSendNotification);

            const int destinationTypeId = destinationType.getSelectedId();
            juce::StringArray destinationItems;
            destinationItems.add (L10n::tr ("Uscita ASIO"));
            destinationItems.add (L10n::tr ("Endpoint render Windows"));
            destinationItems.add (L10n::tr ("Endpoint virtuale NL (driver)"));
            destinationType.clear (juce::dontSendNotification);
            destinationType.addItemList (destinationItems, 1);
            destinationType.setSelectedId (destinationTypeId > 0 ? destinationTypeId : 1,
                                           juce::dontSendNotification);

            const int sourcePairId = sourceDriverEndpoint.getSelectedId();
            sourceDriverEndpoint.clear (juce::dontSendNotification);
            for (int n = 1; n <= NLMATRIX_MAX_ENDPOINTS; ++n)
                sourceDriverEndpoint.addItem (L10n::tr ("Ingresso virtuale NL ", "NL virtual input ")
                                                  + juce::String (n), n);
            sourceDriverEndpoint.setSelectedId (sourcePairId > 0 ? sourcePairId : 1,
                                                juce::dontSendNotification);

            const int destinationPairId = destinationDriverEndpoint.getSelectedId();
            destinationDriverEndpoint.clear (juce::dontSendNotification);
            for (int n = 1; n <= NLMATRIX_MAX_ENDPOINTS; ++n)
                destinationDriverEndpoint.addItem (L10n::tr ("Uscita virtuale NL ", "NL virtual output ")
                                                       + juce::String (n), n);
            destinationDriverEndpoint.setSelectedId (destinationPairId > 0 ? destinationPairId : 1,
                                                     juce::dontSendNotification);

            const int busChannelsId = busChannels.getSelectedId();
            busChannels.clear (juce::dontSendNotification);
            for (int n = 1; n <= 16; ++n)
                busChannels.addItem (juce::String (n) + " "
                                         + (n == 1 ? L10n::tr ("canale", "channel")
                                                   : L10n::tr ("canali", "channels")), n);
            busChannels.setSelectedId (busChannelsId > 0 ? busChannelsId : 2,
                                       juce::dontSendNotification);

            // Preserve the selection by item id (the id order is stable) instead
            // of by text: restoring the old text would put the previous language's
            // "(Main engine)" label back after a language/theme rebuild.
            const int previousSourceDeviceId = sourceDevice.getSelectedId();
            const int previousDestinationDeviceId = destinationDevice.getSelectedId();
            const auto previousSourceDeviceText = sourceDevice.getText();
            const auto previousDestinationDeviceText = destinationDevice.getText();
            sourceDevice.clear (juce::dontSendNotification);
            destinationDevice.clear (juce::dontSendNotification);
            sourceDevice.addItem (L10n::tr ("(Motore principale)", "(Main engine)"), 1);
            destinationDevice.addItem (L10n::tr ("(Motore principale)", "(Main engine)"), 1);
            for (int i = 0; i < asioDrivers.size(); ++i)
            {
                sourceDevice.addItem (asioDrivers[i], i + 2);
                destinationDevice.addItem (asioDrivers[i], i + 2);
            }
            sourceDevice.setSelectedId (previousSourceDeviceId > 0 ? previousSourceDeviceId : 1,
                                        juce::dontSendNotification);
            destinationDevice.setSelectedId (previousDestinationDeviceId > 0 ? previousDestinationDeviceId : 1,
                                             juce::dontSendNotification);
            // A device selected by name from a loaded profile has no combo id yet:
            // keep it only when it is a real ASIO name (never the main-engine entry).
            const auto isMainEngineLabel = [] (const juce::String& text)
            {
                return text == "(Main engine)" || text == L10n::tr ("(Motore principale)", "(Main engine)");
            };
            if (sourceDevice.getSelectedId() <= 0 && previousSourceDeviceText.isNotEmpty()
                && ! isMainEngineLabel (previousSourceDeviceText))
                sourceDevice.setText (previousSourceDeviceText, juce::dontSendNotification);
            if (destinationDevice.getSelectedId() <= 0 && previousDestinationDeviceText.isNotEmpty()
                && ! isMainEngineLabel (previousDestinationDeviceText))
                destinationDevice.setText (previousDestinationDeviceText, juce::dontSendNotification);
        }

        void addSection (juce::Label& label, const juce::String& text)
        {
            label.setText (text, juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, Palette::text);
            label.setFont (juce::FontOptions (17.0f, juce::Font::bold));
            addAndMakeVisible (label);
        }

        void updateSourceFields()
        {
            const auto type = sourceTypeKey (sourceType.getSelectedId());
            sourceEndpoint.setVisible (type == "loopback" || type == "capture");
            sourceChannels.setVisible (type == "asio-in");
            sourceDevice.setVisible (type == "asio-in");
            sourcePickChannels.setVisible (type != "bus");
            sourceProcess.setVisible (type == "process");
            sourcePickProcess.setVisible (type == "process");
            sourceProcessTree.setVisible (type == "process");
            sourceEnabled.setVisible (true);
            sourceBus.setVisible (type == "bus");
            sourceDriverEndpoint.setVisible (type == "driver");
            resized();
        }

        void updateDestinationFields()
        {
            const auto type = destinationTypeKey (destinationType.getSelectedId());
            destinationEndpoint.setVisible (type == "render");
            destinationChannels.setVisible (type == "asio-out");
            destinationDevice.setVisible (type == "asio-out");
            destinationPickChannels.setVisible (type != "driver");
            destinationDriverEndpoint.setVisible (type == "driver");
            destinationDriverTarget.setVisible (type == "driver");
            resized();
        }

        void showVirtualDevices()
        {
            auto* dialog = new VirtualDevicesDialog();
            juce::String assignments;
            for (const auto& s : sources)
                if (s.type == "driver")
                    assignments << "NL Return " << (juce::jlimit (0, NLMATRIX_MAX_ENDPOINTS - 1,
                                                                   s.driverEndpoint) + 1)
                                << " -> source " << s.name << "\n";
            for (const auto& d : destinations)
                if (d.type == "driver")
                    assignments << "NL Mic " << (juce::jlimit (0, NLMATRIX_MAX_ENDPOINTS - 1,
                                                                d.driverEndpoint) + 1)
                                << " -> destination " << d.name << "\n";
            if (assignments.isEmpty())
                assignments = "(no driver nodes in this profile)";
            dialog->setAssignments (assignments);

            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned (dialog);
            options.dialogTitle = L10n::tr ("NL Audio Matrix - dispositivi virtuali",
                                             "NL Audio Matrix - virtual devices");
            options.dialogBackgroundColour = Palette::background;
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true;
            options.resizable = true;
            options.componentToCentreAround = this;
            options.launchAsync();
        }

        void setStatus (const juce::String& text)
        {
            statusLabel.setText (text, juce::dontSendNotification);
        }

        juce::String displayChannels (const std::vector<int>& channels) const
        {
            juce::StringArray parts;
            for (int ch : channels) parts.add (juce::String (ch + 1));
            return parts.joinIntoString ("/");
        }

        juce::StringArray discoverChannels (bool forSource)
        {
            juce::StringArray names;
            const auto type = forSource ? sourceTypeKey (sourceType.getSelectedId())
                                        : destinationTypeKey (destinationType.getSelectedId());

            if (type == "asio-in" || type == "asio-out")
            {
                if (auto at = std::unique_ptr<juce::AudioIODeviceType> (
                        juce::AudioIODeviceType::createAudioIODeviceType_ASIO()))
                {
                    at->scanForDevices();
                    auto* selectedDevice = forSource ? &sourceDevice : &destinationDevice;
                    const auto deviceName = selectedDevice->getSelectedId() > 1
                                                ? selectedDevice->getText() : asioBox.getText();
                    auto device = std::unique_ptr<juce::AudioIODevice> (
                        at->createDevice (deviceName, deviceName));
                    if (device != nullptr)
                    {
                        const auto reported = forSource ? device->getInputChannelNames()
                                                        : device->getOutputChannelNames();
                        for (const auto& n : reported) names.add (n);
                    }
                }
                if (names.isEmpty())
                {
                    names.add ("Canale 1 (ASIO occupato/offline, manuale)");
                    names.add ("Canale 2 (ASIO occupato/offline, manuale)");
                }
                return names;
            }

            if (type == "driver")
            {
                const int pair = forSource ? sourceDriverEndpoint.getSelectedId()
                                            : destinationDriverEndpoint.getSelectedId();
                const juce::String prefix = forSource ? "NL Return " : "NL Mic ";
                names.add (prefix + juce::String (juce::jmax (1, pair)) + " L");
                names.add (prefix + juce::String (juce::jmax (1, pair)) + " R");
                return names;
            }

            if (type == "process")
            {
                names.add ("Process L");
                names.add ("Process R");
                return names;
            }

            if (type == "bus")
            {
                const int busRow = forSource ? sourceBus.getSelectedItemIndex() : -1;
                const int count = busRow >= 0 && busRow < (int) buses.size()
                                      ? buses[(size_t) busRow].channels : 2;
                for (int i = 0; i < count; ++i)
                    names.add ("Bus channel " + juce::String (i + 1));
                return names;
            }

            // WASAPI loopback/capture/render: ask the endpoint directly.
            if (auto wt = std::unique_ptr<juce::AudioIODeviceType> (
                    juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (
                        juce::WASAPIDeviceMode::shared)))
            {
                wt->scanForDevices();
                const auto endpoint = forSource ? sourceEndpoint.getText()
                                                : destinationEndpoint.getText();
                if (endpoint.isNotEmpty())
                {
                    auto device = std::unique_ptr<juce::AudioIODevice> (
                        wt->createDevice (endpoint, {}));
                    if (device != nullptr)
                    {
                        const auto reported = forSource ? device->getInputChannelNames()
                                                        : device->getOutputChannelNames();
                        for (const auto& n : reported) names.add (n);
                    }
                }
            }
            if (names.isEmpty())
            {
                names.add ("Canale 1 (Windows endpoint)");
                names.add ("Canale 2 (Windows endpoint)");
            }
            return names;
        }

        juce::String autoNodeName (bool forSource)
        {
            const auto type = forSource ? sourceTypeKey (sourceType.getSelectedId())
                                        : destinationTypeKey (destinationType.getSelectedId());
            const auto channels = forSource ? parseChannels (sourceChannels.getText())
                                            : parseChannels (destinationChannels.getText());
            juce::String device;
            if (type == "asio-in" || type == "asio-out")
            {
                juce::ComboBox& selectedDevice = forSource ? sourceDevice : destinationDevice;
                device = selectedDevice.getSelectedId() > 1 ? selectedDevice.getText() : asioBox.getText();
            }
            else if (type == "loopback" || type == "capture" || type == "render")
                device = forSource ? sourceEndpoint.getText() : destinationEndpoint.getText();
            else if (type == "driver")
                device = (forSource ? "NL Return " : "NL Mic ")
                         + juce::String (juce::jmax (1, forSource ? sourceDriverEndpoint.getSelectedId()
                                                                  : destinationDriverEndpoint.getSelectedId()));
            else if (type == "bus")
                device = forSource ? sourceBus.getText() : "Internal";
            else if (type == "process")
                device = sourceProcess.getText().isNotEmpty() ? sourceProcess.getText() : "Process";
            if (device.isEmpty()) device = type;
            const auto channelText = displayChannels (channels);
            return channelText.isEmpty() ? device
                                        : device + L10n::utf8 (" · ") + (forSource ? "Input " : "Output ")
                                              + channelText;
        }

        void openProcessPicker()
        {
            auto* content = new ProcessPickerComponent();
            juce::Component::SafePointer<MainComponent> safe (this);
            content->onAccept = [safe, content] (const RunningApp& app, bool includeChildren)
            {
                if (safe == nullptr) return;
                safe->sourceProcess.setText (app.executable, juce::dontSendNotification);
                safe->selectedProcessTitle = app.title;
                safe->sourceProcessTree.setToggleState (includeChildren, juce::dontSendNotification);
                if (safe->sourceName.getText().trim().isEmpty())
                    safe->sourceName.setText (app.title.isNotEmpty() ? app.title : app.executable,
                                              juce::dontSendNotification);
                safe->setStatus (L10n::tr ("Processo selezionato: ", "Selected process: ")
                                         + app.title + " (" + app.executable
                                         + ", PID " + juce::String (app.pid) + ")");
            };
            content->onCloseRequest = [content]
            {
                if (auto* window = content->findParentComponentOfClass<juce::DialogWindow>())
                    window->exitModalState (0);
            };
            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned (content);
            options.dialogTitle = L10n::tr ("Scegli applicazione da catturare",
                                            "Choose the application to capture");
            options.dialogBackgroundColour = Palette::background;
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true;
            options.resizable = true;
            options.componentToCentreAround = this;
            options.launchAsync();
        }
        void openChannelPicker (bool forSource)
        {
            const auto names = discoverChannels (forSource);
            const auto current = forSource ? parseChannels (sourceChannels.getText())
                                           : parseChannels (destinationChannels.getText());
            juce::String title = forSource ? L10n::tr ("Seleziona canali sorgente", "Select source channels")
                                           : L10n::tr ("Seleziona canali destinazione", "Select destination channels");
            const auto type = forSource ? sourceTypeKey (sourceType.getSelectedId())
                                : destinationTypeKey (destinationType.getSelectedId());
            juce::String deviceLabel = title;
            if (type == "asio-in" || type == "asio-out") deviceLabel = "ASIO" + L10n::utf8 (" · ") + asioBox.getText();
            else if (type == "loopback" || type == "capture" || type == "render")
                deviceLabel = "WASAPI" + L10n::utf8 (" · ")
                              + (forSource ? sourceEndpoint.getText() : destinationEndpoint.getText());
            else if (type == "driver")
                deviceLabel = (forSource ? "NL Return " : "NL Mic ") + juce::String (juce::jmax (1, forSource ? sourceDriverEndpoint.getSelectedId() : destinationDriverEndpoint.getSelectedId()));
            auto* picker = new ChannelPickerComponent (deviceLabel, names, current);
            auto* window = new juce::AlertWindow (
                title,
                L10n::utf8 ("Canali riportati dal dispositivo selezionato. "
                            "La modalità manuale resta disponibile nel pannello."),
                juce::MessageBoxIconType::NoIcon);
            window->addCustomComponent (picker);
            window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            window->addButton ("Annulla", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            fitComponentToWorkArea (*window, 620, 650);
            juce::Component::SafePointer<MainComponent> safe (this);
            window->enterModalState (true,
                juce::ModalCallbackFunction::create ([safe, picker, window, forSource] (int result)
                {
                    if (safe != nullptr && result == 1)
                    {
                        const auto chosen = picker->selectedChannels();
                        auto& editor = forSource ? safe->sourceChannels : safe->destinationChannels;
                        editor.setText (channelsToText (chosen), juce::dontSendNotification);
                        auto& nameEditor = forSource ? safe->sourceName : safe->destinationName;
                        if (nameEditor.getText().trim().isEmpty())
                            nameEditor.setText (safe->autoNodeName (forSource), juce::dontSendNotification);
                    }
                    delete window;
                }), true);
        }
        //======================================================================
        void refreshDevices()
        {
            asioBox.clear();
            if (auto at = std::unique_ptr<juce::AudioIODeviceType> (
                    juce::AudioIODeviceType::createAudioIODeviceType_ASIO()))
            {
                at->scanForDevices();
                const auto names = at->getDeviceNames (false);
                for (int i = 0; i < names.size(); ++i)
                    asioBox.addItem (names[i], i + 1);
                asioDrivers = names;
                sourceDevice.clear();
                destinationDevice.clear();
                sourceDevice.addItem (L10n::tr ("(Motore principale)", "(Main engine)"), 1);
                destinationDevice.addItem (L10n::tr ("(Motore principale)", "(Main engine)"), 1);
                for (int i = 0; i < names.size(); ++i)
                {
                    sourceDevice.addItem (names[i], i + 2);
                    destinationDevice.addItem (names[i], i + 2);
                }
                sourceDevice.setSelectedId (1, juce::dontSendNotification);
                destinationDevice.setSelectedId (1, juce::dontSendNotification);
                if (names.size() > 0)
                {
                    int preferred = 1;
                    for (int i = 0; i < names.size(); ++i)
                        if (names[i].containsIgnoreCase ("Universal Audio")
                            || names[i].containsIgnoreCase ("ReaRoute"))
                            preferred = i + 1;
                    asioBox.setSelectedId (preferred, juce::dontSendNotification);
                }
            }

            wasapiEndpoints.clear();
            if (auto wt = std::unique_ptr<juce::AudioIODeviceType> (
                    juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (
                        juce::WASAPIDeviceMode::shared)))
            {
                wt->scanForDevices();
                for (const auto& n : wt->getDeviceNames (false))
                    wasapiEndpoints.add (n);
                for (const auto& n : wt->getDeviceNames (true))
                    wasapiEndpoints.add (n);
            }

            sourceEndpoint.clear();
            destinationEndpoint.clear();
            for (int i = 0; i < wasapiEndpoints.size(); ++i)
            {
                sourceEndpoint.addItem (wasapiEndpoints[i], i + 1);
                destinationEndpoint.addItem (wasapiEndpoints[i], i + 1);
            }
            if (wasapiEndpoints.size() > 0)
            {
                sourceEndpoint.setSelectedId (1, juce::dontSendNotification);
                destinationEndpoint.setSelectedId (1, juce::dontSendNotification);
            }
        }

        //======================================================================
        juce::String uniqueId (const juce::String& prefix,
                               const std::vector<Source>& allSources,
                               const std::vector<Destination>& allDestinations,
                               const std::vector<Bus>& allBuses)
        {
            int n = 1;
            juce::String id;
            do
            {
                id = prefix + juce::String (n++);
            } while (std::any_of (allSources.begin(), allSources.end(), [&] (const Source& s) { return s.id == id; })
                     || std::any_of (allDestinations.begin(), allDestinations.end(), [&] (const Destination& d) { return d.id == id; })
                     || std::any_of (allBuses.begin(), allBuses.end(), [&] (const Bus& b) { return b.id == id; }));
            return id;
        }

        juce::String selectedText (const juce::ComboBox& box) const
        {
            return box.getText();
        }

        int deviceComboIndexFor (const juce::String& deviceId) const
        {
            if (deviceId.isEmpty()) return 1;
            const auto driver = deviceId.startsWith ("asio:") ? deviceId.substring (5) : deviceId;
            const int index = asioDrivers.indexOf (driver);
            return index >= 0 ? index + 2 : 1;
        }

        void fillSourceFields (int row)
        {
            const auto& s = sources[(size_t) row];
            sourceType.setSelectedId (sourceTypeId (s.type), juce::dontSendNotification);
            updateSourceFields();
            sourceName.setText (s.name, juce::dontSendNotification);
            sourceEnabled.setToggleState (s.enabled, juce::dontSendNotification);
            sourceGain.setText (juce::String (s.gainDb, 1), juce::dontSendNotification);
            sourcePan.setText (juce::String (s.pan, 2), juce::dontSendNotification);
            sourcePolarity.setToggleState (s.polarity, juce::dontSendNotification);
            sourceDevice.setSelectedId (deviceComboIndexFor (s.deviceId), juce::dontSendNotification);
            sourceChannels.setText (channelsToText (s.channels), juce::dontSendNotification);
            sourceEndpoint.setText (s.endpoint, juce::dontSendNotification);
            sourceProcess.setText (s.process, juce::dontSendNotification);
            selectedProcessTitle = s.processTitle;
            sourceProcessTree.setToggleState (true, juce::dontSendNotification);
            sourceDriverEndpoint.setSelectedId (juce::jlimit (1, NLMATRIX_MAX_ENDPOINTS,
                                                              s.driverEndpoint + 1),
                                                juce::dontSendNotification);
            for (int i = 0; i < (int) buses.size(); ++i)
                if (buses[(size_t) i].id == s.bus)
                    sourceBus.setSelectedItemIndex (i, juce::dontSendNotification);
        }

        void updateSelectedSource()
        {
            const int row = sourceList.getSelectedRow();
            if (row < 0 || row >= (int) sources.size())
            {
                setStatus (L10n::tr ("Seleziona una sorgente da aggiornare", "Select a source to update"));
                return;
            }

            Source s = sources[(size_t) row];
            s.type = sourceTypeKey (sourceType.getSelectedId());
            s.name = sourceName.getText().trim();
            if (s.name.isEmpty()) s.name = autoNodeName (true);
            s.endpoint = sourceEndpoint.getText();
            s.deviceId = sourceDevice.getSelectedId() <= 1
                             ? juce::String() : "asio:" + sourceDevice.getText();
            s.process = sourceProcess.getText().trim();
            s.processTitle = selectedProcessTitle;
            s.includeProcessTree = sourceProcessTree.getToggleState();
            s.driverEndpoint = juce::jmax (0, sourceDriverEndpoint.getSelectedId() - 1);
            s.channels = parseChannels (sourceChannels.getText());
            s.enabled = sourceEnabled.getToggleState();
            s.gainDb = sourceGain.getText().getFloatValue();
            s.pan = juce::jlimit (-1.0f, 1.0f, sourcePan.getText().getFloatValue());
            s.polarity = sourcePolarity.getToggleState();
            if (s.type == "bus")
            {
                const int busIndex = sourceBus.getSelectedItemIndex();
                if (busIndex < 0 || busIndex >= (int) buses.size())
                {
                    setStatus (L10n::tr ("La sorgente bus richiede un bus", "Bus source needs a bus"));
                    return;
                }
                s.bus = buses[(size_t) busIndex].id;
            }
            else
            {
                s.bus.clear();
            }

            if (s.type == "asio-in" && s.channels.empty())
            {
                setStatus (L10n::tr ("La sorgente ASIO richiede i canali", "ASIO input source needs channels"));
                return;
            }
            if ((s.type == "loopback" || s.type == "capture") && s.endpoint.isEmpty())
            {
                setStatus (L10n::tr ("Seleziona un endpoint Windows per la sorgente", "Select a Windows endpoint for the source"));
                return;
            }
            if (s.type == "process" && s.process.isEmpty())
            {
                setStatus (L10n::tr ("La sorgente processo richiede un nome processo", "Process source needs a process name"));
                return;
            }

            sources[(size_t) row] = s;
            refreshAll();
            setStatus (L10n::tr ("Sorgente aggiornata", "Source updated"));
        }

        void fillDestinationFields (int row)
        {
            const auto& d = destinations[(size_t) row];
            destinationType.setSelectedId (destinationTypeId (d.type), juce::dontSendNotification);
            updateDestinationFields();
            destinationName.setText (d.name, juce::dontSendNotification);
            destinationGain.setText (juce::String (d.gainDb, 1), juce::dontSendNotification);
            destinationDevice.setSelectedId (deviceComboIndexFor (d.deviceId), juce::dontSendNotification);
            destinationMonitor.setToggleState (d.monitor, juce::dontSendNotification);
            {
                int mixId = 1;
                for (int i = 0; i < (int) sources.size(); ++i)
                    if (! d.mixMinus.empty() && sources[(size_t) i].id == d.mixMinus.front())
                        mixId = i + 2;
                destinationMixMinus.setSelectedId (mixId, juce::dontSendNotification);
            }
            destinationChannels.setText (channelsToText (d.channels), juce::dontSendNotification);
            destinationEndpoint.setText (d.endpoint, juce::dontSendNotification);
            destinationDriverEndpoint.setSelectedId (juce::jlimit (1, NLMATRIX_MAX_ENDPOINTS,
                                                                   d.driverEndpoint + 1),
                                                       juce::dontSendNotification);
            destinationDriverTarget.setText (d.driverTargetMs > 0
                                                 ? juce::String (d.driverTargetMs)
                                                 : juce::String ("0"),
                                             juce::dontSendNotification);
        }

        void updateSelectedDestination()
        {
            const int row = destinationList.getSelectedRow();
            if (row < 0 || row >= (int) destinations.size())
            {
                setStatus (L10n::tr ("Seleziona una destinazione da aggiornare", "Select a destination to update"));
                return;
            }

            Destination d = destinations[(size_t) row];
            d.type = destinationTypeKey (destinationType.getSelectedId());
            d.name = destinationName.getText().trim();
            if (d.name.isEmpty()) d.name = autoNodeName (false);
            d.endpoint = destinationEndpoint.getText();
            d.deviceId = destinationDevice.getSelectedId() <= 1
                             ? juce::String() : "asio:" + destinationDevice.getText();
            d.monitor = destinationMonitor.getToggleState();
            d.mixMinus.clear();
            if (destinationMixMinus.getSelectedId() >= 2
                && destinationMixMinus.getSelectedId() - 2 < (int) sources.size())
                d.mixMinus.push_back (sources[(size_t) (destinationMixMinus.getSelectedId() - 2)].id);
            d.driverEndpoint = juce::jmax (0, destinationDriverEndpoint.getSelectedId() - 1);
            d.driverTargetMs = juce::jlimit (0, 500, destinationDriverTarget.getText().getIntValue());
            d.channels = parseChannels (destinationChannels.getText());
            d.gainDb = destinationGain.getText().getFloatValue();
            d.deviceId = destinationDevice.getSelectedId() <= 1
                             ? juce::String() : "asio:" + destinationDevice.getText();
            d.monitor = destinationMonitor.getToggleState();

            if (d.type == "asio-out" && d.channels.empty())
            {
                setStatus (L10n::tr ("La destinazione ASIO richiede i canali", "ASIO output destination needs channels"));
                return;
            }
            if (d.type == "render" && d.endpoint.isEmpty())
            {
                setStatus (L10n::tr ("Seleziona un endpoint Windows per la destinazione", "Select a Windows endpoint for the destination"));
                return;
            }

            destinations[(size_t) row] = d;
            refreshAll();
            setStatus (L10n::tr ("Destinazione aggiornata", "Destination updated"));
        }

        void updateSelectedBus()
        {
            const int row = busList.getSelectedRow();
            if (row < 0 || row >= (int) buses.size())
            {
                setStatus (L10n::tr ("Seleziona un bus da aggiornare", "Select a bus to update"));
                return;
            }
            const auto name = busName.getText().trim();
            if (name.isEmpty())
            {
                setStatus (L10n::tr ("Il nome del bus non può essere vuoto", "Bus name cannot be empty"));
                return;
            }
            buses[(size_t) row].name = name;
            buses[(size_t) row].gainDb = busGain.getText().getFloatValue();
            buses[(size_t) row].channels = juce::jlimit (1, 16, busChannels.getSelectedId());
            refreshAll();
            setStatus (L10n::tr ("Bus aggiornato", "Bus updated"));
        }

        void addSource()
        {
            Source s;
            s.id = uniqueId ("s", sources, destinations, buses);
            s.type = sourceTypeKey (sourceType.getSelectedId());
            s.name = sourceName.getText().trim();
            if (s.name.isEmpty())
                s.name = autoNodeName (true);
            s.endpoint = sourceEndpoint.getText();
            s.deviceId = sourceDevice.getSelectedId() <= 1
                             ? juce::String() : "asio:" + sourceDevice.getText();
            s.process = sourceProcess.getText().trim();
            s.processTitle = selectedProcessTitle;
            s.includeProcessTree = sourceProcessTree.getToggleState();
            s.bus = sourceBus.getText();
            s.driverEndpoint = juce::jmax (0, sourceDriverEndpoint.getSelectedId() - 1);
            s.channels = parseChannels (sourceChannels.getText());
            s.enabled = sourceEnabled.getToggleState();
            s.gainDb = sourceGain.getText().getFloatValue();
            s.pan = juce::jlimit (-1.0f, 1.0f, sourcePan.getText().getFloatValue());
            s.polarity = sourcePolarity.getToggleState();

            if (s.type == "asio-in" && s.channels.empty())
            {
                setStatus (L10n::tr ("La sorgente ASIO richiede i canali (es. 1,2)", "ASIO input source needs channels (e.g. 1,2)"));
                return;
            }
            if ((s.type == "loopback" || s.type == "capture") && s.endpoint.isEmpty())
            {
                setStatus (L10n::tr ("Seleziona un endpoint Windows per la sorgente", "Select a Windows endpoint for the source"));
                return;
            }
            if (s.type == "process" && s.process.isEmpty())
            {
                setStatus (L10n::tr ("La sorgente processo richiede un nome processo", "Process source needs a process name"));
                return;
            }
            if (s.type == "bus")
            {
                const int busIndex = sourceBus.getSelectedItemIndex();
                if (busIndex < 0 || busIndex >= (int) buses.size())
                {
                    setStatus (L10n::tr ("La sorgente bus richiede un bus", "Bus source needs a bus"));
                    return;
                }
                s.bus = buses[(size_t) busIndex].id;
            }

            sources.push_back (s);
            sourceName.clear();
            sourceChannels.clear();
            sourceProcess.clear();
            refreshAll();
            setStatus (L10n::tr ("Sorgente aggiunta", "Source added"));
        }

        void removeSelectedSource()
        {
            const int row = sourceList.getSelectedRow();
            if (row < 0 || row >= (int) sources.size())
                return;
            const auto id = sources[(size_t) row].id;
            sources.erase (sources.begin() + row);
            routes.erase (std::remove_if (routes.begin(), routes.end(),
                                          [&] (const Route& r) { return r.sourceId == id; }),
                          routes.end());
            refreshAll();
        }

        void addDestination()
        {
            Destination d;
            d.id = uniqueId ("d", sources, destinations, buses);
            d.type = destinationTypeKey (destinationType.getSelectedId());
            d.name = destinationName.getText().trim();
            if (d.name.isEmpty())
                d.name = autoNodeName (false);
            d.endpoint = destinationEndpoint.getText();
            d.deviceId = destinationDevice.getSelectedId() <= 1
                             ? juce::String() : "asio:" + destinationDevice.getText();
            d.monitor = destinationMonitor.getToggleState();
            d.mixMinus.clear();
            if (destinationMixMinus.getSelectedId() >= 2
                && destinationMixMinus.getSelectedId() - 2 < (int) sources.size())
                d.mixMinus.push_back (sources[(size_t) (destinationMixMinus.getSelectedId() - 2)].id);
            d.driverEndpoint = juce::jmax (0, destinationDriverEndpoint.getSelectedId() - 1);
            d.driverTargetMs = juce::jlimit (0, 500, destinationDriverTarget.getText().getIntValue());
            d.channels = parseChannels (destinationChannels.getText());
            d.gainDb = destinationGain.getText().getFloatValue();
            d.deviceId = destinationDevice.getSelectedId() <= 1
                             ? juce::String() : "asio:" + destinationDevice.getText();
            d.monitor = destinationMonitor.getToggleState();

            if (d.type == "asio-out" && d.channels.empty())
            {
                setStatus (L10n::tr ("La destinazione ASIO richiede i canali", "ASIO output destination needs channels"));
                return;
            }
            if (d.type == "render" && d.endpoint.isEmpty())
            {
                setStatus (L10n::tr ("Seleziona un endpoint Windows per la destinazione", "Select a Windows endpoint for the destination"));
                return;
            }

            destinations.push_back (d);
            destinationName.clear();
            destinationChannels.clear();
            refreshAll();
            setStatus (L10n::tr ("Destinazione aggiunta", "Destination added"));
        }

        void removeSelectedDestination()
        {
            const int row = destinationList.getSelectedRow();
            if (row < 0 || row >= (int) destinations.size())
                return;
            const auto id = destinations[(size_t) row].id;
            destinations.erase (destinations.begin() + row);
            routes.erase (std::remove_if (routes.begin(), routes.end(),
                                          [&] (const Route& r) { return r.destinationId == id; }),
                          routes.end());
            refreshAll();
        }

        void addBus()
        {
            Bus b;
            b.id = uniqueId ("b", sources, destinations, buses);
            b.name = busName.getText().trim();
            if (b.name.isEmpty())
                b.name = "Mix Bus " + juce::String (buses.size() + 1);
            b.channels = juce::jlimit (1, 16, busChannels.getSelectedId());
            b.gainDb = busGain.getText().getFloatValue();
            buses.push_back (b);
            busName.clear();
            refreshAll();
            setStatus (L10n::tr ("Bus aggiunto", "Bus added"));
        }

        void removeSelectedBus()
        {
            const int row = busList.getSelectedRow();
            if (row < 0 || row >= (int) buses.size())
                return;
            const auto id = buses[(size_t) row].id;
            buses.erase (buses.begin() + row);
            std::set<juce::String> removedBusSources;
            for (const auto& s : sources)
                if (s.type == "bus" && s.bus == id)
                    removedBusSources.insert (s.id);
            sources.erase (std::remove_if (sources.begin(), sources.end(),
                                           [&] (const Source& s)
                                           { return s.type == "bus" && removedBusSources.count (s.id) != 0; }),
                           sources.end());
            routes.erase (std::remove_if (routes.begin(), routes.end(),
                                          [&] (const Route& r)
                                          { return r.destinationId == id
                                                   || removedBusSources.count (r.sourceId) != 0; }),
                          routes.end());
            refreshAll();
            applyProfile();
        }

        juce::String routeSourceId() const
        {
            const int index = routeSource.getSelectedItemIndex();
            return index >= 0 && index < (int) sources.size() ? sources[(size_t) index].id : juce::String();
        }

        juce::String routeDestinationId() const
        {
            const int index = routeDestination.getSelectedItemIndex();
            const int destCount = (int) destinations.size();
            if (index >= 0 && index < destCount)
                return destinations[(size_t) index].id;
            const int busIndex = index - destCount;
            return busIndex >= 0 && busIndex < (int) buses.size() ? buses[(size_t) busIndex].id : juce::String();
        }

        void connectFromPatchbay (const juce::String& sourceId,
                                  const juce::String& destinationId)
        {
            // A bus is both a mixer input and a source. The router addresses
            // it through an explicit bus-source entry; create it on demand so
            // dragging from a bus node always produces a valid graph.
            bool sourceExists = false;
            for (const auto& s : sources)
                if (s.id == sourceId)
                    sourceExists = true;
            juce::String resolvedSource = sourceId;
            if (! sourceExists)
            {
                // The canvas identifies a bus by its bus id; the router needs
                // the explicit bus-source id. Reuse an existing bus-source
                // instead of creating duplicates every time a cable is added.
                bool busId = false;
                for (const auto& b : buses)
                    if (b.id == sourceId)
                        busId = true;
                if (busId)
                {
                    for (const auto& s : sources)
                        if (s.type == "bus" && s.bus == sourceId)
                        {
                            resolvedSource = s.id;
                            sourceExists = true;
                            break;
                        }
                    if (! sourceExists)
                        for (const auto& b : buses)
                            if (b.id == sourceId)
                            {
                                Source s;
                                s.id = "bus-src-" + b.id;
                                s.name = b.name;
                                s.type = "bus";
                                s.bus = b.id;
                                s.channels = { 0, 1 };
                                sources.push_back (s);
                                resolvedSource = s.id;
                                break;
                            }
                }
            }
            for (const auto& r : routes)
                if (r.sourceId == sourceId && r.destinationId == destinationId)
                {
                    setStatus (L10n::tr ("La rotta esiste già", "Route already exists"));
                    return;
                }
            Route r;
            r.sourceId = resolvedSource;
            r.destinationId = destinationId;
            r.gainDb = 0.0;
            r.enabled = true;
            routes.push_back (r);
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Collegato ", "Patched ") + sourceNameFor (sourceId) + " -> "
                       + destinationNameFor (destinationId));
        }

        void moveNodeInModel (const juce::String& kind, const juce::String& id,
                              float x, float y)
        {
            if (kind == "source")
                for (auto& s : sources) if (s.id == id) { s.x = x; s.y = y; }
            if (kind == "destination")
                for (auto& d : destinations) if (d.id == id) { d.x = x; d.y = y; }
            if (kind == "bus")
                for (auto& b : buses) if (b.id == id) { b.x = x; b.y = y; }
        }

        void deleteRouteAt (int index)
        {
            if (index < 0 || index >= (int) routes.size())
                return;
            routes.erase (routes.begin() + index);
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Cavo rimosso", "Cable removed"));
        }

        void changeRouteAt (int index, float gain, bool enabled)
        {
            if (index < 0 || index >= (int) routes.size())
                return;
            routes[(size_t) index].gainDb = gain;
            routes[(size_t) index].enabled = enabled;
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Rotta aggiornata dal vivo", "Route updated live"));
        }

        void reconnectRoute (const juce::String& source, const juce::String& oldDest,
                             const juce::String& newDest)
        {
            if (oldDest == newDest)
                return;
            for (const auto& r : routes)
                if (r.sourceId == source && r.destinationId == newDest)
                {
                    setStatus (L10n::tr ("La rotta esiste già", "Route already exists"));
                    return;
                }
            for (auto& r : routes)
                if (r.sourceId == source && r.destinationId == oldDest)
                {
                    r.destinationId = newDest;
                    refreshAll();
                    applyProfile();
                    setStatus (L10n::tr ("Destinazione del cavo cambiata", "Cable destination changed"));
                    return;
                }
        }

        void removeNode (const juce::String& kind, const juce::String& id)
        {
            if (kind == "source")
            {
                sources.erase (std::remove_if (sources.begin(), sources.end(),
                                               [&id] (const Source& s) { return s.id == id; }),
                               sources.end());
                routes.erase (std::remove_if (routes.begin(), routes.end(),
                                              [&id] (const Route& r) { return r.sourceId == id; }),
                              routes.end());
            }
            else if (kind == "destination")
            {
                destinations.erase (std::remove_if (destinations.begin(), destinations.end(),
                                                    [&id] (const Destination& d) { return d.id == id; }),
                                    destinations.end());
                routes.erase (std::remove_if (routes.begin(), routes.end(),
                                              [&id] (const Route& r) { return r.destinationId == id; }),
                              routes.end());
            }
            else if (kind == "bus")
            {
                buses.erase (std::remove_if (buses.begin(), buses.end(),
                                             [&id] (const Bus& b) { return b.id == id; }),
                             buses.end());
                std::set<juce::String> removedBusSources;
                for (const auto& s : sources)
                    if (s.type == "bus" && s.bus == id)
                        removedBusSources.insert (s.id);
                sources.erase (std::remove_if (sources.begin(), sources.end(),
                                               [&removedBusSources] (const Source& s)
                                               { return s.type == "bus"
                                                        && removedBusSources.count (s.id) != 0; }),
                               sources.end());
                routes.erase (std::remove_if (routes.begin(), routes.end(),
                                              [&id, &removedBusSources] (const Route& r)
                                              { return r.destinationId == id
                                                       || removedBusSources.count (r.sourceId) != 0; }),
                              routes.end());
            }
            else
            {
                routes.erase (std::remove_if (routes.begin(), routes.end(),
                                              [&id] (const Route& r)
                                              { return r.sourceId == id || r.destinationId == id; }),
                              routes.end());
            }
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Nodo rimosso; cavi associati puliti", "Node removed; associated cables cleaned"));
        }

        void setNodeEnabled (const juce::String& kind, const juce::String& id, bool enabled)
        {
            if (kind == "source")
                for (auto& s : sources) if (s.id == id) s.enabled = enabled;
            if (kind == "destination")
                for (auto& d : destinations) if (d.id == id) d.enabled = enabled;
            if (kind == "bus")
                for (auto& b : buses) if (b.id == id) b.enabled = enabled;
            refreshAll();
            applyProfile();
            setStatus (enabled ? L10n::tr ("Nodo attivo", "Node enabled")
                              : L10n::tr ("Nodo disattivato", "Node disabled"));
        }

        void renameNode (const juce::String& kind, const juce::String& id,
                         const juce::String& name)
        {
            if (name.isEmpty())
                return;
            if (kind == "source")
                for (auto& s : sources) if (s.id == id) s.name = name;
            if (kind == "destination")
                for (auto& d : destinations) if (d.id == id) d.name = name;
            if (kind == "bus")
                for (auto& b : buses) if (b.id == id) b.name = name;
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Nodo rinominato", "Node renamed"));
        }

        void addNodeFromCanvas (const juce::String& kind)
        {
            const int n = kind == "source" ? (int) sources.size()
                        : kind == "destination" ? (int) destinations.size()
                                                : (int) buses.size();
            if (kind == "source")
            {
                Source s;
                s.id = "src-" + juce::String (juce::Time::currentTimeMillis());
                s.name = (asioBox.getText().isNotEmpty() ? asioBox.getText() : "ASIO")
                         + L10n::utf8 (" · Input 1/2");
                s.type = "asio-in";
                s.channels = { 0, 1 };
                s.x = 40.0f; s.y = 40.0f + 82.0f * n;
                sources.push_back (s);
            }
            else if (kind == "destination")
            {
                Destination d;
                d.id = "dst-" + juce::String (juce::Time::currentTimeMillis());
                d.name = (asioBox.getText().isNotEmpty() ? asioBox.getText() : "ASIO")
                         + L10n::utf8 (" · Output 1/2");
                d.type = "asio-out";
                d.channels = { 0, 1 };
                d.x = 680.0f; d.y = 40.0f + 82.0f * n;
                destinations.push_back (d);
            }
            else
            {
                Bus b;
                b.id = "bus-" + juce::String (juce::Time::currentTimeMillis());
                b.name = "Mix Bus " + juce::String (n + 1);
                b.channels = 2;
                b.x = 360.0f; b.y = 40.0f + 82.0f * n;
                buses.push_back (b);
            }
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Nodo aggiunto — modifica il dispositivo nel pannello laterale", "Node added — edit its device in the side panel"));
        }

        void addRoute()
        {
            Route r;
            r.sourceId = routeSourceId();
            r.destinationId = routeDestinationId();
            r.gainDb = routeGain.getText().getDoubleValue();
            r.enabled = routeEnabled.getToggleState();
            if (r.sourceId.isEmpty() || r.destinationId.isEmpty())
            {
                setStatus (L10n::tr ("Seleziona sorgente e destinazione per la rotta", "Select source and destination for the route"));
                return;
            }
            routes.push_back (r);
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Rotta aggiunta", "Route added"));
        }

        void updateSelectedRoute()
        {
            const int row = routeList.getSelectedRow();
            if (row < 0 || row >= (int) routes.size())
            {
                setStatus (L10n::tr ("Seleziona una rotta da aggiornare", "Select a route to update"));
                return;
            }
            routes[(size_t) row].gainDb = routeGain.getText().getDoubleValue();
            routes[(size_t) row].enabled = routeEnabled.getToggleState();
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Rotta aggiornata", "Route updated"));
        }

        void removeSelectedRoute()
        {
            const int row = routeList.getSelectedRow();
            if (row < 0 || row >= (int) routes.size())
                return;
            routes.erase (routes.begin() + row);
            refreshAll();
            applyProfile();
            setStatus (L10n::tr ("Connessione rimossa", "Connection removed"));
        }

        //======================================================================
        juce::String sourceNameFor (const juce::String& id) const
        {
            for (const auto& s : sources)
                if (s.id == id)
                    return s.name;
            return id;
        }

        juce::String destinationNameFor (const juce::String& id) const
        {
            for (const auto& d : destinations)
                if (d.id == id)
                    return d.name;
            for (const auto& b : buses)
                if (b.id == id)
                    return "bus: " + b.name;
            return id;
        }

        void refreshAll()
        {
            sourceModel.rows.clear();
            for (const auto& s : sources)
                sourceModel.rows.add (s.name + "  [" + s.type + "]");
            sourceModel.rows.add ("");
            sourceList.updateContent();

            destinationModel.rows.clear();
            for (const auto& d : destinations)
                destinationModel.rows.add (d.name + "  [" + d.type + "]");
            destinationModel.rows.add ("");
            destinationList.updateContent();

            busModel.rows.clear();
            for (const auto& b : buses)
                busModel.rows.add (b.name + "  [" + b.id + "]");
            busModel.rows.add ("");
            busList.updateContent();

            routeModel.rows.clear();
            for (const auto& r : routes)
                routeModel.rows.add (sourceNameFor (r.sourceId) + " -> "
                                     + destinationNameFor (r.destinationId) + "  "
                                     + juce::String (r.gainDb, 1) + " dB"
                                     + (r.enabled ? "" : "  [off]"));
            routeList.updateContent();
            removeRouteButton.setEnabled (! routes.empty());
            patchbay.setData (sources, destinations, buses, routes);

            const auto currentSource = routeSource.getText();
            const auto currentDest = routeDestination.getText();
            routeSource.clear();
            routeDestination.clear();
            for (int i = 0; i < (int) sources.size(); ++i)
                routeSource.addItem (sources[(size_t) i].name, i + 1);
            for (int i = 0; i < (int) destinations.size(); ++i)
                routeDestination.addItem (destinations[(size_t) i].name, i + 1);
            for (int i = 0; i < (int) buses.size(); ++i)
                routeDestination.addItem (L10n::tr ("bus: ", "bus: ") + buses[(size_t) i].name,
                                          (int) destinations.size() + i + 1);
            if (currentSource.isNotEmpty()) routeSource.setText (currentSource, juce::dontSendNotification);
            if (currentDest.isNotEmpty()) routeDestination.setText (currentDest, juce::dontSendNotification);

            sourceBus.clear();
            for (int i = 0; i < (int) buses.size(); ++i)
                sourceBus.addItem (buses[(size_t) i].name, i + 1);
            if (buses.size() > 0)
                sourceBus.setSelectedId (1, juce::dontSendNotification);

            const int previousMixMinus = destinationMixMinus.getSelectedId();
            destinationMixMinus.clear (juce::dontSendNotification);
            destinationMixMinus.addItem (L10n::tr ("Mix-minus: nessuno", "Mix-minus: none"), 1);
            for (int i = 0; i < (int) sources.size(); ++i)
                destinationMixMinus.addItem (L10n::tr ("Escludi: ", "Exclude: ")
                                                 + sources[(size_t) i].name, i + 2);
            if (previousMixMinus > 0 && previousMixMinus <= (int) sources.size() + 1)
                destinationMixMinus.setSelectedId (previousMixMinus, juce::dontSendNotification);
            else
                destinationMixMinus.setSelectedId (1, juce::dontSendNotification);

            setStatus (L10n::tr ("Profilo: ", "Profile: ") + profileName.getText());
        }

        //======================================================================
        bool writeProfile (const juce::File& file, juce::String& error)
        {
            auto rootVar = juce::var (new juce::DynamicObject());
            auto* root = rootVar.getDynamicObject();
            root->setProperty ("version", 3);
            root->setProperty ("name", profileName.getText());
            root->setProperty ("asio", asioBox.getText());
            // Engine v2 buffering policy.  The router compares this object as
            // part of its topology key, so a change requires a restart.
            root->setProperty ("engine", engineSettings.toVar());
            juce::Array<juce::var> deviceArray;
            auto addDevice = [&deviceArray, this] (const juce::String& driver, bool master)
            {
                if (driver.isEmpty()) return;
                const auto format = formatForDriver (driver);
                auto deviceObj = juce::var (new juce::DynamicObject());
                auto* d = deviceObj.getDynamicObject();
                d->setProperty ("id", "asio:" + driver);
                d->setProperty ("backend", "asio");
                d->setProperty ("driver", driver);
                d->setProperty ("displayName", driver);
                // G4: the router applies these when it opens the device; they are
                // no longer write-only decorations (readProfile restores them).
                d->setProperty ("sampleRate", master ? preferredSampleRate
                                                     : (format.sampleRate > 0.0 ? format.sampleRate : 48000.0));
                d->setProperty ("buffer", master ? preferredBuffer : format.bufferSize);
                d->setProperty ("clockMaster", master);
                deviceArray.add (deviceObj);
            };
            const auto masterDriver = asioBox.getText().isNotEmpty()
                                          ? asioBox.getText()
                                          : (asioDrivers.isEmpty() ? juce::String() : asioDrivers[0]);
            if (masterDriver.isNotEmpty())
                addDevice (masterDriver, true);
            for (const auto& driver : asioDrivers)
                if (driver != masterDriver)
                    addDevice (driver, false);
            root->setProperty ("devices", deviceArray);

            juce::Array<juce::var> busArray;
            for (const auto& b : buses)
            {
                auto busObj = juce::var (new juce::DynamicObject());
                busObj.getDynamicObject()->setProperty ("id", b.id);
                busObj.getDynamicObject()->setProperty ("name", b.name);
                busObj.getDynamicObject()->setProperty ("enabled", b.enabled);
                busObj.getDynamicObject()->setProperty ("gainDb", b.gainDb);
                busObj.getDynamicObject()->setProperty ("muted", b.muted);
                busObj.getDynamicObject()->setProperty ("solo", b.solo);
                busObj.getDynamicObject()->setProperty ("channels", b.channels);
                busObj.getDynamicObject()->setProperty ("x", b.x);
                busObj.getDynamicObject()->setProperty ("y", b.y);
                busArray.add (busObj);
            }
            root->setProperty ("buses", busArray);

            juce::Array<juce::var> sourceArray;
            for (const auto& s : sources)
            {
                auto obj = juce::var (new juce::DynamicObject());
                auto* o = obj.getDynamicObject();
                o->setProperty ("id", s.id);
                o->setProperty ("name", s.name);
                o->setProperty ("type", s.type);
                if (s.deviceId.isNotEmpty()) o->setProperty ("deviceId", s.deviceId);
                if (s.endpoint.isNotEmpty()) o->setProperty ("endpoint", s.endpoint);
                if (s.process.isNotEmpty()) o->setProperty ("process", s.process);
                if (s.processTitle.isNotEmpty()) o->setProperty ("processTitle", s.processTitle);
                o->setProperty ("includeProcessTree", s.includeProcessTree);
                if (s.bus.isNotEmpty()) o->setProperty ("bus", s.bus);
                o->setProperty ("driverEndpoint", s.driverEndpoint);
                o->setProperty ("enabled", s.enabled);
                o->setProperty ("gainDb", s.gainDb);
                o->setProperty ("pan", s.pan);
                o->setProperty ("polarity", s.polarity);
                o->setProperty ("muted", s.muted);
                o->setProperty ("solo", s.solo);
                o->setProperty ("x", s.x);
                o->setProperty ("y", s.y);
                if (! s.channels.empty()) o->setProperty ("channels", channelsToVar (s.channels));
                if (s.type == "test")
                {
                    o->setProperty ("baseFrequency", s.baseFrequency);
                    o->setProperty ("level", s.testLevel);
                }
                sourceArray.add (obj);
            }
            root->setProperty ("sources", sourceArray);

            juce::Array<juce::var> destArray;
            for (const auto& d : destinations)
            {
                auto obj = juce::var (new juce::DynamicObject());
                auto* o = obj.getDynamicObject();
                o->setProperty ("id", d.id);
                o->setProperty ("name", d.name);
                o->setProperty ("type", d.type);
                if (d.deviceId.isNotEmpty()) o->setProperty ("deviceId", d.deviceId);
                if (d.endpoint.isNotEmpty()) o->setProperty ("endpoint", d.endpoint);
                o->setProperty ("driverEndpoint", d.driverEndpoint);
                if (d.driverTargetMs > 0)
                    o->setProperty ("driverTargetMs", d.driverTargetMs);
                o->setProperty ("gainDb", d.gainDb);
                o->setProperty ("muted", d.muted);
                o->setProperty ("monitor", d.monitor);
                o->setProperty ("solo", d.solo);
                o->setProperty ("enabled", d.enabled);
                o->setProperty ("x", d.x);
                o->setProperty ("y", d.y);
                if (! d.channels.empty()) o->setProperty ("channels", channelsToVar (d.channels));
                if (! d.mixMinus.empty())
                {
                    juce::Array<juce::var> excluded;
                    for (const auto& id : d.mixMinus) excluded.add (id);
                    o->setProperty ("mixMinus", excluded);
                }
                destArray.add (obj);
            }
            root->setProperty ("destinations", destArray);

            juce::Array<juce::var> routeArray;
            for (const auto& r : routes)
            {
                auto obj = juce::var (new juce::DynamicObject());
                auto* o = obj.getDynamicObject();
                o->setProperty ("source", r.sourceId);
                o->setProperty ("destination", r.destinationId);
                o->setProperty ("gainDb", r.gainDb);
                if (! r.channelMap.empty())
                {
                    juce::Array<juce::var> map;
                    for (int ch : r.channelMap) map.add (ch);
                    o->setProperty ("channelMap", map);
                }
                o->setProperty ("enabled", r.enabled);
                routeArray.add (obj);
            }
            root->setProperty ("routes", routeArray);

            if (! file.replaceWithText (juce::JSON::toString (rootVar, true)))
            {
                error = "Could not write " + file.getFullPathName();
                return false;
            }
            return true;
        }

        bool readProfile (const juce::File& file, juce::String& error)
        {
            const auto parsed = juce::JSON::parse (file);
            if (parsed.isVoid())
            {
                error = "Invalid JSON: " + file.getFullPathName();
                return false;
            }
            auto* root = parsed.getDynamicObject();
            if (root == nullptr)
            {
                error = "Profile root is not an object";
                return false;
            }

            sources.clear();
            destinations.clear();
            buses.clear();
            routes.clear();
            profileName.setText (root->getProperty ("name").toString(), juce::dontSendNotification);
            profileName.setCaretPosition (0); // show the start, not the scrolled end
            asioBox.setText (root->getProperty ("asio").toString(), juce::dontSendNotification);
            // A loaded profile carries the buffering policy it was saved with:
            // take it over so a later save does not silently reset it to
            // Balanced.  Profiles without an engine section keep the current
            // GUI/preferences settings.
            const auto engineSection = root->getProperty ("engine");
            if (! engineSection.isVoid())
            {
                engineSettings = EngineUiSettings::fromVar (engineSection);
                if (sessionPersistenceEnabled)
                    savePreferences();
            }


            // G4: restore ASIO device formats from devices[].  This is the mirror
            // of writeProfile and makes a saved profile authoritative for the
            // master engine rate/buffer and for the extra ASIO devices.
            bool devicesLoaded = false;
            if (auto* deviceArray = root->getProperty ("devices").getArray())
                for (const auto& item : *deviceArray)
                {
                    auto* o = item.getDynamicObject();
                    if (o == nullptr) continue;
                    const auto driver = o->getProperty ("driver").toString();
                    if (driver.isEmpty())
                        continue;
                    const double rate = (double) o->getProperty ("sampleRate");
                    const int buffer = (int) o->getProperty ("buffer");
                    DeviceFormatChoice choice;
                    choice.deviceId = o->getProperty ("id").toString().isNotEmpty()
                                          ? o->getProperty ("id").toString()
                                          : "asio:" + driver;
                    choice.driver = driver;
                    choice.sampleRate = rate > 0.0 ? rate : 48000.0;
                    choice.bufferSize = buffer;
                    deviceFormats[choice.deviceId] = choice;
                    devicesLoaded = true;
                    if ((bool) o->getProperty ("clockMaster")
                        || o->getProperty ("driver").toString() == root->getProperty ("asio").toString())
                    {
                        if (choice.sampleRate > 0.0) preferredSampleRate = choice.sampleRate;
                        if (choice.bufferSize > 0) preferredBuffer = choice.bufferSize;
                    }
                }

            if (auto* array = root->getProperty ("buses").getArray())
                for (const auto& item : *array)
                {
                    auto* o = item.getDynamicObject();
                    if (o == nullptr) continue;
                    Bus b;
                    b.id = o->getProperty ("id").toString();
                    b.name = o->getProperty ("name").toString();
                    const auto en = o->getProperty ("enabled");
                    b.enabled = en.isVoid() ? true : (bool) en;
                    const auto busGain = o->getProperty ("gainDb");
                    b.gainDb = busGain.isVoid() ? 0.0f : (float) (double) busGain;
                    b.muted = (bool) o->getProperty ("muted");
                    b.solo = (bool) o->getProperty ("solo");
                    const auto busChannelValue = o->getProperty ("channels");
                    if (! busChannelValue.isVoid()) b.channels = juce::jlimit (1, 32, (int) busChannelValue);
                    b.x = o->hasProperty ("x") ? (float) (double) o->getProperty ("x") : -1.0f;
                    b.y = o->hasProperty ("y") ? (float) (double) o->getProperty ("y") : -1.0f;
                    if (b.name.isEmpty()) b.name = b.id;
                    if (b.id.isNotEmpty()) buses.push_back (b);
                }

            if (auto* array = root->getProperty ("sources").getArray())
                for (const auto& item : *array)
                {
                    auto* o = item.getDynamicObject();
                    if (o == nullptr) continue;
                    Source s;
                    s.id = o->getProperty ("id").toString();
                    s.name = o->getProperty ("name").toString();
                    s.type = o->getProperty ("type").toString();
                    s.deviceId = o->getProperty ("deviceId").toString();
                    s.endpoint = o->getProperty ("endpoint").toString();
                    s.process = o->getProperty ("process").toString();
                    s.processTitle = o->getProperty ("processTitle").toString();
                    const auto tree = o->getProperty ("includeProcessTree");
                    s.includeProcessTree = tree.isVoid() ? true : (bool) tree;
                    s.bus = o->getProperty ("bus").toString();
                    s.driverEndpoint = (int) o->getProperty ("driverEndpoint");
                    const auto en = o->getProperty ("enabled");
                    s.enabled = en.isVoid() ? true : (bool) en;
                    const auto gain = o->getProperty ("gainDb");
                    s.gainDb = gain.isVoid() ? 0.0f : (float) (double) gain;
                    const auto pan = o->getProperty ("pan");
                    s.pan = pan.isVoid() ? 0.0f : juce::jlimit (-1.0f, 1.0f, (float) (double) pan);
                    s.polarity = (bool) o->getProperty ("polarity");
                    s.solo = (bool) o->getProperty ("solo");
                    s.muted = (bool) o->getProperty ("muted");
                    s.x = o->hasProperty ("x") ? (float) (double) o->getProperty ("x") : -1.0f;
                    s.y = o->hasProperty ("y") ? (float) (double) o->getProperty ("y") : -1.0f;
                    if (s.name.isEmpty()) s.name = s.id;
                    if (auto* chans = o->getProperty ("channels").getArray())
                        for (const auto& c : *chans)
                            s.channels.push_back ((int) c);
                    const auto baseFrequency = o->getProperty ("baseFrequency");
                    if (! baseFrequency.isVoid()) s.baseFrequency = (double) baseFrequency;
                    const auto level = o->getProperty ("level");
                    if (! level.isVoid()) s.testLevel = (double) level;
                    if (s.id.isNotEmpty()) sources.push_back (s);
                }

            // Older test builds could create one bus-source per cable; keep a
            // single canonical source per bus when loading existing profiles.
            {
                std::set<juce::String> busSourceIds;
                sources.erase (std::remove_if (sources.begin(), sources.end(),
                                               [&busSourceIds] (const Source& s)
                                               {
                                                   if (s.type != "bus")
                                                       return false;
                                                   const auto key = s.bus.isNotEmpty() ? s.bus : s.id;
                                                   if (! busSourceIds.insert (key).second)
                                                       return true;
                                                   return false;
                                               }),
                               sources.end());
            }

            if (auto* array = root->getProperty ("destinations").getArray())
                for (const auto& item : *array)
                {
                    auto* o = item.getDynamicObject();
                    if (o == nullptr) continue;
                    Destination d;
                    d.id = o->getProperty ("id").toString();
                    d.name = o->getProperty ("name").toString();
                    d.type = o->getProperty ("type").toString();
                    d.deviceId = o->getProperty ("deviceId").toString();
                    d.endpoint = o->getProperty ("endpoint").toString();
                    d.driverEndpoint = (int) o->getProperty ("driverEndpoint");
                    d.driverTargetMs = juce::jlimit (0, 500, (int) o->getProperty ("driverTargetMs"));
                    const auto destGain = o->getProperty ("gainDb");
                    d.gainDb = destGain.isVoid() ? 0.0f : (float) (double) destGain;
                    d.monitor = (bool) o->getProperty ("monitor");
                    d.solo = (bool) o->getProperty ("solo");
                    d.muted = (bool) o->getProperty ("muted");
                    const auto en = o->getProperty ("enabled");
                    d.enabled = en.isVoid() ? true : (bool) en;
                    d.x = o->hasProperty ("x") ? (float) (double) o->getProperty ("x") : -1.0f;
                    d.y = o->hasProperty ("y") ? (float) (double) o->getProperty ("y") : -1.0f;
                    if (d.name.isEmpty()) d.name = d.id;
                    if (auto* chans = o->getProperty ("channels").getArray())
                        for (const auto& c : *chans)
                            d.channels.push_back ((int) c);
                    if (auto* excluded = o->getProperty ("mixMinus").getArray())
                        for (const auto& value : *excluded)
                            d.mixMinus.push_back (value.toString());
                    if (d.id.isNotEmpty()) destinations.push_back (d);
                }

            if (auto* array = root->getProperty ("routes").getArray())
                for (const auto& item : *array)
                {
                    auto* o = item.getDynamicObject();
                    if (o == nullptr) continue;
                    Route r;
                    r.sourceId = o->getProperty ("source").toString();
                    r.destinationId = o->getProperty ("destination").toString();
                    r.gainDb = (double) o->getProperty ("gainDb");
                    if (auto* map = o->getProperty ("channelMap").getArray())
                        for (const auto& value : *map)
                            r.channelMap.push_back ((int) value);
                    const auto enabled = o->getProperty ("enabled");
                    r.enabled = enabled.isVoid() ? true : (bool) enabled;
                    if (r.sourceId.isNotEmpty() && r.destinationId.isNotEmpty())
                        routes.push_back (r);
                }

            if (devicesLoaded && sessionPersistenceEnabled)
                savePreferences(); // keep preferences.json in sync with the profile
            refreshAll();
            return true;
        }

        void saveProfile()
        {
            auto initial = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile (profileName.getText() + ".json");
            activeChooser = std::make_unique<juce::FileChooser> ("Save NLMatrix profile", initial, "*.json");
            activeChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                            | juce::FileBrowserComponent::canSelectFiles
                                            | juce::FileBrowserComponent::warnAboutOverwriting,
                                        [this] (const juce::FileChooser& chooser)
                                        {
                                            const auto file = chooser.getResult();
                                            if (file != juce::File())
                                            {
                                                juce::String error;
                                                if (writeProfile (file, error))
                                                {
                                                    currentProfileFile = file;
                                                    setStatus (L10n::tr ("Salvato ", "Saved ") + file.getFileName());
                                                }
                                                else
                                                    setStatus (error);
                                            }
                                            activeChooser.reset();
                                        });
        }

        void loadProfile()
        {
            activeChooser = std::make_unique<juce::FileChooser> (
                "Load NLMatrix profile",
                juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                "*.json");
            activeChooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectFiles,
                                        [this] (const juce::FileChooser& chooser)
                                        {
                                            const auto file = chooser.getResult();
                                            if (file != juce::File())
                                            {
                                                stopRouter();
                                                juce::String error;
                                                if (readProfile (file, error))
                                                    setStatus (L10n::tr ("Caricato ", "Loaded ") + file.getFileName());
                                                else
                                                    setStatus (error);
                                            }
                                            activeChooser.reset();
                                        });
        }

        //======================================================================
        juce::File routerExecutable() const
        {
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
            // Installed layout: the router sits next to the GUI.
            auto candidate = exe.getParentDirectory().getChildFile ("NLMatrixRouter.exe");
            if (candidate.existsAsFile())
                return candidate;

            // Development layout: .../app/build/NLMatrixUI_artefacts/Release.
            const auto buildDir = exe.getParentDirectory().getParentDirectory().getParentDirectory();
            const auto release = buildDir.getChildFile ("NLMatrixRouter_artefacts").getChildFile ("Release");
            candidate = release.getChildFile ("NLMatrixRouter.exe");
            if (candidate.existsAsFile())
                return candidate;
            return juce::File::getCurrentWorkingDirectory()
                .getChildFile ("app").getChildFile ("build")
                .getChildFile ("NLMatrixRouter_artefacts").getChildFile ("Release")
                .getChildFile ("NLMatrixRouter.exe");
        }

        juce::File profileTempFile() const
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("nlmatrix_ui_config.json");
        }

        void logLine (const juce::String& text)
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("nlmatrix_ui.log");
            file.appendText (juce::Time::getCurrentTime().toString (true, true)
                             + " " + text + "\n");
        }

        juce::String topologyKey() const
        {
            juce::String key = asioBox.getText();
            for (const auto& s : sources)
                key << "|S:" << s.id << ':' << s.type << ':' << s.endpoint
                    << ':' << s.process << ':' << s.bus;
            for (const auto& d : destinations)
                key << "|D:" << d.id << ':' << d.type << ':' << d.endpoint
                    << ':' << d.driverTargetMs;
            // G4: device formats are part of the router topology (E3): a rate or
            // buffer change must restart the engine, never apply silently.
            for (const auto& [deviceId, choice] : deviceFormats)
                key << "|F:" << deviceId << ':' << choice.sampleRate << ':' << choice.bufferSize;
            for (const auto& b : buses)
                key << "|B:" << b.id;
            // Engine v2 buffering is part of the router topology key: changing
            // mode or tuning must restart the engine, never apply live.
            key << "|E:" << engineSettings.mode
                << ':' << engineSettings.ringTargetMinMs
                << '-' << engineSettings.ringTargetMaxMs
                << ':' << engineSettings.driverTargetMs
                << '-' << engineSettings.driverTargetMaxMs
                << ':' << engineSettings.extraAsioTargetMs
                << ':' << engineSettings.renderTargetMs
                << ':' << engineSettings.writeChunkMs
                << ':' << engineSettings.servoIntervalMs;
            return key;
        }

        void resetEngineTelemetry()
        {
            hasRouterBufferTelemetry = false;
            routerEngineState.clear();
            routerBufferMode.clear();
            hasRouterTuning = false;
            routerCallbackGapMaxMs = 0.0;
            hasRouterCallbackGap = false;
            routerFrameDeficit = 0;
            routerExpectedFrames = 0;
            hasRouterFrameStats = false;
            routerAsioRestarts = 0;
            routerDevices.clear();
            deviceFormatMismatchReported = false;
            lastFormatReport.clear();
        }

        void startRouter()
        {
            if (router != nullptr && router->isRunning())
                return;
            if (router != nullptr)
            {
                joinRouterReader();
                router.reset();
            }
            idleStopped = false;

            const auto temp = profileTempFile();
            juce::String error;
            if (! writeProfile (temp, error))
            {
                setStatus (error);
                return;
            }

            const auto exe = routerExecutable();
            if (! exe.existsAsFile())
            {
                setStatus (L10n::tr ("NLMatrixRouter.exe non trovato in ", "NLMatrixRouter.exe not found at ") + exe.getFullPathName());
                return;
            }

            router = std::make_unique<juce::ChildProcess>();
            juce::StringArray args;
            args.add (exe.getFullPathName());
            juce::String profileError;
            if (! saveLiveProfile (profileError))
            {
                setStatus (L10n::tr ("Impossibile preparare il profilo del router: ", "Cannot prepare router profile: ") + profileError);
                return;
            }
            args.add ("--config");
            args.add (temp.getFullPathName());
            telemetryFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("nlmatrix_router_telemetry.log");
            telemetryFile.deleteFile();
            routerLogFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("nlmatrix_router.log");
            routerLogFile.deleteFile();
            lastRouterLogLine.clear();
            args.add ("--telemetry");
            args.add (telemetryFile.getFullPathName());
            args.add ("--log");
            args.add (routerLogFile.getFullPathName());
            // E1: graceful stop channel. The GUI writes "stop" here; the router
            // fades destinations out and exits 0 instead of being hard-killed.
            routerControlFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("nlmatrix_router_control.txt");
            routerControlFile.deleteFile();
            args.add ("--control");
            args.add (routerControlFile.getFullPathName());
            logLine ("start " + exe.getFullPathName() + " --config " + temp.getFullPathName());
            if (! router->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            {
                setStatus (L10n::tr ("Impossibile avviare NLMatrixRouter: ", "Could not start NLMatrixRouter: ") + exe.getFullPathName());
                logLine ("start failed");
                router.reset();
                return;
            }
            logLine ("started");

            routerOutputBuffer.clear();
            routerReaderStop.store (false);
            routerReader = std::thread ([this]
            {
                char buffer[2048];
                while (! routerReaderStop.load())
                {
                    const int num = router != nullptr
                                        ? router->readProcessOutput (buffer, (int) sizeof (buffer))
                                        : 0;
                    if (num > 0)
                    {
                        const juce::ScopedLock lock (routerOutputLock);
                        routerOutputBuffer += juce::String::fromUTF8 (buffer, num);
                        if (routerOutputBuffer.length() > 131072)
                            routerOutputBuffer = routerOutputBuffer.substring (routerOutputBuffer.length() - 131072);
                    }
                    else
                    {
                        if (router == nullptr || ! router->isRunning())
                            break;
                        juce::Thread::sleep (5);
                    }
                }
            });

            meterStates.clear();
            resetEngineTelemetry();
            lastEngineMeterMs = 0.0;
            routerStartMs = juce::Time::getMillisecondCounterHiRes();
            engineHealthy = true;
            engineStallReported = false;
            recentRouterLines.clear();
            metersPanel.setRows ({});
            runningTopologyKey = topologyKey();
            setStatus (L10n::tr ("Router avviato", "Router started"));
            startStop.setButtonText (L10n::tr ("Ferma"));
        }

        void joinRouterReader()
        {
            routerReaderStop.store (true);
            if (routerReader.joinable())
                routerReader.join();
        }

        void stopRouter()
        {
            if (router != nullptr)
                stopRouterGracefully();
            runningTopologyKey.clear();
            meterStates.clear();
            resetEngineTelemetry();
            metersPanel.setRows ({});
            startStop.setButtonText (L10n::tr ("Avvia"));
        }

        void toggleRouter()
        {
            if (router != nullptr && router->isRunning())
            {
                stopRouter();
                setStatus (L10n::tr ("Router fermato", "Router stopped"));
            }
            else
            {
                startRouter();
            }
        }

        bool sessionPersistenceEnabled = true;

        void applyProfile()
        {
            saveLastSessionProfile();
            const bool wasRunning = router != nullptr && router->isRunning();
            if (! wasRunning)
            {
                juce::String error;
                if (! saveLiveProfile (error))
                    setStatus (error);
                else
                    setStatus (L10n::tr ("Profilo validato localmente; premi Start per eseguire", "Profile validated locally; press Start to run"));
                return;
            }

            const bool routeActive = std::any_of (routes.begin(), routes.end(),
                                                  [] (const Route& r) { return r.enabled; });
            if (wasRunning && ! routeActive)
            {
                stopRouter();
                idleStopped = true;
                setStatus (L10n::tr ("Motore in pausa: nessuna rotta attiva; dispositivi rilasciati", "Engine paused: no active route; devices released"));
                return;
            }
            if (! wasRunning && idleStopped && routeActive)
            {
                idleStopped = false;
                startRouter();
                setStatus (L10n::tr ("Motore ripreso: rotta attiva rilevata", "Engine resumed: active route detected"));
                return;
            }

            const bool topologyChanged = topologyKey() != runningTopologyKey;
            if (topologyChanged)
            {
                stopRouter();
                startRouter();
                setStatus (L10n::tr ("Topologia dispositivi cambiata: motore riavviato", "Device topology changed: engine restarted"));
                return;
            }

            juce::String error;
            if (! saveLiveProfile (error))
            {
                setStatus (error);
                return;
            }
            setStatus (L10n::tr ("Applicato dal vivo (nessun riavvio)", "Applied live (no restart)"));
        }

        void timerCallback() override
        {
            // C9: while the Theme is "System", follow the Windows light/dark
            // setting live.  This only polls the registry on the existing UI
            // timer; the audio/meter path is untouched.
            pollSystemTheme();
            if (router == nullptr)
                return;

            juce::String output;
            {
                const juce::ScopedLock lock (routerOutputLock);
                output = routerOutputBuffer;
                routerOutputBuffer.clear();
            }
            if (output.isNotEmpty())
            {
                juce::StringArray lines;
                lines.addLines (output);
                for (const auto& line : lines)
                {
                    if (line.startsWith ("METERS "))
                    {
                        applyMeterJson (line.substring (7));
                        continue;
                    }
                    if (line.startsWith ("callbacks ") && line.contains ("|"))
                        continue;
                    if (line.isNotEmpty())
                    {
                        setStatus (line);
                        recentRouterLines.add (line);
                        logLine ("router: " + line);
                        while (recentRouterLines.size() > 12)
                            recentRouterLines.remove (0);
                    }
                }
                refreshMeters();
            }

            if (routerLogFile.existsAsFile())
            {
                auto logText = routerLogFile.loadFileAsString();
                if (logText.length() > 16384)
                    logText = logText.substring (logText.length() - 16384);
                juce::StringArray logLines;
                logLines.addLines (logText);
                for (const auto& line : logLines)
                    if (line.isNotEmpty() && line != lastRouterLogLine)
                    {
                        lastRouterLogLine = line;
                        recentRouterLines.add (line);
                        logLine ("router log: " + line);
                        while (recentRouterLines.size() > 16)
                            recentRouterLines.remove (0);
                    }
            }

            if (telemetryFile.existsAsFile())
            {
                auto text = telemetryFile.loadFileAsString();
                if (text.length() > 65536)
                    text = text.substring (text.length() - 65536);
                juce::StringArray telemetryLines;
                telemetryLines.addLines (text);
                for (int i = telemetryLines.size(); --i >= 0;)
                    if (telemetryLines[i].startsWith ("METERS "))
                    {
                        applyMeterJson (telemetryLines[i].substring (7));
                        refreshMeters();
                        break;
                    }
            }

            if (! router->isRunning())
            {
                joinRouterReader();
                juce::String lateOutput;
                {
                    const juce::ScopedLock lock (routerOutputLock);
                    lateOutput = routerOutputBuffer;
                    routerOutputBuffer.clear();
                }
                if (lateOutput.isNotEmpty())
                {
                    juce::StringArray late;
                    late.addLines (lateOutput);
                    for (const auto& line : late)
                        if (line.isNotEmpty() && ! line.startsWith ("METERS "))
                            recentRouterLines.add (line);
                }
                const int exitCode = router->getExitCode();
                juce::String detail;
                for (int i = recentRouterLines.size(); --i >= 0;)
                    if (recentRouterLines[i].isNotEmpty()
                        && ! recentRouterLines[i].containsIgnoreCase ("callbacks"))
                    {
                        detail = recentRouterLines[i];
                        break;
                    }
                juce::String category;
                switch (exitCode)
                {
                    case 1: category = "ASIO non disponibile (tipo backend assente)"; break;
                    case 2: category = "dispositivo ASIO non trovato: " + asioBox.getText(); break;
                    case 3: category = "apertura dispositivo ASIO fallita"; break;
                    case 4: category = "avvio dispositivo ASIO fallito"; break;
                    case 5:
                    case 6: category = "formato/canali ASIO non disponibili"; break;
                    case 10: category = "endpoint capture non trovato"; break;
                    case 11:
                    case 12: category = "apertura sorgente capture fallita"; break;
                    case 13:
                    case 18: category = "tipo sorgente/destinazione non valido nel profilo"; break;
                    case 14: category = "endpoint render non trovato"; break;
                    case 16: category = "driver NLMatrixAudio non installato o endpoint assenti"; break;
                    case 19: category = "grafo di routing non valido"; break;
                    case 21: category = "avvio ASIO fallito"; break;
                    case 24: category = "formato dispositivo non disponibile (sample rate richiesto non supportato)"; break;
                    default: break;
                }
                juce::String message = L10n::tr ("Motore fermato (exit ", "Engine stopped (exit ")
                                       + juce::String (exitCode) + ")";
                if (category.isNotEmpty())
                    message << L10n::utf8 (" — ") << category;
                if (detail.isNotEmpty())
                    message << L10n::utf8 (" — ") << detail;
                logLine (message);
                setStatus (message);
                startStop.setButtonText (L10n::tr ("Avvia"));
                meterStates.clear();
                resetEngineTelemetry();
                metersPanel.setRows ({});
                router.reset();
            }
        }

        void applyMeterJson (const juce::String& json)
        {
            const auto parsed = juce::JSON::parse (json);
            if (auto* object = parsed.getDynamicObject())
            {
                lastEngineMeterMs = juce::Time::getMillisecondCounterHiRes();
                engineHealthy = ! object->hasProperty ("engineHealthy")
                                || (bool) object->getProperty ("engineHealthy");
                lastCallbackAgeMs = (double) object->getProperty ("lastCallbackAgeMs");
                framesProcessed = (juce::int64) object->getProperty ("framesProcessed");
                engineRate = (double) object->getProperty ("engineRate");
                engineBuffer = (int) object->getProperty ("engineBuffer");
                engineCallbacks = (juce::int64) object->getProperty ("callbacks");

                // Engine v2 buffering/health block.  Every field is optional so
                // an older router still drives the meters exactly as before.
                hasRouterBufferTelemetry = object->hasProperty ("engineState")
                                           || object->hasProperty ("bufferMode")
                                           || object->hasProperty ("callbackGapMaxMs")
                                           || object->hasProperty ("tuning");
                routerEngineState = object->getProperty ("engineState").toString();
                routerBufferMode = object->getProperty ("bufferMode").toString();
                hasRouterCallbackGap = object->hasProperty ("callbackGapMaxMs");
                routerCallbackGapMaxMs = hasRouterCallbackGap
                                             ? (double) object->getProperty ("callbackGapMaxMs") : 0.0;
                hasRouterFrameStats = object->hasProperty ("frameDeficit")
                                      && object->hasProperty ("expectedFrames");
                routerFrameDeficit = (juce::int64) object->getProperty ("frameDeficit");
                routerExpectedFrames = (juce::int64) object->getProperty ("expectedFrames");
                routerAsioRestarts = (int) object->getProperty ("asioRestarts");
                const auto tuningVar = object->getProperty ("tuning");
                hasRouterTuning = tuningVar.isObject();
                if (hasRouterTuning)
                {
                    routerTuning = EngineUiSettings::fromVar (tuningVar);
                    if (routerBufferMode.isEmpty())
                        routerBufferMode = routerTuning.mode;
                }

                if (auto* nodes = object->getProperty ("nodes").getArray())
                    for (const auto& item : *nodes)
                    {
                        auto* node = item.getDynamicObject();
                        if (node == nullptr) continue;
                        MeterState state;
                        state.l = (float) (double) node->getProperty ("l");
                        state.r = (float) (double) node->getProperty ("r");
                        state.clip = (bool) node->getProperty ("clip");
                        state.online = (bool) node->getProperty ("online");
                        state.dropped = (juce::int64) node->getProperty ("dropped");
                        state.underruns = (juce::int64) node->getProperty ("underruns");
                        state.kind = node->getProperty ("kind").toString();
                        state.type = node->getProperty ("type").toString();
                        state.writerStarved = (juce::int64) node->getProperty ("writerStarved");
                        state.writes = (juce::int64) node->getProperty ("writes");
                        state.driverDroppedBytes = (juce::int64) node->getProperty ("driverDroppedBytes");
                        state.driverUnderrunBytes = (juce::int64) node->getProperty ("driverUnderrunBytes");
                        state.hasBufferTelemetry = node->hasProperty ("ringTargetMs");
                        if (node->hasProperty ("ringFillMs"))
                            state.ringFillMs = (double) node->getProperty ("ringFillMs");
                        if (node->hasProperty ("ringTargetMs"))
                            state.ringTargetMs = (double) node->getProperty ("ringTargetMs");
                        if (node->hasProperty ("driverUsedMs"))
                            state.driverUsedMs = (double) node->getProperty ("driverUsedMs");
                        if (node->hasProperty ("driverTargetMs"))
                            state.driverTargetMs = (double) node->getProperty ("driverTargetMs");
                        if (node->hasProperty ("maxWakeMs"))
                            state.maxWakeMs = (double) node->getProperty ("maxWakeMs");
                        meterStates[node->getProperty ("id").toString()] = state;
                    }

                // G4: per-device format telemetry.  The router reports both the
                // requested and the actually opened rate/buffer; drivers may round
                // the buffer (never claim the requested value as active).
                routerDevices.clear();
                if (auto* devices = object->getProperty ("devices").getArray())
                    for (const auto& item : *devices)
                    {
                        auto* device = item.getDynamicObject();
                        if (device == nullptr) continue;
                        RouterDeviceTelemetry t;
                        t.id = device->getProperty ("id").toString();
                        t.backend = device->getProperty ("backend").toString();
                        t.driver = device->getProperty ("driver").toString();
                        t.clockMaster = (bool) device->getProperty ("clockMaster");
                        t.requestedRate = (double) device->getProperty ("requestedRate");
                        t.requestedBuffer = (int) device->getProperty ("requestedBuffer");
                        t.rate = (double) device->getProperty ("rate");
                        t.buffer = (int) device->getProperty ("buffer");
                        t.inputs = (int) device->getProperty ("inputs");
                        t.outputs = (int) device->getProperty ("outputs");
                        routerDevices.push_back (t);
                    }
                lastFormatReport = buildFormatReport();
                if (! deviceFormatMismatchReported)
                {
                    for (const auto& d : routerDevices)
                    {
                        const bool mismatch = d.requestedRate > 0.0
                                              && std::abs (d.requestedRate - d.rate) > 0.5;
                        const bool bufferRounded = d.requestedBuffer > 0 && d.buffer != d.requestedBuffer;
                        if (mismatch || bufferRounded)
                        {
                            deviceFormatMismatchReported = true;
                            const auto text = L10n::tr ("Formato dispositivo: richiesto ", "Device format: requested ")
                                              + juce::String (d.requestedRate, 0) + " Hz / "
                                              + juce::String (d.requestedBuffer) + " — "
                                              + L10n::tr ("attivo ", "active ")
                                              + juce::String (d.rate, 0) + " Hz / " + juce::String (d.buffer)
                                              + (mismatch ? L10n::tr (" (non supportato)", " (not supported)")
                                                          : L10n::tr (" (arrotondato dal driver)", " (rounded by the driver)"));
                            setStatus (text);
                            logLine (text);
                            break;
                        }
                    }
                }
            }
        }

        void refreshMeters()
        {
            std::vector<MeterRow> rows;
            auto add = [&rows, this] (const juce::String& id, const juce::String& name,
                                      const juce::String& kind, bool isOnline)
            {
                MeterRow row;
                row.id = id;
                row.name = name;
                row.kind = kind;
                const auto it = meterStates.find (id);
                if (it != meterStates.end())
                {
                    row.l = it->second.l;
                    row.r = it->second.r;
                    row.clip = it->second.clip;
                    isOnline = it->second.online;
                }
                row.online = isOnline && engineHealthy;
                rows.push_back (row);
            };
            for (const auto& s : sources)
                if (s.type != "bus")
                    add (s.id, s.name, s.type, true);
            for (const auto& b : buses)
                add (b.id, b.name, "bus", true);
            for (const auto& d : destinations)
                add (d.id, d.name, d.type, true);
            metersPanel.setRows (std::move (rows));

            std::map<juce::String, bool> online;
            for (const auto& s : sources)
                online[s.id] = true;
            for (const auto& b : buses)
                online[b.id] = true;
            for (const auto& d : destinations)
                online[d.id] = true;
            for (const auto& [id, state] : meterStates)
                online[id] = state.online;
            patchbay.setOnlineStates (online);
        }

        //======================================================================
        juce::TextEditor profileName;
        juce::ComboBox asioBox;
        juce::TextButton startStop, applyButton, saveButton, loadButton, refreshButton;
        juce::Label statusLabel;

        juce::Label sourceTitle, destinationTitle, busTitle, routeTitle, metersTitle;
        MeterPanel metersPanel;
        RowModel sourceModel, destinationModel, busModel, routeModel;
        juce::ListBox sourceList, destinationList, busList, routeList;

        juce::ComboBox sourceType, sourceEndpoint, sourceBus, sourceDriverEndpoint;
        juce::TextEditor sourceName, sourceChannels, sourceProcess;
        juce::TextButton addSourceButton, updateSourceButton, removeSourceButton, sourceHelpButton;
        juce::TextButton sourcePickChannels, sourcePickProcess;
        juce::TextEditor sourceGain, destinationGain, busGain, sourcePan;
        juce::ToggleButton sourcePolarity { "Invert polarity" };
        juce::ComboBox sourceDevice, destinationDevice;
        juce::ToggleButton destinationMonitor { "Solo monitor destination" };
        juce::ComboBox destinationMixMinus;
        juce::StringArray asioDrivers;
        juce::ToggleButton sourceEnabled;
        juce::ToggleButton sourceProcessTree;
        juce::String selectedProcessTitle;
        juce::Label asioLabel;

        juce::ComboBox destinationType, destinationEndpoint, destinationDriverEndpoint;
        juce::TextEditor destinationName, destinationChannels, destinationDriverTarget;
        juce::TextButton addDestinationButton, updateDestinationButton, removeDestinationButton,
                         destinationHelpButton, destinationPickChannels;

        juce::TextEditor busName;
        juce::ComboBox busChannels;
        juce::TextButton addBusButton, updateBusButton, removeBusButton, busHelpButton, asioHelpButton;
        juce::TextButton virtualDevicesButton;

        juce::ComboBox routeSource, routeDestination;
        juce::TextEditor routeGain;
        juce::ToggleButton routeEnabled;
        juce::TextButton addRouteButton, updateRouteButton, removeRouteButton;
        juce::Label routeSourceLabel, routeDestLabel, routeGainLabel;

        juce::StringArray wasapiEndpoints;

        std::vector<Source> sources;
        std::vector<Destination> destinations;
        std::vector<Bus> buses;
        std::vector<Route> routes;
        std::map<juce::String, MeterState> meterStates;
        juce::StringArray recentRouterLines;

        NeonLookAndFeel lookAndFeel;
        juce::Component leftContent;
        juce::Viewport leftViewport;
        // G2 collapsible panel state (persisted in preferences.json "ui").
        DisclosureButton leftPanelArrow { DisclosureButton::Direction::left };
        DisclosureButton sourceSectionArrow { DisclosureButton::Direction::down };
        DisclosureButton destinationSectionArrow { DisclosureButton::Direction::down };
        DisclosureButton busSectionArrow { DisclosureButton::Direction::down };
        bool leftPanelCollapsed = false;
        bool sourcesCollapsed = false;
        bool destinationsCollapsed = false;
        bool busesCollapsed = false;
        PatchbayCanvas patchbay;
        std::unique_ptr<juce::ChildProcess> router;
        bool idleStopped = false;
        std::thread routerReader;
        std::atomic<bool> routerReaderStop { false };
        juce::CriticalSection routerOutputLock;
        juce::String routerOutputBuffer;
        std::unique_ptr<juce::FileChooser> activeChooser;
        juce::File currentProfileFile, telemetryFile, routerLogFile, routerControlFile;
        // G4: ASIO device formats (key = "asio:<driver>"), router-reported device
        // telemetry and the requested-vs-actual mismatch report.
        std::map<juce::String, DeviceFormatChoice> deviceFormats;
        std::vector<RouterDeviceTelemetry> routerDevices;
        bool deviceFormatMismatchReported = false;
        juce::String lastFormatReport;
        MixerComponent mixerPanel;
        MixerResizer mixerResizer;
        ThemeMode themeMode = ThemeMode::dark;
        bool lastSystemLight = false;
        int themePollTicks = 0;
        bool engineHealthy = true;
        double lastCallbackAgeMs = 0.0;
        juce::int64 framesProcessed = 0;
        double lastEngineMeterMs = 0.0;
        bool engineStallReported = false;
        double routerStartMs = 0.0;
        bool mixerVisible = false;
        bool mixerConfigured = false;
        int mixerHeight = 360;
        double engineRate = 0.0;
        int engineBuffer = 0;
        juce::int64 engineCallbacks = 0;
        double preferredSampleRate = 48000.0;
        int preferredBuffer = 256;
        EngineUiSettings engineSettings;

        // Engine v2 buffering/health telemetry from METERS (empty/false when
        // the running router is an older build).
        bool hasRouterBufferTelemetry = false;
        juce::String routerEngineState, routerBufferMode;
        EngineUiSettings routerTuning;
        bool hasRouterTuning = false;
        double routerCallbackGapMaxMs = 0.0;
        bool hasRouterCallbackGap = false;
        juce::int64 routerFrameDeficit = 0, routerExpectedFrames = 0;
        bool hasRouterFrameStats = false;
        int routerAsioRestarts = 0;
        juce::String lastRouterLogLine, runningTopologyKey;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
    };

    //==============================================================================
    class MainWindow final : public juce::DocumentWindow, private juce::MenuBarModel
    {
    public:
        MainWindow()
            : juce::DocumentWindow ("NL Audio Matrix" + L10n::utf8 ("  —  ") + juce::String (nlmatrix::buildVersion())
                                    + L10n::utf8 ("  ·  ") + juce::String (nlmatrix::buildId()),
                                    Palette::background,
                                    juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setMenuBar (this);
            setResizable (true, true);
            setResizeLimits (900, 520, 4000, 3000);
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                const auto screen = display->userBounds.toNearestInt();
                const int w = juce::jlimit (900, 1600, screen.getWidth() - 24);
                const int h = juce::jlimit (520, 1000, screen.getHeight() - 56);
                setBounds (screen.withSizeKeepingCentre (w, h));
            }
            else
                centreWithSize (1600, 1000);
            if (auto* content = getMainComponent())
                content->restoreLastLayoutOnStartup();
            // A language switch rebuilds the native menu bar and window texts.
            L10n::languageChangedHook() = [this]
            {
                menuItemsChanged();
                if (tray != nullptr)
                    tray->refreshLocalisedTexts();
                repaint();
            };
            setVisible (true);
            tray = std::make_unique<TrayIcon> (*this);
        }

        ~MainWindow() override
        {
            L10n::languageChangedHook() = nullptr;
        }

        juce::StringArray getMenuBarNames() override
        {
            return { L10n::tr ("File"), L10n::tr ("Routing"), L10n::tr ("Dispositivi"),
                     L10n::tr ("Strumenti"), L10n::tr ("Vista"), L10n::tr ("Aiuto") };
        }

        juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String&) override
        {
            juce::PopupMenu menu;
            if (topLevelMenuIndex == 0)
            {
                menu.addItem (1001, L10n::tr ("Nuovo progetto"));
                menu.addItem (1002, L10n::tr ("Apri profilo..."));
                menu.addItem (1003, L10n::tr ("Salva profilo..."));
                menu.addSeparator();
                menu.addItem (1005, L10n::tr ("Esci"));
            }
            else if (topLevelMenuIndex == 1)
            {
                menu.addItem (2001, L10n::tr ("Aggiungi sorgente"));
                menu.addItem (2002, L10n::tr ("Aggiungi destinazione"));
                menu.addItem (2003, L10n::tr ("Aggiungi bus"));
                menu.addSeparator();
                menu.addItem (2004, L10n::tr ("Avvia / ferma motore"));
                menu.addItem (2005, L10n::tr ("Applica live"));
            }
            else if (topLevelMenuIndex == 2)
            {
                menu.addItem (3001, L10n::tr ("Aggiorna dispositivi"));
                menu.addItem (3002, L10n::tr ("Dispositivi virtuali..."));
                menu.addItem (3003, L10n::tr ("Info motore ASIO"));
                menu.addItem (3004, L10n::tr ("Browser dispositivi..."));
            }
            else if (topLevelMenuIndex == 3)
            {
                menu.addItem (6001, L10n::tr ("Diagnostica / telemetria..."));
                menu.addItem (6002, L10n::tr ("Preferenze..."));
            }
            else if (topLevelMenuIndex == 4)
            {
                menu.addItem (4001, L10n::tr ("Adatta canvas"));
                menu.addItem (4002, "Mixer...");
                menu.addItem (4003, L10n::tr ("Salva layout..."));
                menu.addItem (4004, L10n::tr ("Carica layout..."));
                menu.addItem (4005, L10n::tr ("Ripristina pannelli"));
                menu.addSeparator();
                menu.addItem (7001, L10n::tr ("Tema: Scuro"));
                menu.addItem (7002, L10n::tr ("Tema: Sistema"));
                menu.addItem (7003, L10n::tr ("Tema: Chiaro"));
                menu.addSeparator();
                menu.addItem (7004, L10n::tr ("Lingua: Italiano"), true,
                              L10n::currentLanguage() == L10n::Language::italian);
                menu.addItem (7005, L10n::tr ("Lingua: English"), true,
                              L10n::currentLanguage() == L10n::Language::english);
            }
            else if (topLevelMenuIndex == 5)
            {
                menu.addItem (5002, L10n::tr ("Scorciatoie da tastiera"));
                menu.addItem (5001, L10n::tr ("Info su NL Audio Matrix"));
            }
            return menu;
        }

        void menuItemSelected (int menuItemID, int) override
        {
            if (auto* main = getMainComponent())
                main->handleMenuCommand (menuItemID);
        }
        MainComponent* getMainComponent()
        {
            return dynamic_cast<MainComponent*> (getContentComponent());
        }

        void showWindow()
        {
            setVisible (true);
            toFront (true);
        }

        void performTrayAction (int result)
        {
            auto* main = getMainComponent();
            if (result == 1) showWindow();
            else if (result == 2) setVisible (false);
            else if (result == 3 && main != nullptr) main->startEngine();
            else if (result == 4 && main != nullptr) main->stopEngine();
            else if (result == 5)
            {
                if (main != nullptr) main->stopEngine();
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }
        }

        bool getTrayRect (RECT& rect) const
        {
#if JUCE_WINDOWS
            if (tray == nullptr)
                return false;
            auto* nid = static_cast<NOTIFYICONDATA*> (tray->getNativeHandle());
            if (nid == nullptr)
                return false;
            NOTIFYICONIDENTIFIER id {};
            id.cbSize = sizeof (id);
            id.hWnd = nid->hWnd;
            id.uID = nid->uID;
            return SUCCEEDED (Shell_NotifyIconGetRect (&id, &rect));
#else
            juce::ignoreUnused (rect);
            return false;
#endif
        }

        static bool clickNativeMenuItem (int itemIndex, int attempts = 30)
        {
#if JUCE_WINDOWS
            HWND menu = nullptr;
            for (int attempt = 0; attempt < attempts && menu == nullptr; ++attempt)
            {
                menu = FindWindowA ("#32768", nullptr);
                if (menu == nullptr)
                    juce::Thread::sleep (100);
            }
            if (menu == nullptr)
                return false;
            RECT rect {};
            GetWindowRect (menu, &rect);
            const int itemHeight = juce::jmax (18, GetSystemMetrics (SM_CYMENU));
            const int x = (rect.left + rect.right) / 2;
            const int y = rect.top + 6 + itemIndex * itemHeight + itemHeight / 2;
            SetCursorPos (x, y);
            mouse_event (MOUSEEVENTF_MOVE, 0, 0, 0, 0);
            mouse_event (MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            mouse_event (MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            return true;
#else
            juce::ignoreUnused (itemIndex);
            return false;
#endif
        }

        void openTrayMenuAndClick (int itemIndex)
        {
            auto* nid = static_cast<NOTIFYICONDATA*> (tray->getNativeHandle());
            if (nid == nullptr)
                return;
            RECT rect {};
            if (getTrayRect (rect))
            {
                SetCursorPos ((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
                mouse_event (MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
                mouse_event (MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            }
            const HWND trayHwnd = nid->hWnd;
            std::thread ([itemIndex, trayHwnd]
            {
                juce::Thread::sleep (700);
                bool actualClick = false;
                if (clickNativeMenuItem (itemIndex, 8))
                {
                    juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("nlmatrix_traytest.log")
                        .appendText ("PASS actual tray icon right-click opened native menu\n");
                    return;
                }
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("nlmatrix_traytest.log")
                    .appendText ("FALLBACK icon click did not open menu; using shell callback\n");
                PostMessageW (trayHwnd, WM_USER + 100, 0, WM_RBUTTONDOWN);
                juce::Thread::sleep (700);
                actualClick = clickNativeMenuItem (itemIndex, 20);
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("nlmatrix_traytest.log")
                    .appendText (actualClick ? "PASS fallback native menu clicked\n"
                                             : "FAIL native menu not found\n");
            }).detach();
        }

        void runTrayMenuTest()
        {
            auto log = [] (const juce::String& text)
            {
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("nlmatrix_traytest.log")
                    .appendText (text + "\n");
            };
            juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("nlmatrix_traytest.log").deleteFile();
            log ("NL Audio Matrix tray menu test");
            performTrayAction (1);

            juce::Timer::callAfterDelay (600, [this, log]
            {
                RECT rect {};
                if (! getTrayRect (rect))
                {
                    log ("FAIL tray icon rectangle not found");
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                    return;
                }
                log ("tray rect " + juce::String ((int) rect.left) + ","
                     + juce::String ((int) rect.top));
                openTrayMenuAndClick (1); // Nascondi
                juce::Timer::callAfterDelay (2600, [this, log]
                {
                    log (juce::String (! isVisible() ? "PASS " : "FAIL ")
                         + "tray menu Hide");
                    openTrayMenuAndClick (0); // Mostra
                    juce::Timer::callAfterDelay (2600, [this, log]
                    {
                        log (juce::String (isVisible() ? "PASS " : "FAIL ")
                             + "tray menu Show");
                        saveTraySnapshot();
                        log ("END");
                        juce::JUCEApplication::getInstance()->systemRequestedQuit();
                    });
                });
            });
        }

        void saveScreenSnapshot (const juce::String& name)
        {
#if JUCE_WINDOWS
            const int width = GetSystemMetrics (SM_CXSCREEN);
            const int height = GetSystemMetrics (SM_CYSCREEN);
            if (width <= 0 || height <= 0)
                return;
            HDC screen = GetDC (nullptr);
            HDC memory = CreateCompatibleDC (screen);
            HBITMAP bitmap = CreateCompatibleBitmap (screen, width, height);
            HGDIOBJ old = SelectObject (memory, bitmap);
            BitBlt (memory, 0, 0, width, height, screen, 0, 0, SRCCOPY);
            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            juce::Image image (juce::Image::ARGB, width, height, true);
            GetDIBits (memory, bitmap, 0, (UINT) height,
                       image.getPixelData(), &info, DIB_RGB_COLORS);
            SelectObject (memory, old);
            DeleteObject (bitmap);
            DeleteDC (memory);
            ReleaseDC (nullptr, screen);
            auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
            file.deleteFile();
            if (auto stream = file.createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (image, *stream);
            }
#else
            juce::ignoreUnused (name);
#endif
        }

        void saveTraySnapshot()
        {
            const auto snapshot = createComponentSnapshot (getLocalBounds());
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("nlmatrix_tray_snapshot.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (snapshot, *stream);
            }
        }

        void runVirtualDeviceTest()
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("nlmatrix_vdevtest.log");
            file.deleteFile();
            file.appendText ("NL Audio Matrix virtual device test\n");
            file.appendText ("Before:\n" + VirtualDevicesDialog::listWindowsEndpoints() + "\n");
            file.appendText ("Rename:\n" + VirtualDevicesDialog::normalizeWindowsNames() + "\n");
            file.appendText ("After:\n" + VirtualDevicesDialog::listWindowsEndpoints() + "\n");
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void runTrayTest()
        {
            juce::StringArray report;
            auto* main = getMainComponent();
            performTrayAction (1);
            report.add ((isVisible() ? "PASS " : "FAIL ") + juce::String ("tray Show action"));
            if (main != nullptr)
            {
                performTrayAction (3);
                juce::Thread::sleep (700);
                const bool running = main->isEngineRunning();
                performTrayAction (2);
                report.add ((! isVisible() ? "PASS " : "FAIL ") + juce::String ("tray Hide action"));
                report.add ((running ? "PASS " : "FAIL ") + juce::String ("engine survives window hide"));
                performTrayAction (1);
                closeButtonPressed();
                report.add ((! isVisible() ? "PASS " : "FAIL ")
                            + juce::String ("window close button hides instead of quitting"));
                performTrayAction (1);
                performTrayAction (4);
                report.add ((! main->isEngineRunning() ? "PASS " : "FAIL ")
                            + juce::String ("tray Start/Stop actions"));
            }
            else
            {
                report.add ("FAIL tray: no MainComponent");
            }
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("nlmatrix_selftest.log");
            file.appendText (report.joinIntoString ("\n") + "\n");
        }

        void closeButtonPressed() override
        {
            if (auto* content = getMainComponent())
                content->saveLastLayoutOnClose();
            // Keep the audio engine alive; the tray icon is the explicit way
            // to stop it or quit.
            setVisible (false);
        }

    private:
        struct TrayIcon final : public juce::SystemTrayIconComponent
        {
            explicit TrayIcon (MainWindow& w) : owner (w)
            {
                juce::Image image (juce::Image::ARGB, 32, 32, true);
                juce::Graphics g (image);
                g.setColour (Palette::background);
                g.fillRoundedRectangle (1.0f, 1.0f, 30.0f, 30.0f, 7.0f);
                g.setColour (Palette::violet.withAlpha (0.8f));
                g.drawRoundedRectangle (1.5f, 1.5f, 29.0f, 29.0f, 7.0f, 1.0f);
                g.setColour (Palette::magenta);
                g.fillRoundedRectangle (13.0f, 7.0f, 6.0f, 12.0f, 3.0f);
                g.setColour (Palette::cyan);
                juce::Path wave;
                wave.startNewSubPath (4.0f, 21.0f);
                wave.lineTo (9.0f, 21.0f);
                wave.lineTo (12.0f, 14.0f);
                wave.lineTo (16.0f, 26.0f);
                wave.lineTo (20.0f, 17.0f);
                wave.lineTo (23.0f, 21.0f);
                wave.lineTo (28.0f, 21.0f);
                g.strokePath (wave, juce::PathStrokeType (1.5f));
                setIconImage (image, image);
                refreshLocalisedTexts();
            }

            // C8: the tray tooltip follows the interface language.
            void refreshLocalisedTexts()
            {
                setIconTooltip (L10n::tr ("NL Audio Matrix — motore audio",
                                          "NL Audio Matrix — audio engine"));
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
                {
                    suppressMenuOnUp = true;
                    showNativeMenu();
                }
                else
                {
                    leftButtonWasDown = true;
                    owner.performTrayAction (1);
                }
            }

            void mouseUp (const juce::MouseEvent&) override
            {
                // Shell_NotifyIcon may deliver only WM_RBUTTONUP for a tray
                // right-click; JUCE maps that to mouseUp without a button flag.
                // A left click is already handled by mouseDown above.
                if (leftButtonWasDown)
                {
                    leftButtonWasDown = false;
                    return;
                }
                if (suppressMenuOnUp)
                {
                    suppressMenuOnUp = false;
                    return;
                }
                showNativeMenu();
            }

            void mouseDoubleClick (const juce::MouseEvent&) override
            {
                owner.performTrayAction (1);
            }

        private:
            void showNativeMenu()
            {
#if JUCE_WINDOWS
                auto* nid = static_cast<NOTIFYICONDATA*> (getNativeHandle());
                HWND hwnd = nid != nullptr ? nid->hWnd : nullptr;
                HMENU menu = CreatePopupMenu();
                AppendMenuW (menu, MF_STRING, 1, L10n::tr ("Mostra NL Audio Matrix").toWideCharPointer());
                AppendMenuW (menu, MF_STRING, 2, L10n::tr ("Nascondi finestra").toWideCharPointer());
                AppendMenuW (menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW (menu, MF_STRING, 3, L10n::tr ("Avvia engine audio").toWideCharPointer());
                AppendMenuW (menu, MF_STRING, 4, L10n::tr ("Ferma engine audio").toWideCharPointer());
                AppendMenuW (menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW (menu, MF_STRING, 5, L10n::tr ("Esci").toWideCharPointer());

                POINT point {};
                GetCursorPos (&point);
                if (hwnd != nullptr)
                    SetForegroundWindow (hwnd);
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("nlmatrix_traymenu.log")
                    .appendText ("menu open at " + juce::String (point.x) + ","
                                 + juce::String (point.y) + " hwnd="
                                 + juce::String ((juce::pointer_sized_int) hwnd) + "\n");
                const int command = TrackPopupMenu (menu,
                                                    TPM_RETURNCMD | TPM_NONOTIFY
                                                        | TPM_RIGHTBUTTON,
                                                    point.x, point.y, 0, hwnd, nullptr);
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("nlmatrix_traymenu.log")
                    .appendText ("menu command " + juce::String (command) + "\n");
                DestroyMenu (menu);
                if (hwnd != nullptr)
                    PostMessageW (hwnd, WM_NULL, 0, 0);
                if (command != 0)
                    owner.performTrayAction (command);
#else
                owner.performTrayAction (1);
#endif
            }

            MainWindow& owner;
            bool leftButtonWasDown = false;
            bool suppressMenuOnUp = false;
        };

        std::unique_ptr<TrayIcon> tray;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    class MatrixApplication final : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override { return "NL Audio Matrix"; }
        const juce::String getApplicationVersion() override { return "0.4.0-alpha"; }
        bool moreThanOneInstanceAllowed() override { return true; }

        void initialise (const juce::String&) override
        {
            const auto args = getCommandLineParameterArray();
            window = std::make_unique<MainWindow>();
            const bool testMode = args.contains ("--selftest") || args.contains ("--mixertest")
                                  || args.contains ("--layouttest") || args.contains ("--diagtest")
                                  || args.contains ("--devicetest") || args.contains ("--abouttest")
                                  || args.contains ("--preftest") || args.contains ("--traytest")
                                  || args.contains ("--guiverify");
            if (auto* content = window->getMainComponent(); testMode)
                content->setSessionPersistenceEnabled (false);

            const int verifyIndex = args.indexOf ("--guiverify");
            if (verifyIndex >= 0)
            {
                if (auto* content = window->getMainComponent())
                {
                    const auto outputDirectory = verifyIndex + 1 < args.size()
                                                     ? juce::File (args[verifyIndex + 1])
                                                     : juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                           .getChildFile ("nlmatrix_gui_verify");
                    content->runGuiVerify (outputDirectory);
                    return;
                }
            }
            const int profileIndex = args.indexOf ("--profile");
            if (profileIndex >= 0 && profileIndex + 1 < args.size())
                if (auto* content = window->getMainComponent())
                    content->loadProfileFile (juce::File (args[profileIndex + 1]));
            else if (auto* content = window->getMainComponent())
            {
                juce::Component::SafePointer<MainComponent> safe (content);
                juce::Timer::callAfterDelay (250, [safe] { if (safe != nullptr) safe->restoreLastSessionProfile(); });
            }

            if (args.contains ("--diagtest"))
                if (auto* content = window->getMainComponent())
                    content->openDiagnostics();

            if (args.contains ("--layouttest"))
                if (auto* content = window->getMainComponent())
                {
                    content->runLayoutTest();
                    setApplicationReturnValue (0);
                    quit();
                    return;
                }

            if (args.contains ("--preftest"))
                if (auto* content = window->getMainComponent())
                    content->openPreferences();

            if (args.contains ("--devicetest"))
                if (auto* content = window->getMainComponent())
                    content->openDeviceBrowser();

            for (const auto& arg : args)
            {
                if (arg.startsWith ("--theme="))
                {
                    const auto value = arg.substring (8).toLowerCase();
                    if (auto* content = window->getMainComponent())
                        content->setThemeMode (value == "light" ? ThemeMode::light
                                              : value == "system" ? ThemeMode::system : ThemeMode::dark);
                }
            }

            if (args.contains ("--abouttest"))
                new AboutWindow();

            if (args.contains ("--mixertest"))
                if (auto* content = window->getMainComponent())
                    content->openMixer();

            if (args.contains ("--selftest"))
            {
                if (auto* content = window->getMainComponent())
                    content->runSelfTest();
                window->runTrayTest();
                setApplicationReturnValue (0);
                quit();
                return;
            }

            if (args.contains ("--clicktest"))
            {
                if (auto* content = window->getMainComponent())
                    content->runRealInputTest();
                return;
            }

            if (args.contains ("--traytest"))
            {
                window->runTrayMenuTest();
                return;
            }

            if (args.contains ("--vdevtest"))
            {
                window->runVirtualDeviceTest();
                return;
            }

            if (args.contains ("--snapshot"))
            {
                auto* content = window->getMainComponent();
                if (content != nullptr)
                    content->startEngine();
                juce::Timer::callAfterDelay (2600, [content]
                {
                    if (content != nullptr)
                    {
                        content->saveSnapshot();
                        content->stopEngine();
                    }
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                });
                return;
            }

            if (args.contains ("--autostart"))
                if (auto* content = window->getMainComponent())
                    content->startEngine();
        }

        void shutdown() override
        {
            // Persist the last session on a real exit (tray Exit / File > Exit):
            // until now only "Apply live" wrote it, so a clean shutdown could
            // leave a stale file that no longer reflected the running profile.
            // Test modes disable session persistence, so automation is a no-op.
            if (window != nullptr)
                if (auto* content = window->getMainComponent())
                    content->saveLastSessionProfile();
            window.reset();
        }
        void systemRequestedQuit() override { quit(); }
        void anotherInstanceStarted (const juce::String&) override {}

    private:
        std::unique_ptr<MainWindow> window;
    };
} // namespace

START_JUCE_APPLICATION (MatrixApplication)
