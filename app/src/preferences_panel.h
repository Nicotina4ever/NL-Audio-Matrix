#pragma once

// Engine v2 buffering settings as edited by the GUI.
//
// The router reads an "engine" object from the profile JSON (v3):
//
//   "engine": {
//     "bufferMode": "low" | "balanced" | "safe" | "advanced",
//     "ringTargetMinMs": int, "ringTargetMaxMs": int,
//     "driverTargetMs": int, "driverTargetMaxMs": int,
//     "extraAsioTargetMs": int, "renderTargetMs": int,
//     "writeChunkMs": int, "servoIntervalMs": int
//   }
//
// Presets Low/Balanced/Safe use the documented millisecond profiles;
// "advanced" starts from Balanced and applies the explicit numeric fields.
// The two fields that the dialog does not expose (extra ASIO and render
// targets) are carried through so loading and saving a profile never resets
// them silently.
struct EngineUiSettings
{
    juce::String mode = "balanced";

    int ringTargetMinMs = 60;
    int ringTargetMaxMs = 200;
    int driverTargetMs = 50;
    int driverTargetMaxMs = 250;
    int extraAsioTargetMs = 50;
    int renderTargetMs = 50;
    int writeChunkMs = 5;
    int servoIntervalMs = 2;

    static constexpr int numModes = 4;

    static juce::String modeForIndex (int index)
    {
        switch (index)
        {
            case 0:  return "low";
            case 2:  return "safe";
            case 3:  return "advanced";
            default: return "balanced";
        }
    }

    static juce::String labelForMode (const juce::String& modeName)
    {
        if (modeName == "low")      return "Low";
        if (modeName == "safe")     return "Safe";
        if (modeName == "advanced") return "Avanzate";
        return "Balanced";
    }

    static int indexForMode (const juce::String& modeName)
    {
        for (int i = 0; i < numModes; ++i)
            if (modeForIndex (i) == modeName)
                return i;
        return 1;
    }

    static juce::String sanitiseMode (const juce::String& name)
    {
        const auto m = name.trim().toLowerCase();
        if (m == "low" || m == "lowlatency" || m == "low latency") return "low";
        if (m == "safe") return "safe";
        if (m == "advanced" || m == "avanzate") return "advanced";
        return "balanced";
    }

    static EngineUiSettings preset (const juce::String& name)
    {
        EngineUiSettings s;
        s.mode = sanitiseMode (name);

        if (s.mode == "low")
        {
            s.ringTargetMinMs = 20;
            s.ringTargetMaxMs = 60;
            s.driverTargetMs = 25;
            s.driverTargetMaxMs = 80;
            s.extraAsioTargetMs = 20;
            s.renderTargetMs = 20;
            s.writeChunkMs = 5;
            s.servoIntervalMs = 2;
        }
        else if (s.mode == "safe")
        {
            s.ringTargetMinMs = 150;
            s.ringTargetMaxMs = 400;
            s.driverTargetMs = 100;
            s.driverTargetMaxMs = 400;
            s.extraAsioTargetMs = 100;
            s.renderTargetMs = 100;
            s.writeChunkMs = 10;
            s.servoIntervalMs = 2;
        }
        else
        {
            // "balanced" and "advanced" share the Balanced numbers; advanced
            // will be overwritten by the numeric fields of the dialog.
            s.mode = s.mode == "advanced" ? "advanced" : "balanced";
        }
        return s;
    }

    // Reads an engine section from profile JSON (key "bufferMode") or the
    // router METERS "tuning" object (key "mode").  Missing object/keys fall
    // back to the Balanced preset, which keeps old preferences.json files and
    // old profiles fully compatible.
    static EngineUiSettings fromVar (const juce::var& section)
    {
        EngineUiSettings s = preset ("balanced");
        auto* o = section.getDynamicObject();
        if (o == nullptr)
            return s;

        auto get = [o] (const char* key) { return o->getProperty (juce::Identifier (key)); };

        auto modeVar = get ("bufferMode");
        if (modeVar.isVoid())
            modeVar = get ("mode");
        if (! modeVar.isVoid())
            s = preset (sanitiseMode (modeVar.toString()));

        auto overrideInt = [&] (const char* key, int& destination, int lo, int hi)
        {
            const auto v = get (key);
            if (v.isVoid())
                return;
            const int value = v.toString().getIntValue();
            if (value > 0) // 0 means "keep the preset value"
                destination = juce::jlimit (lo, hi, value);
        };

        overrideInt ("ringTargetMinMs", s.ringTargetMinMs, 5, 500);
        overrideInt ("ringTargetMaxMs", s.ringTargetMaxMs, s.ringTargetMinMs, 1000);
        overrideInt ("driverTargetMs", s.driverTargetMs, 10, 500);
        overrideInt ("driverTargetMaxMs", s.driverTargetMaxMs, s.driverTargetMs, 1000);
        overrideInt ("extraAsioTargetMs", s.extraAsioTargetMs, 5, 500);
        overrideInt ("renderTargetMs", s.renderTargetMs, 5, 500);
        overrideInt ("writeChunkMs", s.writeChunkMs, 1, 50);
        overrideInt ("servoIntervalMs", s.servoIntervalMs, 1, 20);
        return s;
    }

