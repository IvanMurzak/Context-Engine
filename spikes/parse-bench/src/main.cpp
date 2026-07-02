// parse-bench — M0 throwaway spike measuring the moat-perf bound (R-FILE-011(a)):
// parse -> canonicalize -> hash throughput over the synthetic corpus, plus
// `context merge-file`-class three-way structural merge throughput (R-FILE-012).
//
// The derivation/cache key is the CANONICAL-content hash (R-FILE-001): every authored
// JSON file is parsed (simdjson DOM), rebuilt into a mutable tree, canonically
// re-serialized, and the canonical bytes are hashed (BLAKE3). Binary sidecars take the
// raw-byte digest (raw hash == canonical hash for binaries, R-FILE-001).
//
// Subject CLI contract (bench/harness.py): every mode prints ONE JSON object on stdout.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <blake3.h>
#include <simdjson.h>

#include "canonical.h"
#include "json_value.h"
#include "merge.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

uint64_t splitmix64(uint64_t& state) {
    state += 0x9e3779b97f4a7c15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double seconds(uint64_t ns) { return static_cast<double>(ns) / 1e9; }

uint64_t elapsedNs(Clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "parse-bench: %s\n", msg.c_str());
    std::exit(1);
}

// ---------------------------------------------------------------------------
// simdjson DOM -> mutable tree
// ---------------------------------------------------------------------------

ctx::JsonValue buildTree(simdjson::dom::element el) {
    ctx::JsonValue v;
    switch (el.type()) {
        case simdjson::dom::element_type::NULL_VALUE:
            v.type = ctx::JsonValue::Type::Null;
            break;
        case simdjson::dom::element_type::BOOL:
            v.type = ctx::JsonValue::Type::Bool;
            v.boolean = el.get_bool().value_unsafe();
            break;
        case simdjson::dom::element_type::INT64:
            v.type = ctx::JsonValue::Type::Int;
            v.i64 = el.get_int64().value_unsafe();
            break;
        case simdjson::dom::element_type::UINT64:
            v.type = ctx::JsonValue::Type::Uint;
            v.u64 = el.get_uint64().value_unsafe();
            break;
        case simdjson::dom::element_type::DOUBLE:
            v.type = ctx::JsonValue::Type::Double;
            v.dbl = el.get_double().value_unsafe();
            break;
        case simdjson::dom::element_type::STRING: {
            v.type = ctx::JsonValue::Type::String;
            std::string_view sv = el.get_string().value_unsafe();
            v.str.assign(sv.data(), sv.size());
            break;
        }
        case simdjson::dom::element_type::ARRAY: {
            v.type = ctx::JsonValue::Type::Array;
            simdjson::dom::array a = el.get_array().value_unsafe();
            v.arr.reserve(a.size());
            for (simdjson::dom::element child : a) v.arr.push_back(buildTree(child));
            break;
        }
        case simdjson::dom::element_type::OBJECT: {
            v.type = ctx::JsonValue::Type::Object;
            simdjson::dom::object o = el.get_object().value_unsafe();
            v.obj.reserve(o.size());
            for (simdjson::dom::key_value_pair kv : o)
                v.obj.emplace_back(std::string(kv.key), buildTree(kv.value));
            break;
        }
        default:
            // Future simdjson element kinds (e.g. BIGINT): not produced by the
            // canonical corpus; treat as null rather than failing the build.
            v.type = ctx::JsonValue::Type::Null;
            break;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

uint64_t blake3Prefix64(const void* data, size_t len) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    uint64_t prefix = 0;
    std::memcpy(&prefix, out, sizeof(prefix));
    return prefix;
}

// ---------------------------------------------------------------------------
// Corpus enumeration
// ---------------------------------------------------------------------------

struct FileList {
    std::vector<fs::path> jsonFiles;
    std::vector<fs::path> binFiles;
};

FileList enumerateCorpus(const fs::path& corpus) {
    FileList list;
    const fs::path root = corpus / "project";
    if (!fs::exists(root)) fail("corpus has no project/ dir: " + corpus.string());
    for (auto it = fs::recursive_directory_iterator(root);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        const fs::path& p = it->path();
        if (p.extension() == ".json")
            list.jsonFiles.push_back(p);
        else
            list.binFiles.push_back(p);
    }
    // Deterministic order regardless of filesystem iteration order.
    std::sort(list.jsonFiles.begin(), list.jsonFiles.end());
    std::sort(list.binFiles.begin(), list.binFiles.end());
    return list;
}

bool readWholeFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(f.read(out.data(), size));
}

