-- Benchbase-mode StockLevel transaction. Read-only. Sample: w_id=1, d_id=5,
-- threshold=15; d_next_o_id=3763 read at runtime.

BEGIN;

-- Read district's next o_id to fix the recent-orders window.
SELECT d_next_o_id FROM district WHERE d_w_id = 1 AND d_id = 5;

-- Count distinct stock items below threshold among the last 20 orders.
SELECT COUNT(DISTINCT s_i_id)
  FROM order_line, stock
 WHERE ol_w_id = 1
   AND ol_d_id = 5
   AND ol_o_id >= 3743
   AND ol_o_id <  3763
   AND s_w_id  = 1
   AND s_i_id  = ol_i_id
   AND s_quantity < 15;

COMMIT;
