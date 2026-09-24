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
enum class BootRequest { None, Admin, Pair };
// Consumes the request left by the previous boot.
BootRequest takeBootRequest();
// Restarts now; the next boot honours the request (admin mode or radio pairing).
[[noreturn]] void restartInto(BootRequest);
// Restarts as the console asked: admin, pairing or plain operation.
[[noreturn]] void restartFor(const cajui::Provisioning&);
} // namespace board
