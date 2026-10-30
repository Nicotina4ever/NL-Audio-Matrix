#pragma once

// Studio surface: the bidirectional side of the product in one place.
//   * return paths  NL Return N  ->  ASIO output 1/2
//   * buses, routed to a destination
//   * per-destination monitoring and mix-minus (N-1) state
// The panel owns no audio model: it renders StudioState and asks MainComponent to
// perform each action, exactly like the mixer strips do.

struct StudioReturnInfo
{
    int endpointIndex = 0;
    juce::String label;
    bool active = false;
};

struct StudioBusInfo
{
    juce::String id, name;
    int routeCount = 0;
    bool enabled = true;
};

struct StudioMonitorInfo
{
    juce::String id, name;
    bool monitor = false;
    int mixMinusCount = 0;
};

struct StudioState
{
    std::vector<StudioReturnInfo> returns;
    std::vector<StudioBusInfo> buses;
    std::vector<StudioMonitorInfo> destinations;
    juce::StringArray asioOutputs;
};

class StudioComponent final : public juce::Component, private juce::Timer
{
public:
    std::function<StudioState()> getState;
    std::function<void (int endpointIndex)> onToggleReturn;
    std::function<void()> onAddBus;
    std::function<void (const juce::String& busId, const juce::String& destinationId)> onRouteBus;
    std::function<void (const juce::String& destinationId, bool monitor)> onMonitor;
    std::function<void (const juce::String& destinationId, juce::Component* anchor)> onMixMinus;
    std::function<void()> onClose;

    StudioComponent()
    {
        addAndMakeVisible (closeButton);
        closeButton.setButtonText ("X");
        closeButton.setTooltip (L10n::tr ("Chiudi Studio", "Close Studio"));
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        startTimerHz (2);
        refresh();
    }