    // Emits exactly the engine object the router loadConfig() reads.
    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("bufferMode", mode);
        o->setProperty ("ringTargetMinMs", ringTargetMinMs);
        o->setProperty ("ringTargetMaxMs", ringTargetMaxMs);
        o->setProperty ("driverTargetMs", driverTargetMs);
        o->setProperty ("driverTargetMaxMs", driverTargetMaxMs);
        o->setProperty ("extraAsioTargetMs", extraAsioTargetMs);
        o->setProperty ("renderTargetMs", renderTargetMs);
        o->setProperty ("writeChunkMs", writeChunkMs);
        o->setProperty ("servoIntervalMs", servoIntervalMs);
        return juce::var (o);
    }

    // Validation shown to the user: ring 5..500, driver 10..500, max >= min,
    // chunk 1..50, servo 1..20.
    EngineUiSettings clampedToUiRanges() const
    {
        EngineUiSettings s = *this;
        s.ringTargetMinMs = juce::jlimit (5, 500, s.ringTargetMinMs);
        s.ringTargetMaxMs = juce::jlimit (s.ringTargetMinMs, 500, s.ringTargetMaxMs);
        s.driverTargetMs = juce::jlimit (10, 500, s.driverTargetMs);
        s.driverTargetMaxMs = juce::jlimit (s.driverTargetMs, 500, s.driverTargetMaxMs);
        s.writeChunkMs = juce::jlimit (1, 50, s.writeChunkMs);
        s.servoIntervalMs = juce::jlimit (1, 20, s.servoIntervalMs);
        return s;
    }

    bool operator== (const EngineUiSettings& other) const
    {
        return mode == other.mode
               && ringTargetMinMs == other.ringTargetMinMs
               && ringTargetMaxMs == other.ringTargetMaxMs
               && driverTargetMs == other.driverTargetMs
               && driverTargetMaxMs == other.driverTargetMaxMs
               && extraAsioTargetMs == other.extraAsioTargetMs
               && renderTargetMs == other.renderTargetMs
               && writeChunkMs == other.writeChunkMs
               && servoIntervalMs == other.servoIntervalMs;
    }
    bool operator!= (const EngineUiSettings& other) const { return ! (*this == other); }
};

class PreferencesComponent final : public juce::Component
{
public:
    std::function<void (double sampleRate, int bufferSize, bool rateAuto, EngineUiSettings engine,
                        std::vector<DeviceFormatChoice> extraFormats)> onApply;
    std::function<void (const juce::String& languageCode)> onLanguageChanged;
    std::function<void()> onClose;

    PreferencesComponent (double sampleRateIn, int bufferIn, EngineUiSettings engineIn,
                          std::vector<double> availableRatesIn,
                          std::vector<int> availableBuffersIn,
                          std::vector<DeviceFormatChoice> extraDevicesIn,
                          bool rateAutoIn = true)
        : initialRate (sampleRateIn), initialBuffer (bufferIn), initialRateAuto (rateAutoIn),
          engine (engineIn),
          availableRates (std::move (availableRatesIn)),
          availableBuffers (std::move (availableBuffersIn)),
          extraDevices (std::move (extraDevicesIn))
    {
        engine.mode = EngineUiSettings::sanitiseMode (engine.mode);
        engine = engine.clampedToUiRanges();

        addAndMakeVisible (title);
        L10n::bindLabelText (title, "PREFERENZE");
        title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, Palette::text);

