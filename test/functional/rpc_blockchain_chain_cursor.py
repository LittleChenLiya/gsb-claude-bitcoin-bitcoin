#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the chain-tip cursor consistency extension.

Tests the optional 'chain_cursor' parameter of getblock/getblockheader together
with the getchaintipcursor RPC:

- Normal path: a cursor stays valid while the active chain only grows, and the
  updated cursor is returned alongside the queried data.
- Reorg path: after the active chain is reorganized away from the cursor block,
  queries carrying the stale cursor fail with RPC_CHAIN_CURSOR_MISMATCH (-38),
  while the legacy interface (no cursor) silently returns data from the new
  chain -- the mixed-chain result the cursor is meant to prevent.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

CHAIN_CURSOR_MISMATCH = -38


class ChainCursorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.uses_wallet = None

    def run_test(self):
        node = self.nodes[0]

        # ---- Normal path: the cursor stays valid while the chain grows ----
        self.log.info("Mining 10 blocks...")
        self.generate(node, 10)

        cursor = node.getchaintipcursor()
        assert_equal(cursor["height"], 10)
        assert_equal(cursor["hash"], node.getbestblockhash())
        assert_equal(cursor["cursor"], cursor["hash"])
        self.log.info("Initial cursor at height %d: %s", cursor["height"], cursor["cursor"])

        block7_hash_old = node.getblockhash(7)

        # Querying with a cursor returns the (updated) cursor alongside the data.
        block7 = node.getblock(block7_hash_old, 1, cursor["cursor"])
        assert_equal(block7["hash"], block7_hash_old)
        assert_equal(block7["height"], 7)
        assert_equal(block7["chain_cursor"], cursor["cursor"])

        # A block header query with a cursor behaves the same.
        header7 = node.getblockheader(block7_hash_old, True, cursor["cursor"])
        assert_equal(header7["chain_cursor"], cursor["cursor"])

        # Growing the chain keeps the cursor valid (the cursor block remains on
        # the active chain); the returned cursor advances to the new tip.
        self.generate(node, 1)
        tip11 = node.getbestblockhash()
        block7_after_growth = node.getblock(block7_hash_old, 1, cursor["cursor"])
        assert_equal(block7_after_growth["hash"], block7_hash_old)
        assert_equal(block7_after_growth["chain_cursor"], tip11)
        self.log.info("Cursor still valid after chain growth; new cursor: %s", tip11)

        # ---- Reorg path: a stale cursor must fail, the legacy interface must not ----
        self.log.info("Simulating a reorganization below block 7...")
        # Invalidate block 6: the active chain rolls back to height 5.
        node.invalidateblock(node.getblockhash(6))
        assert_equal(node.getblockcount(), 5)
        # Advance the clock so the fork blocks get distinct timestamps; otherwise
        # a fork block could hash identically to the invalidated block (same
        # coinbase, same second, regtest low difficulty) and be rejected.
        node.setmocktime(node.getblockheader(node.getbestblockhash())["time"] + 1000)
        # Build a competing fork of 6 blocks on top of block 5 so that it
        # overtakes the old chain (height 11 > old height 10).
        self.generate(node, 6)
        assert_equal(node.getblockcount(), 11)

        new_cursor = node.getchaintipcursor()
        assert_equal(new_cursor["height"], 11)
        assert new_cursor["cursor"] != cursor["cursor"]
        self.log.info("New active chain tip after reorg: %s", new_cursor["cursor"])

        block7_hash_new = node.getblockhash(7)
        assert block7_hash_new != block7_hash_old
        self.log.info("Block at height 7 before reorg: %s", block7_hash_old)
        self.log.info("Block at height 7 after  reorg: %s", block7_hash_new)

        # Legacy interface (no cursor): silently returns data from the new chain.
        legacy_block7 = node.getblock(block7_hash_new)
        assert_equal(legacy_block7["hash"], block7_hash_new)
        assert "chain_cursor" not in legacy_block7
        self.log.info("Legacy getblock without cursor returned block %s with no reorg indication", legacy_block7["hash"])

        # Cursor interface: the stale cursor is rejected instead of returning
        # mixed-chain data.
        assert_raises_rpc_error(
            CHAIN_CURSOR_MISMATCH,
            "no longer part of the active chain",
            node.getblock, block7_hash_new, 1, cursor["cursor"])
        assert_raises_rpc_error(
            CHAIN_CURSOR_MISMATCH,
            "no longer part of the active chain",
            node.getblockheader, block7_hash_new, True, cursor["cursor"])
        # verbosity 0 verifies the cursor too.
        assert_raises_rpc_error(
            CHAIN_CURSOR_MISMATCH,
            "no longer part of the active chain",
            node.getblock, block7_hash_new, 0, cursor["cursor"])
        self.log.info("Stale cursor correctly rejected with error code %d", CHAIN_CURSOR_MISMATCH)

        # An unknown cursor is rejected as well.
        bogus_cursor = "00" * 32
        assert_raises_rpc_error(
            CHAIN_CURSOR_MISMATCH,
            "no longer part of the active chain",
            node.getblock, block7_hash_new, 1, bogus_cursor)

        # A fresh cursor obtained after the reorg works fine.
        ok_block7 = node.getblock(block7_hash_new, 1, new_cursor["cursor"])
        assert_equal(ok_block7["hash"], block7_hash_new)
        assert_equal(ok_block7["chain_cursor"], new_cursor["cursor"])
        self.log.info("Fresh cursor accepted; block %s read on the active chain", ok_block7["hash"])

        self.log.info("All chain cursor checks passed")


if __name__ == "__main__":
    ChainCursorTest(__file__).main()