// ---------------------------------------------------------------------------
// The measured pipeline: read -> parse -> build tree -> canonicalize -> hash
// ---------------------------------------------------------------------------

struct WorkerStats {
    uint64_t files = 0, jsonFiles = 0, binFiles = 0;
    uint64_t bytes = 0, jsonBytes = 0, binBytes = 0, canonicalBytes = 0;
    uint64_t readNs = 0, parseNs = 0, buildNs = 0, canonNs = 0, hashNs = 0;
    uint64_t parseErrors = 0;
    uint64_t hashXor = 0;  // keeps the hash from being optimized away

    void add(const WorkerStats& o) {
        files += o.files;
        jsonFiles += o.jsonFiles;
        binFiles += o.binFiles;
        bytes += o.bytes;
        jsonBytes += o.jsonBytes;
        binBytes += o.binBytes;
        canonicalBytes += o.canonicalBytes;
        readNs += o.readNs;
        parseNs += o.parseNs;
        buildNs += o.buildNs;
        canonNs += o.canonNs;
        hashNs += o.hashNs;
        parseErrors += o.parseErrors;
        hashXor ^= o.hashXor;
    }
};

void processJsonFile(const fs::path& p, simdjson::dom::parser& parser, WorkerStats& st,
                     std::string& canonicalBuf) {
    auto t0 = Clock::now();
    simdjson::padded_string content;
    if (simdjson::padded_string::load(p.string()).get(content) != simdjson::SUCCESS) {
        ++st.parseErrors;
        return;
    }
    st.readNs += elapsedNs(t0);
    st.bytes += content.size();
    st.jsonBytes += content.size();

    t0 = Clock::now();
    simdjson::dom::element doc;
    if (parser.parse(content).get(doc) != simdjson::SUCCESS) {
        ++st.parseErrors;
        return;
    }
    st.parseNs += elapsedNs(t0);

    t0 = Clock::now();
    ctx::JsonValue tree = buildTree(doc);
    st.buildNs += elapsedNs(t0);

    t0 = Clock::now();
    canonicalBuf.clear();
    ctx::canonicalWrite(tree, canonicalBuf);
    canonicalBuf.push_back('\n');
    st.canonNs += elapsedNs(t0);
    st.canonicalBytes += canonicalBuf.size();

    t0 = Clock::now();
    st.hashXor ^= blake3Prefix64(canonicalBuf.data(), canonicalBuf.size());
    st.hashNs += elapsedNs(t0);

    ++st.files;
    ++st.jsonFiles;
}

void processBinFile(const fs::path& p, WorkerStats& st, std::string& buf) {
    auto t0 = Clock::now();
    if (!readWholeFile(p, buf)) {
        ++st.parseErrors;
        return;
    }
    st.readNs += elapsedNs(t0);
    st.bytes += buf.size();
    st.binBytes += buf.size();

    t0 = Clock::now();
    st.hashXor ^= blake3Prefix64(buf.data(), buf.size());
    st.hashNs += elapsedNs(t0);

    ++st.files;
    ++st.binFiles;
}

WorkerStats processFiles(const std::vector<fs::path>& jsonFiles,
                         const std::vector<fs::path>& binFiles, unsigned threads) {
    std::atomic<size_t> nextJson{0}, nextBin{0};
    std::vector<WorkerStats> partials(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned tI = 0; tI < threads; ++tI) {
        pool.emplace_back([&, tI]() {
            simdjson::dom::parser parser;
            std::string buf;
            WorkerStats& st = partials[tI];
            for (;;) {
                const size_t i = nextJson.fetch_add(1, std::memory_order_relaxed);
                if (i >= jsonFiles.size()) break;
                processJsonFile(jsonFiles[i], parser, st, buf);
            }
            for (;;) {
                const size_t i = nextBin.fetch_add(1, std::memory_order_relaxed);
                if (i >= binFiles.size()) break;
                processBinFile(binFiles[i], st, buf);
            }
        });
    }
    for (auto& th : pool) th.join();
    WorkerStats total;
    for (const auto& p : partials) total.add(p);
    return total;
}

// ---------------------------------------------------------------------------
// Merge scenario: synthesize ours/theirs from a parsed base, then 3-way merge
// ---------------------------------------------------------------------------

