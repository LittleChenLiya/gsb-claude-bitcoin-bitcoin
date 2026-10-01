// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/transaction_utils.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(mempool_eviction_tests, TestingSetup)

namespace {

// Small (but valid) mempool size limit: Flatten() requires the limit to be at
// least the cluster size limit (101000 vbytes * WITNESS_SCALE_FACTOR weight).
constexpr int64_t SMALL_MEMPOOL_SIZE{101000 * 40};
constexpr auto MEMPOOL_EXPIRY{std::chrono::seconds{100}};
constexpr auto BASE_TIME{NodeSeconds{std::chrono::seconds{1000000}}};
constexpr auto NOW{BASE_TIME + std::chrono::seconds{200}};

// Pad a transaction to about 90000 vbytes so that only a few dozen
// transactions are needed to exceed the small mempool size limit. This stays
// below the 101000 vbyte per-cluster limit.
constexpr int32_t LARGE_TX_WEIGHT{360000};

CTransactionRef MakeLargeTx(uint32_t idx)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(idx)}), 0});
    mtx.vout.emplace_back(1 * COIN, GetScriptForDestination(WitnessV0ScriptHash(CScript() << OP_TRUE)));
    BulkTransaction(mtx, LARGE_TX_WEIGHT); // about 90000 vbytes
    return MakeTransactionRef(mtx);
}

// A small, cheap transaction with a unique input.
CTransactionRef MakeSmallTx(uint32_t idx)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(80 + idx)}), 0});
    mtx.vout.emplace_back(1 * COIN, GetScriptForDestination(WitnessV0ScriptHash(CScript() << OP_TRUE)));
    return MakeTransactionRef(mtx);
}

} // namespace

