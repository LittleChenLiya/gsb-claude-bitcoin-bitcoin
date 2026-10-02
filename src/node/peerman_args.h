// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#ifndef BITCOIN_NODE_PEERMAN_ARGS_H
#define BITCOIN_NODE_PEERMAN_ARGS_H

#include <net_processing.h>
#include <util/result.h>

class ArgsManager;

namespace node {
//! Read peer-manager related settings from @p argsman into @p options.
//!
//! In addition to applying the individual options, this validates that the
//! connection-limit related options the user explicitly configured do not
//! conflict with each other. Only explicitly provided options are examined, so
//! leaving every option at its default can never result in an error. On
//! failure, the returned error names the specific option that is involved
//! instead of producing a generic argument parsing failure.
[[nodiscard]] util::Result<void> ApplyArgsManOptions(const ArgsManager& argsman, PeerManager::Options& options);
} // namespace node

#endif // BITCOIN_NODE_PEERMAN_ARGS_H
