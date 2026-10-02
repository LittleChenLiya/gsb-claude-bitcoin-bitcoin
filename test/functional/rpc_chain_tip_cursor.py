#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the optional chain-tip cursor for sequential block queries.

The block querying RPCs (getblockhash, getblockheader, getblock) accept an
optional chain_tip cursor that pins the query to the active chain tip
observed by the caller. If the chain is reorganized between two reads, the
cursor no longer matches and the call must explicitly fail
(RPC_CHAIN_CURSOR_MISMATCH, code -38) instead of silently returning data
taken from the new, different chain.

Tests correspond to code in src/rpc/blockchain.cpp.
"""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.test_framework import BitcoinTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_raises_rpc_error

# src/rpc/protocol.h: RPC_CHAIN_CURSOR_MISMATCH
CURSOR_MISMATCH_CODE = -38


class ChainTipCursorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False

    def mine_block(self, node, tip_hash, nheight, time_offset=1):
        """Create and submit a regtest-difficulty block on top of tip_hash.

        time_offset is added to the parent block time. Callers building
        competing forks must pass distinct offsets so the blocks (and their
        deterministic anyone-can-spend coinbases) do not collide.
        """
        block_time = node.getblockheader(tip_hash)["time"] + time_offset
        block = create_block(
            hashprev=int(tip_hash, 16),
            coinbase=create_coinbase(nheight),
            ntime=block_time,
        )
        block.solve()
        result = node.submitblock(block.serialize().hex())
        # None: accepted on the active chain. "inconclusive": valid block on a
        # shorter competing branch (headers/block stored, tip unchanged).
        assert result in (None, "inconclusive"), f"submitblock rejected block: {result}"
        return block

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Generate the initial chain and the query fixture")
        self.generate(node, 6, sync_fun=self.no_op)
        h1_a = node.getblockhash(1)
        h2_a = node.getblockhash(2)
        tip_a = node.getbestblockhash()
        assert_equal(node.getblockcount(), 6)

        self.log.info("1. Legacy interface: cursor omitted keeps old behaviour")
        assert_equal(node.getblockhash(1), h1_a)
        assert_equal(node.getblock(h1_a, 1)["hash"], h1_a)
        header = node.getblockheader(h1_a, True)
        assert_equal(header["hash"], h1_a)
        assert_equal(header["confirmations"], 6)

        self.log.info("2. Happy path: matching cursor returns data on the pinned chain")
        assert_equal(node.getblockhash(1, tip_a), h1_a)
        assert_equal(node.getblockhash(2, chain_tip=tip_a), h2_a)
        header = node.getblockheader(h1_a, True, tip_a)
        assert_equal(header["hash"], h1_a)
        assert_equal(header["confirmations"], 6)
        block = node.getblock(h2_a, 1, tip_a)
        assert_equal(block["hash"], h2_a)
        assert_equal(block["confirmations"], 5)
        # verbosity 0 also validates the cursor before returning raw data
        assert isinstance(node.getblock(h2_a, 0, tip_a), str)

        self.log.info("3. Reorg: invalidate block 2 and build two competing branches")
        node.invalidateblock(h2_a)
        assert_equal(node.getblockcount(), 1)
        assert_equal(node.getbestblockhash(), h1_a)
        # Old branch (currently active): two blocks at heights 2-3.
        old_2 = self.mine_block(node, tip_hash=h1_a, nheight=2, time_offset=1)
        old_3 = self.mine_block(node, tip_hash=old_2.hash_hex, nheight=3, time_offset=1)
        old_tip = old_3.hash_hex
        assert_equal(node.getbestblockhash(), old_tip)

        self.log.info("4. First read of the sequential query on the current chain")
        # A client paging through blocks pins the sequence to the tip it sees
        # before the first read.
        cursor = node.getbestblockhash()
        first_hash = node.getblockhash(2, cursor)
        assert_equal(first_hash, old_2.hash_hex)

        self.log.info("5. Active chain switches (reorg) between the two reads")
        # Build a 3-block fork at height 1 so it overtakes the 2-block branch.
        fork_1 = self.mine_block(node, tip_hash=h1_a, nheight=2, time_offset=100)
        fork_2 = self.mine_block(node, tip_hash=fork_1.hash_hex, nheight=3, time_offset=1)
        assert_equal(node.getbestblockhash(), old_tip)  # old branch still longer
        fork_3 = self.mine_block(node, tip_hash=fork_2.hash_hex, nheight=4, time_offset=1)
        new_tip = fork_3.hash_hex
        assert_equal(node.getbestblockhash(), new_tip)  # reorg happened
        # The height-2 slot now contains a block from a different chain.
        h2_new = node.getblockhash(2)
        assert h2_new != old_2.hash_hex

        self.log.info("6. Failure path: stale cursor fails explicitly, no mixed-chain data")
        try:
            node.getblockhash(2, cursor)
            raise AssertionError("getblockhash with a stale cursor must fail")
        except JSONRPCException as e:
            assert_equal(e.error["code"], CURSOR_MISMATCH_CODE)
            err = e.error["message"]
            assert "Chain tip cursor mismatch" in err
            assert cursor in err, f"cursor missing from error message: {err}"
            assert new_tip in err, f"new tip missing from error message: {err}"
        assert_raises_rpc_error(
            CURSOR_MISMATCH_CODE,
            "Chain tip cursor mismatch",
            node.getblockheader,
            old_2.hash_hex,
            True,
            cursor,
        )
        assert_raises_rpc_error(
            CURSOR_MISMATCH_CODE,
            "Chain tip cursor mismatch",
            node.getblock,
            old_2.hash_hex,
            1,
            cursor,
        )
        # Even verbosity 0 (raw hex) must fail instead of returning a payload.
        assert_raises_rpc_error(
            CURSOR_MISMATCH_CODE,
            "Chain tip cursor mismatch",
            node.getblock,
            old_2.hash_hex,
            0,
            cursor,
        )
        # Named-argument style fails the same way.
        assert_raises_rpc_error(
            CURSOR_MISMATCH_CODE,
            "Chain tip cursor mismatch",
            lambda: node.getblockhash(height=2, chain_tip=cursor),
        )

        self.log.info("7. Legacy interface silently serves data from the NEW chain")
        # This is the exact hazard the cursor prevents: the second read of the
        # same paging sequence returns a block from a different chain than the
        # first read.
        assert_equal(node.getblockhash(2), h2_new)
        assert h2_new != first_hash
        assert_equal(node.getblock(h2_new, 1)["confirmations"], 3)

        self.log.info("8. Recovery: refresh the cursor and restart on the new chain")
        new_cursor = node.getbestblockhash()
        assert_equal(node.getblockhash(2, new_cursor), h2_new)
        assert_equal(node.getblockheader(h2_new, True, new_cursor)["confirmations"], 3)
        assert_equal(node.getblock(h2_new, 1, new_cursor)["hash"], h2_new)

        self.log.info("9. Simple chain extension also invalidates a stale cursor")
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), 5)
        # Any tip change is reported honestly; the caller must refresh.
        assert_raises_rpc_error(
            CURSOR_MISMATCH_CODE,
            "Chain tip cursor mismatch",
            node.getblockhash,
            1,
            new_cursor,
        )

        self.log.info("10. Invalid cursor values are rejected with RPC_INVALID_PARAMETER")
        assert_raises_rpc_error(-8, "chain_tip must be of length", node.getblockhash, 1, "abcd")
        assert_raises_rpc_error(
            -8,
            "chain_tip must be hexadecimal string",
            node.getblockhash,
            1,
            "z" * 64,
        )
        # A well-formed hash that is not the current tip is a mismatch, not a
        # missing block: the cursor is compared, never looked up.
        assert_raises_rpc_error(
            CURSOR_MISMATCH_CODE,
            "Chain tip cursor mismatch",
            node.getblockhash,
            1,
            tip_a,
        )


if __name__ == "__main__":
    ChainTipCursorTest(__file__).main()
