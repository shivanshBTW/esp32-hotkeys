#pragma once

#include "lumos/core/logger.hpp"

namespace hotkeys {

// Placeholder for keypad matrix + action dispatch. Not implemented yet.
class HotkeysService {
public:
    void start() {
        lumos::Logger{"hotkeys"}.info("HotkeysService stub (keypad not implemented)");
    }

    bool enabled() const { return false; }
};

} // namespace hotkeys