BOOST_AUTO_TEST_CASE(EvictionStatsTest)
{
    bilingual_str error;
    CTxMemPool::Options opts = MemPoolOptionsForTest(m_node);
    opts.max_size_bytes = SMALL_MEMPOOL_SIZE;
    opts.expiry = MEMPOOL_EXPIRY;
    CTxMemPool pool{opts, error};
    BOOST_CHECK(error.empty());

    LOCK2(cs_main, pool.cs);

    TestMemPoolEntryHelper entry;

    // Initially all counters are zero.
    auto stats = pool.GetEvictionStats();
    BOOST_CHECK_EQUAL(stats.size_limit, 0u);
    BOOST_CHECK_EQUAL(stats.expiry, 0u);
    BOOST_CHECK_EQUAL(stats.replaced, 0u);

    // --- 1. Capacity eviction (MemPoolRemovalReason::SIZELIMIT) -----------
    // Add enough large transactions to exceed the small size limit, then trim
    // to size, mirroring what validation does on every new block.
    std::vector<Txid> added;
    for (uint32_t i = 0; i < 80; ++i) {
        auto tx = MakeLargeTx(i);
        TryAddToMempool(pool, entry.Fee(0).Time(NOW).FromTx(tx));
        added.push_back(tx->GetHash());
    }
    BOOST_CHECK_EQUAL(pool.size(), 80u);
    pool.TrimToSize(pool.m_opts.max_size_bytes);

    stats = pool.GetEvictionStats();
    BOOST_CHECK_GT(stats.size_limit, 0u);
    BOOST_CHECK_EQUAL(stats.expiry, 0u);
    BOOST_CHECK_EQUAL(stats.replaced, 0u);
    const uint64_t size_evicted = stats.size_limit;

    // The pool must be back at or below its size limit, and every remaining
    // transaction must be one we added.
    BOOST_CHECK_LE(pool.DynamicMemoryUsage(), static_cast<uint64_t>(SMALL_MEMPOOL_SIZE));
    size_t remaining = 0;
    for (const auto& txid : added) {
        if (pool.exists(txid)) ++remaining;
    }
    BOOST_CHECK_EQUAL(remaining, pool.size());
    BOOST_CHECK_EQUAL(pool.size(), 80u - size_evicted);

    // --- 2. Expiry eviction (MemPoolRemovalReason::EXPIRY) -----------------
    // Add two transactions with old entry times, then advance the mock clock
    // past the expiry horizon and call Expire(), as validation does.
    auto tx_expired_old = MakeSmallTx(0);
    auto tx_expired_new = MakeSmallTx(1);
    TryAddToMempool(pool, entry.Fee(100).Time(BASE_TIME).FromTx(tx_expired_old));
    TryAddToMempool(pool, entry.Fee(100).Time(BASE_TIME + std::chrono::seconds{50}).FromTx(tx_expired_new));

    SetMockTime(NOW);
    const int expired = pool.Expire(GetTime<std::chrono::seconds>() - pool.m_opts.expiry);
    BOOST_CHECK_EQUAL(expired, 2);

    stats = pool.GetEvictionStats();
    BOOST_CHECK_EQUAL(stats.expiry, 2u);
    BOOST_CHECK_EQUAL(stats.size_limit, size_evicted); // capacity counter untouched
    BOOST_CHECK_EQUAL(stats.replaced, 0u);
    BOOST_CHECK(!pool.exists(tx_expired_old->GetHash()));
    BOOST_CHECK(!pool.exists(tx_expired_new->GetHash()));
    // The large transactions (recent entry time) survived expiry.
    BOOST_CHECK_EQUAL(pool.size(), 80u - size_evicted);

    // --- 3. Conflict replacement (MemPoolRemovalReason::REPLACED) ---------
    // Stage an RBF-style replacement through the mempool changeset: the
    // conflicting transaction is removed and the replacement is added.
    auto tx_conflict = MakeSmallTx(2);
    TryAddToMempool(pool, entry.Fee(100).Time(NOW).FromTx(tx_conflict));
    const COutPoint conflict_outpoint{tx_conflict->vin[0].prevout};

    CMutableTransaction mtx_replacement;
    mtx_replacement.vin.emplace_back(conflict_outpoint);
    mtx_replacement.vout.emplace_back(2 * COIN, GetScriptForDestination(WitnessV0ScriptHash(CScript() << OP_TRUE)));
    const auto tx_replacement = MakeTransactionRef(mtx_replacement);

    {
        auto changeset = pool.GetChangeSet();
        auto conflict_iter = pool.GetIter(tx_conflict->GetHash());
        BOOST_REQUIRE(conflict_iter.has_value());
        changeset->StageRemoval(*conflict_iter);
        changeset->StageAddition(tx_replacement, /*fee=*/10000,
                /*time=*/TicksSinceEpoch<std::chrono::seconds>(NOW), /*entry_height=*/1,
                /*entry_sequence=*/0, /*spends_coinbase=*/false,
                /*sigops_cost=*/4, LockPoints{});
        changeset->Apply();
    }

    stats = pool.GetEvictionStats();
    BOOST_CHECK_EQUAL(stats.replaced, 1u);
    BOOST_CHECK_EQUAL(stats.size_limit, size_evicted);
    BOOST_CHECK_EQUAL(stats.expiry, 2u);
    BOOST_CHECK(!pool.exists(tx_conflict->GetHash()));
    BOOST_CHECK(pool.exists(tx_replacement->GetHash()));
    // The replacement is a new transaction that stays in the pool.
    BOOST_CHECK_EQUAL(pool.size(), 80u - size_evicted + 1);

    // --- 4. Non-eviction removals are not counted -------------------------
    // Removals for block inclusion (BLOCK) and in-block conflicts (CONFLICT)
    // must not advance the eviction counters.
    auto tx_block = MakeSmallTx(3);
    auto tx_inblock_conflict = MakeSmallTx(4);
    TryAddToMempool(pool, entry.Fee(100).Time(NOW).FromTx(tx_block));
    TryAddToMempool(pool, entry.Fee(100).Time(NOW).FromTx(tx_inblock_conflict));
    const auto stats_before_noneviction = pool.GetEvictionStats();
    pool.removeRecursive(*tx_block, MemPoolRemovalReason::BLOCK);
    pool.removeRecursive(*tx_inblock_conflict, MemPoolRemovalReason::CONFLICT);
    const auto stats_after_noneviction = pool.GetEvictionStats();
    BOOST_CHECK_EQUAL(stats_after_noneviction.size_limit, stats_before_noneviction.size_limit);
    BOOST_CHECK_EQUAL(stats_after_noneviction.expiry, stats_before_noneviction.expiry);
    BOOST_CHECK_EQUAL(stats_after_noneviction.replaced, stats_before_noneviction.replaced);

    // Reorg recovery (REORG) must not be miscounted as an eviction either.
    // Exercise the same removeForReorg() path validation uses.
    auto tx_reorg = MakeSmallTx(5);
    TryAddToMempool(pool, entry.Fee(100).Time(NOW).FromTx(tx_reorg));
    const auto stats_before_reorg = pool.GetEvictionStats();
    pool.removeForReorg(m_node.chainman->ActiveChain(), [](CTxMemPool::txiter) { return true; });
    const auto stats_after_reorg = pool.GetEvictionStats();
    BOOST_CHECK_EQUAL(stats_after_reorg.size_limit, stats_before_reorg.size_limit);
    BOOST_CHECK_EQUAL(stats_after_reorg.expiry, stats_before_reorg.expiry);
    BOOST_CHECK_EQUAL(stats_after_reorg.replaced, stats_before_reorg.replaced);
    BOOST_CHECK(!pool.exists(tx_reorg->GetHash()));
    BOOST_CHECK_EQUAL(pool.size(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