ctx::JsonValue* transformField(ctx::JsonValue& entity, const char* group, const char* axis) {
    ctx::JsonValue* comps = entity.find("components");
    if (comps == nullptr) return nullptr;
    ctx::JsonValue* tr = comps->find("ctx:Transform");
    if (tr == nullptr) return nullptr;
    ctx::JsonValue* g = tr->find(group);
    if (g == nullptr) return nullptr;
    return g->find(axis);
}

// Apply `edits` mutations to `scene`. `sideSalt` decorates side-local choices;
// `sharedSeed` drives the overlapping fraction that produces genuine conflicts.
void mutateScene(ctx::JsonValue& scene, uint64_t sideSalt, uint64_t sharedSeed, int edits) {
    ctx::JsonValue* ents = scene.find("entities");
    if (ents == nullptr || ents->type != ctx::JsonValue::Type::Array || ents->arr.empty())
        return;
    uint64_t rng = sharedSeed ^ (sideSalt * 0x9e3779b97f4a7c15ull);
    uint64_t sharedRng = sharedSeed;  // identical stream on both sides
    static const char* kAxes[3] = {"x", "y", "z"};
    for (int e = 0; e < edits; ++e) {
        const uint64_t roll = splitmix64(rng) % 100;
        auto& arr = ents->arr;
        if (roll < 60) {
            // Disjoint numeric field edit (side-local target and value).
            ctx::JsonValue& ent = arr[splitmix64(rng) % arr.size()];
            ctx::JsonValue* f = transformField(ent, "position", kAxes[splitmix64(rng) % 3]);
            if (f != nullptr)
                *f = ctx::JsonValue::makeDouble(
                    static_cast<double>(splitmix64(rng) % 100000) / 100.0);
        } else if (roll < 70) {
            // OVERLAPPING edit: same entity + field on both sides (shared stream),
            // side-different value -> a genuine field-path conflict.
            const size_t target = splitmix64(sharedRng) % arr.size();
            const char* axis = kAxes[splitmix64(sharedRng) % 3];
            ctx::JsonValue* f = transformField(arr[target], "scale", axis);
            if (f != nullptr)
                *f = ctx::JsonValue::makeDouble(
                    static_cast<double>(splitmix64(rng) % 1000) / 10.0 + 0.001);
        } else if (roll < 80) {
            // Rename (side-local).
            ctx::JsonValue& ent = arr[splitmix64(rng) % arr.size()];
            ctx::JsonValue* name = ent.find("name");
            if (name != nullptr && name->type == ctx::JsonValue::Type::String)
                name->str += (sideSalt & 1) ? "-ours" : "-theirs";
        } else if (roll < 90) {
            // Add a new entity. 50% side-local id (clean add), 50% shared id with
            // side-different payload (the both-sides-added structural conflict).
            const bool sharedId = (splitmix64(rng) % 2) == 0;
            uint64_t idBits = sharedId ? splitmix64(sharedRng) : splitmix64(rng);
            char idBuf[17];
            std::snprintf(idBuf, sizeof(idBuf), "%016llx",
                          static_cast<unsigned long long>(idBits));
            ctx::JsonValue ent;
            ent.type = ctx::JsonValue::Type::Object;
            ent.obj.emplace_back("id", ctx::JsonValue::makeString(idBuf));
            ent.obj.emplace_back(
                "name", ctx::JsonValue::makeString(
                            std::string("added-") + ((sideSalt & 1) ? "ours" : "theirs")));
            ctx::JsonValue pos;
            pos.type = ctx::JsonValue::Type::Object;
            pos.obj.emplace_back("x", ctx::JsonValue::makeDouble(
                                          static_cast<double>(splitmix64(rng) % 100)));
            pos.obj.emplace_back("y", ctx::JsonValue::makeDouble(0.0));
            pos.obj.emplace_back("z", ctx::JsonValue::makeDouble(0.0));
            ctx::JsonValue tr;
            tr.type = ctx::JsonValue::Type::Object;
            tr.obj.emplace_back("position", std::move(pos));
            ctx::JsonValue comps;
            comps.type = ctx::JsonValue::Type::Object;
            comps.obj.emplace_back("ctx:Transform", std::move(tr));
            ent.obj.emplace_back("components", std::move(comps));
            arr.push_back(std::move(ent));
        } else {
            // Delete an entity (side-local pick).
            if (arr.size() > 2) arr.erase(arr.begin() +
                                          static_cast<std::ptrdiff_t>(
                                              splitmix64(rng) % arr.size()));
        }
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Args {
    std::string mode;
    fs::path corpus;
    unsigned threads = 0;
    int count = 2000;
    uint64_t seed = 1;
    int sample = 200;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    if (argc < 2) fail("usage: parse-bench <attach|edit|bulk|import|merge|canon-check> --corpus DIR [--threads N] [--count K] [--seed S] [--sample N]");
    a.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) fail("missing value for " + flag);
            return argv[++i];
        };
        if (flag == "--corpus")
            a.corpus = next();
        else if (flag == "--threads")
            a.threads = static_cast<unsigned>(std::stoul(next()));
        else if (flag == "--count")
            a.count = std::stoi(next());
        else if (flag == "--seed")
            a.seed = std::stoull(next());
        else if (flag == "--sample")
            a.sample = std::stoi(next());
        else
            fail("unknown flag: " + flag);
    }
    if (a.corpus.empty()) fail("--corpus is required");
    if (a.threads == 0) a.threads = std::max(1u, std::thread::hardware_concurrency());
    return a;
}

