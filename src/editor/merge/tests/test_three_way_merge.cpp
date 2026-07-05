// Structural three-way merge — the auto-merge matrices + conflict cases (R-FILE-012 / R-QA-013).

#include "merge_test.h"

#include "context/editor/merge/three_way_merge.h"

using namespace context::editor::merge;
using context::editor::serializer::JsonValue;
using mergetest::canon;
using mergetest::conflict_at;
using mergetest::parse;

namespace
{

// Assert the merged tree equals `expected` (compared as canonical bytes, so member order / notation
// differences never make an equal tree look unequal).
void check_merged(const MergeResult& result, const char* expected)
{
    CHECK(canon(result.merged) == canon(parse(expected)));
}

// A conflicted merge's output must ALWAYS be valid JSON (the marker-free invariant): re-parsing its
// canonical bytes round-trips. `context merge-file` never writes <<<<<<< text markers.
void check_marker_free(const MergeResult& result)
{
    const std::string bytes = canon(result.merged);
    context::editor::serializer::ParseResult reparsed =
        context::editor::serializer::parse_json(bytes);
    CHECK(reparsed.ok);
    CHECK(bytes.find("<<<<<<<") == std::string::npos);
    CHECK(bytes.find(">>>>>>>") == std::string::npos);
}

void test_both_unchanged()
{
    const JsonValue base = parse(R"({"a": 1, "b": 2})");
    MergeResult r = merge_documents(base, base, base);
    CHECK(r.clean);
    CHECK(r.conflicts.empty());
    check_merged(r, R"({"a": 1, "b": 2})");
}

void test_one_sided_edits()
{
    const JsonValue base = parse(R"({"a": 1, "b": 2})");
    // only ours changed
    MergeResult ours = merge_documents(base, parse(R"({"a": 9, "b": 2})"), base);
    CHECK(ours.clean);
    check_merged(ours, R"({"a": 9, "b": 2})");
    // only theirs changed
    MergeResult theirs = merge_documents(base, base, parse(R"({"a": 1, "b": 8})"));
    CHECK(theirs.clean);
    check_merged(theirs, R"({"a": 1, "b": 8})");
    // both made the SAME change
    MergeResult same = merge_documents(base, parse(R"({"a": 5, "b": 2})"), parse(R"({"a": 5, "b": 2})"));
    CHECK(same.clean);
    check_merged(same, R"({"a": 5, "b": 2})");
}

void test_disjoint_fields_automerge()
{
    const JsonValue base = parse(R"({"a": 1, "b": 2, "c": 3})");
    MergeResult r = merge_documents(base, parse(R"({"a": 10, "b": 2, "c": 3})"),
                                    parse(R"({"a": 1, "b": 20, "c": 3})"));
    CHECK(r.clean);
    CHECK(r.conflicts.empty());
    check_merged(r, R"({"a": 10, "b": 20, "c": 3})");
}

void test_nested_disjoint_automerge()
{
    const JsonValue base = parse(R"({"t": {"x": 0, "y": 0, "z": 0}})");
    MergeResult r = merge_documents(base, parse(R"({"t": {"x": 1, "y": 0, "z": 0}})"),
                                    parse(R"({"t": {"x": 0, "y": 0, "z": 2}})"));
    CHECK(r.clean);
    check_merged(r, R"({"t": {"x": 1, "y": 0, "z": 2}})");
}

void test_field_conflict()
{
    const JsonValue base = parse(R"({"a": 1})");
    MergeResult r = merge_documents(base, parse(R"({"a": 2})"), parse(R"({"a": 3})"));
    CHECK(!r.clean);
    const Conflict* c = conflict_at(r, "/a");
    CHECK(c != nullptr);
    CHECK(c->klass == ConflictClass::field);
    CHECK(c->base.has_value() && c->ours.has_value() && c->theirs.has_value());
    CHECK(canon(*c->ours) == canon(parse("2")));
    CHECK(canon(*c->theirs) == canon(parse("3")));
    check_merged(r, R"({"a": 2})"); // deterministic ours placeholder
    check_marker_free(r);
}

void test_add_add_same_and_different()
{
    const JsonValue base = parse(R"({"keep": 1})");
    // added on both with SAME value => merged, no conflict
    MergeResult same = merge_documents(base, parse(R"({"keep": 1, "n": 5})"),
                                       parse(R"({"keep": 1, "n": 5})"));
    CHECK(same.clean);
    check_merged(same, R"({"keep": 1, "n": 5})");
    // added on both with DIFFERENT value => field conflict
    MergeResult diff = merge_documents(base, parse(R"({"keep": 1, "n": 5})"),
                                       parse(R"({"keep": 1, "n": 6})"));
    CHECK(!diff.clean);
    const Conflict* c = conflict_at(diff, "/n");
    CHECK(c != nullptr && c->klass == ConflictClass::field);
    CHECK(!c->base.has_value()); // absent on base (added on both)
}

void test_remove_clean_and_delete_modify()
{
    const JsonValue base = parse(R"({"a": 1, "b": 2})");
    // ours untouched a, theirs removed a => removal wins, clean
    MergeResult removed = merge_documents(base, base, parse(R"({"b": 2})"));
    CHECK(removed.clean);
    check_merged(removed, R"({"b": 2})");
    // ours MODIFIED a, theirs removed a => delete/modify conflict
    MergeResult dm = merge_documents(base, parse(R"({"a": 99, "b": 2})"), parse(R"({"b": 2})"));
    CHECK(!dm.clean);
    const Conflict* c = conflict_at(dm, "/a");
    CHECK(c != nullptr && c->klass == ConflictClass::delete_modify);
    CHECK(c->base.has_value() && c->ours.has_value() && !c->theirs.has_value());
    check_merged(dm, R"({"a": 99, "b": 2})"); // ours modification kept as placeholder
}

void test_id_keyed_disjoint_entity_edits()
{
    const JsonValue base = parse(R"({"entities": [
        {"id": "e1", "name": "A", "hp": 10},
        {"id": "e2", "name": "B", "hp": 20}]})");
    // ours edits e1.hp, theirs edits e2.hp
    MergeResult r = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "name": "A", "hp": 11}, {"id": "e2", "name": "B", "hp": 20}]})"),
        parse(R"({"entities": [{"id": "e1", "name": "A", "hp": 10}, {"id": "e2", "name": "B", "hp": 22}]})"));
    CHECK(r.clean);
    check_merged(r, R"({"entities": [{"id": "e1", "name": "A", "hp": 11}, {"id": "e2", "name": "B", "hp": 22}]})");
}

