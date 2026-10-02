// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.
//
// Standalone fixture test for handshake failure phase recording
// (src/handshake_failure.{h,cpp} and its call sites in net_processing.cpp /
// net.cpp).
//
// It injects VERSION message fixtures (wrong protocol version, missing
// service bits) and a handshake timeout, and checks that the recorded
// failure phases match. It also checks the boundaries: records carry no
// address or authentication material, and a successful handshake produces
// no failure record.
//
// This file is intentionally not part of the Boost unit test suite so it
// can be built and run without compiling the full node. From the
// repository root:
//
//   g++ -std=c++20 -Wall -Wextra -I src
//       src/handshake_failure.cpp
//       src/test/handshake_failure_fixture_test.cpp
//       -o /tmp/handshake_failure_fixture_test
//   /tmp/handshake_failure_fixture_test
//
// Exit code 0 means all checks passed.

#include <handshake_failure.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// Mirror of src/node/protocol_version.h (kept local so this test stays
// self-contained): inline constexpr int MIN_PEER_PROTO_VERSION = 31800;
constexpr int MIN_PEER_PROTO_VERSION{31800};

// Mirror of the service bits from src/protocol.h used by
// PeerManagerImpl::GetDesirableServiceFlags for a full node near the tip:
// NODE_NETWORK = (1 << 0), NODE_WITNESS = (1 << 3).
constexpr uint64_t NODE_NETWORK{1 << 0};
constexpr uint64_t NODE_WITNESS{1 << 3};
constexpr uint64_t DESIRABLE_SERVICES{NODE_NETWORK | NODE_WITNESS};

int g_failures{0};

void Check(bool ok, const std::string& what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

// --- VERSION message fixtures ---------------------------------------------
// Little-endian serialization of the leading VERSION message fields, exactly
// as read by the "version" handler in net_processing.cpp:
//   vRecv >> nVersion >> services(8) >> nTime;

void PushLE(std::vector<uint8_t>& out, uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<uint8_t>(value & 0xff));
        value >>= 8;
    }
}

std::vector<uint8_t> MakeVersionFixture(int32_t version, uint64_t services, int64_t time = 1700000000)
{
    std::vector<uint8_t> fixture;
    PushLE(fixture, static_cast<uint32_t>(version), 4);
    PushLE(fixture, services, 8);
    PushLE(fixture, static_cast<uint64_t>(time), 8);
    return fixture;
}

struct VersionFixtureReader {
    const std::vector<uint8_t>& bytes;
    size_t pos{0};

    uint64_t ReadLE(int num_bytes)
    {
        uint64_t value{0};
        for (int i = 0; i < num_bytes; ++i) value |= uint64_t{bytes.at(pos++)} << (8 * i);
        return value;
    }
    int32_t Version() { return static_cast<int32_t>(ReadLE(4)); }
    uint64_t Services() { return ReadLE(8); }
    int64_t Time() { return static_cast<int64_t>(ReadLE(8)); }
};

// Drive a VERSION fixture through the same classification the "version"
// handler in net_processing.cpp uses (outbound full-relay style connection,
// i.e. services are expected).
std::optional<HandshakeFailurePhase> InjectVersionFixture(
    const std::vector<uint8_t>& fixture, int64_t peer_id, bool expect_services = true)
{
    VersionFixtureReader reader{fixture};
    const int32_t version{reader.Version()};
    const uint64_t services{reader.Services()};
    reader.Time(); // consumed but unused, like the nTime sanity clamp
    return NoteVersionHandshakeFailure(expect_services, services, DESIRABLE_SERVICES,
                                       version, MIN_PEER_PROTO_VERSION, peer_id);
}

} // namespace

int main()
{
    // Boundary: the record type must not be able to hold addresses or
    // authentication material - only a phase, a numeric id and a timestamp.
    static_assert(std::is_same_v<decltype(HandshakeFailureRecord::peer_id), int64_t>,
                  "records must carry a numeric node id, never an address");
    static_assert(!std::is_convertible_v<HandshakeFailureRecord, std::string>);
    Check(sizeof(HandshakeFailureRecord) <= 32,
          "record layout holds only phase, numeric peer id and timestamp (no address/auth material)");

    auto& recorder{GetHandshakeFailureRecorder()};
    recorder.Clear();

    // 1. Wrong protocol version (obsolete) -> version-negotiation phase.
    {
        const auto fixture{MakeVersionFixture(/*version=*/30000, DESIRABLE_SERVICES)};
        const auto phase{InjectVersionFixture(fixture, /*peer_id=*/11)};
        Check(phase == HandshakeFailurePhase::kVersionNegotiation,
              "obsolete version fixture classified as version-negotiation");
    }

    // 2. Missing service bits -> service-flags phase.
    {
        const auto fixture{MakeVersionFixture(/*version=*/70016, /*services=*/0)};
        const auto phase{InjectVersionFixture(fixture, /*peer_id=*/12)};
        Check(phase == HandshakeFailurePhase::kServiceFlags,
              "missing service bits fixture classified as service-flags");
    }

    // 2b. Service bits are checked before the version (matches the order in
    //     net_processing.cpp): a peer failing both reports service-flags.
    {
        const auto fixture{MakeVersionFixture(/*version=*/30000, /*services=*/0)};
        const auto phase{InjectVersionFixture(fixture, /*peer_id=*/13)};
        Check(phase == HandshakeFailurePhase::kServiceFlags,
              "service bits checked before protocol version");
    }

    // 3. Handshake timeout (no VERSION/VERACK in time, as in
    //    CConnman::InactivityCheck) -> timeout phase.
    {
        NoteHandshakeTimeout(/*peer_id=*/14);
    }

    // 4. Successful handshake: valid version + all desirable service bits
    //    must not produce any failure record.
    const size_t size_before_success{recorder.Size()};
    {
        const auto fixture{MakeVersionFixture(/*version=*/70016, DESIRABLE_SERVICES)};
        const auto phase{InjectVersionFixture(fixture, /*peer_id=*/15)};
        Check(phase == std::nullopt, "valid version fixture passes both checks");
    }
    Check(recorder.Size() == size_before_success,
          "successful handshake produces no failure record");

    // Verify the recorded phases, order and peer ids.
    const auto records{recorder.Snapshot()};
    Check(records.size() == 4, "exactly four failure records (3 fixtures + 1 timeout)");
    if (records.size() == 4) {
        Check(records[0].phase == HandshakeFailurePhase::kVersionNegotiation && records[0].peer_id == 11,
              "record 1: version-negotiation for peer 11");
        Check(records[1].phase == HandshakeFailurePhase::kServiceFlags && records[1].peer_id == 12,
              "record 2: service-flags for peer 12");
        Check(records[2].phase == HandshakeFailurePhase::kServiceFlags && records[2].peer_id == 13,
              "record 3: service-flags for peer 13");
        Check(records[3].phase == HandshakeFailurePhase::kTimeout && records[3].peer_id == 14,
              "record 4: timeout for peer 14");
        for (const auto& r : records) {
            std::printf("  recorded: peer=%d phase=%s\n", int(r.peer_id),
                        HandshakeFailurePhaseToString(r.phase));
        }
    }

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