void printKv(std::string& out, const char* key, double v, bool last = false) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    out += "  \"";
    out += key;
    out += "\": ";
    out += buf;
    if (!last) out += ",";
    out += "\n";
}

void printKv(std::string& out, const char* key, uint64_t v, bool last = false) {
    out += "  \"";
    out += key;
    out += "\": ";
    out += std::to_string(v);
    if (!last) out += ",";
    out += "\n";
}

void printKvS(std::string& out, const char* key, const std::string& v, bool last = false) {
    out += "  \"";
    out += key;
    out += "\": \"";
    out += jsonEscape(v);
    out += "\"";
    if (!last) out += ",";
    out += "\n";
}

int runAttach(const Args& a) {
    const auto tEnum0 = Clock::now();
    FileList list = enumerateCorpus(a.corpus);
    const double enumerateSeconds = seconds(elapsedNs(tEnum0));

    const auto t0 = Clock::now();
    WorkerStats st = processFiles(list.jsonFiles, list.binFiles, a.threads);
    const double procSeconds = seconds(elapsedNs(t0));
    const double wall = enumerateSeconds + procSeconds;

    std::string out = "{\n";
    printKvS(out, "scenario", "attach");
    printKv(out, "threads", static_cast<uint64_t>(a.threads));
    printKv(out, "files", st.files);
    printKv(out, "json_files", st.jsonFiles);
    printKv(out, "bin_files", st.binFiles);
    printKv(out, "bytes_total", st.bytes);
    printKv(out, "json_bytes", st.jsonBytes);
    printKv(out, "bin_bytes", st.binBytes);
    printKv(out, "canonical_bytes", st.canonicalBytes);
    printKv(out, "parse_errors", st.parseErrors);
    printKv(out, "enumerate_seconds", enumerateSeconds);
    printKv(out, "process_seconds", procSeconds);
    printKv(out, "wall_seconds", wall);
    printKv(out, "files_per_sec", static_cast<double>(st.files) / wall);
    printKv(out, "mb_per_sec", static_cast<double>(st.bytes) / 1e6 / wall);
    printKv(out, "json_mb_per_sec", static_cast<double>(st.jsonBytes) / 1e6 / wall);
    printKv(out, "cpu_read_seconds", seconds(st.readNs));
    printKv(out, "cpu_parse_seconds", seconds(st.parseNs));
    printKv(out, "cpu_build_seconds", seconds(st.buildNs));
    printKv(out, "cpu_canonicalize_seconds", seconds(st.canonNs));
    printKv(out, "cpu_hash_seconds", seconds(st.hashNs));
    char hx[24];
    std::snprintf(hx, sizeof(hx), "%016llx", static_cast<unsigned long long>(st.hashXor));
    printKvS(out, "hash_check", hx, true);
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    return st.parseErrors == 0 ? 0 : 1;
}

int runEdit(const Args& a) {
    FileList list = enumerateCorpus(a.corpus);
    if (list.jsonFiles.empty()) fail("no JSON files in corpus");
    uint64_t rng = a.seed;
    const fs::path& target = list.jsonFiles[splitmix64(rng) % list.jsonFiles.size()];

    const auto t0 = Clock::now();
    simdjson::dom::parser parser;
    WorkerStats st;
    std::string buf;
    processJsonFile(target, parser, st, buf);
    const double latencyMs = seconds(elapsedNs(t0)) * 1e3;

    std::string out = "{\n";
    printKvS(out, "scenario", "edit");
    printKvS(out, "file", target.generic_string());
    printKv(out, "bytes", st.bytes);
    printKv(out, "parse_errors", st.parseErrors);
    printKv(out, "latency_ms", latencyMs, true);
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    return st.parseErrors == 0 ? 0 : 1;
}

