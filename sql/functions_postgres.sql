CREATE FUNCTION urand(lower INTEGER, upper INTEGER) RETURNS INTEGER
LANGUAGE plpgsql STRICT VOLATILE AS $$
BEGIN
    RETURN floor(random() * (upper - lower + 1))::int + lower;
END;
$$;

CREATE FUNCTION urandexcept(lower INTEGER, upper INTEGER, v INTEGER) RETURNS INTEGER
LANGUAGE plpgsql STRICT VOLATILE AS $$
DECLARE
    r INTEGER;
BEGIN
    IF upper <= lower THEN
        RETURN lower;
    END IF;
    r := floor(random() * (upper - lower))::int + lower;
    IF r >= v THEN
        RETURN r + 1;
    ELSE
        RETURN r;
    END IF;
END;
$$;

CREATE FUNCTION nurand(a INTEGER, lower INTEGER, upper INTEGER) RETURNS INTEGER
LANGUAGE plpgsql STRICT VOLATILE AS $$
BEGIN
    RETURN (((floor(random() * a)::int |
             (floor(random() * (upper - lower + 1))::int + lower)) + 42)
            % (upper - lower + 1)) + lower;
END;
$$;

CREATE FUNCTION namePart(id INTEGER) RETURNS VARCHAR(20)
LANGUAGE plpgsql STRICT IMMUTABLE AS $$
BEGIN
    RETURN CASE id
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
END;
$$;

CREATE FUNCTION genName(id INTEGER) RETURNS VARCHAR(20)
LANGUAGE plpgsql STRICT IMMUTABLE AS $$
BEGIN
    RETURN namePart(mod(id / 100, 10))
        || namePart(mod(id / 10, 10))
        || namePart(mod(id, 10));
END;
$$;

--===========================================================================
-- delivery
--===========================================================================
CREATE PROCEDURE delivery(var_w_id INTEGER) LANGUAGE plpgsql AS $$
DECLARE
    var_o_new_carrier_id  INTEGER;
    var_ol_new_delivery_d TIMESTAMP;
    var_d_id              INTEGER;
    var_no_o_id           INTEGER;
    var_o_c_id            INTEGER;
    var_ol_total          NUMERIC(12,2);
    var_ol_amount         NUMERIC(6,2);
BEGIN
    var_o_new_carrier_id := urand(1, 10);
    var_ol_new_delivery_d := now();

    FOR var_d_id IN 1..10 LOOP
        BEGIN
            SELECT no_o_id INTO var_no_o_id
            FROM new_order
            WHERE new_order.no_w_id = var_w_id
              AND new_order.no_d_id = var_d_id
            ORDER BY no_o_id ASC
            LIMIT 1;

            IF NOT FOUND THEN
                CONTINUE;
            END IF;

            DELETE FROM new_order
            WHERE new_order.no_o_id = var_no_o_id
              AND new_order.no_d_id = var_d_id
              AND new_order.no_w_id = var_w_id;

            SELECT o_c_id INTO var_o_c_id
            FROM oorder
            WHERE oorder.o_id = var_no_o_id
              AND oorder.o_d_id = var_d_id
              AND oorder.o_w_id = var_w_id;

            UPDATE oorder
            SET o_carrier_id = var_o_new_carrier_id
            WHERE oorder.o_id = var_no_o_id
              AND oorder.o_d_id = var_d_id
              AND oorder.o_w_id = var_w_id;

            var_ol_total := 0;
            FOR var_ol_amount IN
                SELECT ol_amount FROM order_line
                WHERE order_line.ol_o_id = var_no_o_id
                  AND order_line.ol_d_id = var_d_id
                  AND order_line.ol_w_id = var_w_id
            LOOP
                var_ol_total := var_ol_total + var_ol_amount;
            END LOOP;

            UPDATE order_line
            SET ol_delivery_d = var_ol_new_delivery_d
            WHERE order_line.ol_o_id = var_no_o_id
              AND order_line.ol_d_id = var_d_id
              AND order_line.ol_w_id = var_w_id;

            UPDATE customer
            SET c_balance = c_balance + var_ol_total,
                c_delivery_cnt = c_delivery_cnt + 1
            WHERE customer.c_id = var_o_c_id
              AND customer.c_d_id = var_d_id
              AND customer.c_w_id = var_w_id;
        EXCEPTION
            WHEN serialization_failure THEN
                RAISE NOTICE 'serialization failure';
        END;
    END LOOP;

    COMMIT;
