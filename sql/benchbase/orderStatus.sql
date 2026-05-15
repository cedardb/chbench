-- Benchbase-mode OrderStatus transaction. Read-only. 60% by name, 40% by id.
-- Sample parameters: w_id=1, d_id=5, c_id=1930.

BEGIN;

-- ----- variant A (60%): customer lookup by last name ----------------------
-- Client picks the median row by c_first.
SELECT c_id, c_credit, c_balance, c_ytd_payment, c_payment_cnt
  FROM customer
 WHERE c_w_id = 1 AND c_d_id = 5 AND c_last = 'PRESOUGHTABLE'
 ORDER BY c_first;

-- ----- variant B (40%): customer lookup by id (validation only) -----------
-- (issued instead of the by-name lookup; not in addition).
-- SELECT c_credit, c_balance, c_ytd_payment, c_payment_cnt
--   FROM customer WHERE c_w_id = 1 AND c_d_id = 5 AND c_id = 1930;

-- Newest order for that customer.
SELECT o_id, o_carrier_id, o_entry_d
  FROM oorder
 WHERE o_w_id = 1 AND o_d_id = 5 AND o_c_id = 1930
 ORDER BY o_id DESC LIMIT 1;

-- Order lines (skipped if the previous query returned 0 rows).
SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d
  FROM order_line
 WHERE ol_o_id = 1930 AND ol_d_id = 5 AND ol_w_id = 1;

COMMIT;
