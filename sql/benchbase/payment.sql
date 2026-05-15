-- Benchbase-mode Payment transaction. 60% of invocations look up the customer
-- by last name (variant A); 40% by id (variant B). 10% of looked-up customers
-- have c_credit='BC' which adds an extra c_data update. Sample parameters:
-- w_id=1, d_id=5, payment=1234.56, customer w/d=1/5 (local).

BEGIN;

-- Increment warehouse YTD (in-place; benchbase does not pre-read).
UPDATE warehouse SET w_ytd = w_ytd + 1234.56 WHERE w_id = 1;

-- Read warehouse name + address for the response/h_data.
SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip
  FROM warehouse WHERE w_id = 1;

-- Increment district YTD (in-place).
UPDATE district SET d_ytd = d_ytd + 1234.56
 WHERE d_w_id = 1 AND d_id = 5;

-- Read district name + address.
SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip
  FROM district WHERE d_w_id = 1 AND d_id = 5;

-- ----- variant A (60%): customer lookup by last name ----------------------
-- Returns N rows; client picks the (N-1)/2-indexed row sorted by c_first.
SELECT c_id, c_credit, c_balance, c_ytd_payment, c_payment_cnt
  FROM customer
 WHERE c_w_id = 1 AND c_d_id = 5 AND c_last = 'PRESOUGHTABLE'
 ORDER BY c_first;

-- ----- variant B (40%): customer lookup by id -----------------------------
-- (issued instead of the by-name lookup; not in addition).
-- SELECT c_credit, c_balance, c_ytd_payment, c_payment_cnt
--   FROM customer WHERE c_w_id = 1 AND c_d_id = 5 AND c_id = 1234;

-- ----- BC branch (10% of customers): pre-read c_data for prefixing --------
-- Skipped for GC customers.
SELECT c_data FROM customer WHERE c_w_id = 1 AND c_d_id = 5 AND c_id = 2198;

-- ----- Update customer balance + counters ----------------------------------
-- BC variant (with c_data):
UPDATE customer SET c_balance     = -1234.56,
                    c_ytd_payment = 11234.56,
                    c_payment_cnt = 2,
                    c_data        = '2198 5 1 5 1 1234.56 | <old c_data truncated to 500>'
 WHERE c_w_id = 1 AND c_d_id = 5 AND c_id = 2198;

-- GC variant (without c_data, used in 90% of cases):
-- UPDATE customer SET c_balance = -1234.56, c_ytd_payment = 11234.56,
--                     c_payment_cnt = 2
--  WHERE c_w_id = 1 AND c_d_id = 5 AND c_id = 1;

-- Insert history (h_data = w_name[:10] + "    " + d_name[:10]).
INSERT INTO history (h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id,
                     h_date, h_amount, h_data)
VALUES (5, 1, 2198, 5, 1, NOW(), 1234.56, 'name      name      ');

COMMIT;