END;
$$;

--===========================================================================
-- newOrder
--===========================================================================
CREATE PROCEDURE newOrder(var_w_id INTEGER, var_warehouse_count INTEGER) LANGUAGE plpgsql AS $$
DECLARE
    var_d_id            INTEGER;
    var_c_id            INTEGER;
    var_o_ol_cnt        INTEGER;
    var_o_all_local     INTEGER := 1;
    var_should_rollback BOOLEAN;
    var_ol_number       INTEGER;
    var_ol_i_id         INTEGER;
    var_ol_supply_w_id  INTEGER;
    var_ol_quantity     INTEGER;
    var_w_tax           NUMERIC(4,4);
    var_d_next_o_id     INTEGER;
    var_d_tax           NUMERIC(4,4);
    var_c_discount      NUMERIC(4,4);
    var_c_last          VARCHAR(16);
    var_c_credit        CHAR(2);
    var_new_d_next_o_id INTEGER;
    var_i_price         NUMERIC(5,2);
    var_s_quantity      INTEGER;
    var_s_dist          CHAR(24);
    var_s_ytd           NUMERIC(8,2);
    var_s_order_cnt     INTEGER;
    var_s_remote_cnt    INTEGER;
    var_s_new_quantity  INTEGER;
    var_s_new_remote_cnt INTEGER;
    var_s_new_order_cnt INTEGER;
    var_s_new_ytd       NUMERIC(8,2);
    var_pos_row         RECORD;
BEGIN
    var_d_id := urand(1, 10);
    var_c_id := nurand(1023, 1, 3000);
    var_o_ol_cnt := urand(5, 15);
    var_should_rollback := (urand(1, 100) <= 1);

    BEGIN
        FOR var_ol_number IN 1..var_o_ol_cnt LOOP
            var_ol_i_id := 0;
            var_ol_supply_w_id := var_w_id;
            var_ol_quantity := urand(1, 10);

            IF var_ol_number = var_o_ol_cnt AND var_should_rollback THEN
                var_ol_i_id := 100001;
            ELSE
                var_ol_i_id := nurand(8191, 1, 100000);
            END IF;

            IF (var_warehouse_count > 1) AND (urand(1, 100) <= 1) THEN
                var_ol_supply_w_id := urandexcept(1, var_warehouse_count, var_w_id);
                var_o_all_local := 0;
            END IF;

            INSERT INTO positions (ol_i_id, ol_number, ol_supply_w_id, ol_quantity)
            VALUES (var_ol_i_id, var_ol_number, var_ol_supply_w_id, var_ol_quantity);
        END LOOP;

        SELECT w_tax INTO var_w_tax
        FROM warehouse
        WHERE warehouse.w_id = var_w_id;

        SELECT d_next_o_id, d_tax INTO var_d_next_o_id, var_d_tax
        FROM district
        WHERE district.d_id = var_d_id
          AND district.d_w_id = var_w_id;

        SELECT c_discount, c_last, c_credit
          INTO var_c_discount, var_c_last, var_c_credit
        FROM customer
        WHERE customer.c_id = var_c_id
          AND customer.c_d_id = var_d_id
          AND customer.c_w_id = var_w_id;

        var_new_d_next_o_id := var_d_next_o_id + 1;

        UPDATE district
        SET d_next_o_id = var_new_d_next_o_id
        WHERE district.d_id = var_d_id
          AND district.d_w_id = var_w_id;

        INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local)
        VALUES (var_d_next_o_id, var_d_id, var_w_id, var_c_id, NOW(), var_o_ol_cnt, var_o_all_local);

        INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
        VALUES (var_d_next_o_id, var_d_id, var_w_id);

        FOR var_pos_row IN SELECT ol_i_id, ol_number, ol_supply_w_id, ol_quantity FROM positions LOOP
            var_ol_i_id        := var_pos_row.ol_i_id;
            var_ol_number      := var_pos_row.ol_number;
            var_ol_supply_w_id := var_pos_row.ol_supply_w_id;
            var_ol_quantity    := var_pos_row.ol_quantity;

            SELECT i_price INTO var_i_price
            FROM item
            WHERE item.i_id = var_ol_i_id;

            IF NOT FOUND THEN
                CONTINUE;
            END IF;

            SELECT s_quantity,
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
                   END,
                   s_ytd,
                   s_order_cnt,
                   s_remote_cnt
              INTO var_s_quantity, var_s_dist, var_s_ytd, var_s_order_cnt, var_s_remote_cnt
            FROM stock
            WHERE stock.s_w_id = var_ol_supply_w_id
              AND stock.s_i_id = var_ol_i_id;

            IF var_s_quantity >= var_ol_quantity + 10 THEN
                var_s_new_quantity := var_s_quantity - var_ol_quantity;
            ELSE
                var_s_new_quantity := var_s_quantity + 91 - var_ol_quantity;
            END IF;
            var_s_new_remote_cnt := var_s_remote_cnt
                + CASE WHEN var_ol_supply_w_id <> var_w_id THEN 1 ELSE 0 END;
            var_s_new_order_cnt := var_s_order_cnt + 1;
            var_s_new_ytd := var_s_ytd + var_ol_quantity;

            UPDATE stock
            SET s_quantity = var_s_new_quantity,
                s_remote_cnt = var_s_new_remote_cnt,
                s_order_cnt = var_s_new_order_cnt,
                s_ytd = var_s_new_ytd
            WHERE stock.s_w_id = var_ol_supply_w_id
              AND stock.s_i_id = var_ol_i_id;

            INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info)
            VALUES (var_d_next_o_id, var_d_id, var_w_id, var_ol_number, var_ol_i_id, var_ol_supply_w_id, var_ol_quantity, var_ol_quantity * var_i_price, var_s_dist);
        END LOOP;

        IF var_should_rollback THEN
            RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'tpcc_intentional_rollback';
        END IF;
    EXCEPTION
        WHEN serialization_failure THEN
            RAISE NOTICE 'serialization failure';
        WHEN raise_exception THEN
            NULL;
    END;

    COMMIT;
