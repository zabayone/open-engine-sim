#ifndef ENGINE_SIM_TRAINER_PANEL_H
#define ENGINE_SIM_TRAINER_PANEL_H
#include "ui_button.h"
#include <array>
#include <string>
#include <vector>

// A native view of the Python trainer model. File access stays on the UI thread.
class TrainerPanel final : public UiElement {
public:
    void initialize(EngineSimApplication *app) override;
    void update(float dt) override;
    void render() override;
    void cancelEditing();
    void signal(UiElement *element, Event event) override;
    void onMouseScroll(int amount) override;
private:
    struct Row { std::string kind, key, label, value; };
    static constexpr int PageSize = 12;
    std::array<UiButton *, 4> m_navigation{};
    std::vector<Row> m_navigationRows;
    std::array<UiButton *, PageSize> m_buttons{};
    UiButton *m_previous = nullptr;
    UiButton *m_next = nullptr;
    std::vector<Row> m_rows;
    int m_offset = 0;
    std::string m_editKey, m_editValue, m_notice, m_lastView;
    float m_elapsed = 1.0f;
    void submit(const std::string &key, const std::string &value);
};
#endif
