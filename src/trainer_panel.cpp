#include "../include/trainer_panel.h"
#include "../include/engine_sim_application.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
struct TrainerLayout {
    float inset = 18.0f;
    float footer = 55.0f;
    float gap = 5.0f;
};
constexpr TrainerLayout layout{};
std::string hex(const std::string &value) {
    const char *digits = "0123456789abcdef";
    std::string result;
    for (unsigned char c : value) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
std::string unhex(const std::string &value) {
    std::string result;
    for (size_t i = 0; i + 1 < value.size(); i += 2)
        result += static_cast<char>(std::stoi(value.substr(i, 2), nullptr, 16));
    return result;
}
}

void TrainerPanel::initialize(EngineSimApplication *app) {
    UiElement::initialize(app);
    for (auto &button : m_navigation) button = addElement<UiButton>(this);
    for (auto &button : m_buttons) button = addElement<UiButton>(this);
    m_previous = addElement<UiButton>(this);
    m_next = addElement<UiButton>(this);
    m_previous->m_text = "PREVIOUS ROWS";
    m_next->m_text = "NEXT ROWS";
}

void TrainerPanel::submit(const std::string &key, const std::string &value) {
    try {
        const auto directory = std::filesystem::path(m_app->trainerSession());
        if (std::filesystem::exists(directory / "request")) {
            m_notice = "Please wait for the previous action.";
            return;
        }
        std::ofstream output(directory / "request.tmp");
        output << hex(key) << '\t' << hex(value);
        output.close();
        if (!output) throw std::runtime_error("Cannot write trainer request");
        std::filesystem::rename(directory / "request.tmp", directory / "request");
        m_notice.clear();
        if (key.rfind("page:", 0) == 0) m_offset = 0;
    } catch (const std::exception &error) { m_notice = error.what(); }
}

void TrainerPanel::update(float dt) {
    if (!isVisible()) return;
    m_elapsed += dt;
    auto *platform = m_app->getPlatform();
    if (!m_editKey.empty()) {
        const std::string input = platform->textInput();
        for (char c : input) {
            if (c == '\b') { if (!m_editValue.empty()) m_editValue.pop_back(); }
            else if (c >= ' ' && m_editValue.size() < 128) m_editValue += c;
        }
        if (platform->wasKeyPressed(DesktopKey::Return)) {
            submit(m_editKey, m_editValue);
            m_editKey.clear();
            platform->setTextInput(false);
        }
    }
    if (m_elapsed >= 0.2f && m_editKey.empty()) {
        m_elapsed = 0;
        try {
            std::ifstream input(std::filesystem::path(m_app->trainerSession()) / "view");
            std::string view((std::istreambuf_iterator<char>(input)), {});
            if (!view.empty() && view != m_lastView) {
                std::vector<Row> rows;
                std::istringstream lines(view);
                std::string line;
                while (std::getline(lines, line)) {
                    std::istringstream fields(line);
                    std::array<std::string, 4> values;
                    for (auto &value : values) { std::getline(fields, value, '\t'); value = unhex(value); }
                    rows.push_back({values[0], values[1], values[2], values[3]});
                }
                if (rows.size() >= m_navigation.size()) {
                    m_navigationRows.assign(rows.begin(), rows.begin() + m_navigation.size());
                    rows.erase(rows.begin(), rows.begin() + m_navigation.size());
                }
                m_rows = std::move(rows);
                m_lastView = view;
                m_offset = std::min(m_offset, std::max(0, static_cast<int>(m_rows.size()) - 1));
            }
        } catch (const std::exception &error) { m_notice = error.what(); }
    }
    const float scale = m_app->getScreenWidth() / 1440.0f;
    const float font = 22.0f * scale;
    const Bounds area = m_bounds.inset(layout.inset);
    for (size_t i = 0; i < m_navigation.size(); ++i) {
        auto *button = m_navigation[i];
        button->m_bounds = Bounds(area.width() / m_navigation.size() - layout.gap, 38 * scale,
            {area.left() + i * area.width() / m_navigation.size(), area.top()}, Bounds::tl);
        button->m_text = i < m_navigationRows.size() ? m_navigationRows[i].label : "";
        button->m_fontSize = 16 * scale;
    }
    const Bounds content(area.width(), area.height() - 48 * scale, {area.left(), area.top() - 48 * scale}, Bounds::tl);
    const float rowHeight = std::max(20.0f, (content.height() - layout.footer) / PageSize);
    for (int i = 0; i < PageSize; ++i) {
        auto *button = m_buttons[i];
        const int index = m_offset + i;
        button->setVisible(index < static_cast<int>(m_rows.size()));
        if (!button->isVisible()) continue;
        const auto &row = m_rows[index];
        button->m_bounds = Bounds(content.width(), rowHeight - layout.gap,
            {content.left(), content.top() - i * rowHeight}, Bounds::tl);
        button->m_text = row.label;
        if (row.kind == "edit") button->m_text += ": " + (row.key == m_editKey ? m_editValue + "_" : row.value);
        const float textWidth = m_app->getTextRenderer()->CalculateWidth(button->m_text, font);
        button->m_fontSize = std::min(font, font * (content.width() - 20) / std::max(1.0f, textWidth));
        button->m_drawFrame = row.kind != "text";
        button->m_inverted = false;
    }
    m_previous->m_bounds = Bounds(content.width() * 0.24f, 32 * scale, {content.left(), content.bottom()}, Bounds::bl);
    m_next->m_bounds = Bounds(content.width() * 0.24f, 32 * scale, {content.right(), content.bottom()}, Bounds::br);
    m_previous->m_fontSize = m_next->m_fontSize = 16 * scale;
    UiElement::update(dt);
}

