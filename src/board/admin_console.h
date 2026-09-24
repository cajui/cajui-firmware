#pragma once
#include "cajui_provisioning.h"

namespace board {
// Answers CJ1 commands arriving line by line on USB serial. The same console serves
// both modes; the Provisioning object decides what each mode accepts.
class Console {
public:
    explicit Console(cajui::Provisioning& provisioning) : provisioning_(provisioning) {}
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    // Handles every complete line already received. True once a restart was requested.
    bool poll();

private:
    cajui::Provisioning& provisioning_;
    char line_[cajui::CommandCapacity]{};
    char reply_[cajui::ReplyCapacity]{};
    size_t used_ = 0;
    bool overflow_ = false;
};
// Consumes the request left by the previous boot: true means start in admin mode.
bool adminBootRequested();
// Restarts now; the next boot starts in admin mode if the console asked for it.
[[noreturn]] void restartFor(const cajui::Provisioning&);
} // namespace board
