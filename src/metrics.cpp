#include "wss/metrics.hpp"

namespace wss {

std::ostream& operator<<(std::ostream& os, const RuntimeMetrics& m) {
    return os << "submitted=" << m.tasks_submitted << " completed=" << m.tasks_completed
              << " panicked=" << m.tasks_panicked << " stolen=" << m.tasks_stolen;
}

} // namespace wss
