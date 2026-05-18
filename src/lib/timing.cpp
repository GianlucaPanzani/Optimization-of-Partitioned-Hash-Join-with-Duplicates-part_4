#include <chrono>
#include <cmath>

#include "timing.hpp"


double get_time() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}