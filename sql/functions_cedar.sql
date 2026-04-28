CREATE FUNCTION urand(lower INTEGER, upper INTEGER) RETURNS INTEGER
AS
$$
    return random(upper - lower + 1) + lower;
$$ LANGUAGE 'cedarscript' STRICT
                          VOLATILE;

CREATE FUNCTION urandexcept(lower INTEGER, upper INTEGER, v INTEGER) RETURNS INTEGER
AS
$$
    if upper <= lower {
        return lower;
    } else {
        let r = random(upper - lower) + lower;
        return case when r >= v then r + 1 else r end;
    }
$$ LANGUAGE 'cedarscript' STRICT
                          VOLATILE;

CREATE FUNCTION nurand(a INTEGER, lower INTEGER, upper INTEGER) RETURNS INTEGER
AS
$$
    return (((random(a) | (random(upper - lower + 1) + lower)) + 42) % (upper - lower + 1)) + lower;
$$ LANGUAGE 'cedarscript' STRICT
                          VOLATILE;

CREATE FUNCTION namePart(id INTEGER) RETURNS VARCHAR(20)
AS
$$
    return
    CASE id
        WHEN 0 THEN 'BAR'
        WHEN 1 THEN 'OUGHT'
        WHEN 2 THEN 'ABLE'
        WHEN 3 THEN 'PRI'
        WHEN 4 THEN 'PRES'
        WHEN 5 THEN 'ESE'
        WHEN 6 THEN 'ANTI'
        WHEN 7 THEN 'CALLY'
        WHEN 8 THEN 'ATION'
        ELSE 'EING'
    END;
$$ LANGUAGE 'cedarscript' STRICT
                          IMMUTABLE;

CREATE FUNCTION genName(id INTEGER) RETURNS VARCHAR(20)
AS
$$
   return namePart(mod(id/100,10)) || namePart(mod(id/10,10)) || namePart(mod(id,10));
$$ LANGUAGE 'cedarscript' STRICT
                          IMMUTABLE;

CREATE PROCEDURE delivery(var_w_id INTEGER)
AS
$$
    let var_o_new_carrier_id : INTEGER;
    let var_ol_new_delivery_d : DATE;

    var_o_new_carrier_id = urand(1, 10);
    var_ol_new_delivery_d = now();

    SELECT d_id AS var_d_id FROM generate_series(1, 10) g(d_id) {
        SELECT no_o_id AS var_no_o_id
        FROM new_order
        WHERE new_order.no_w_id = var_w_id
          AND new_order.no_d_id = CAST(var_d_id AS INTEGER)
        ORDER BY no_o_id ASC
        LIMIT 1
        when no_data_found {
            continue;
        }

        DELETE
        FROM new_order
        WHERE new_order.no_o_id = var_no_o_id
          AND new_order.no_d_id = CAST (var_d_id AS INTEGER)
          AND new_order.no_w_id = var_w_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }

        SELECT o_c_id AS var_o_c_id
        FROM oorder
        WHERE oorder.o_id = var_no_o_id
          AND oorder.o_d_id = CAST (var_d_id AS INTEGER)
          AND oorder.o_w_id = var_w_id;

        UPDATE oorder
        SET o_carrier_id = var_o_new_carrier_id
        WHERE oorder.o_id = var_no_o_id
          AND oorder.o_d_id = CAST (var_d_id AS INTEGER)
          AND oorder.o_w_id = var_w_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }

        let mut var_ol_total : NUMERIC(6, 2) = 0;

        SELECT ol_amount AS var_ol_amount
        FROM order_line
        WHERE order_line.ol_o_id = var_no_o_id
          AND order_line.ol_d_id = CAST (var_d_id AS INTEGER)
          AND order_line.ol_w_id = var_w_id {
            var_ol_total = var_ol_total + var_ol_amount;
        }

        UPDATE order_line
        SET ol_delivery_d = var_ol_new_delivery_d
        WHERE order_line.ol_o_id = var_no_o_id
          AND order_line.ol_d_id = CAST (var_d_id AS INTEGER)
          AND order_line.ol_w_id = var_w_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }

        UPDATE customer
        SET c_balance = c_balance + var_ol_total,
            c_delivery_cnt = c_delivery_cnt + 1
        WHERE customer.c_id = var_o_c_id
          AND customer.c_d_id = CAST (var_d_id AS INTEGER)
          AND customer.c_w_id = var_w_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }
    }

    COMMIT;