    void resized() override
    {
        auto header = getLocalBounds().reduced (8).removeFromTop (26);
        closeButton.setBounds (header.removeFromRight (30));
        viewport.setBounds (getLocalBounds().reduced (8).withTrimmedTop (34));
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);
        g.setColour (Palette::text);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText (L10n::tr ("STUDIO — percorsi bidirezionali", "STUDIO — bidirectional paths"),
                    getLocalBounds().reduced (12).removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (Palette::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (L10n::tr ("Ritorni, bus, monitoraggio e mix-minus sul progetto corrente.",
                              "Returns, buses, monitoring and mix-minus on the current project."),
                    getLocalBounds().reduced (12).withTrimmedTop (22).removeFromTop (18),
                    juce::Justification::centredLeft);
    }

private:
    struct Row
    {
        enum class Kind { Title, ReturnRow, BusAdd, BusRow, MonitorRow };
        Kind kind = Kind::Title;
        juce::String text;
        juce::String id;   // bus id or destination id
        int endpointIndex = 0;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::TextButton> action;
        std::unique_ptr<juce::ComboBox> destinationChoice;
        std::unique_ptr<juce::ToggleButton> monitorToggle;
    };

    void timerCallback() override { refresh(); }

    void refresh()
    {
        if (! getState)
            return;
        const auto state = getState();
        // Cheap signature: rebuild only when the visible studio state changes, so
        // buttons do not fight the user's pointer every timer tick.
        juce::String signature;
        for (const auto& r : state.returns)
            signature << r.label << (r.active ? "1" : "0") << '|';
        for (const auto& b : state.buses)
            signature << b.id << b.name << b.routeCount << (b.enabled ? "1" : "0") << '|';
        for (const auto& d : state.destinations)
            signature << d.id << d.name << (d.monitor ? "1" : "0") << d.mixMinusCount << '|';
        signature << state.asioOutputs.joinIntoString (",");
        if (signature == lastSignature)
            return;
        lastSignature = signature;
        stateSnapshot = state;
        rebuild();
    }

    void addTitle (const juce::String& text)
    {
        auto row = std::make_unique<Row>();
        row->kind = Row::Kind::Title;
        row->text = text;
        row->label = std::make_unique<juce::Label> (juce::String(), text);
        row->label->setColour (juce::Label::textColourId, Palette::cyan);
        row->label->setFont (juce::FontOptions (13.0f, juce::Font::bold));
        content.addAndMakeVisible (*row->label);
        rows.push_back (std::move (row));
    }

    void rebuild()
    {
        // Delete the rows first (each owns its components, which detach from the
        // content component); the content then holds no stale children.
        rows.clear();
        content.removeAllChildren();

        addTitle (L10n::tr ("Ritorni: applicazioni Windows -> ASIO out 1/2",
                            "Returns: Windows applications -> ASIO out 1/2"));
        for (const auto& info : stateSnapshot.returns)
        {
            auto row = std::make_unique<Row>();
            row->kind = Row::Kind::ReturnRow;
            row->endpointIndex = info.endpointIndex;
            row->text = info.label;
            row->label = std::make_unique<juce::Label> (juce::String(),
                                                        info.label + (info.active
                                                                            ? L10n::tr ("  · attivo", "  · active")
                                                                            : L10n::tr ("  · assente", "  · absent")));
            row->label->setColour (juce::Label::textColourId,
                                   info.active ? Palette::success : Palette::muted);
            row->action = std::make_unique<juce::TextButton> (
                info.active ? L10n::tr ("Rimuovi ritorno", "Remove return")
                            : L10n::tr ("Crea ritorno", "Create return"));
            const int endpointIndex = info.endpointIndex;
            row->action->onClick = [this, endpointIndex]
            {
                if (onToggleReturn)
                    onToggleReturn (endpointIndex);
            };
            content.addAndMakeVisible (*row->label);
            content.addAndMakeVisible (*row->action);
            rows.push_back (std::move (row));
        }

        addTitle (L10n::tr ("Bus", "Buses"));
        {
            auto row = std::make_unique<Row>();
            row->kind = Row::Kind::BusAdd;
            row->action = std::make_unique<juce::TextButton> (L10n::tr ("Aggiungi bus", "Add bus"));
            row->action->onClick = [this] { if (onAddBus) onAddBus(); };
            content.addAndMakeVisible (*row->action);
            rows.push_back (std::move (row));
        }
        for (const auto& info : stateSnapshot.buses)
        {
            auto row = std::make_unique<Row>();
            row->kind = Row::Kind::BusRow;
            row->id = info.id;
            row->label = std::make_unique<juce::Label> (juce::String(),
                                                        info.name + "  · "
                                                            + juce::String (info.routeCount) + " "
                                                            + L10n::tr ("rotte", "routes"));
            row->label->setColour (juce::Label::textColourId,
                                   info.enabled ? Palette::text : Palette::muted);
            row->destinationChoice = std::make_unique<juce::ComboBox>();
            int destinationId = 1;
            for (const auto& d : stateSnapshot.destinations)
                row->destinationChoice->addItem (d.name, destinationId++);
            row->destinationChoice->setSelectedItemIndex (0, juce::dontSendNotification);
            row->action = std::make_unique<juce::TextButton> (L10n::tr ("Collega", "Connect"));
            const auto busId = info.id;
            auto* choice = row->destinationChoice.get();
            row->action->onClick = [this, busId, choice]
            {
                const int index = choice->getSelectedItemIndex();
                if (onRouteBus && index >= 0 && index < (int) stateSnapshot.destinations.size())
                    onRouteBus (busId, stateSnapshot.destinations[(size_t) index].id);
            };
            content.addAndMakeVisible (*row->label);
            content.addAndMakeVisible (*row->destinationChoice);
            content.addAndMakeVisible (*row->action);
            rows.push_back (std::move (row));
        }

        addTitle (L10n::tr ("Monitoraggio e mix-minus", "Monitoring and mix-minus"));
        for (const auto& info : stateSnapshot.destinations)
        {
            auto row = std::make_unique<Row>();
            row->kind = Row::Kind::MonitorRow;
            row->id = info.id;
            row->label = std::make_unique<juce::Label> (juce::String(), info.name);
            row->label->setColour (juce::Label::textColourId, Palette::text);
            row->monitorToggle = std::make_unique<juce::ToggleButton> (
                L10n::tr ("Monitor", "Monitor"));
            row->monitorToggle->setToggleState (info.monitor, juce::dontSendNotification);
            const auto destinationId = info.id;
            row->monitorToggle->onClick = [this, destinationId, toggle = row->monitorToggle.get()]
            {
                if (onMonitor)
                    onMonitor (destinationId, toggle->getToggleState());
            };
            row->action = std::make_unique<juce::TextButton> (
                "Mix-minus (" + juce::String (info.mixMinusCount) + ")");
            row->action->onClick = [this, destinationId, button = row->action.get()]
            {
                if (onMixMinus)
                    onMixMinus (destinationId, button);
            };
            content.addAndMakeVisible (*row->label);
            content.addAndMakeVisible (*row->monitorToggle);
            content.addAndMakeVisible (*row->action);
            rows.push_back (std::move (row));
        }

        layoutRows();
        repaint();
    }

    void layoutRows()
    {
        const int width = juce::jmax (200, viewport.getWidth() - 14);
        int y = 4;
        for (auto& row : rows)
        {
            const int h = row->kind == Row::Kind::Title ? 24 : 30;
            juce::Rectangle<int> line (4, y, width, h);
            switch (row->kind)
            {
                case Row::Kind::Title:
                    if (row->label != nullptr)
                        row->label->setBounds (line);
                    break;
                case Row::Kind::ReturnRow:
                    if (row->label != nullptr)
                        row->label->setBounds (line.removeFromLeft (width - 160));
                    if (row->action != nullptr)
                        row->action->setBounds (line.removeFromLeft (150));
                    break;
                case Row::Kind::BusAdd:
                    if (row->action != nullptr)
                        row->action->setBounds (line.removeFromLeft (150));
                    break;
                case Row::Kind::BusRow:
                    if (row->label != nullptr)
                        row->label->setBounds (line.removeFromLeft (width - 330));
                    if (row->destinationChoice != nullptr)
                        row->destinationChoice->setBounds (line.removeFromLeft (180).reduced (0, 3));
                    if (row->action != nullptr)
                        row->action->setBounds (line.removeFromLeft (140));
                    break;
                case Row::Kind::MonitorRow:
                    if (row->label != nullptr)
                        row->label->setBounds (line.removeFromLeft (width - 300));
                    if (row->monitorToggle != nullptr)
                        row->monitorToggle->setBounds (line.removeFromLeft (110));
                    if (row->action != nullptr)
                        row->action->setBounds (line.removeFromLeft (180));
                    break;
            }
            y += h + 2;
        }
        content.setSize (width + 8, juce::jmax (y + 8, viewport.getHeight()));
    }

    juce::Viewport viewport;
    juce::Component content;
    juce::TextButton closeButton;
    StudioState stateSnapshot;
    juce::String lastSignature;
    std::vector<std::unique_ptr<Row>> rows;
};

class StudioWindow final : public juce::DocumentWindow
{
public:
    std::function<void()> onClose;
    explicit StudioWindow (StudioComponent* content)
        : juce::DocumentWindow ("NL Audio Matrix - Studio", Palette::background,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, true);
        setResizeLimits (520, 320, 1600, 1200);
        centreWithSize (820, 460);
        setVisible (true);
    }
    void closeButtonPressed() override
    {
        if (onClose) onClose();
        delete this;
    }
};