void TrainerPanel::signal(UiElement *element, Event event) {
    if (event != Event::Clicked) return;
    for (size_t i = 0; i < m_navigation.size(); ++i) {
        if (element == m_navigation[i] && i < m_navigationRows.size()) {
            m_editKey.clear();
            m_app->getPlatform()->setTextInput(false);
            submit(m_navigationRows[i].key, m_navigationRows[i].value);
            return;
        }
    }
    if (element == m_previous) { onMouseScroll(1); return; }
    if (element == m_next) { onMouseScroll(-1); return; }
    for (int i = 0; i < PageSize; ++i) {
        if (element != m_buttons[i] || m_offset + i >= static_cast<int>(m_rows.size())) continue;
        const Row row = m_rows[m_offset + i];
        if (row.kind == "text") return;
        if (!m_editKey.empty()) {
            submit(m_editKey, m_editValue);
            m_editKey.clear();
            m_app->getPlatform()->setTextInput(false);
            return;
        }
        if (row.kind == "edit") {
            m_editKey = row.key;
            m_editValue.clear();
            m_notice = "Type replacement value; Enter saves. Blank clears optional values.";
            m_app->getPlatform()->setTextInput(true);
        } else if (row.kind == "save" || row.kind == "folder") {
            m_app->getPlatform()->chooseTrainerDestination(m_app->trainerSession(), row.key, row.value, row.kind == "folder");
        } else if (row.kind == "files") {
            m_app->getPlatform()->chooseTrainerFiles(m_app->trainerSession());
        } else submit(row.key, row.value);
        return;
    }
}

void TrainerPanel::onMouseScroll(int amount) {
    if (!m_editKey.empty()) return;
    m_offset = std::clamp(m_offset - amount * 6, 0, std::max(0, static_cast<int>(m_rows.size()) - PageSize));
}

void TrainerPanel::render() {
    if (!isVisible()) return;
    UiElement::render();
    const auto content = m_bounds.inset(layout.inset);
    const float scale = m_app->getScreenWidth() / 1440.0f;
    const std::string hint = m_notice.empty() ? "Scroll / row buttons for more" : m_notice;
    const float width = m_app->getTextRenderer()->CalculateWidth(hint, 14 * scale);
    drawCenteredText(hint, Bounds(content.width() * 0.5f, 30,
        {content.center_h(), content.bottom()}, Bounds::bm),
        std::min(14 * scale, 14 * scale * content.width() * 0.48f / std::max(1.0f, width)), Bounds::center);
}

void TrainerPanel::cancelEditing() {
    m_editKey.clear();
    m_notice.clear();
    m_app->getPlatform()->setTextInput(false);
}
