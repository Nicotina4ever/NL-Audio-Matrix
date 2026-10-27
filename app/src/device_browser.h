#pragma once

struct DeviceBrowserItem
{
    juce::String category, name, detail, deviceId;
    juce::String sourceType, destinationType, endpoint;
    int driverEndpoint = 0;
};

class DeviceBrowserComponent final : public juce::Component, private juce::ListBoxModel
{
public:
    std::function<void (const DeviceBrowserItem&, bool asSource)> onChoose;
    std::function<void()> onClose;

    DeviceBrowserComponent (std::vector<DeviceBrowserItem> availableItems)
        : items (std::move (availableItems)), filtered (items)
    {
        addAndMakeVisible (search);
        search.setTextToShowWhenEmpty (L10n::tr ("Cerca dispositivo...", "Search device..."),
                                      Palette::muted);
        search.onTextChange = [this] { rebuild(); };

        addAndMakeVisible (target);
        target.addItem (L10n::tr ("Aggiungi come sorgente", "Add as source"), 1);
        target.addItem (L10n::tr ("Aggiungi come destinazione", "Add as destination"), 2);
        target.setSelectedId (1);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (38);
        list.setOutlineThickness (1);
        list.setColour (juce::ListBox::backgroundColourId, Palette::panel);
        list.setColour (juce::ListBox::outlineColourId, Palette::outline);

        addAndMakeVisible (chooseButton);
        chooseButton.setButtonText (L10n::tr ("Usa dispositivo", "Use device"));
        chooseButton.onClick = [this] { choose(); };

        addAndMakeVisible (closeButton);
        closeButton.setButtonText (L10n::tr ("Chiudi", "Close"));
        closeButton.onClick = [this] { if (onClose) onClose(); };

        setSize (620, 480);
        rebuild();
    }

    int getNumRows() override { return (int) filtered.size(); }

    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { choose(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (row < 0 || row >= (int) filtered.size()) return;
        const auto& item = filtered[(size_t) row];
        if (rowIsSelected) g.fillAll (Palette::magenta.withAlpha (0.18f));
        else if (row % 2 == 0) g.fillAll (Palette::panel.brighter (0.02f));

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (10, 4);
        g.setColour (Palette::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (item.name, area.removeFromTop (18), juce::Justification::centredLeft, 1);
        g.setColour (Palette::text.withAlpha (0.6f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText (item.category + "  ·  " + item.detail, area,
                          juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto top = area.removeFromTop (30);
        search.setBounds (top.removeFromLeft (juce::jmax (160, top.getWidth() - 230)));
        top.removeFromLeft (6);
        target.setBounds (top.removeFromLeft (220));
        area.removeFromTop (6);
        auto footer = area.removeFromBottom (32);
        chooseButton.setBounds (footer.removeFromLeft (160));
        footer.removeFromRight (6);
        closeButton.setBounds (footer.removeFromRight (80));
        list.setBounds (area);
    }

private:
    void rebuild()
    {
        const auto query = search.getText().trim().toLowerCase();
        filtered.clear();
        for (const auto& item : items)
            if (query.isEmpty()
                || item.name.toLowerCase().contains (query)
                || item.category.toLowerCase().contains (query)
                || item.detail.toLowerCase().contains (query))
                filtered.push_back (item);
        list.updateContent();
        if (! filtered.empty())
            list.selectRow (0);
        repaint();
    }

    void choose()
    {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= (int) filtered.size()) return;
        if (onChoose)
            onChoose (filtered[(size_t) row], target.getSelectedId() != 2);
    }

    std::vector<DeviceBrowserItem> items, filtered;
    juce::TextEditor search;
    juce::ComboBox target;
    juce::ListBox list;
    juce::TextButton chooseButton, closeButton;
};

class DeviceBrowserWindow final : public juce::DocumentWindow
{
public:
    explicit DeviceBrowserWindow (DeviceBrowserComponent* content)
        : juce::DocumentWindow ("NL Audio Matrix - "
                                    + L10n::tr ("Browser dispositivi", "Device browser"), Palette::background,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, true);
        setResizeLimits (480, 320, 1200, 900);
        centreWithSize (620, 480);
        setVisible (true);
    }

    void closeButtonPressed() override { delete this; }
};