        addAndMakeVisible (advanced);
        L10n::bindButtonText (advanced, "Modalità avanzata (sample rate e buffer manuali)");
        advanced.onClick = [this] { updateVisibility(); };

        addAndMakeVisible (latencyLabel);
        L10n::bindLabelText (latencyLabel, "Profilo latenza");
        addAndMakeVisible (latency);
        refreshLatencyItems();
        addAndMakeVisible (engineLabel);
        L10n::bindLabelText (engineLabel, "Buffer del motore");
        addAndMakeVisible (engineMode);
        refreshEngineModeItems();
        engineMode.setTooltip (L10n::tr ("Preset del ring buffer dei device (engine v2): "
                                        "Low = 20-60 ms, Balanced = 60-200 ms, Safe = 150-400 ms. "
                                        "Avanzate usa i valori numerici espliciti. "
                                        "Un cambio richiede il riavvio automatico del router.",
                                        "Device ring-buffer presets (engine v2): "
                                        "Low = 20-60 ms, Balanced = 60-200 ms, Safe = 150-400 ms. "
                                        "Advanced uses the explicit numeric values. "
                                        "Changing it restarts the router automatically."));
        engineMode.onChange = [this]
        {
            const auto selected = selectedEngineMode();
            if (selected != "advanced")
                setAdvancedFields (EngineUiSettings::preset (selected));
            updateVisibility();
        };

        // G4: the rate/buffer lists come from the real ASIO driver capabilities
        // (probed by the main window while the engine is stopped). If the driver
        // could not be probed, fall back to the standard rates/sizes and tell the
        // user that the value is verified only on apply.
        addAndMakeVisible (rateLabel);
        L10n::bindLabelText (rateLabel, "Sample rate del motore master");
        addAndMakeVisible (rate);
        {
            std::vector<double> rates = availableRates;
            if (rates.empty())
                rates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
            for (double r : rates)
                rate.addItem (juce::String (r, 0), juce::jmax (1, (int) r));
            rate.setSelectedId (juce::jmax (1, (int) sampleRateIn), juce::dontSendNotification);
            if (rate.getSelectedId() <= 0 && rate.getNumItems() > 0)
                rate.setSelectedItemIndex (0, juce::dontSendNotification);
        }

        addAndMakeVisible (bufferLabel);
        L10n::bindLabelText (bufferLabel, "Buffer ASIO (campioni)");
        addAndMakeVisible (buffer);
        {
            std::vector<int> buffers = availableBuffers;
            if (buffers.empty())
                buffers = { 32, 64, 128, 256, 512, 1024, 2048 };
            for (int b : buffers)
                buffer.addItem (juce::String (b), juce::jmax (1, b));
            buffer.setSelectedId (juce::jmax (1, bufferIn), juce::dontSendNotification);
            if (buffer.getSelectedId() <= 0 && buffer.getNumItems() > 0)
                buffer.setSelectedItemIndex (0, juce::dontSendNotification);
        }

        configureField (ringMinLabel, ringMinEditor, "Ring minimo (ms)",
                        "Fill target iniziale del ring del device, 5..500 ms.", engine.ringTargetMinMs);
        configureField (ringMaxLabel, ringMaxEditor, "Ring massimo (ms)",
                        "Limite massimo del ring adattivo, >= minimo e <= 500 ms.", engine.ringTargetMaxMs);
        configureField (driverMinLabel, driverMinEditor, "Driver minimo (ms)",
                        "Fill target del ring kernel NLMatrix, 10..500 ms.", engine.driverTargetMs);
        configureField (driverMaxLabel, driverMaxEditor, "Driver massimo (ms)",
                        "Limite massimo del ring kernel, >= minimo e <= 500 ms.", engine.driverTargetMaxMs);
        configureField (chunkLabel, chunkEditor, "Chunk scrittura (ms)",
                        "Audio massimo scritto per iterazione del servo, 1..50 ms.", engine.writeChunkMs);
        configureField (servoLabel, servoEditor, "Intervallo servo (ms)",
                        "Periodo del servo writer ad alta risoluzione, 1..20 ms.", engine.servoIntervalMs);

