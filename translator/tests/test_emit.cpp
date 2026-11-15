#include <string>

#include "dream/translator/emit.h"

#include "doctest.h"

using dream::translator::EmitOptions;
using dream::translator::FunctionSpec;
using dream::translator::Image;

namespace {

// A function that does nothing: `rts; nop` at 0x8C010000.
Image trivial_function() {
    Image img;
    img.base = 0x8C010000u;
    img.bytes = {0x0B, 0x00, 0x09, 0x00};
    return img;
}

std::string emit(bool irq_checks) {
    FunctionSpec spec;
    spec.entry = 0x8C010000u;
    spec.end = 0x8C010004u;
    EmitOptions options;
    options.irq_checks = irq_checks;
    return dream::translator::emit_unit(trivial_function(), {spec}, options).source;
}

}  // namespace

// An interrupt handler that switches tasks returns through RTE to SPC, and the runtime re-enters
// the function there. The poll at the top of __resume used to record the function's entry as SPC
// even when the function had been resumed in the middle, so a task switch taken at that poll
// restarted it from its prologue with the stack frame already pushed.
TEST_CASE("emit: the entry poll reports the resume address when there is one") {
    const std::string source = emit(true);
    const std::string poll = "if (c.cycles >= c.next_event) { c.pc = resume_pc ? resume_pc : ";
    const auto at = source.find(poll);
    REQUIRE(at != std::string::npos);
    // The plain entry (resume_pc == 0) still reports the function's own address.
    CHECK(source.compare(at + poll.size(), 10, "0x8c010000") == 0);
    CHECK(source.find("deliver_irq(c, m); }", at) != std::string::npos);
}

TEST_CASE("emit: no interrupt poll is emitted when irq_checks is off") {
    CHECK(emit(false).find("deliver_irq") == std::string::npos);
}
