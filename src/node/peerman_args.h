// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_PEERMAN_ARGS_H
#define BITCOIN_NODE_PEERMAN_ARGS_H

#include <net_processing.h>
#include <util/result.h>

class ArgsManager;
namespace node {

/**
 * Overlay the peer-related startup options set in \p argsman on top of the
 * corresponding members in \p options.
 *
 * Besides mapping individual options, this verifies that the requested
 * peer-to-peer configuration is consistent and rejects incompatible
 * combinations with an error that names the offending parameter(s), instead
 * of failing with a generic parse error. The checked connection-limit
 * combinations are:
 *
 *   - -listen=1 together with -maxconnections=0: accepting inbound
 *     connections requires at least one automatic connection slot.
 *   - -listen=1 together with -connect (or -noconnect): -connect restricts
 *     automatic connections to the specified peers and disables listening for
 *     inbound connections entirely.
 *   - -blocksonly=1 together with -txreconciliation=1: transaction
 *     reconciliation (BIP 330) relays transaction inventories, while
 *     -blocksonly rejects all transactions from network peers; enabling both
 *     is contradictory.
 *
 * Boundary values are accepted as long as they do not form one of the above
 * combinations, and unset parameters keep their built-in defaults.
 *
 * @param[in]  argsman The ArgsManager in which to check set options.
 * @param[in,out] options The PeerManager::Options to modify according to
 *                        \p argsman.
 * @returns An error naming the conflicting parameter(s) if an incompatible
 *          combination is configured, otherwise an empty result.
 */
[[nodiscard]] util::Result<void> ApplyArgsManOptions(const ArgsManager& argsman, PeerManager::Options& options);

} // namespace node

#endif // BITCOIN_NODE_PEERMAN_ARGS_H