END;
$$;

--===========================================================================
-- orderStatus
--===========================================================================
CREATE PROCEDURE orderStatus(var_w_id INTEGER) LANGUAGE plpgsql AS $$
DECLARE
    var_c_id          INTEGER := 0;
    var_d_id          INTEGER;
    var_c_last        VARCHAR(16);
    var_cust_cnt      INTEGER := 0;
    var_middle        INTEGER;
    var_pos           INTEGER := 0;
    var_it            INTEGER;
    var_c_first       VARCHAR(16);
    var_c_middle      CHAR(2);
    var_c_balance     NUMERIC(12,2);
    var_o_id          INTEGER;
    var_o_entry_d     TIMESTAMP;
    var_o_carrier_id  INTEGER;
    var_ol            RECORD;
BEGIN
    var_d_id := urand(1, 10);

    BEGIN
        IF urand(1, 100) <= 60 THEN
            -- order status by name: pick the middle customer per TPC-C 2.6.2.2
            var_c_last := genName(nurand(255, 0, 999));

            SELECT count(*) INTO var_cust_cnt
            FROM customer
            WHERE customer.c_last = var_c_last
              AND customer.c_d_id = var_d_id
              AND customer.c_w_id = var_w_id;

            IF var_cust_cnt = 0 THEN
                RAISE EXCEPTION 'no customer found';
            END IF;

            var_middle := (var_cust_cnt + 1) / 2;
            var_pos := 0;
            FOR var_it IN
                SELECT customer.c_id
                FROM customer
                WHERE customer.c_last = var_c_last
                  AND customer.c_d_id = var_d_id
                  AND customer.c_w_id = var_w_id
                ORDER BY c_first ASC
            LOOP
                var_pos := var_pos + 1;
                IF var_pos = var_middle THEN
                    var_c_id := var_it;
                END IF;
            END LOOP;
        ELSE
            var_c_id := nurand(1023, 1, 3000);
        END IF;

        SELECT c_first, c_middle, c_last, c_balance
          INTO var_c_first, var_c_middle, var_c_last, var_c_balance
        FROM customer
        WHERE customer.c_id = var_c_id
          AND customer.c_d_id = var_d_id
          AND customer.c_w_id = var_w_id;

        SELECT o_id, o_entry_d, o_carrier_id
          INTO var_o_id, var_o_entry_d, var_o_carrier_id
        FROM oorder
        WHERE oorder.o_c_id = var_c_id
          AND oorder.o_d_id = var_d_id
          AND oorder.o_w_id = var_w_id
        ORDER BY o_id DESC
        LIMIT 1;

        FOR var_ol IN
            SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d
            FROM order_line
            WHERE order_line.ol_o_id = var_o_id
              AND order_line.ol_d_id = var_d_id
              AND order_line.ol_w_id = var_w_id
        LOOP
            -- do nothing, we just have to retrieve data
            NULL;
        END LOOP;
    EXCEPTION
        WHEN serialization_failure THEN
            RAISE NOTICE 'serialization failure';
    END;

    COMMIT;