std::vector<fs::path> samplePaths(const std::vector<fs::path>& all, size_t count,
                                  uint64_t seed) {
    std::vector<size_t> idx(all.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    uint64_t rng = seed;
    // Partial Fisher-Yates: deterministic sample without full shuffle.
    const size_t n = std::min(count, idx.size());
    for (size_t i = 0; i < n; ++i) {
        const size_t j = i + splitmix64(rng) % (idx.size() - i);
        std::swap(idx[i], idx[j]);
    }
    std::vector<fs::path> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(all[idx[i]]);
    return out;
}

int runBulk(const Args& a) {
    FileList list = enumerateCorpus(a.corpus);
    std::vector<fs::path> sample =
        samplePaths(list.jsonFiles, static_cast<size_t>(a.count), a.seed);

    const auto t0 = Clock::now();
    WorkerStats st = processFiles(sample, {}, a.threads);
    const double wall = seconds(elapsedNs(t0));

    std::string out = "{\n";
    printKvS(out, "scenario", "bulk");
    printKv(out, "threads", static_cast<uint64_t>(a.threads));
    printKv(out, "count", st.files);
    printKv(out, "bytes_total", st.bytes);
    printKv(out, "parse_errors", st.parseErrors);
    printKv(out, "wall_seconds", wall);
    printKv(out, "files_per_sec", static_cast<double>(st.files) / wall);
    printKv(out, "mb_per_sec", static_cast<double>(st.bytes) / 1e6 / wall, true);
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    return st.parseErrors == 0 ? 0 : 1;
}

int runMerge(const Args& a) {
    FileList list = enumerateCorpus(a.corpus);
    std::vector<fs::path> scenes;
    for (const auto& p : list.jsonFiles) {
        const std::string name = p.filename().generic_string();
        if (name.size() > 11 && name.rfind(".scene.json") == name.size() - 11)
            scenes.push_back(p);
    }
    if (scenes.empty()) fail("no *.scene.json files in corpus");
    std::vector<fs::path> sample =
        samplePaths(scenes, static_cast<size_t>(a.count), a.seed);

    // Prepare base/ours/theirs OUTSIDE the timed region (a real `context merge-file`
    // also parses its three inputs; that cost is measured by the attach scenario and
    // composed analytically in FINDINGS.md).
    struct Prepared {
        ctx::JsonValue base, ours, theirs;
    };
    std::vector<Prepared> prepared(sample.size());
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        const unsigned prepThreads = a.threads;
        pool.reserve(prepThreads);
        std::atomic<uint64_t> prepErrors{0};
        for (unsigned tI = 0; tI < prepThreads; ++tI) {
            pool.emplace_back([&]() {
                simdjson::dom::parser parser;
                for (;;) {
                    const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= sample.size()) break;
                    simdjson::padded_string content;
                    simdjson::dom::element doc;
                    if (simdjson::padded_string::load(sample[i].string()).get(content) !=
                            simdjson::SUCCESS ||
                        parser.parse(content).get(doc) != simdjson::SUCCESS) {
                        ++prepErrors;
                        continue;
                    }
                    Prepared& pr = prepared[i];
                    pr.base = buildTree(doc);
                    pr.ours = pr.base;
                    pr.theirs = pr.base;
                    const ctx::JsonValue* ents = pr.base.find("entities");
                    const int entityCount =
                        ents != nullptr ? static_cast<int>(ents->arr.size()) : 0;
                    const int edits = std::clamp(entityCount / 25, 4, 32);
                    const uint64_t sharedSeed = a.seed * 1000003ull + i;
                    mutateScene(pr.ours, /*sideSalt=*/1, sharedSeed, edits);
                    mutateScene(pr.theirs, /*sideSalt=*/2, sharedSeed, edits);
                }
            });
        }
        for (auto& th : pool) th.join();
        if (prepErrors.load() != 0) fail("merge prep: parse errors");
    }

    // TIMED: three-way structural merge + canonical serialization of the merged tree
    // (a real merge driver writes canonical output — R-FILE-001).
    std::atomic<size_t> next{0};
    std::vector<uint64_t> mergeNsPer(a.threads, 0), canonNsPer(a.threads, 0);
    std::vector<uint64_t> conflictsPer(a.threads, 0), conflictedPer(a.threads, 0);
    std::vector<uint64_t> outBytesPer(a.threads, 0);
    const auto t0 = Clock::now();
    {
        std::vector<std::thread> pool;
        pool.reserve(a.threads);
        for (unsigned tI = 0; tI < a.threads; ++tI) {
            pool.emplace_back([&, tI]() {
                std::string buf;
                for (;;) {
                    const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= prepared.size()) break;
                    const Prepared& pr = prepared[i];
                    auto m0 = Clock::now();
                    ctx::MergeResult r = ctx::merge3(&pr.base, &pr.ours, &pr.theirs);
                    mergeNsPer[tI] += elapsedNs(m0);
                    m0 = Clock::now();
                    buf.clear();
                    ctx::canonicalWrite(r.merged, buf);
                    buf.push_back('\n');
                    canonNsPer[tI] += elapsedNs(m0);
                    outBytesPer[tI] += buf.size();
                    conflictsPer[tI] += r.conflicts.size();
                    if (!r.conflicts.empty()) ++conflictedPer[tI];
                }
            });
        }
        for (auto& th : pool) th.join();
    }
    const double wall = seconds(elapsedNs(t0));

    uint64_t mergeNs = 0, canonNs = 0, conflicts = 0, conflicted = 0, outBytes = 0;
    for (unsigned tI = 0; tI < a.threads; ++tI) {
        mergeNs += mergeNsPer[tI];
        canonNs += canonNsPer[tI];
        conflicts += conflictsPer[tI];
        conflicted += conflictedPer[tI];
        outBytes += outBytesPer[tI];
    }

    std::string out = "{\n";
    printKvS(out, "scenario", "merge");
    printKv(out, "threads", static_cast<uint64_t>(a.threads));
    printKv(out, "merges", static_cast<uint64_t>(prepared.size()));
    printKv(out, "wall_seconds", wall);
    printKv(out, "merges_per_sec", static_cast<double>(prepared.size()) / wall);
    printKv(out, "cpu_merge_seconds", seconds(mergeNs));
    printKv(out, "cpu_canonicalize_seconds", seconds(canonNs));
    printKv(out, "merged_output_bytes", outBytes);
    printKv(out, "total_conflicts", conflicts);
    printKv(out, "conflicted_merges", conflicted, true);
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    return 0;
}

