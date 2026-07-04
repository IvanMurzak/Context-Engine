// R-CLI-017 resource-handle tests: URI round-trip (happy), strict-parse rejection of malformed /
// hostile URIs (failure paths), the largeResult JSON shape, and the describe-advertised descriptor.
// (R-QA-013: happy + edge + failure coverage.)

#include "context/editor/contract/json.h"
#include "context/editor/contract/resource_handle.h"
#include "contract_test.h"

#include <string>

using namespace context::editor::contract;

int main()
{
    // --- URI round-trip (happy path) -------------------------------------------------------------
    {
        ResourceHandle h;
        h.instance_id = "inc-2026.07.04_a1";
        h.payload_id = 42;
        h.size_bytes = 9000000;
        const std::string uri = h.to_uri();
        CHECK(uri == "context-res://v0/inc-2026.07.04_a1/42?bytes=9000000");

        const auto parsed = ResourceHandle::parse(uri);
        CHECK(parsed.has_value());
        CHECK(parsed->instance_id == h.instance_id);
        CHECK(parsed->payload_id == h.payload_id);
        CHECK(parsed->size_bytes == h.size_bytes);
    }

    // --- edge: zero-size payload, payload id 0, minimal instance id ------------------------------
    {
        ResourceHandle h;
        h.instance_id = "x";
        const auto parsed = ResourceHandle::parse(h.to_uri());
        CHECK(parsed.has_value());
        CHECK(parsed->payload_id == 0);
        CHECK(parsed->size_bytes == 0);
    }

    // --- failure paths: strict parse rejects malformed / hostile input ---------------------------
    CHECK(!ResourceHandle::parse("").has_value());
    CHECK(!ResourceHandle::parse("file:///etc/passwd").has_value());          // wrong scheme
    CHECK(!ResourceHandle::parse("context-res://v1/i/1?bytes=1").has_value()); // unknown version
    CHECK(!ResourceHandle::parse("context-res://v0/1?bytes=1").has_value());   // missing payload id
    CHECK(!ResourceHandle::parse("context-res://v0//1?bytes=1").has_value());  // empty instance
    CHECK(!ResourceHandle::parse("context-res://v0/a b/1?bytes=1").has_value()); // bad charset
    // ".." IS charset-valid: an instance id is a MAP-KEY the resolver looks up (it can only fail to
    // match a live incarnation), never a filesystem path — no traversal semantics exist to exploit.
    CHECK(ResourceHandle::parse("context-res://v0/../1?bytes=1").has_value());
    CHECK(!ResourceHandle::parse("context-res://v0/i/x?bytes=1").has_value()); // non-numeric payload
    CHECK(!ResourceHandle::parse("context-res://v0/i/1?bytes=").has_value());  // empty byte count
    CHECK(!ResourceHandle::parse("context-res://v0/i/1?bytes=12x").has_value()); // trailing junk
    CHECK(!ResourceHandle::parse("context-res://v0/i/1").has_value());         // missing query

    // --- the largeResult JSON shape (with and without the same-FS hint) --------------------------
    {
        ResourceHandle h;
        h.instance_id = "inst";
        h.payload_id = 7;
        h.size_bytes = 1234;

        const Json bare = h.to_json();
        CHECK(bare.at("handle").as_string() == h.to_uri());
        CHECK(bare.at("sizeBytes").as_int() == 1234);
        CHECK(!bare.contains("localPath")); // the hint is optional — absent when not supplied

        const Json hinted = h.to_json("C:/proj/.editor/resources/7.res");
        CHECK(hinted.at("localPath").as_string() == "C:/proj/.editor/resources/7.res");
    }

    // --- the describe-advertised descriptor (R-CLI-013 shape) ------------------------------------
    {
        const Json d = large_result_descriptor();
        CHECK(d.at("uriScheme").as_string() == "context-res");
        CHECK(d.at("rpcMethod").as_string() == "resource.read");
        CHECK(d.at("cliCommand").as_string() == "context fetch");
        CHECK(d.at("chunkEncoding").as_string() == "hex");
        CHECK(d.at("handleShape").is_object());
        CHECK(d.at("handleShape").contains("handle"));
        CHECK(d.at("handleShape").contains("sizeBytes"));
        CHECK(d.at("handleShape").contains("localPath"));
        CHECK(d.at("readResultShape").is_object());
        CHECK(d.at("readResultShape").contains("chunkHex"));
        CHECK(d.at("readResultShape").contains("eof"));
        // The document survives a dump/parse cycle (it ships inside describe).
        const Json reparsed = Json::parse(d.dump());
        CHECK(reparsed.at("uriScheme").as_string() == "context-res");
    }

    CONTRACT_TEST_MAIN_END();
}