END;
$$;

--===========================================================================
-- payment
--===========================================================================
CREATE PROCEDURE payment(var_w_id INTEGER, var_warehouse_count INTEGER) LANGUAGE plpgsql AS $$
DECLARE
    var_d_id          INTEGER;
    var_c_id          INTEGER := 0;
    var_c_w_id        INTEGER;
    var_c_d_id        INTEGER;
    var_h_date        TIMESTAMP;
    var_h_amount      NUMERIC(6,2);
    var_c_last        VARCHAR(16);
    var_cust_cnt      INTEGER := 0;
    var_middle        INTEGER;
    var_pos           INTEGER := 0;
    var_it            INTEGER;
    var_w_name        VARCHAR(10);
    var_w_street_1    VARCHAR(20);
    var_w_street_2    VARCHAR(20);
    var_w_city        VARCHAR(20);
    var_w_state       CHAR(2);
    var_w_zip         CHAR(9);
    var_w_ytd         NUMERIC(12,2);
    var_w_new_ytd     NUMERIC(12,2);
    var_d_name        VARCHAR(10);
    var_d_street_1    VARCHAR(20);
    var_d_street_2    VARCHAR(20);
    var_d_city        VARCHAR(20);
    var_d_state       CHAR(2);
    var_d_zip         CHAR(9);
    var_d_ytd         NUMERIC(12,2);
    var_d_new_ytd     NUMERIC(12,2);
    var_c_first       VARCHAR(16);
    var_c_middle      CHAR(2);
    var_c_street_1    VARCHAR(20);
    var_c_street_2    VARCHAR(20);
    var_c_city        VARCHAR(20);
    var_c_state       CHAR(2);
    var_c_zip         CHAR(9);
    var_c_phone       CHAR(16);
    var_c_since       TIMESTAMP;
    var_c_credit      CHAR(2);
    var_c_credit_lim  NUMERIC(12,2);
    var_c_discount    NUMERIC(4,4);
    var_c_balance     NUMERIC(12,2);
    var_c_ytd_payment FLOAT;
    var_c_payment_cnt INTEGER;
    var_c_new_balance NUMERIC(12,2);
    var_c_new_ytd_payment FLOAT;
    var_c_new_payment_cnt INTEGER;
    var_c_data        VARCHAR(500);
    var_c_new_data    TEXT;
