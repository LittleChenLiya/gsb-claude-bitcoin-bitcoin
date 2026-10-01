// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit.

#include <node/peerman_args.h>

#include <common/args.h>
#include <net.h>
#include <net_processing.h>
#include <tinyformat.h>
#include <util/translation.h>

#include <algorithm>
#include <limits>

namespace node {

util::Result<void> ApplyArgsManOptions(const ArgsManager& argsman, PeerManager::Options& options)
{
    // Peer-to-peer parameter conflict check.
    //
    // The following combinations of connection-limit / listening parameters
    // are incompatible. They are reported with an explicit error that names
    // the conflicting parameter(s), so the user can fix the configuration
    // instead of being faced with a generic parse error. Boundary values
    // (e.g. -maxconnections=0 on its own) are accepted, and unset parameters
    // keep their built-in defaults.

    // Accepting inbound connections requires at least one automatic
    // connection slot. With -maxconnections=0 there are no slots left, so
    // -listen=1 cannot possibly work.
    if (argsman.IsArgSet("-listen") && argsman.GetBoolArg("-listen", DEFAULT_LISTEN)) {
        if (argsman.IsArgSet("-maxconnections") &&
            argsman.GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS) == 0) {
            return util::Error{Untranslated(strprintf(
                "Incompatible connection limits: -listen=1 cannot be combined with -maxconnections=0. "
                "Listening for inbound connections requires at least one automatic connection slot."))};
        }
        // -connect restricts automatic connections to the specified peers and
        // disables listening for inbound connections entirely.
        if (!argsman.GetArgs("-connect").empty() || argsman.IsArgNegated("-connect")) {
            return util::Error{Untranslated(strprintf(
                "Incompatible connection limits: -listen=1 cannot be combined with -connect. "
                "When -connect is set, automatic connections are restricted to the specified peers "
                "and inbound connections are not accepted."))};
        }
    }

    // Transaction reconciliation (BIP 330) relays transaction inventories to
    // peers, while -blocksonly rejects all transactions from network peers.
    // Enabling both is contradictory.
    if (argsman.GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY) &&
        argsman.GetBoolArg("-txreconciliation", DEFAULT_TXRECONCILIATION_ENABLE)) {
        return util::Error{Untranslated(strprintf(
            "Incompatible peer options: -blocksonly=1 cannot be combined with -txreconciliation=1. "
            "Transaction reconciliation requires relaying transactions, which -blocksonly disables."))};
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