        // C8: language selector.  Switching applies immediately (the owner
        // persists the choice) so the main window, its menu bar and this dialog
        // refresh without restarting the application.
        addAndMakeVisible (languageLabel);
        L10n::bindLabelText (languageLabel, "Lingua");
        addAndMakeVisible (language);
        language.addItem ("Italiano", 1);
        language.addItem ("English", 2);
        language.setSelectedId (L10n::currentLanguage() == L10n::Language::english ? 2 : 1,
                                juce::dontSendNotification);
        language.setTooltip ("Lingua dell'interfaccia / Interface language");
        language.onChange = [this]
        {
            const auto code = language.getSelectedId() == 2 ? juce::String ("en")
                                                            : juce::String ("it");
            L10n::setLanguage (L10n::languageFromCode (code));
            if (onLanguageChanged) onLanguageChanged (code);
            L10n::notifyLanguageChanged();
            repaint();
        };

        addAndMakeVisible (note);
        L10n::bindLabelText (note, "Il profilo latenza consiglia il buffer ASIO; il buffer del motore è indipendente. "
                                  "Le modifiche al buffer del motore riavviano il router; sample rate/buffer ASIO riaprono il device.");
        note.setColour (juce::Label::textColourId, Palette::muted);
        note.setFont (juce::FontOptions (12.0f));

        // G4: additional ASIO devices referenced by sources/destinations.  The
        // master device is edited above; these rows carry the per-device rate and
        // buffer that are written to devices[] and applied by the router.
        addAndMakeVisible (formatsTitle);
        L10n::bindLabelText (formatsTitle, "Formato dispositivi ASIO aggiuntivi");
        formatsTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        if (extraDevices.empty())
        {
            addAndMakeVisible (formatsEmpty);
            L10n::bindLabelText (formatsEmpty, "Nessun dispositivo ASIO aggiuntivo nel profilo.");
            formatsEmpty.setColour (juce::Label::textColourId, Palette::muted);
            formatsEmpty.setFont (juce::FontOptions (12.0f));
        }
        for (auto& choice : extraDevices)
            createFormatRow (choice);
        if (availableRates.empty())
        {
            addAndMakeVisible (capabilityNote);
            L10n::bindLabelText (capabilityNote, "Capacità del driver non lette (motore attivo): i valori verranno verificati all'applicazione.");
            capabilityNote.setColour (juce::Label::textColourId, Palette::muted);
            capabilityNote.setFont (juce::FontOptions (11.5f));
        }

        addAndMakeVisible (apply);
        L10n::bindButtonText (apply, "Applica");
        apply.onClick = [this]
        {
            int b = buffer.getSelectedId();
            if (! advanced.getToggleState())
                b = latency.getSelectedId() == 1 ? 64
                    : latency.getSelectedId() == 3 ? 1024 : 256;
            const double r = advanced.getToggleState() && rate.getSelectedId() > 0
                                 ? (double) rate.getSelectedId() : initialRate;
            if (onApply) onApply (r, b, initialRateAuto, readEngineSettings(), readExtraFormats());
        };
        addAndMakeVisible (cancel);
        L10n::bindButtonText (cancel, "Annulla");
        cancel.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (close);
        L10n::bindButtonText (close, "Chiudi");
        close.onClick = [this] { if (onClose) onClose(); };

        L10n::bindAction ([safe = juce::Component::SafePointer<PreferencesComponent> (this)]
        {
            if (safe != nullptr) safe->refreshLatencyItems();
        });
        L10n::bindAction ([safe = juce::Component::SafePointer<PreferencesComponent> (this)]
        {
            if (safe != nullptr) safe->refreshEngineModeItems();
        });

