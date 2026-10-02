// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <handshake_failure.h>

const char* HandshakeFailurePhaseToString(HandshakeFailurePhase phase)
{
    switch (phase) {
    case HandshakeFailurePhase::kVersionNegotiation: return "version-negotiation";
    case HandshakeFailurePhase::kServiceFlags: return "service-flags";
    case HandshakeFailurePhase::kTimeout: return "timeout";
    }
    return "unknown";
}

void HandshakeFailureRecorder::Record(HandshakeFailurePhase phase, int64_t peer_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_records.push_back(HandshakeFailureRecord{
        .phase = phase,
        .peer_id = peer_id,
        .time = std::chrono::system_clock::now(),
    });
}

std::vector<HandshakeFailureRecord> HandshakeFailureRecorder::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records;
}

size_t HandshakeFailureRecorder::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records.size();
}

void HandshakeFailureRecorder::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_records.clear();
}

HandshakeFailureRecorder& GetHandshakeFailureRecorder()
{
    static HandshakeFailureRecorder recorder;
    return recorder;
}

void RecordHandshakeFailure(HandshakeFailurePhase phase, int64_t peer_id)
{
    GetHandshakeFailureRecorder().Record(phase, peer_id);
}

std::optional<HandshakeFailurePhase> NoteVersionHandshakeFailure(
    bool expect_services,
    uint64_t offered_services,
    uint64_t desirable_services,
    int peer_version,
    int min_peer_proto_version,
    int64_t peer_id)
{
    // Same check order as the "version" message handler in
    // net_processing.cpp: service bits first, then protocol version.
    if (expect_services && (offered_services & desirable_services) != desirable_services) {
        RecordHandshakeFailure(HandshakeFailurePhase::kServiceFlags, peer_id);
        return HandshakeFailurePhase::kServiceFlags;
    }
    if (peer_version < min_peer_proto_version) {
        RecordHandshakeFailure(HandshakeFailurePhase::kVersionNegotiation, peer_id);
        return HandshakeFailurePhase::kVersionNegotiation;
    }
    return std::nullopt;
}

void NoteHandshakeTimeout(int64_t peer_id)
{
    RecordHandshakeFailure(HandshakeFailurePhase::kTimeout, peer_id);
}