$$ LANGUAGE 'cedarscript';

CREATE PROCEDURE newOrder(var_w_id INTEGER, var_warehouse_count INTEGER)
AS
$$
    let var_d_id : INTEGER;
    let var_c_id : INTEGER;
    let var_o_ol_cnt : INTEGER;
    let mut var_o_all_local : INTEGER = 1;
    let var_should_rollback : BOOL;

    var_d_id = urand(1, 10);
    var_c_id = nurand(1023, 1, 3000);
    var_o_ol_cnt = urand(5, 15);
    var_should_rollback = (urand(1,100) <= 1);

    SELECT ol_number AS var_ol_number FROM generate_series(1, var_o_ol_cnt) g(ol_number) {
        let mut var_ol_i_id : INTEGER = 0;
        let mut var_ol_supply_w_id : INTEGER = var_w_id;
        let var_ol_quantity : INTEGER = urand(1, 10);

        if (var_ol_number = var_o_ol_cnt) AND var_should_rollback {
            var_ol_i_id = 100001;
        } else {
            var_ol_i_id = nurand(8191, 1, 100000);
        }

        if (var_warehouse_count > 1) AND (urand(1, 100) <= 1) {
            var_ol_supply_w_id = urandexcept(1, var_warehouse_count, var_w_id);
            var_o_all_local = 0;
        }

        INSERT INTO positions (ol_i_id, ol_number, ol_supply_w_id, ol_quantity)
        VALUES (var_ol_i_id, var_ol_number, var_ol_supply_w_id, var_ol_quantity);
    }

    SELECT w_tax AS var_w_tax
    FROM warehouse
    WHERE warehouse.w_id = var_w_id;

    SELECT d_next_o_id AS var_d_next_o_id,
           d_tax AS var_d_tax
    FROM district
    WHERE district.d_id = var_d_id
      AND district.d_w_id = var_w_id;

    SELECT c_discount AS var_c_discount,
           c_last AS var_c_last,
           c_credit AS var_c_credit
    FROM customer
    WHERE customer.c_id = var_c_id
      AND customer.c_d_id = var_d_id
      AND customer.c_w_id = var_w_id;

    let var_new_d_next_o_id = var_d_next_o_id + 1;

    UPDATE district
    SET d_next_o_id = var_new_d_next_o_id
    WHERE district.d_id = var_d_id
      AND district.d_w_id = var_w_id
    catch serialization_failure {
        raise notice 'serialization failure';
        return;
    }

    INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local)
    VALUES (var_d_next_o_id, var_d_id, var_w_id, var_c_id, NOW(), var_o_ol_cnt, var_o_all_local)
    catch serialization_failure {
        raise notice 'serialization failure';
        return;
    }

    INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
    VALUES (var_d_next_o_id, var_d_id, var_w_id)
    catch serialization_failure {
        raise notice 'serialization failure';
        return;
    }

    SELECT ol_i_id AS var_ol_i_id,
           ol_number AS var_ol_number,
           ol_supply_w_id AS var_ol_supply_w_id,
           ol_quantity AS var_ol_quantity
    FROM positions {
        SELECT i_price AS var_i_price
          FROM item
         WHERE item.i_id = var_ol_i_id
        when no_data_found {
            continue;
        }

        SELECT s_quantity AS var_s_quantity,
               CASE var_d_id
                    WHEN 1 THEN s_dist_01
                    WHEN 2 THEN s_dist_02
                    WHEN 3 THEN s_dist_03
                    WHEN 4 THEN s_dist_04
                    WHEN 5 THEN s_dist_05
                    WHEN 6 THEN s_dist_06
                    WHEN 7 THEN s_dist_07
                    WHEN 8 THEN s_dist_08
                    WHEN 9 THEN s_dist_09
                    WHEN 10 THEN s_dist_10
               END AS var_s_dist,
               s_ytd AS var_s_ytd,
               s_order_cnt AS var_s_order_cnt,
               s_remote_cnt AS var_s_remote_cnt
        FROM stock
        WHERE stock.s_w_id = var_ol_supply_w_id
          AND stock.s_i_id = var_ol_i_id;

        let var_s_new_quantity = CASE WHEN var_s_quantity >= var_ol_quantity + 10 then var_s_quantity - var_ol_quantity else var_s_quantity + 91 - var_ol_quantity END;
        let var_s_new_remote_cnt = var_s_remote_cnt + CASE WHEN var_ol_supply_w_id <> var_w_id THEN 1 ELSE 0 END;
        let var_s_new_order_cnt = var_s_order_cnt + 1;
        let var_s_new_ytd = var_s_ytd + var_ol_quantity;

        UPDATE stock
        SET s_quantity = var_s_new_quantity,
            s_remote_cnt = var_s_new_remote_cnt,
            s_order_cnt = var_s_new_order_cnt,
            s_ytd = var_s_new_ytd
        WHERE stock.s_w_id = var_ol_supply_w_id
          AND stock.s_i_id = var_ol_i_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }

        INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info)
        VALUES (var_d_next_o_id, var_d_id, var_w_id, var_ol_number, var_ol_i_id, var_ol_supply_w_id, var_ol_quantity, var_ol_quantity * var_i_price, var_s_dist)
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }
    }

    if var_should_rollback {
        ROLLBACK;
    } else {
        COMMIT;
    }