        setSize (560, preferredHeight());
        updateVisibility();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        title.setBounds (area.removeFromTop (30));
        advanced.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);

        auto row = area.removeFromTop (30);
        latencyLabel.setBounds (row.removeFromLeft (190));
        latency.setBounds (row);

        area.removeFromTop (6);
        row = area.removeFromTop (30);
        engineLabel.setBounds (row.removeFromLeft (190));
        engineMode.setBounds (row);

        area.removeFromTop (6);
        row = area.removeFromTop (30);
        languageLabel.setBounds (row.removeFromLeft (190));
        language.setBounds (row);

        if (advanced.getToggleState())
        {
            area.removeFromTop (6);
            row = area.removeFromTop (30);
            rateLabel.setBounds (row.removeFromLeft (190));
            rate.setBounds (row);
            area.removeFromTop (6);
            row = area.removeFromTop (30);
            bufferLabel.setBounds (row.removeFromLeft (190));
            buffer.setBounds (row);
        }

        if (isEngineAdvanced())
        {
            area.removeFromTop (8);
            auto grid = area.removeFromTop (3 * fieldHeight + 2 * fieldGap);
            const int cellGap = 18;
            const int cellWidth = (grid.getWidth() - cellGap) / 2;
            auto layoutCell = [] (juce::Rectangle<int> cell, juce::Label& label,
                                  juce::TextEditor& editor)
            {
                const int labelWidth = juce::jmin (128, cell.getWidth() / 2);
                label.setBounds (cell.removeFromLeft (labelWidth));
                editor.setBounds (cell);
            };
            for (int i = 0; i < 3; ++i)
            {
                auto line = grid.removeFromTop (fieldHeight);
                auto leftCell = line.removeFromLeft (cellWidth);
                line.removeFromLeft (cellGap);
                auto rightCell = line;
                switch (i)
                {
                    case 0:
                        layoutCell (leftCell, ringMinLabel, ringMinEditor);
                        layoutCell (rightCell, ringMaxLabel, ringMaxEditor);
                        break;
                    case 1:
                        layoutCell (leftCell, driverMinLabel, driverMinEditor);
                        layoutCell (rightCell, driverMaxLabel, driverMaxEditor);
                        break;
                    default:
                        layoutCell (leftCell, chunkLabel, chunkEditor);
                        layoutCell (rightCell, servoLabel, servoEditor);
                        break;
                }
                grid.removeFromTop (fieldGap);
            }
        }

        area.removeFromTop (8);
        note.setBounds (area.removeFromTop (58));
        // G4: per-device format rows for the extra ASIO devices.
        area.removeFromTop (6);
        formatsTitle.setBounds (area.removeFromTop (24));
        if (! formatRows.empty())
        {
            for (auto& formatRow : formatRows)
            {
                auto line = area.removeFromTop (formatRowHeight);
                formatRow->label.setBounds (line.removeFromLeft (juce::jmax (120, line.getWidth() - 236)));
                line.removeFromLeft (6);
                formatRow->rate.setBounds (line.removeFromLeft (juce::jmin (110, line.getWidth() / 2)));
                line.removeFromLeft (6);
                formatRow->buffer.setBounds (line.removeFromLeft (juce::jmin (110, line.getWidth())));
            }
        }
        else if (formatsEmpty.isVisible())
        {
            formatsEmpty.setBounds (area.removeFromTop (22));
        }
        if (capabilityNote.isVisible())
        {
            area.removeFromTop (2);
            capabilityNote.setBounds (area.removeFromTop (32));
        }
        auto footer = area.removeFromBottom (30);
        apply.setBounds (footer.removeFromRight (100));
        footer.removeFromRight (6);
        cancel.setBounds (footer.removeFromRight (90));
        footer.removeFromRight (6);
        close.setBounds (footer.removeFromRight (80));
    }