BEGIN
    var_d_id := urand(1, 10);

    IF urand(1, 100) > 85 THEN
        var_c_w_id := urandexcept(1, var_warehouse_count, var_w_id);
        var_c_d_id := urand(1, 10);
    ELSE
        var_c_w_id := var_w_id;
        var_c_d_id := var_d_id;
    END IF;

    var_h_date := now();
    var_h_amount := 0.01 * urand(100, 500000);

    BEGIN
        IF urand(1, 100) <= 60 THEN
            -- payment by name: pick the middle customer per TPC-C 2.5.2.2
            var_c_last := genName(nurand(255, 0, 999));

            SELECT count(*) INTO var_cust_cnt
            FROM customer
            WHERE customer.c_last = var_c_last
              AND customer.c_d_id = var_c_d_id
              AND customer.c_w_id = var_c_w_id;

            IF var_cust_cnt = 0 THEN
                RAISE EXCEPTION 'no customer found';
            END IF;

            var_middle := (var_cust_cnt + 1) / 2;
            var_pos := 0;
            FOR var_it IN
                SELECT customer.c_id
                FROM customer
                WHERE customer.c_last = var_c_last
                  AND customer.c_d_id = var_c_d_id
                  AND customer.c_w_id = var_c_w_id
                ORDER BY c_first ASC
            LOOP
                var_pos := var_pos + 1;
                IF var_pos = var_middle THEN
                    var_c_id := var_it;
                END IF;
            END LOOP;
        ELSE
            var_c_id := nurand(1023, 1, 3000);
        END IF;

        SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip, w_ytd
          INTO var_w_name, var_w_street_1, var_w_street_2, var_w_city, var_w_state, var_w_zip, var_w_ytd
        FROM warehouse
        WHERE warehouse.w_id = var_w_id;

        var_w_new_ytd := var_w_ytd + var_h_amount;
        UPDATE warehouse
        SET w_ytd = var_w_new_ytd
        WHERE warehouse.w_id = var_w_id;

        SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip, d_ytd
          INTO var_d_name, var_d_street_1, var_d_street_2, var_d_city, var_d_state, var_d_zip, var_d_ytd
        FROM district
        WHERE district.d_id = var_d_id
          AND district.d_w_id = var_w_id;

        var_d_new_ytd := var_d_ytd + var_h_amount;
        UPDATE district
        SET d_ytd = var_d_new_ytd
        WHERE district.d_id = var_d_id
          AND district.d_w_id = var_w_id;

        SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip,
               c_phone, c_since, c_credit, c_credit_lim, c_discount, c_balance,
               c_ytd_payment, c_payment_cnt
          INTO var_c_first, var_c_middle, var_c_last, var_c_street_1, var_c_street_2, var_c_city,
               var_c_state, var_c_zip, var_c_phone, var_c_since, var_c_credit, var_c_credit_lim,
               var_c_discount, var_c_balance, var_c_ytd_payment, var_c_payment_cnt
        FROM customer
        WHERE customer.c_id = var_c_id
          AND customer.c_d_id = var_c_d_id
          AND customer.c_w_id = var_c_w_id;

        var_c_new_balance := var_c_balance - var_h_amount;
        var_c_new_ytd_payment := var_c_ytd_payment + var_h_amount;
        var_c_new_payment_cnt := var_c_payment_cnt + 1;

        IF var_c_credit = 'BC' THEN
            SELECT c_data INTO var_c_data
            FROM customer
            WHERE customer.c_id = var_c_id
              AND customer.c_d_id = var_c_d_id
              AND customer.c_w_id = var_c_w_id;

            -- TPC-C 2.5.2.2: new history info inserted at the *beginning* of
            -- C_DATA, shifting existing content right; keep leftmost 500 chars.
            var_c_new_data := var_c_id::TEXT || ' ' || var_c_d_id::TEXT || ' '
                || var_c_w_id::TEXT || ' ' || var_d_id::TEXT || ' '
                || var_w_id::TEXT || ' ' || var_h_amount::TEXT || ' '
                || var_h_date::TEXT || ' ' || var_w_name::TEXT || ' '
                || var_d_name::TEXT || ' ' || var_c_data;

            UPDATE customer
            SET c_balance = var_c_new_balance,
                c_ytd_payment = var_c_new_ytd_payment,
                c_payment_cnt = var_c_new_payment_cnt,
                c_data = substring(var_c_new_data FROM 1 FOR 500)
            WHERE customer.c_id = var_c_id
              AND customer.c_d_id = var_c_d_id
              AND customer.c_w_id = var_c_w_id;
        ELSE
            UPDATE customer
            SET c_balance = var_c_new_balance,
                c_ytd_payment = var_c_new_ytd_payment,
                c_payment_cnt = var_c_new_payment_cnt
            WHERE customer.c_id = var_c_id
              AND customer.c_d_id = var_c_d_id
              AND customer.c_w_id = var_c_w_id;
        END IF;

        INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
        VALUES (var_c_id, var_c_d_id, var_c_w_id, var_d_id, var_w_id, var_h_date,
                var_h_amount, var_w_name || ' ' || var_d_name);
    EXCEPTION
        WHEN serialization_failure THEN
            RAISE NOTICE 'serialization failure';
    END;

    COMMIT;
END;
$$;

--===========================================================================
-- stockLevel
--===========================================================================
CREATE PROCEDURE stockLevel(var_w_id INTEGER) LANGUAGE plpgsql AS $$
DECLARE
    var_d_id        INTEGER;
    var_threshold   INTEGER;
    var_d_next_o_id INTEGER;
    var_low_stock   BIGINT;
BEGIN
    var_d_id := urand(1, 10);
    var_threshold := urand(10, 20);

    BEGIN
        SELECT d_next_o_id INTO var_d_next_o_id
        FROM district
        WHERE district.d_id = var_d_id
          AND district.d_w_id = var_w_id;

        SELECT count(*) INTO var_low_stock
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
    EXCEPTION
        WHEN serialization_failure THEN
            RAISE NOTICE 'serialization failure';
    END;

    COMMIT;
END;
$$;
