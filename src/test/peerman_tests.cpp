// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/params.h>
#include <net.h>
#include <net_processing.h>
#include <node/block_template_manager.h>
#include <node/miner.h>
#include <node/peerman_args.h>
#include <pow.h>
#include <primitives/block.h>
#include <protocol.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <util/check.h>
#include <util/result.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(peerman_tests, RegTestingSetup)

/** Window, in blocks, for connecting to NODE_NETWORK_LIMITED peers */
static constexpr int64_t NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS = 144;

static void mineBlock(node::NodeContext& node, FakeNodeClock& clock, std::chrono::seconds block_time)
{
    auto curr_time = GetTime<std::chrono::seconds>();
    clock.set(block_time); // update time so the block is created with it
    auto& block_template_manager{*Assert(node.block_template_manager)};
    auto block_template{block_template_manager.CreateNewTemplate({})};
    BOOST_REQUIRE(block_template);
    CBlock block{block_template->block};
    while (!CheckProofOfWork(block.GetHash(), block.nBits, node.chainman->GetConsensus())) ++block.nNonce;
    block.fChecked = true; // little speedup
    clock.set(curr_time); // process block at current time
    Assert(node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, nullptr));
    node.validation_signals->SyncWithValidationInterfaceQueue(); // drain events queue
}

// Verifying when network-limited peer connections are desirable based on the node's proximity to the tip
BOOST_AUTO_TEST_CASE(connections_desirable_service_flags)
{
    FakeNodeClock clock{};
    std::unique_ptr<PeerManager> peerman = PeerManager::make(*m_node.connman, *m_node.addrman, nullptr, *m_node.chainman, *m_node.mempool, *m_node.warnings, {});
    auto consensus = m_node.chainman->GetParams().GetConsensus();

    // Check we start connecting to full nodes
    ServiceFlags peer_flags{NODE_WITNESS | NODE_NETWORK_LIMITED};
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));

    // Make peerman aware of the initial best block and verify we accept limited peers when we start close to the tip time.
    auto tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip());
    uint64_t tip_block_time = tip->GetBlockTime();
    int tip_block_height = tip->nHeight;
    peerman->SetBestBlock(tip_block_height, std::chrono::seconds{tip_block_time});

    clock.set(std::chrono::seconds{tip_block_time + 1}); // Set node time to tip time
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS));

    // Check we don't disallow limited peers connections when we are behind but still recoverable (below the connection safety window)
    clock += std::chrono::seconds{consensus.nPowTargetSpacing * (NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS - 1)};
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS));

    // Check we disallow limited peers connections when we are further than the limited peers safety window
    clock += std::chrono::seconds{consensus.nPowTargetSpacing * 2};
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));

    // By now, we tested that the connections desirable services flags change based on the node's time proximity to the tip.
    // Now, perform the same tests for when the node receives a block.
    m_node.validation_signals->RegisterValidationInterface(peerman.get());

    // First, verify a block in the past doesn't enable limited peers connections
    // At this point, our time is (NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS + 1) * 10 minutes ahead the tip's time.
    mineBlock(m_node, clock, /*block_time=*/std::chrono::seconds{tip_block_time + 1});
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));

    // Verify a block close to the tip enables limited peers connections
    mineBlock(m_node, clock, /*block_time=*/GetTime<std::chrono::seconds>());
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS));

    // Lastly, verify the stale tip checks can disallow limited peers connections after not receiving blocks for a prolonged period.
    clock += std::chrono::seconds{consensus.nPowTargetSpacing * NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS + 1};
    BOOST_CHECK(peerman->GetDesirableServiceFlags(peer_flags) == ServiceFlags(NODE_NETWORK | NODE_WITNESS));
}