void test_id_keyed_same_entity_disjoint_fields()
{
    const JsonValue base = parse(R"({"entities": [{"id": "e1", "x": 0, "y": 0}]})");
    MergeResult r = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "x": 1, "y": 0}]})"),
        parse(R"({"entities": [{"id": "e1", "x": 0, "y": 2}]})"));
    CHECK(r.clean);
    check_merged(r, R"({"entities": [{"id": "e1", "x": 1, "y": 2}]})");
}

void test_id_keyed_same_field_conflict()
{
    const JsonValue base = parse(R"({"entities": [{"id": "e1", "hp": 10}]})");
    MergeResult r = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "hp": 11}]})"),
        parse(R"({"entities": [{"id": "e1", "hp": 12}]})"));
    CHECK(!r.clean);
    const Conflict* c = conflict_at(r, "/entities/0/hp");
    CHECK(c != nullptr && c->klass == ConflictClass::field);
    CHECK(c->id == "e1"); // the addressed element's stable id is surfaced
}

void test_id_add_add_is_conflict_even_when_identical()
{
    const JsonValue base = parse(R"({"entities": [{"id": "e1", "hp": 1}]})");
    // both sides add the SAME id e2 — even with identical content this is a structural conflict.
    MergeResult identical = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "e2", "hp": 5}]})"),
        parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "e2", "hp": 5}]})"));
    // whole trees are equal => the top-level shortcut makes this clean (both produced identical files)
    CHECK(identical.clean);

    // both add id e2 with DIFFERENT content, plus a divergent e1 edit so the trees differ.
    MergeResult diff = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "hp": 2}, {"id": "e2", "hp": 5}]})"),
        parse(R"({"entities": [{"id": "e1", "hp": 3}, {"id": "e2", "hp": 6}]})"));
    CHECK(!diff.clean);
    const Conflict* add = conflict_at(diff, "/entities/1");
    CHECK(add != nullptr && add->klass == ConflictClass::id_add_add);
    CHECK(add->id == "e2");
    CHECK(!add->base.has_value());
    check_marker_free(diff);
}

void test_id_keyed_reorder_with_edit_automerges()
{
    const JsonValue base = parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "e2", "hp": 2}]})");
    // ours REORDERS to [e2, e1]; theirs edits e2.hp. Merge keeps ours order + theirs field edit.
    MergeResult r = merge_documents(base,
        parse(R"({"entities": [{"id": "e2", "hp": 2}, {"id": "e1", "hp": 1}]})"),
        parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "e2", "hp": 9}]})"));
    CHECK(r.clean);
    check_merged(r, R"({"entities": [{"id": "e2", "hp": 9}, {"id": "e1", "hp": 1}]})");
}

void test_id_keyed_one_sided_adds_merge()
{
    const JsonValue base = parse(R"({"entities": [{"id": "e1", "hp": 1}]})");
    MergeResult r = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "eo", "hp": 7}]})"),
        parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "et", "hp": 8}]})"));
    CHECK(r.clean);
    // ours order first, then theirs-only additions appended.
    check_merged(r, R"({"entities": [{"id": "e1", "hp": 1}, {"id": "eo", "hp": 7}, {"id": "et", "hp": 8}]})");
}