$$ LANGUAGE 'cedarscript';

CREATE PROCEDURE orderStatus(var_w_id INTEGER)
AS
$$
    let mut var_c_id : INTEGER = 0;
    let var_d_id : INTEGER;

    var_d_id = urand(1,10);

    if urand(1,100) <= 60 {
        -- order status by name: pick the middle customer per TPC-C 2.6.2.2
        let var_c_last = genName(nurand(255, 0, 999));

        let mut var_cust_cnt : INTEGER = 0;
        SELECT count(*) AS var_c
        FROM customer
        WHERE customer.c_last = var_c_last
          AND customer.c_d_id = var_d_id
          AND customer.c_w_id = var_w_id {
            var_cust_cnt = var_c;
        }

        if var_cust_cnt = 0 {
            raise error 'no customer found';
        }

        let var_middle = (var_cust_cnt + 1) / 2;
        let mut var_pos : INTEGER = 0;
        SELECT customer.c_id AS var_it
        FROM customer
        WHERE customer.c_last = var_c_last
          AND customer.c_d_id = var_d_id
          AND customer.c_w_id = var_w_id
        ORDER BY c_first ASC {
            var_pos = var_pos + 1;
            if var_pos = var_middle {
                var_c_id = var_it;
            }
        }
    } else {
        -- order status by id
        var_c_id = nurand(1023, 1, 3000);
    }

    SELECT c_first AS var_c_first,
           c_middle AS var_c_middle,
           c_last AS var_c_last,
           c_balance AS var_c_balance
    FROM customer
    WHERE customer.c_id = var_c_id
      AND customer.c_d_id = var_d_id
      AND customer.c_w_id = var_w_id;

    SELECT o_id AS var_o_id,
           o_entry_d AS var_o_entry_d,
           o_carrier_id AS var_o_carrier_id
    FROM oorder
    WHERE oorder.o_c_id = var_c_id
      AND oorder.o_d_id = var_d_id
      AND oorder.o_w_id = var_w_id
    ORDER BY o_id DESC
    LIMIT 1;

    SELECT ol_i_id AS var_o_i_id,
           ol_supply_w_id AS var_ol_supply_w_id,
           ol_quantity AS var_ol_quantity,
           ol_amount AS var_ol_amount,
           ol_delivery_d AS var_ol_delivery_d
    FROM order_line
    WHERE order_line.ol_o_id = var_o_id
      AND order_line.ol_d_id = var_d_id
      AND order_line.ol_w_id = var_w_id {
      -- do nothing, we just have to retrieve data
    }

    COMMIT;
