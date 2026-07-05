// The post-merge convergence gate: duplicate-intra-file-id diagnostic + re-key (R-FILE-012(c)).

#include "merge_test.h"

#include "context/editor/merge/rekey.h"

#include <string>

using namespace context::editor::merge;
using context::editor::serializer::JsonValue;
using mergetest::canon;
using mergetest::parse;

namespace
{

// Stable-id-form (16 lowercase hex) id so find_duplicate_ids recognizes it as an identity.
constexpr const char* kIdA = "aaaa0000aaaa0001";

void test_find_duplicate_ids()
{
    // Two entities sharing an id => one duplicate group with both pointers.
    JsonValue dup = parse(R"({"entities": [
        {"id": "aaaa0000aaaa0001", "hp": 1},
        {"id": "aaaa0000aaaa0001", "hp": 2}]})");
    std::vector<DuplicateId> found = find_duplicate_ids(dup);
    CHECK(found.size() == 1);
    CHECK(found[0].id == kIdA);
    CHECK(found[0].pointers.size() == 2);
    CHECK(found[0].pointers[0] == "/entities/0");
    CHECK(found[0].pointers[1] == "/entities/1");

    // Unique ids => no duplicates.
    JsonValue unique = parse(R"({"entities": [
        {"id": "aaaa0000aaaa0001"}, {"id": "bbbb0000bbbb0002"}]})");
    CHECK(find_duplicate_ids(unique).empty());

    // Non-stable-form keys ("e1") are not identities and are never reported.
    JsonValue readable = parse(R"({"entities": [{"id": "e1"}, {"id": "e1"}]})");
    CHECK(find_duplicate_ids(readable).empty());
}

void test_rekey_duplicate_splits_collision()
{
    // A duplicate id + a reference to it. Re-keying the SECOND holder must NOT rewrite the reference
    // (still ambiguous: the first holder keeps the old id), and must clear the duplicate.
    JsonValue doc = parse(R"({"entities": [
        {"id": "aaaa0000aaaa0001", "hp": 1},
        {"id": "aaaa0000aaaa0001", "hp": 2},
        {"id": "cccc0000cccc0003", "target": {"$entity": "aaaa0000aaaa0001"}}]})");
    RekeyResult r = rekey_entity(doc, "/entities/1");
    CHECK(r.ok);
    CHECK(r.old_id == kIdA);
    CHECK(r.new_id.size() == 16);
    CHECK(r.new_id != r.old_id);
    CHECK(r.references_rewritten == 0); // ambiguous: the old id is still live on /entities/0
    CHECK(find_duplicate_ids(doc).empty());
}

void test_rekey_unique_rewrites_references()
{
    // A uniquely-held id referenced elsewhere: re-keying rewrites the reference (0 holders remain).
    JsonValue doc = parse(R"({"entities": [
        {"id": "aaaa0000aaaa0001"},
        {"id": "bbbb0000bbbb0002", "target": {"$entity": "aaaa0000aaaa0001"}}]})");
    RekeyResult r = rekey_entity(doc, "/entities/0");
    CHECK(r.ok);
    CHECK(r.references_rewritten == 1);
    // The reference now names the new id, and the old id appears nowhere.
    const std::string bytes = canon(doc);
    CHECK(bytes.find(kIdA) == std::string::npos);
    CHECK(bytes.find(r.new_id) != std::string::npos);
}

void test_rekey_explicit_new_id()
{
    JsonValue doc = parse(R"({"entities": [{"id": "aaaa0000aaaa0001"}]})");
    RekeyResult r = rekey_entity(doc, "/entities/0", "dddd0000dddd0004");
    CHECK(r.ok);
    CHECK(r.new_id == "dddd0000dddd0004");
}

void test_rekey_failures()
{
    JsonValue doc = parse(R"({"entities": [{"id": "aaaa0000aaaa0001"}, {"noId": true}]})");
    // pointer to an object without a stable id
    CHECK(!rekey_entity(doc, "/entities/1").ok);
    // pointer that does not resolve
    CHECK(!rekey_entity(doc, "/entities/9").ok);
    // a non-stable requested id is rejected
    RekeyResult bad = rekey_entity(doc, "/entities/0", "not-a-stable-id");
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());
}

} // namespace

int main()
{
    test_find_duplicate_ids();
    test_rekey_duplicate_splits_collision();
    test_rekey_unique_rewrites_references();
    test_rekey_explicit_new_id();
    test_rekey_failures();
    MERGE_TEST_MAIN_END();
}