void test_id_keyed_delete_modify()
{
    const JsonValue base = parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "e2", "hp": 2}]})");
    // ours removes e2; theirs modifies e2 => delete/modify conflict.
    MergeResult r = merge_documents(base,
        parse(R"({"entities": [{"id": "e1", "hp": 1}]})"),
        parse(R"({"entities": [{"id": "e1", "hp": 1}, {"id": "e2", "hp": 99}]})"));
    CHECK(!r.clean);
    bool found = false;
    for (const Conflict& c : r.conflicts)
        if (c.klass == ConflictClass::delete_modify && c.id == "e2")
            found = true;
    CHECK(found);
}

void test_non_id_array_is_opaque()
{
    // A scalar array is NOT id-keyed: both sides change it differently => whole-array field conflict,
    // never a positional element merge.
    const JsonValue base = parse(R"({"tags": ["a", "b"]})");
    MergeResult r = merge_documents(base, parse(R"({"tags": ["a", "b", "ours"]})"),
                                    parse(R"({"tags": ["a", "b", "theirs"]})"));
    CHECK(!r.clean);
    const Conflict* c = conflict_at(r, "/tags");
    CHECK(c != nullptr && c->klass == ConflictClass::field);
}

void test_non_id_array_one_sided_clean()
{
    const JsonValue base = parse(R"({"tags": ["a"]})");
    MergeResult r = merge_documents(base, parse(R"({"tags": ["a", "x"]})"), base);
    CHECK(r.clean);
    check_merged(r, R"({"tags": ["a", "x"]})");
}

void test_meta_guid_whole_file()
{
    const JsonValue base = parse(R"({"guid": "0000000000000000"})");
    MergeResult r = merge_documents(base, parse(R"({"guid": "aaaa000000000000", "importer": "png"})"),
                                    parse(R"({"guid": "bbbb000000000000", "importer": "png"})"));
    CHECK(!r.clean);
    CHECK(r.whole_file);
    const Conflict* c = conflict_at(r, "");
    CHECK(c != nullptr && c->klass == ConflictClass::meta_guid);
}

void test_meta_guid_one_sided_automerges()
{
    // A ONE-sided guid change is NOT a whole-file conflict — it auto-merges to the changed side like
    // any other field. Only a BOTH-sides divergent mint (test_meta_guid_whole_file) is whole-file.
    const JsonValue base = parse(R"({"guid": "0000000000000000", "importer": "png"})");
    // only theirs re-guids (ours == base)
    MergeResult theirs = merge_documents(base, base,
                                         parse(R"({"guid": "bbbb000000000000", "importer": "png"})"));
    CHECK(theirs.clean);
    CHECK(!theirs.whole_file);
    check_merged(theirs, R"({"guid": "bbbb000000000000", "importer": "png"})");
    // only ours re-guids (theirs == base)
    MergeResult ours = merge_documents(base,
                                       parse(R"({"guid": "aaaa000000000000", "importer": "png"})"), base);
    CHECK(ours.clean);
    CHECK(!ours.whole_file);
    check_merged(ours, R"({"guid": "aaaa000000000000", "importer": "png"})");
}

void test_newer_stamped_whole_file()
{
    MergeOptions opts;
    opts.floor.kind_versions["ctx:scene"] = 1; // installed schema understands version 1
    const JsonValue base = parse(R"({"$schema": "ctx:scene", "version": 1})");
    // theirs is stamped version 2 (> installed 1) => whole-file newer_stamped, NOT a parse error.
    MergeResult r = merge_documents(base, parse(R"({"$schema": "ctx:scene", "version": 1, "a": 1})"),
                                    parse(R"({"$schema": "ctx:scene", "version": 2, "a": 2})"), opts);
    CHECK(!r.clean);
    CHECK(r.whole_file);
    const Conflict* c = conflict_at(r, "");
    CHECK(c != nullptr && c->klass == ConflictClass::newer_stamped);
}

} // namespace

int main()
{
    test_both_unchanged();
    test_one_sided_edits();
    test_disjoint_fields_automerge();
    test_nested_disjoint_automerge();
    test_field_conflict();
    test_add_add_same_and_different();
    test_remove_clean_and_delete_modify();
    test_id_keyed_disjoint_entity_edits();
    test_id_keyed_same_entity_disjoint_fields();
    test_id_keyed_same_field_conflict();
    test_id_add_add_is_conflict_even_when_identical();
    test_id_keyed_reorder_with_edit_automerges();
    test_id_keyed_one_sided_adds_merge();
    test_id_keyed_delete_modify();
    test_non_id_array_is_opaque();
    test_non_id_array_one_sided_clean();
    test_meta_guid_whole_file();
    test_meta_guid_one_sided_automerges();
    test_newer_stamped_whole_file();
    MERGE_TEST_MAIN_END();
}
