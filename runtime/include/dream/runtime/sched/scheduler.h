// Virtual clock and event queue (WP2.2). Time is SH-4 cycles at 200 MHz. Devices register an
// event once and re-arm it with a relative delay; `advance` moves the clock and runs due events in
// deadline order. Everything the runtime does on a timer goes through here, so a run is
// deterministic for a given guest instruction stream.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dream::state {
class Writer;
class Reader;
}  // namespace dream::state

namespace dream::sched {

constexpr std::uint64_t kSh4Clock = 200'000'000;
constexpr std::uint64_t kNever = ~std::uint64_t{0};

// `late` is how many cycles past the deadline the clock had moved when the event ran (0 when the
// scheduler stopped exactly on it; positive when a coarse `advance` overshot).
using Callback = std::function<void(std::uint64_t now, std::uint64_t late)>;

class Scheduler {
public:
    int add(std::string name, Callback cb);
    // Arms the event `cycles` from now; 0 runs it on the next advance; kNever disarms.
    void request(int id, std::uint64_t cycles);
    void cancel(int id) { request(id, kNever); }
    bool armed(int id) const noexcept {
        return events_[static_cast<std::size_t>(id)].deadline != kNever;
    }
    std::uint64_t deadline(int id) const noexcept {
        return events_[static_cast<std::size_t>(id)].deadline;
    }

    std::uint64_t now() const noexcept { return now_; }
    std::uint64_t next_deadline() const noexcept;  // kNever when nothing is armed
    // Runs every event whose deadline is <= target, in order, with now() set to each deadline,
    // then leaves now() == target.
    void advance_to(std::uint64_t target);
    void advance(std::uint64_t cycles) { advance_to(now_ + cycles); }
    // Save states (state/state.h): this device's registers and internal state.
    void save_state(state::Writer& w);
    void load_state(state::Reader& r);
    const std::string& name(int id) const { return events_[static_cast<std::size_t>(id)].name; }

private:
    struct Event {
        std::string name;
        Callback cb;
        std::uint64_t deadline = kNever;
    };
    std::vector<Event> events_;
    std::uint64_t now_ = 0;
};

}  // namespace dream::sched