private:
    static constexpr int fieldHeight = 28;
    static constexpr int fieldGap = 4;

    void configureField (juce::Label& label, juce::TextEditor& editor,
                         const char* text, const juce::String& tooltip, int value)
    {
        L10n::bindLabelText (label, text);
        label.setFont (juce::FontOptions (13.5f));
        label.setColour (juce::Label::textColourId, Palette::text.withAlpha (0.85f));
        addAndMakeVisible (label);

        editor.setInputRestrictions (4, "0123456789");
        editor.setJustification (juce::Justification::centred);
        editor.setText (juce::String (value), juce::dontSendNotification);
        editor.setTooltip (tooltip);
        addAndMakeVisible (editor);
    }

    // C8: combo item lists are rebuilt on a language switch so their labels
    // follow the interface language without reopening the dialog.
    void refreshLatencyItems()
    {
        const int selected = latency.getSelectedId() > 0 ? latency.getSelectedId() : 2;
        latency.clear (juce::dontSendNotification);
        latency.addItem (L10n::tr ("Bassa latenza"), 1);
        latency.addItem ("Balanced", 2);
        latency.addItem ("Safe", 3);
        latency.setSelectedId (juce::jlimit (1, 3, selected), juce::dontSendNotification);
        latency.setTooltip (L10n::tr ("Preset del buffer ASIO: Low latency 64 campioni, Balanced 256, Safe 1024. "
                                        "Non cambia il buffer del motore, che è indipendente.",
                                        "ASIO buffer preset: Low latency 64 samples, Balanced 256, Safe 1024. "
                                        "It does not change the engine buffer, which is independent."));
    }

    void refreshEngineModeItems()
    {
        const int selected = engineMode.getSelectedId() > 0
                                 ? engineMode.getSelectedId()
                                 : EngineUiSettings::indexForMode (engine.mode) + 1;
        engineMode.clear (juce::dontSendNotification);
        for (int i = 0; i < EngineUiSettings::numModes; ++i)
            engineMode.addItem (L10n::tr (EngineUiSettings::labelForMode (
                                    EngineUiSettings::modeForIndex (i))),
                                i + 1);
        engineMode.setSelectedId (juce::jlimit (1, EngineUiSettings::numModes, selected),
                                  juce::dontSendNotification);
    }

    // G4: one extra ASIO device format row.
    struct FormatRow
    {
        DeviceFormatChoice choice;
        juce::Label label;
        juce::ComboBox rate, buffer;
    };

    static constexpr int formatRowHeight = 30;

    void createFormatRow (const DeviceFormatChoice& choice)
    {
        auto row = std::make_unique<FormatRow>();
        row->choice = choice;
        row->label.setText (choice.driver + (choice.role.isNotEmpty()
                                                ? "  —  " + choice.role : juce::String()),
                            juce::dontSendNotification);
        row->label.setColour (juce::Label::textColourId, Palette::text);
        row->label.setFont (juce::FontOptions (12.5f));
        addAndMakeVisible (row->label);

        std::vector<double> rates = availableRates;
        if (rates.empty())
            rates = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        for (double r : rates)
            row->rate.addItem (juce::String (r, 0), juce::jmax (1, (int) r));
        row->rate.setSelectedId (juce::jmax (1, (int) (choice.sampleRate > 0.0 ? choice.sampleRate : 48000.0)),
                                 juce::dontSendNotification);
        addAndMakeVisible (row->rate);

        row->buffer.addItem ("Auto", 1);
        std::vector<int> buffers = availableBuffers;
        if (buffers.empty())
            buffers = { 32, 64, 128, 256, 512, 1024, 2048 };
        for (int b : buffers)
            row->buffer.addItem (juce::String (b), juce::jmax (2, b + 1));
        row->buffer.setSelectedId (choice.bufferSize > 0 ? choice.bufferSize + 1 : 1,
                                   juce::dontSendNotification);
        addAndMakeVisible (row->buffer);
        formatRows.push_back (std::move (row));
    }

    std::vector<DeviceFormatChoice> readExtraFormats() const
    {
        std::vector<DeviceFormatChoice> result;
        for (const auto& row : formatRows)
        {
            auto choice = row->choice;
            choice.sampleRate = (double) juce::jmax (1, row->rate.getSelectedId());
            const int bufferId = row->buffer.getSelectedId();
            choice.bufferSize = bufferId > 1 ? bufferId - 1 : 0;
            result.push_back (choice);
        }
        return result;
    }

    juce::String selectedEngineMode() const
    {
        const int index = engineMode.getSelectedId() - 1;
        if (index < 0 || index >= EngineUiSettings::numModes)
            return engine.mode;
        return EngineUiSettings::modeForIndex (index);
    }

    bool isEngineAdvanced() const { return selectedEngineMode() == "advanced"; }

    void setAdvancedFields (const EngineUiSettings& s)
    {
        ringMinEditor.setText (juce::String (s.ringTargetMinMs), juce::dontSendNotification);
        ringMaxEditor.setText (juce::String (s.ringTargetMaxMs), juce::dontSendNotification);
        driverMinEditor.setText (juce::String (s.driverTargetMs), juce::dontSendNotification);
        driverMaxEditor.setText (juce::String (s.driverTargetMaxMs), juce::dontSendNotification);
        chunkEditor.setText (juce::String (s.writeChunkMs), juce::dontSendNotification);
        servoEditor.setText (juce::String (s.servoIntervalMs), juce::dontSendNotification);
    }

    static int readInt (const juce::TextEditor& editor, int fallback)
    {
        const auto text = editor.getText().trim();
        return text.isEmpty() ? fallback : text.getIntValue();
    }

    EngineUiSettings readEngineSettings() const
    {
        auto s = EngineUiSettings::preset (selectedEngineMode());
        if (selectedEngineMode() == "advanced")
        {
            s.ringTargetMinMs = readInt (ringMinEditor, s.ringTargetMinMs);
            s.ringTargetMaxMs = readInt (ringMaxEditor, s.ringTargetMaxMs);
            s.driverTargetMs = readInt (driverMinEditor, s.driverTargetMs);
            s.driverTargetMaxMs = readInt (driverMaxEditor, s.driverTargetMaxMs);
            s.writeChunkMs = readInt (chunkEditor, s.writeChunkMs);
            s.servoIntervalMs = readInt (servoEditor, s.servoIntervalMs);
        }
        return s.clampedToUiRanges();
    }

    int preferredHeight() const
    {
        int h = 36;             // top + bottom padding
        h += 30;                // title
        h += 28;                // advanced toggle
        h += 8;
        h += 30 + 6;            // latency profile
        h += 30;                // engine buffer mode
        h += 30 + 6;            // language
        if (advanced.getToggleState())
            h += 2 * (6 + 30);  // sample rate + ASIO buffer
        if (isEngineAdvanced())
            h += 8 + 3 * fieldHeight + 2 * fieldGap;
        h += 8 + 58;            // note
        h += 6 + 24;            // formats title
        h += extraDevices.empty() ? 22 : (int) extraDevices.size() * formatRowHeight;
        if (availableRates.empty())
            h += 34;            // capability note
        h += 8 + 30;            // footer
        return juce::jlimit (330, 760, h);
    }

    void applyPreferredWindowSize()
    {
        const int h = preferredHeight();
        setSize (560, h);
        if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
            top->setSize (juce::jmax (520, top->getWidth()), h);
    }

    void updateVisibility()
    {
        const bool adv = advanced.getToggleState();
        rateLabel.setVisible (adv);
        rate.setVisible (adv);
        bufferLabel.setVisible (adv);
        buffer.setVisible (adv);
        latency.setEnabled (! adv);

        const bool engineAdv = isEngineAdvanced();
        for (juce::Component* c : { (juce::Component*) &ringMinLabel, (juce::Component*) &ringMinEditor,
                                    (juce::Component*) &ringMaxLabel, (juce::Component*) &ringMaxEditor,
                                    (juce::Component*) &driverMinLabel, (juce::Component*) &driverMinEditor,
                                    (juce::Component*) &driverMaxLabel, (juce::Component*) &driverMaxEditor,
                                    (juce::Component*) &chunkLabel, (juce::Component*) &chunkEditor,
                                    (juce::Component*) &servoLabel, (juce::Component*) &servoEditor })
            c->setVisible (engineAdv);

        resized();
        applyPreferredWindowSize();
    }

    double initialRate;
    int initialBuffer;
    bool initialRateAuto;
    EngineUiSettings engine;
    std::vector<double> availableRates;
    std::vector<int> availableBuffers;
    std::vector<DeviceFormatChoice> extraDevices;
    std::vector<std::unique_ptr<FormatRow>> formatRows;
    juce::Label title, latencyLabel, engineLabel, rateLabel, bufferLabel, note, languageLabel;
    juce::Label formatsTitle, formatsEmpty, capabilityNote;
    juce::Label ringMinLabel, ringMaxLabel, driverMinLabel, driverMaxLabel, chunkLabel, servoLabel;
    juce::ToggleButton advanced;
    juce::ComboBox latency, engineMode, rate, buffer, language;
    juce::TextEditor ringMinEditor, ringMaxEditor, driverMinEditor, driverMaxEditor,
                     chunkEditor, servoEditor;
    juce::TextButton apply, cancel, close;
};

class PreferencesWindow final : public juce::DocumentWindow
{
public:
    explicit PreferencesWindow (PreferencesComponent* content)
        : juce::DocumentWindow (L10n::tr ("NL Audio Matrix - Preferenze"), Palette::background,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        const int width = 560;
        int height = content != nullptr ? juce::jmax (330, content->getHeight()) : 330;
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            height = juce::jmin (height, juce::jmax (330, (int) display->userBounds.getHeight() - 80));
        if (content != nullptr)
            content->setSize (width, height);
        setContentOwned (content, true);
        setResizable (true, true);
        setResizeLimits (520, 300, 900, juce::jmax (height, 700));
        centreWithSize (width, height);
        setVisible (true);
    }
    void closeButtonPressed() override { delete this; }
};