$$ LANGUAGE 'cedarscript';

CREATE PROCEDURE payment(var_w_id INTEGER, var_warehouse_count INTEGER)
AS
$$
    let var_d_id : INTEGER;
    let mut var_c_id : INTEGER = 0;
    let var_c_w_id : INTEGER;
    let var_c_d_id : INTEGER;
    let var_h_date : DATE;
    let var_h_amount : NUMERIC(6,2);

    var_d_id = urand(1,10);

    if (urand(1,100) > 85) {
        -- remote customer
        var_c_w_id = urandexcept(1, var_warehouse_count, var_w_id);
        var_c_d_id = urand(1, 10);
    } else {
        -- local customer
        var_c_w_id = var_w_id;
        var_c_d_id = var_d_id;
    }

    var_h_date = now();
    var_h_amount = 0.01 * urand(100, 500000);

    if urand(1,100) <= 60 {
        -- payment by name: pick the middle customer per TPC-C 2.5.2.2
        let var_c_last = genName(nurand(255, 0, 999));

        let mut var_cust_cnt : INTEGER = 0;
        SELECT count(*) AS var_c
        FROM customer
        WHERE customer.c_last = var_c_last
          AND customer.c_d_id = var_c_d_id
          AND customer.c_w_id = var_c_w_id {
            var_cust_cnt = var_c;
        }

        if var_cust_cnt = 0 {
            raise error 'no customer found';
        }

        let var_middle = (var_cust_cnt + 1) / 2;
        let mut var_pos : INTEGER = 0;
        SELECT customer.c_id AS var_it
        FROM customer
        WHERE customer.c_last = var_c_last
          AND customer.c_d_id = var_c_d_id
          AND customer.c_w_id = var_c_w_id
        ORDER BY c_first ASC {
            var_pos = var_pos + 1;
            if var_pos = var_middle {
                var_c_id = var_it;
            }
        }
    } else {
        -- payment by id
        var_c_id = nurand(1023, 1, 3000);
    }

    SELECT w_name AS var_w_name,
           w_street_1 AS var_w_street_1,
           w_street_2 AS var_w_street_2,
           w_city AS var_w_city,
           w_state AS var_w_state,
           w_zip AS var_w_zip,
           w_ytd AS var_w_ytd
    FROM warehouse
    WHERE warehouse.w_id = var_w_id;

    let var_w_new_ytd = var_w_ytd + var_h_amount;

    UPDATE warehouse
    SET w_ytd = var_w_new_ytd
    WHERE warehouse.w_id = var_w_id
    catch serialization_failure {
        raise notice 'serialization failure';
        return;
    }

    SELECT d_name AS var_d_name,
           d_street_1 AS var_d_street_1,
           d_street_2 AS var_d_street_2,
           d_city AS var_d_city,
           d_state AS var_d_state,
           d_zip AS var_d_zip,
           d_ytd AS var_d_ytd
    FROM district
    WHERE district.d_id = var_d_id
      AND district.d_w_id = var_w_id;

    let var_d_new_ytd = var_d_ytd + var_h_amount;

    UPDATE district
    SET d_ytd = var_d_new_ytd
    WHERE district.d_id = var_d_id
      AND district.d_w_id = var_w_id
    catch serialization_failure {
        raise notice 'serialization failure';
        return;
    }

    SELECT c_first AS var_c_first,
           c_middle AS var_c_middle,
           c_last AS var_c_last,
           c_street_1 AS var_c_street_1,
           c_street_2 AS var_c_street_2,
           c_city AS var_c_city,
           c_state AS var_c_state,
           c_zip AS var_c_zip,
           c_phone AS var_c_phone,
           c_since AS var_c_since,
           c_credit AS var_c_credit,
           c_credit_lim AS var_c_credit_lim,
           c_discount AS var_c_discount,
           c_balance AS var_c_balance,
           c_ytd_payment AS var_c_ytd_payment,
           c_payment_cnt AS var_c_payment_cnt
    FROM customer
    WHERE customer.c_id = var_c_id
      AND customer.c_d_id = var_c_d_id
      AND customer.c_w_id = var_c_w_id;

    let var_c_new_balance = var_c_balance - var_h_amount;
    let var_c_new_ytd_payment = var_c_ytd_payment + var_h_amount;
    let var_c_new_payment_cnt = var_c_payment_cnt + 1;

    if var_c_credit = 'BC' {
        SELECT c_data AS var_c_data
        FROM customer
        WHERE customer.c_id = var_c_id
          AND customer.c_d_id = var_c_d_id
          AND customer.c_w_id = var_c_w_id;

        -- TPC-C 2.5.2.2: new history info is inserted at the *beginning* of
        -- C_DATA, shifting existing content right; excess bytes are discarded
        -- by keeping the leftmost 500 characters.
        let var_c_new_data = var_c_id::TEXT || ' ' || var_c_d_id::TEXT || ' ' || var_c_w_id::TEXT || ' ' || var_d_id::TEXT || ' ' || var_w_id::TEXT || ' ' || var_h_amount::TEXT || ' ' || var_h_date::TEXT || ' ' || var_w_name::TEXT || ' ' || var_d_name::TEXT || ' ' || var_c_data;

        UPDATE customer
        SET c_balance = var_c_new_balance,
            c_ytd_payment = var_c_new_ytd_payment,
            c_payment_cnt = var_c_new_payment_cnt,
            c_data = substring(var_c_new_data from 1 for 500)
        WHERE customer.c_id = var_c_id
          AND customer.c_d_id = var_c_d_id
          AND customer.c_w_id = var_c_w_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }
    } else {
        UPDATE customer
        SET c_balance = var_c_new_balance,
            c_ytd_payment = var_c_new_ytd_payment,
            c_payment_cnt = var_c_new_payment_cnt
        WHERE customer.c_id = var_c_id
          AND customer.c_d_id = var_c_d_id
          AND customer.c_w_id = var_c_w_id
        catch serialization_failure {
            raise notice 'serialization failure';
            return;
        }
    }

    INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
    VALUES (var_c_id, var_c_d_id, var_c_w_id, var_d_id, var_w_id, var_h_date, var_h_amount, var_w_name || ' ' || var_d_name)
    catch serialization_failure {
        raise notice 'serialization failure';
        return;
    }

    COMMIT;
$$ LANGUAGE 'cedarscript';

CREATE PROCEDURE stockLevel(var_w_id INTEGER)
AS
$$
    let var_d_id : INTEGER;
    let var_threshold : INTEGER;

    var_d_id = urand(1, 10);
    var_threshold = urand(10, 20);

    SELECT d_next_o_id AS var_d_next_o_id
    FROM district
    WHERE district.d_id = var_d_id
      AND district.d_w_id = var_w_id;

    SELECT count(*) AS var_low_stock
    FROM (
        SELECT DISTINCT ol_i_id
        FROM order_line
        WHERE order_line.ol_w_id = var_w_id
          AND order_line.ol_d_id = var_d_id
          AND order_line.ol_o_id >= var_d_next_o_id - 20
          AND order_line.ol_o_id <  var_d_next_o_id
    ) recent_items
    JOIN stock
      ON stock.s_w_id = var_w_id
     AND stock.s_i_id = recent_items.ol_i_id
    WHERE stock.s_quantity < var_threshold;

    COMMIT;
$$ LANGUAGE 'cedarscript';
