SET async_commit = true;
CREATE TEMPORARY TABLE positions
(
    ol_i_id        INTEGER NOT NULL,
    ol_number      INTEGER NOT NULL,
    ol_supply_w_id INTEGER NOT NULL,
    ol_quantity    INTEGER NOT NULL
) ON COMMIT DELETE ROWS;
