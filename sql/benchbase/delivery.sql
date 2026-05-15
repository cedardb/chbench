-- Benchbase-mode Delivery transaction. Loops over all 10 districts of one
-- warehouse, processing the oldest pending new_order in each. Sample: w_id=1,
-- carrier=5; the per-district block is identical for d_id 1..10. Some
-- districts may have zero pending new_orders (skipped after the first SELECT).

BEGIN;

-- ===== Per district (repeated 10 times: d_id 1..10) =====

-- d_id = 1
SELECT no_o_id FROM new_order
 WHERE no_d_id = 1 AND no_w_id = 1
 ORDER BY no_o_id ASC LIMIT 1;

DELETE FROM new_order
 WHERE no_o_id = 2815 AND no_d_id = 1 AND no_w_id = 1;

SELECT o_c_id FROM oorder
 WHERE o_id = 2815 AND o_d_id = 1 AND o_w_id = 1;

UPDATE oorder SET o_carrier_id = 5
 WHERE o_id = 2815 AND o_d_id = 1 AND o_w_id = 1;

UPDATE order_line SET ol_delivery_d = NOW()
 WHERE ol_o_id = 2815 AND ol_d_id = 1 AND ol_w_id = 1;

SELECT SUM(ol_amount) FROM order_line
 WHERE ol_o_id = 2815 AND ol_d_id = 1 AND ol_w_id = 1;

UPDATE customer SET c_balance      = c_balance + 500.00,
                    c_delivery_cnt = c_delivery_cnt + 1
 WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 2368;

-- d_id = 2..10 follow the same 7-statement pattern with the per-district
-- d_id and the no_o_id fetched from the first SELECT in each iteration.
-- Omitted here for brevity; chbench issues all ten iterations identically.

COMMIT;
