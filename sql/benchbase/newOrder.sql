-- Benchbase-mode NewOrder transaction. Statements appear in the order
-- TpccBenchbaseClient::doNewOrder issues them. Sample parameters: w_id=1,
-- d_id=5, c_id=1, ol_cnt=10, items 1..10. The per-line block (4 statements)
-- is executed once per item; ol_cnt averages 10 (range 5..15).

BEGIN;

-- Validate customer (result discarded apart from c_credit/c_last for the
-- response which benchbase doesn't actually format).
SELECT c_discount, c_last, c_credit
  FROM customer
 WHERE c_w_id = 1 AND c_d_id = 5 AND c_id = 1;

-- Validate warehouse.
SELECT w_tax FROM warehouse WHERE w_id = 1;

-- Read district to discover next o_id.
SELECT d_next_o_id, d_tax
  FROM district
 WHERE d_w_id = 1 AND d_id = 5;

-- Increment district next_o_id (server-side increment).
UPDATE district SET d_next_o_id = d_next_o_id + 1
 WHERE d_w_id = 1 AND d_id = 5;

-- Open the order. o_id is the d_next_o_id we just read (3763 in our example).
INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local)
VALUES (3763, 5, 1, 1, NOW(), 10, 1);

-- Add to new_order queue.
INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (3763, 5, 1);

-- ===== Per order line (repeated 10 times in this example) =====

-- ol_number=1 / item 1
SELECT i_price, i_name, i_data FROM item WHERE i_id = 1;
SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
       s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10
  FROM stock WHERE s_i_id = 1 AND s_w_id = 1;
UPDATE stock SET s_quantity = 50,
                 s_ytd        = s_ytd + 5,
                 s_order_cnt  = s_order_cnt + 1,
                 s_remote_cnt = s_remote_cnt + 0
 WHERE s_i_id = 1 AND s_w_id = 1;
INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
                        ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info)
VALUES (3763, 5, 1, 1, 1, 1, 5, 411.65, 'distinfo');

-- ol_number=2 / item 2
SELECT i_price, i_name, i_data FROM item WHERE i_id = 2;
SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
       s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10
  FROM stock WHERE s_i_id = 2 AND s_w_id = 1;
UPDATE stock SET s_quantity = 50, s_ytd = s_ytd + 5,
                 s_order_cnt = s_order_cnt + 1, s_remote_cnt = s_remote_cnt + 0
 WHERE s_i_id = 2 AND s_w_id = 1;
INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
                        ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info)
VALUES (3763, 5, 1, 2, 2, 1, 5, 346.65, 'distinfo');

-- ol_number=3..10 follow the same 4-statement pattern with ol_number, ol_i_id
-- incremented each iteration. Omitted here for brevity; chbench issues all
-- ten iterations identically.

COMMIT;
