// T1 for the M9 e2 WIRE file-write gateway (R-QA-013, D10 write half): the request shape the daemon
// actually parses, the refusal codes it passes through VERBATIM, the fail-closed unbound posture,
// and the two accounting facts that make the destructive path auditable (`refusals`,
// `unrecoverable_deletes`).
//
// THE MOCK IS AT THE **WIRE**, NOT AT THE CLIENT (mock_channel.h's standing warning): a double
// standing in for `client::Client` would let this suite pass over frames the daemon never sends.
// Here the frames are dispatcher.cpp's and a REAL `client::Client` parses them.
//
// EVERY "IT REFUSED" ASSERTION IS PAIRED WITH THE APPLY IT PERFORMS when the same call is answered
// with success — a refusal test whose fixture could never have applied anything proves nothing about
// the refusal.

#include "context/editor/shell/panels/wire_file_gateway.h"

#include "context/editor/client/client.h"

#include "panels_wire_test.h" // the shared REAL-client-over-a-scripted-wire fixture

#include <memory>
#include <string>
#include <utility>

namespace panels = context::editor::shell::panels;
namespace files = context::editor::gui::panels::files;
using context::editor::shell::panels::WireFileWriteGateway;
using Json = context::editor::contract::Json;
using panelstest::make_wired_client;
using panelstest::Wired;

namespace
{

[[nodiscard]] Json move_reply(const char* to)
{
    Json data = Json::object();
    data.set("from", Json(std::string("art/hero.png")));
    data.set("to", Json(std::string(to)));
    data.set("guid", Json(std::string("00000000000000000000000000000aaa")));
    data.set("generation", Json(static_cast<std::uint64_t>(7)));
    return clientmock::MockChannel::ok_envelope(std::move(data));
}

[[nodiscard]] Json delete_reply(const char* token)
{
    Json data = Json::object();
    data.set("path", Json(std::string("art/hero.png")));
    data.set("guid", Json(std::string("00000000000000000000000000000aaa")));
    data.set("restoreToken", Json(std::string(token)));
    data.set("removedAsset", Json(true));
    data.set("removedMeta", Json(true));
    data.set("generation", Json(static_cast<std::uint64_t>(8)));
    return clientmock::MockChannel::ok_envelope(std::move(data));
}

[[nodiscard]] Json restore_reply()
{
    Json data = Json::object();
    data.set("path", Json(std::string("art/hero.png")));
    data.set("guid", Json(std::string("00000000000000000000000000000aaa")));
    data.set("restoredAsset", Json(true));
    data.set("restoredMeta", Json(true));
    data.set("generation", Json(static_cast<std::uint64_t>(9)));
    return clientmock::MockChannel::ok_envelope(std::move(data));
}

} // namespace

