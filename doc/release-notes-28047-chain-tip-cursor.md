JSON-RPC
--------

The `getblockhash`, `getblockheader` and `getblock` RPCs now accept an
optional `chain_tip` argument (the block hash of the active chain tip).
Callers performing a sequence of block reads can pin the whole sequence to
the tip they observed before the first read. If the active chain tip changes
between reads (for example because of a chain reorganization), subsequent
calls fail with the new error code `-38` (`RPC_CHAIN_CURSOR_MISMATCH`)
instead of silently returning data from a different chain. Omitting the
argument preserves the previous behavior.
