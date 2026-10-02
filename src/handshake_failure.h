// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HANDSHAKE_FAILURE_H
#define BITCOIN_HANDSHAKE_FAILURE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

/** Phase of the peer handshake at which a connection attempt failed. */
enum class HandshakeFailurePhase {
    /** Peer sent an obsolete protocol version during version negotiation. */
    kVersionNegotiation,
    /** Peer did not offer the service bits required for the connection type. */
    kServiceFlags,
    /** Peer did not complete the version (or V2 transport) handshake in time. */
    kTimeout,
};

const char* HandshakeFailurePhaseToString(HandshakeFailurePhase phase);

/**
 * A single handshake failure record.
 *
 * Deliberately limited to the failure phase, the numeric node id and a
 * timestamp: peer addresses and any authentication material (e.g. BIP324
 * session keys, nonces) must never be written into these records.
 */
struct HandshakeFailureRecord {
    HandshakeFailurePhase phase;
    int64_t peer_id; //!< numeric node id only, never an address
    std::chrono::system_clock::time_point time;
};

/**
 * Thread-safe in-memory log of handshake failures, ordered by occurrence.
 * Only failure paths record entries; a successful handshake records nothing.
 */
class HandshakeFailureRecorder
{
public:
    void Record(HandshakeFailurePhase phase, int64_t peer_id);
    std::vector<HandshakeFailureRecord> Snapshot() const;
    size_t Size() const;
    void Clear();

private:
    mutable std::mutex m_mutex;
    std::vector<HandshakeFailureRecord> m_records;
};

/** Global recorder used by the net/net_processing failure paths. */
HandshakeFailureRecorder& GetHandshakeFailureRecorder();

/** Record a handshake failure in the global recorder. */
void RecordHandshakeFailure(HandshakeFailurePhase phase, int64_t peer_id);

/**
 * Classify and record a failed incoming VERSION handshake, applying the
 * checks in the same order as the "version" message handler in
 * net_processing.cpp: required service bits first, then the minimum
 * protocol version.
 *
 * @return the failing phase, or std::nullopt if the message passes both
 *         checks (a passing handshake records nothing).
 */
std::optional<HandshakeFailurePhase> NoteVersionHandshakeFailure(
    bool expect_services,
    uint64_t offered_services,
    uint64_t desirable_services,
    int peer_version,
    int min_peer_proto_version,
    int64_t peer_id);

/** Record a handshake timeout (no VERSION/VERACK or V2 transport handshake
 *  completed before the inactivity timeout fired). */
void NoteHandshakeTimeout(int64_t peer_id);

#endif // BITCOIN_HANDSHAKE_FAILURE_H
