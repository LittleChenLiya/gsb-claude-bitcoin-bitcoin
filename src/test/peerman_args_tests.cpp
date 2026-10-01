// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <net.h>
#include <net_processing.h>
#include <node/peerman_args.h>
#include <test/util/setup_common.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(peerman_args_tests, BasicTestingSetup)

namespace {

//! Register all peer-related parameters on \p args, so command-line parsing
//! accepts them like a real bitcoind invocation would.
void RegisterPeerArgs(ArgsManager& args)
{
    const std::vector<std::pair<std::string, unsigned int>> params{
        {"-blocksonly", ArgsManager::ALLOW_ANY},
        {"-blockreconstructionextratxn", ArgsManager::ALLOW_ANY},
        {"-capturemessages", ArgsManager::ALLOW_ANY},
        {"-connect", ArgsManager::ALLOW_ANY},
        {"-listen", ArgsManager::ALLOW_ANY},
        {"-maxconnections", ArgsManager::ALLOW_ANY},
        {"-privatebroadcast", ArgsManager::ALLOW_ANY},
        {"-txreconciliation", ArgsManager::ALLOW_ANY},
        {"-txsendrate", ArgsManager::ALLOW_ANY},
    };
    for (const auto& [name, flags] : params) {
        args.AddArg(name, "", flags, OptionsCategory::OPTIONS);
    }
}

//! Parse \p argv (without the program name) and apply the peer options.
util::Result<void> ParseAndApply(ArgsManager& args, std::initializer_list<const char*> argv)
{
    std::vector<const char*> full_argv{"/test"};
    full_argv.insert(full_argv.end(), argv.begin(), argv.end());
    std::string error;
    BOOST_REQUIRE_MESSAGE(args.ParseParameters(static_cast<int>(full_argv.size()), full_argv.data(), error), error);
    PeerManager::Options options{};
    return node::ApplyArgsManOptions(args, options);
}

void ExpectConflict(std::initializer_list<const char*> argv,
                    const std::string& first_param, const std::string& second_param)
{
    ArgsManager args;
    RegisterPeerArgs(args);
    const auto result{ParseAndApply(args, argv)};
    BOOST_CHECK_MESSAGE(!result, "expected an error for the conflicting parameter combination");
    const std::string msg{util::ErrorString(result).original};
    BOOST_CHECK_MESSAGE(msg.find(first_param) != std::string::npos,
                        "error should name parameter " + first_param + ", got: " + msg);
    BOOST_CHECK_MESSAGE(msg.find(second_param) != std::string::npos,
                        "error should name parameter " + second_param + ", got: " + msg);
}

void ExpectSuccess(std::initializer_list<const char*> argv)
{
    ArgsManager args;
    RegisterPeerArgs(args);
    const auto result{ParseAndApply(args, argv)};
    BOOST_CHECK_MESSAGE(result, "unexpected error: " + util::ErrorString(result).original);
}

} // namespace

// Default parameter combination: no parameters at all. The built-in defaults
// must be preserved.
BOOST_AUTO_TEST_CASE(default_combination)
{
    ArgsManager args;
    RegisterPeerArgs(args);
    PeerManager::Options options{};
    BOOST_CHECK(node::ApplyArgsManOptions(args, options));

    BOOST_CHECK_EQUAL(options.ignore_incoming_txs, DEFAULT_BLOCKSONLY);
    BOOST_CHECK_EQUAL(options.reconcile_txs, DEFAULT_TXRECONCILIATION_ENABLE);
    BOOST_CHECK_EQUAL(options.max_extra_txs, DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN);
    BOOST_CHECK_EQUAL(options.capture_messages, false);
    BOOST_CHECK_EQUAL(options.private_broadcast, DEFAULT_PRIVATE_BROADCAST);
    BOOST_CHECK_EQUAL(options.tx_send_rate, DEFAULT_TX_SEND_RATE);
}

// Mutually exclusive parameter combinations must be rejected with an error
// that names the conflicting parameters.
BOOST_AUTO_TEST_CASE(mutually_exclusive_combinations)
{
    // -listen=1 x -maxconnections=0: no automatic connection slots left to
    // accept inbound connections.
    ExpectConflict({"-listen=1", "-maxconnections=0"}, "-listen", "-maxconnections");

    // -listen=1 x -connect: automatic connections are restricted to the
    // specified peers, inbound connections are not accepted.
    ExpectConflict({"-listen=1", "-connect=127.0.0.1"}, "-listen", "-connect");

    // -listen=1 x -noconnect: same restriction, expressed as a negation.
    ExpectConflict({"-listen=1", "-noconnect"}, "-listen", "-connect");

    // -blocksonly=1 x -txreconciliation=1: rejecting all network transactions
    // contradicts running the transaction reconciliation protocol.
    ExpectConflict({"-blocksonly=1", "-txreconciliation=1"}, "-blocksonly", "-txreconciliation");
}

// Boundary parameter combinations must be accepted (as long as they do not
// form one of the mutually exclusive pairs above) and must keep the
// historical clamping behavior.
BOOST_AUTO_TEST_CASE(boundary_combinations)
{
    // -maxconnections=0 on its own disables automatic connections but is not
    // a conflict (listening is not explicitly requested).
    ExpectSuccess({"-maxconnections=0"});

    // -maxconnections=0 together with explicitly disabled listening is fine.
    ExpectSuccess({"-maxconnections=0", "-listen=0"});

    // The txsendrate clamp boundaries.
    ExpectSuccess({"-txsendrate=1"});
    ExpectSuccess({"-txsendrate=1000"});

    // Out-of-range values keep the historical clamp behavior: values below 1
    // become 1, values above 1000 become 1000.
    {
        ArgsManager args;
        RegisterPeerArgs(args);
        PeerManager::Options options{};
        BOOST_CHECK(node::ApplyArgsManOptions(args, options));
        args.ForceSetArg("-txsendrate", "0");
        BOOST_CHECK(node::ApplyArgsManOptions(args, options));
        BOOST_CHECK_EQUAL(options.tx_send_rate, 1u);
    }
    {
        ArgsManager args;
        RegisterPeerArgs(args);
        PeerManager::Options options{};
        BOOST_CHECK(node::ApplyArgsManOptions(args, options));
        args.ForceSetArg("-txsendrate", "1001");
        BOOST_CHECK(node::ApplyArgsManOptions(args, options));
        BOOST_CHECK_EQUAL(options.tx_send_rate, 1000u);
    }

    // The blockreconstructionextratxn lower boundary.
    ExpectSuccess({"-blockreconstructionextratxn=0"});
}

// Each side of a conflict is valid on its own, and explicitly disabling one
// side keeps the combination valid.
BOOST_AUTO_TEST_CASE(valid_combinations)
{
    ExpectSuccess({"-listen=1"});
    ExpectSuccess({"-connect=127.0.0.1"});
    ExpectSuccess({"-blocksonly=1"});
    ExpectSuccess({"-txreconciliation=1"});
    ExpectSuccess({"-blocksonly=1", "-txreconciliation=0"});
    ExpectSuccess({"-blocksonly=0", "-txreconciliation=1"});
}

BOOST_AUTO_TEST_SUITE_END()
