// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <node/peerman_args.h>

#include <common/args.h>
#include <net.h>
#include <net_processing.h>
#include <util/result.h>
#include <util/translation.h>

#include <algorithm>
#include <limits>

namespace node {

util::Result<void> ApplyArgsManOptions(const ArgsManager& argsman, PeerManager::Options& options)
{
    // Peer-related options are applied below only after validating that the
    // explicitly provided options do not form an incompatible combination. All
    // checks use IsArgSet()/IsArgNegated() (rather than the option's resolved
    // value) so that relying on the built-in defaults never errors.

    const bool private_broadcast{argsman.GetBoolArg("-privatebroadcast", DEFAULT_PRIVATE_BROADCAST)};

    // -privatebroadcast opens its own short-lived outgoing connections to
    // randomly chosen Tor/I2P peers. That is impossible when automatic
    // outgoing connections are disabled via -connect=<ip> (which restricts
    // outgoing connections to the given nodes) or its negated form
    // -noconnect. The error names the specific conflicting option.
    if (private_broadcast &&
        (!argsman.GetArgs("-connect").empty() || argsman.IsArgNegated("-connect"))) {
        return util::Error{_("Private broadcast of own transactions requested (-privatebroadcast), "
                             "but -connect is also configured. They are incompatible because the "
                             "private broadcast needs to open new connections to randomly "
                             "chosen Tor or I2P peers. Consider using -maxconnections=0 -addnode=... "
                             "instead")};
    }

    // -maxconnections=0 disables automatic connections but, unlike -connect,
    // it is an explicitly supported setup for -privatebroadcast (short-lived
    // private broadcast connections have a separate limit). Still, the value
    // must not be negative; point at the offending option instead of a
    // generic parsing error.
    if (argsman.IsArgSet("-maxconnections")) {
        const int max_connections{argsman.GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS)};
        if (max_connections < 0) {
            return util::Error{Untranslated("-maxconnections must be greater or equal than zero")};
        }
    }

    if (auto value{argsman.GetBoolArg("-txreconciliation")}) options.reconcile_txs = *value;

    if (auto value{argsman.GetIntArg("-blockreconstructionextratxn")}) {
        options.max_extra_txs = uint32_t((std::clamp<int64_t>(*value, 0, std::numeric_limits<uint32_t>::max())));
    }

    if (auto value{argsman.GetBoolArg("-capturemessages")}) options.capture_messages = *value;

    if (auto value{argsman.GetBoolArg("-blocksonly")}) options.ignore_incoming_txs = *value;

    if (auto value{argsman.GetIntArg("-txsendrate")}) {
        options.tx_send_rate = uint32_t(std::clamp<int64_t>(*value, 1, 1000));
    }

    if (auto value{argsman.GetBoolArg("-privatebroadcast")}) options.private_broadcast = *value;

    return {};
}

} // namespace node