namespace {
//! Register the arguments read by node::ApplyArgsManOptions so that a plain
//! (non-fixture) ArgsManager accepts them.
void RegisterPeerArgs(ArgsManager& argsman)
{
    argsman.AddArg("-blocksonly", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-blockreconstructionextratxn=<n>", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-capturemessages", "", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-connect=<ip>", "", ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::CONNECTION);
    argsman.AddArg("-maxconnections=<n>", "", ArgsManager::ALLOW_ANY, OptionsCategory::CONNECTION);
    argsman.AddArg("-privatebroadcast", "", ArgsManager::ALLOW_ANY, OptionsCategory::NODE_RELAY);
    argsman.AddArg("-txreconciliation", "", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CONNECTION);
    argsman.AddArg("-txsendrate=<n>", "", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::NODE_RELAY);
}

//! Parse a command line like "-privatebroadcast -connect=1.2.3.4" for @p argsman.
void ParsePeerArgs(ArgsManager& argsman, const std::string& command_line)
{
    std::vector<std::string> tokens{"bitcoind"};
    if (!command_line.empty()) {
        std::string current;
        for (char c : command_line) {
            if (c == ' ') {
                tokens.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        tokens.push_back(current);
    }
    std::vector<const char*> argv;
    argv.reserve(tokens.size());
    for (const std::string& token : tokens) argv.push_back(token.c_str());
    std::string error;
    BOOST_REQUIRE(argsman.ParseParameters(argv.size(), argv.data(), error));
    BOOST_CHECK(error.empty());
}

//! Apply the given command line to a fresh PeerManager::Options and return the result.
util::Result<void> ApplyPeerArgs(const std::string& command_line, PeerManager::Options& opts)
{
    ArgsManager argsman;
    RegisterPeerArgs(argsman);
    ParsePeerArgs(argsman, command_line);
    return node::ApplyArgsManOptions(argsman, opts);
}

//! Convenience wrapper when the resulting options are not inspected.
util::Result<void> CheckPeerArgs(const std::string& command_line)
{
    PeerManager::Options opts;
    return ApplyPeerArgs(command_line, opts);
}
} // namespace

// Verify mutually exclusive connection-limit combinations are rejected and
// that the error message names the specific option instead of producing a
// generic argument parsing failure.
BOOST_AUTO_TEST_CASE(peer_args_conflicts)
{
    // -privatebroadcast together with -connect=<ip> is incompatible.
    {
        auto result{CheckPeerArgs("-privatebroadcast=1 -connect=127.0.0.1:8333")};
        BOOST_REQUIRE(!result);
        const std::string message{util::ErrorString(result).original};
        BOOST_CHECK(message.find("-privatebroadcast") != std::string::npos);
        BOOST_CHECK(message.find("-connect") != std::string::npos);
    }

    // -privatebroadcast together with the negated form -noconnect is
    // incompatible as well, even with no -connect value given.
    {
        auto result{CheckPeerArgs("-privatebroadcast -noconnect")};
        BOOST_REQUIRE(!result);
        const std::string message{util::ErrorString(result).original};
        BOOST_CHECK(message.find("-privatebroadcast") != std::string::npos);
        BOOST_CHECK(message.find("-connect") != std::string::npos);
    }

    // A negative -maxconnections is invalid and the message must point at
    // that specific option.
    {
        auto result{CheckPeerArgs("-maxconnections=-1")};
        BOOST_REQUIRE(!result);
        const std::string message{util::ErrorString(result).original};
        BOOST_CHECK_EQUAL(message, "-maxconnections must be greater or equal than zero");
    }
}

// Boundary values: the smallest allowed -maxconnections (0) must be accepted,
// including in combination with -privatebroadcast, and the options must be
// applied as requested.
BOOST_AUTO_TEST_CASE(peer_args_boundaries)
{
    // -maxconnections=0 alone is valid (disables automatic connections).
    BOOST_CHECK(CheckPeerArgs("-maxconnections=0"));

    // -maxconnections=0 is an explicitly supported setup together with
    // -privatebroadcast: private broadcast connections have their own limit.
    {
        PeerManager::Options opts;
        auto result{ApplyPeerArgs("-maxconnections=0 -privatebroadcast=1", opts)};
        BOOST_REQUIRE(result);
        BOOST_CHECK(opts.private_broadcast);
    }

    // -connect alone (without -privatebroadcast) remains valid.
    BOOST_CHECK(CheckPeerArgs("-connect=127.0.0.1:8333"));

    // -privatebroadcast can be explicitly disabled together with -connect.
    {
        PeerManager::Options opts;
        auto result{ApplyPeerArgs("-privatebroadcast=0 -connect=127.0.0.1:8333", opts)};
        BOOST_REQUIRE(result);
        BOOST_CHECK(!opts.private_broadcast);
    }
}

// When no relevant argument is provided, every built-in default must remain
// unchanged and no error must be reported.
BOOST_AUTO_TEST_CASE(peer_args_defaults)
{
    {
        PeerManager::Options applied;
        const PeerManager::Options defaults;
        auto result{ApplyPeerArgs("", applied)};
        BOOST_REQUIRE(result);
        BOOST_CHECK_EQUAL(applied.ignore_incoming_txs, defaults.ignore_incoming_txs);
        BOOST_CHECK_EQUAL(applied.reconcile_txs, defaults.reconcile_txs);
        BOOST_CHECK_EQUAL(applied.max_extra_txs, defaults.max_extra_txs);
        BOOST_CHECK_EQUAL(applied.capture_messages, defaults.capture_messages);
        BOOST_CHECK_EQUAL(applied.private_broadcast, defaults.private_broadcast);
        BOOST_CHECK_EQUAL(applied.tx_send_rate, defaults.tx_send_rate);
    }

    // Unrelated peer options without conflicts keep working.
    {
        PeerManager::Options opts;
        auto result{ApplyPeerArgs("-txreconciliation=1 -blocksonly=1 -txsendrate=7 -blockreconstructionextratxn=42", opts)};
        BOOST_REQUIRE(result);
        BOOST_CHECK(opts.reconcile_txs);
        BOOST_CHECK(opts.ignore_incoming_txs);
        BOOST_CHECK_EQUAL(opts.tx_send_rate, 7u);
        BOOST_CHECK_EQUAL(opts.max_extra_txs, 42u);
    }

    // Default private broadcast is off, so it does not conflict with the
    // default -connect-less setup or with an explicit -connect when the
    // default is kept.
    {
        PeerManager::Options opts;
        // Note: -connect provided but -privatebroadcast left at its default
        BOOST_REQUIRE(ApplyPeerArgs("-connect=127.0.0.1:8333 -txsendrate=0", opts));
        BOOST_CHECK(!opts.private_broadcast);
        // txsendrate is clamped to the documented minimum of 1.
        BOOST_CHECK_EQUAL(opts.tx_send_rate, 1u);
    }
}

BOOST_AUTO_TEST_SUITE_END()
