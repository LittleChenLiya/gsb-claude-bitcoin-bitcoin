// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>
#include <addrman.h>
#include <banman.h>
#include <chainparams.h>
#include <net.h>
#include <net_processing.h>
#include <protocol.h>
#include <sync.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>

BOOST_FIXTURE_TEST_SUITE(handshake_tests, RegTestingSetup)

namespace {

constexpr ServiceFlags DESIRABLE_SERVICES{NODE_NETWORK | NODE_WITNESS};

CSerializedNetMsg MakeVersionMessage(int32_t version, ServiceFlags services)
{
    return NetMsg::Make(NetMsgType::VERSION,
                        version,
                        Using<CustomUintFormatter<8>>(services),
                        int64_t{},
                        int64_t{},
                        CNetAddr::V1(CService{}),
                        int64_t{},
                        CNetAddr::V1(CService{}),
                        uint64_t{1},
                        std::string{},
                        int32_t{},
                        bool{true});
}

CSerializedNetMsg MakeVerackMessage()
{
    return NetMsg::Make(NetMsgType::VERACK);
}

} // namespace

namespace {

// Helper to set up a PeerManager + ConnmanTestMsg with a single peer.
struct HandshakeFixture {
    TestingSetup& setup;
    ConnmanTestMsg& connman;
    FakeNodeClock clock{1610000000s};
    CNode* peer{nullptr};

    explicit HandshakeFixture(TestingSetup& s, ConnectionType conn_type = ConnectionType::INBOUND)
        : setup{s}, connman{static_cast<ConnmanTestMsg&>(*s.m_node.connman)}
    {
        auto& node = s.m_node;
        connman.Reset();
        node.banman.reset();
        node.addrman.reset();
        node.peerman.reset();
        node.addrman = std::make_unique<AddrMan>(*node.netgroupman, true, 0);
        node.peerman = PeerManager::make(connman, *node.addrman,
                                         nullptr, *node.chainman,
                                         *node.mempool, *node.warnings,
                                         PeerManager::Options{});
        connman.SetMsgProc(node.peerman.get());
        connman.SetAddrman(*node.addrman);

        auto node_holder = std::make_unique<CNode>(0,
                                       std::make_shared<ZeroSock>(),
                                       CAddress{CService{}, NODE_NONE},
                                       0, 0, CService{}, std::string{},
                                       conn_type, false, 0);
        peer = node_holder.release();
        connman.AddTestNode(*peer);
        node.peerman->InitializeNode(*peer, DESIRABLE_SERVICES);
    }

    ~HandshakeFixture()
    {
        setup.m_node.connman->StopNodes();
    }

    void InjectVersion(int32_t version, ServiceFlags services)
    {
        // For outbound connections, we must send our own VERSION first before
        // ProcessMessages will process incoming messages.
        setup.m_node.peerman->SendMessages(*peer);
        connman.FlushSendBuffer(*peer);

        (void)connman.ReceiveMsgFrom(*peer, MakeVersionMessage(version, services));
        peer->fPauseSend = false;
        connman.ProcessMessagesOnce(*peer);
    }

    void InjectVerack()
    {
        (void)connman.ReceiveMsgFrom(*peer, MakeVerackMessage());
        peer->fPauseSend = false;
        connman.ProcessMessagesOnce(*peer);
    }
};

} // namespace

// A peer offering insufficient service flags is recorded with the SERVICE_BITS stage.
// Outbound connections are used because ExpectServicesFromConn() only checks services
// for outbound connection types.
BOOST_AUTO_TEST_CASE(handshake_failure_service_bits)
{
    HandshakeFixture fixture{*this, ConnectionType::OUTBOUND_FULL_RELAY};
    LOCK(NetEventsInterface::g_msgproc_mutex);

    fixture.InjectVersion(PROTOCOL_VERSION, NODE_NONE);

    BOOST_CHECK(fixture.peer->fDisconnect);
    BOOST_CHECK(m_node.peerman->GetHandshakeFailureStage(fixture.peer->GetId()).value() ==
                      HandshakeFailureStage::SERVICE_BITS);
}

// A peer using an obsolete protocol version is recorded with the VERSION_NEGOTIATION stage.
BOOST_AUTO_TEST_CASE(handshake_failure_version_negotiation)
{
    HandshakeFixture fixture{*this};
    LOCK(NetEventsInterface::g_msgproc_mutex);

    fixture.InjectVersion(MIN_PEER_PROTO_VERSION - 1, DESIRABLE_SERVICES);

    BOOST_CHECK(fixture.peer->fDisconnect);
    BOOST_CHECK(m_node.peerman->GetHandshakeFailureStage(fixture.peer->GetId()).value() ==
                      HandshakeFailureStage::VERSION_NEGOTIATION);
}

// A peer that does not complete the handshake in time is recorded with the TIMEOUT stage.
BOOST_AUTO_TEST_CASE(handshake_failure_timeout)
{
    HandshakeFixture fixture{*this};
    LOCK(NetEventsInterface::g_msgproc_mutex);

    // Simulate some traffic so the peer is past the "never sent/received" check.
    fixture.peer->m_last_send = NodeClock::now();
    fixture.peer->m_last_recv = NodeClock::now();

    // Advance time past the peer connect timeout.
    fixture.clock += std::chrono::seconds{DEFAULT_PEER_CONNECT_TIMEOUT} + 1s;

    BOOST_CHECK(!fixture.peer->fSuccessfullyConnected);
    BOOST_CHECK(fixture.connman.InactivityCheckPublic(*fixture.peer, NodeClock::now()));
    BOOST_CHECK(m_node.peerman->GetHandshakeFailureStage(fixture.peer->GetId()).value() ==
                      HandshakeFailureStage::TIMEOUT);
}

// A successful handshake does not produce a failure entry.
BOOST_AUTO_TEST_CASE(handshake_success_no_failure)
{
    HandshakeFixture fixture{*this};
    LOCK(NetEventsInterface::g_msgproc_mutex);

    fixture.InjectVersion(PROTOCOL_VERSION, DESIRABLE_SERVICES);
    fixture.InjectVerack();

    BOOST_CHECK(fixture.peer->fSuccessfullyConnected);
    BOOST_CHECK(!m_node.peerman->GetHandshakeFailureStage(fixture.peer->GetId()).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
