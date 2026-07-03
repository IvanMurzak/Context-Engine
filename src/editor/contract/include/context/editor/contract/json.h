// Json: a compact, dependency-free JSON value with parse + dump (R-CLI-008/013).
//
// The public contract surface (`context describe --json`, the R-CLI-008 result envelope, the
// `context new` template files) is JSON. Rather than pull in a third-party JSON library — which
// would drag a vcpkg dependency and a tools/license-allowlist.json entry (the same trade the
// kernel refused for its test framework) — the contract cluster ships this small stdlib-only DOM.
// Object member order is INSERTION-PRESERVING so `describe` output is deterministic (stable diffs,
// stable golden comparisons). Numbers remember whether they were authored as integers so
// `protocolMajor: 0` and exit codes serialize without a spurious `.0`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::contract
{

class Json
{
public:
    enum class Type
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    Json() = default;
    Json(std::nullptr_t) noexcept {}
    Json(bool b) noexcept : type_(Type::boolean), bool_(b) {}
    Json(int n) : type_(Type::number), num_(static_cast<double>(n)), integral_(true) {}
    Json(std::int64_t n) : type_(Type::number), num_(static_cast<double>(n)), integral_(true) {}
    Json(std::uint64_t n) : type_(Type::number), num_(static_cast<double>(n)), integral_(true) {}
    Json(double n) : type_(Type::number), num_(n), integral_(false) {}
    Json(const char* s) : type_(Type::string), str_(s) {}
    Json(std::string s) : type_(Type::string), str_(std::move(s)) {}

    [[nodiscard]] static Json array() { return Json(Type::array); }
    [[nodiscard]] static Json object() { return Json(Type::object); }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::string; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::object; }

    // Typed reads. as_string/as_bool/as_number return a default on a type mismatch rather than
    // throwing, so schema-validation tests read defensively; prefer is_*() when the type matters.
    [[nodiscard]] bool as_bool() const noexcept { return type_ == Type::boolean && bool_; }
    [[nodiscard]] double as_number() const noexcept { return type_ == Type::number ? num_ : 0.0; }
    [[nodiscard]] std::int64_t as_int() const noexcept
    {
        return type_ == Type::number ? static_cast<std::int64_t>(num_) : 0;
    }
    [[nodiscard]] const std::string& as_string() const noexcept
    {
        return type_ == Type::string ? str_ : empty_string();
    }

    // Object access. contains()/find() are const; at() returns a shared null for a missing key so
    // chained reads in tests never dereference a dangling reference.
    [[nodiscard]] bool contains(const std::string& key) const noexcept;
    [[nodiscard]] const Json& at(const std::string& key) const noexcept;

    // Array access. size() is valid for arrays AND objects (member count); 0 otherwise.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const Json& at(std::size_t index) const noexcept;

    // Mutation. set() upgrades null->object; push_back() upgrades null->array. Re-setting an
    // existing key overwrites in place (preserving its position).
    void set(std::string key, Json value);
    void push_back(Json value);

    // Serialize. indent == 0 => compact single line; indent > 0 => pretty-printed with that many
    // spaces per level.
    [[nodiscard]] std::string dump(int indent = 0) const;

    // Parse. Throws std::runtime_error with a byte offset on malformed input.
    [[nodiscard]] static Json parse(const std::string& text);

private:
    explicit Json(Type t) : type_(t) {}
    static const std::string& empty_string() noexcept;
    static const Json& null_ref() noexcept;

    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::null;
    bool bool_ = false;
    double num_ = 0.0;
    bool integral_ = false;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

} // namespace context::editor::contract