int runCanonCheck(const Args& a) {
    FileList list = enumerateCorpus(a.corpus);
    std::vector<fs::path> sample =
        samplePaths(list.jsonFiles, static_cast<size_t>(a.sample), a.seed);
    simdjson::dom::parser parser;
    uint64_t mismatches = 0;
    std::vector<std::string> firstMismatches;
    for (const auto& p : sample) {
        simdjson::padded_string content;
        simdjson::dom::element doc;
        if (simdjson::padded_string::load(p.string()).get(content) != simdjson::SUCCESS ||
            parser.parse(content).get(doc) != simdjson::SUCCESS) {
            ++mismatches;
            continue;
        }
        ctx::JsonValue tree = buildTree(doc);
        std::string canon;
        ctx::canonicalWrite(tree, canon);
        canon.push_back('\n');
        if (canon.size() != content.size() ||
            std::memcmp(canon.data(), content.data(), canon.size()) != 0) {
            ++mismatches;
            if (firstMismatches.size() < 5)
                firstMismatches.push_back(p.generic_string());
        }
    }
    std::string out = "{\n";
    printKvS(out, "scenario", "canon-check");
    printKv(out, "checked", static_cast<uint64_t>(sample.size()));
    printKv(out, "mismatches", mismatches, firstMismatches.empty());
    if (!firstMismatches.empty()) {
        out += "  \"first_mismatches\": [";
        for (size_t i = 0; i < firstMismatches.size(); ++i) {
            if (i != 0) out += ", ";
            out += "\"" + jsonEscape(firstMismatches[i]) + "\"";
        }
        out += "]\n";
    }
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    return mismatches == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parseArgs(argc, argv);
    if (a.mode == "attach") return runAttach(a);
    if (a.mode == "edit") return runEdit(a);
    if (a.mode == "bulk") return runBulk(a);
    if (a.mode == "merge") return runMerge(a);
    if (a.mode == "canon-check") return runCanonCheck(a);
    if (a.mode == "import") {
        // No importers exist at M0 — the harness records this scenario as pending.
        std::fputs("{\"scenario\": \"import\", \"unsupported\": true}\n", stdout);
        return 0;
    }
    fail("unknown mode: " + a.mode);
}