int main()
{
    // ============ the request SHAPE: exactly the params kernel_server.cpp reads ==================
    {
        Wired wired = make_wired_client();
        std::string seen_method;
        Json seen_params = Json::object();
        wired.channel->on(WireFileWriteGateway::kMoveMethod,
                          [&](const clientmock::Request& request)
                          {
                              seen_method = request.method;
                              seen_params = request.params;
                              return move_reply("art/villain.png");
                          });

        WireFileWriteGateway gateway;
        gateway.bind_client(wired.client.get());
        const files::FileWriteResult out =
            gateway.move_file("art/hero.png", "art/villain.png");

        CHECK(out.ok());
        CHECK(seen_method == std::string(WireFileWriteGateway::kMoveMethod));
        CHECK(seen_params.at("from").as_string() == "art/hero.png");
        CHECK(seen_params.at("to").as_string() == "art/villain.png");
        // The destination is read back off the REPLY, not echoed from the request — the daemon is
        // the authority on what it wrote, and an undo built from an echo would undo what we asked
        // for rather than what happened.
        CHECK(out.path == "art/hero.png");
        CHECK(out.other_path == "art/villain.png");
        CHECK(gateway.writes_issued() == 1);
        CHECK(gateway.writes_applied() == 1);
        CHECK(gateway.refusals() == 0);
    }

    // ======================= the DELETE reply: the restore token is the undo ======================
    {
        Wired wired = make_wired_client();
        Json seen = Json::object();
        wired.channel->on(WireFileWriteGateway::kDeleteMethod,
                          [&](const clientmock::Request& request)
                          {
                              seen = request.params;
                              return delete_reply("00000000000000000000000000000aaa");
                          });

        WireFileWriteGateway gateway;
        gateway.bind_client(wired.client.get());
        const files::FileWriteResult out = gateway.delete_file("art/hero.png");

        CHECK(out.ok());
        CHECK(seen.at("path").as_string() == "art/hero.png");
        CHECK(out.path == "art/hero.png");
        CHECK(out.restore_token == "00000000000000000000000000000aaa");
        // The reversibility accounting: this delete IS undoable, so nothing was counted lost.
        CHECK(gateway.unrecoverable_deletes() == 0);
    }
    {
        // The SIBLING of the assertion above, and the reason that counter exists at all: an APPLIED
        // delete whose reply carried NO token is still applied (the file is gone — reporting
        // otherwise would leave the panel rendering a row that does not exist), but it is counted,
        // because "the human was promised an undo and did not get one" must never be invisible.
        Wired wired = make_wired_client();
        wired.channel->on(WireFileWriteGateway::kDeleteMethod,
                          [](const clientmock::Request&) { return delete_reply(""); });

        WireFileWriteGateway gateway;
        gateway.bind_client(wired.client.get());
        const files::FileWriteResult out = gateway.delete_file("art/hero.png");
        CHECK(out.ok());
        CHECK(out.restore_token.empty());
        CHECK(gateway.unrecoverable_deletes() == 1);
    }

    // ================================ restore ====================================================
    {
        Wired wired = make_wired_client();
        Json seen = Json::object();
        wired.channel->on(WireFileWriteGateway::kRestoreMethod,
                          [&](const clientmock::Request& request)
                          {
                              seen = request.params;
                              return restore_reply();
                          });

        WireFileWriteGateway gateway;
        gateway.bind_client(wired.client.get());
        const files::FileWriteResult out =
            gateway.restore_file("00000000000000000000000000000aaa");

        CHECK(out.ok());
        CHECK(seen.at("restoreToken").as_string() == "00000000000000000000000000000aaa");
        CHECK(out.path == "art/hero.png");
    }

    // ===================== the daemon's REFUSAL CODE crosses VERBATIM =============================
    {
        // Each of these is a different fact with a different remedy; flattening them to one generic
        // "the operation failed" would throw away the only actionable half of a refused destructive
        // operation. The pairing is the point: the SAME gateway, the SAME call, applies when the
        // daemon says yes (the first case above) and refuses — with the daemon's own code — here.
        const char* codes[] = {"asset.delete_referenced", "asset.move_destination_exists",
                               "asset.restore_destination_exists", "asset.restore_missing",
                               "scope.denied", "path.jail_violation"};
        for (const char* code : codes)
        {
            Wired wired = make_wired_client();
            wired.channel->fail_method(WireFileWriteGateway::kDeleteMethod,
                                       "the daemon's own diagnostic", code);

            WireFileWriteGateway gateway;
            gateway.bind_client(wired.client.get());
            const files::FileWriteResult out = gateway.delete_file("art/hero.png");

            CHECK(!out.ok());
            CHECK(out.status == files::FileWriteResult::Status::refused);
            CHECK(out.code == std::string(code));
            CHECK(!out.message.empty()); // a refusal with no message is a refusal nobody can act on
            CHECK(out.path == "art/hero.png");
            CHECK(gateway.refusals() == 1);
            CHECK(gateway.writes_applied() == 0);
            // A refused delete is NOT an unrecoverable one — nothing was deleted.
            CHECK(gateway.unrecoverable_deletes() == 0);
        }
    }

    // =========================== FAIL-CLOSED: no daemon bound ====================================
    {
        // The posture that matters most on this path: an unbound gateway REFUSES, loudly and by
        // name. It never reports `applied` (the one unforgivable lie here) and never silently
        // returns a default-constructed `none` that a caller could read as "nothing to do".
        WireFileWriteGateway gateway;
        CHECK(!gateway.has_client());

        const files::FileWriteResult moved = gateway.move_file("a.json", "b.json");
        const files::FileWriteResult deleted = gateway.delete_file("a.json");
        const files::FileWriteResult restored = gateway.restore_file("tok");

        for (const files::FileWriteResult* out : {&moved, &deleted, &restored})
        {
            CHECK(out->status == files::FileWriteResult::Status::refused);
            CHECK(out->code == std::string(WireFileWriteGateway::kNoDaemonCode));
            CHECK(!out->message.empty());
        }
        CHECK(gateway.refusals() == 3);
        CHECK(gateway.writes_issued() == 0); // nothing reached a wire that does not exist
        CHECK(gateway.writes_applied() == 0);

        // The SIBLING that makes the three refusals above non-vacuous: bind a daemon and the SAME
        // gateway applies. Without this, "it refused" could mean the fixture can never succeed.
        Wired wired = make_wired_client();
        wired.channel->on(WireFileWriteGateway::kMoveMethod,
                          [](const clientmock::Request&) { return move_reply("b.json"); });
        gateway.bind_client(wired.client.get());
        CHECK(gateway.move_file("a.json", "b.json").ok());

        // ...and CLEARING the binding restores the fail-closed posture (the § LIFETIME contract:
        // `bind_write_client(nullptr)` is what turns a would-be use-after-free into a refusal).
        gateway.bind_client(nullptr);
        CHECK(!gateway.move_file("a.json", "b.json").ok());
    }

    PANELS_TEST_MAIN_END();
}
