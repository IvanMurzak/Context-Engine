// input fail-closed refusals (M6 P7, R-QA-013 failure paths): the input.* code strings the contract
// error catalog registers, asserted at their source of truth (errors.h) — an invalid/duplicate
// context, an unknown context or action — is refused deterministically and leaves the router state
// untouched.

#include "context/packages/input/errors.h"
#include "context/packages/input/input_router.h"

#include "input_test.h"

#include <cstring>

using namespace context::packages::input;

namespace
{
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    // --- the code strings are the exact catalog identities (pins the input.* block) ---------------
    CHECK(std::strcmp(kInvalidContextCode, "input.invalid_context") == 0);
    CHECK(std::strcmp(kDuplicateContextCode, "input.duplicate_context") == 0);
    CHECK(std::strcmp(kUnknownContextCode, "input.unknown_context") == 0);
    CHECK(std::strcmp(kUnknownActionCode, "input.unknown_action") == 0);

    // --- invalid context: an empty id / malformed binding is refused, nothing installed ------------
    {
        InputRouter r;
        CHECK(same_code(r.install_context(InputContext{"", Layer::Gameplay, false, {}}),
                        kInvalidContextCode));
        CHECK(same_code(r.install_context(
                            InputContext{"c", Layer::Ui, false, {{"", "Escape", "ui_menu"}}}),
                        kInvalidContextCode));
        CHECK(same_code(r.install_context(
                            InputContext{"c", Layer::Ui, false, {{"keyboard", "Escape", ""}}}),
                        kInvalidContextCode));
        CHECK(same_code(r.install_context(
                            InputContext{"c", Layer::Gameplay, false, {{"pedal", "P", "brake"}}}),
                        kInvalidContextCode));
        CHECK(r.installed_count() == 0);
    }

    // --- duplicate context: the same id twice is refused, the first untouched ----------------------
    {
        InputRouter r;
        const InputContext ctx{"gameplay", Layer::Gameplay, false, {{"keyboard", "W", "move_y"}}};
        CHECK(r.install_context(ctx) == nullptr);
        CHECK(same_code(r.install_context(ctx), kDuplicateContextCode));
        CHECK(r.installed_count() == 1);
    }

    // --- unknown context: push a non-installed id / pop an empty stack ------------------------------
    {
        InputRouter r;
        CHECK(same_code(r.push_context("ghost"), kUnknownContextCode));
        CHECK(same_code(r.pop_context(), kUnknownContextCode)); // empty stack
        CHECK(r.stack_depth() == 0);
    }

    // --- unknown action / unknown context: rebind refusals -----------------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(
                  InputContext{"gameplay", Layer::Gameplay, false, {{"keyboard", "W", "move_y"}}}) ==
              nullptr);
        CHECK(same_code(r.rebind("ghost", "move_y", "keyboard", "Up"), kUnknownContextCode));
        CHECK(same_code(r.rebind("gameplay", "fire", "mouse", "MouseLeft"), kUnknownActionCode));
        // an unchanged binding after the failed rebinds.
        const InputContext* ctx = r.installed("gameplay");
        CHECK(ctx != nullptr);
        CHECK(ctx->bindings.size() == 1);
        CHECK(ctx->bindings[0].code == "W");
    }

    INPUT_TEST_MAIN_END();
}
