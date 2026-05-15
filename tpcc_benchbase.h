// Benchbase-style TPC-C client: executes individual SQL statements with
// client-side logic instead of stored procedure calls.

#pragma once

#include "chbench_base.h"

namespace chbench {

// ---------------------------------------------------------------------------
// TPC-C random utilities
// ---------------------------------------------------------------------------
static constexpr int TPCC_ITEM_COUNT    = 100'000;
static constexpr int TPCC_CUST_PER_DIST = 3'000;
static constexpr int TPCC_DIST_PER_WHSE = 10;
static constexpr int NURAND_ITEM_C      = 7911;
static constexpr int NURAND_CUST_C      = 259;
static constexpr int NURAND_LAST_C      = 223;

static const char* const LAST_NAME_TOKENS[10] = {
    "BAR", "OUGHT", "ABLE", "PRI", "PRES", "ESE", "ANTI", "CALLY", "ATION", "EING"
};

static int bbRand(int lo, int hi, std::mt19937& rng) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

static int bbNurand(int A, int C, int lo, int hi, std::mt19937& rng) {
    return (((bbRand(0, A, rng) | bbRand(lo, hi, rng)) + C) % (hi - lo + 1)) + lo;
}

static int bbItemId(std::mt19937& rng) {
    return bbNurand(8191, NURAND_ITEM_C, 1, TPCC_ITEM_COUNT, rng);
}

static int bbCustomerId(std::mt19937& rng) {
    return bbNurand(1023, NURAND_CUST_C, 1, TPCC_CUST_PER_DIST, rng);
}

static std::string bbLastName(int num) {
    return std::string(LAST_NAME_TOKENS[num / 100])
         + LAST_NAME_TOKENS[(num / 10) % 10]
         + LAST_NAME_TOKENS[num % 10];
}

static std::string bbRandomLastName(std::mt19937& rng) {
    return bbLastName(bbNurand(255, NURAND_LAST_C, 0, 999, rng));
}

// ---------------------------------------------------------------------------
// RAII wrapper for PGresult
// ---------------------------------------------------------------------------
struct PgResult {
    PGresult* r;
    explicit PgResult(PGresult* r) : r(r) {}
    ~PgResult() { if (r) PQclear(r); }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&& o) noexcept : r(o.r) { o.r = nullptr; }

    int ntuples() const { return r ? PQntuples(r) : 0; }
    const char* val(int row, int col) const { return PQgetvalue(r, row, col); }
    int   ival(int row, int col) const { return std::atoi(val(row, col)); }
    double dval(int row, int col) const { return std::atof(val(row, col)); }
};

// ---------------------------------------------------------------------------
// TpccBenchbaseClient
// ---------------------------------------------------------------------------
class TpccBenchbaseClient final : public Client {
public:
    enum Kind : unsigned {
        KPayment = 0, KOrderStatus, KDelivery, KStockLevel, KNewOrder, KCount
    };

private:
    unsigned homeWarehouseId;
    unsigned warehouseCount;
    std::mt19937 rng;
    PGconn* c = nullptr;  // raw connection, set in prepareImpl

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    static std::string p(int v)    { return std::to_string(v); }
    static std::string p(double v) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(6) << v;
        return o.str();
    }

    // Execute a prepared statement; throws on server error.
    PgResult exec(const char* name, std::vector<std::string> args) {
        std::vector<const char*> ptrs;
        ptrs.reserve(args.size());
        for (auto& s : args) ptrs.push_back(s.c_str());
        int n = static_cast<int>(ptrs.size());
        PGresult* r = PQexecPrepared(c, name, n,
                                     n ? ptrs.data() : nullptr,
                                     nullptr, nullptr, 0);
        auto st = PQresultStatus(r);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            std::string msg = PQerrorMessage(c);
            PQclear(r);
            throw std::runtime_error(std::string(name) + ": " + msg);
        }
        return PgResult(r);
    }

    void begin() {
        PGresult* r = PQexec(c, "BEGIN");
        ExecStatusType st = PQresultStatus(r);
        PQclear(r);
        if (st != PGRES_COMMAND_OK)
            throw std::runtime_error("BEGIN failed: " + std::string(PQerrorMessage(c)));
    }

    void commit() {
        PGresult* r = PQexec(c, "COMMIT");
        ExecStatusType st = PQresultStatus(r);
        PQclear(r);
        if (st != PGRES_COMMAND_OK)
            throw std::runtime_error("COMMIT failed: " + std::string(PQerrorMessage(c)));
    }

    void rollback() {
        PGresult* r = PQexec(c, "ROLLBACK");
        PQclear(r);
    }

    // -----------------------------------------------------------------------
    // Customer lookup — shared by Payment, OrderStatus
    // -----------------------------------------------------------------------
    struct Customer {
        int    id;
        std::string credit;
        double balance;
        double ytd_payment;
        int    payment_cnt;
    };

    // Returns the median customer (by c_first) matching c_last.
    Customer getCustomerByName(int w_id, int d_id, const std::string& last) {
        auto res = exec("bb_cust_by_name", {p(w_id), p(d_id), last});
        int n = res.ntuples();
        if (n == 0)
            throw std::runtime_error("getCustomerByName: no rows for " + last);
        // TPC-C 2.5.2.2: position (n/2) rounded up, 1-based → 0-based: (n-1)/2
        int idx = (n - 1) / 2;
        return {res.ival(idx, 0), res.val(idx, 1),
                res.dval(idx, 2), res.dval(idx, 3), res.ival(idx, 4)};
    }

    Customer getCustomerById(int w_id, int d_id, int cid) {
        auto res = exec("bb_cust_by_id", {p(w_id), p(d_id), p(cid)});
        if (res.ntuples() == 0)
            throw std::runtime_error("getCustomerById: no rows");
        return {cid, res.val(0, 0), res.dval(0, 1), res.dval(0, 2), res.ival(0, 3)};
    }

    // -----------------------------------------------------------------------
    // Transactions
    // -----------------------------------------------------------------------
    bool doNewOrder() {
        int w_id  = homeWarehouseId;
        int d_id  = bbRand(1, TPCC_DIST_PER_WHSE, rng);
        int c_id  = bbCustomerId(rng);
        int ol_cnt = bbRand(5, 15, rng);

        std::vector<int> item_ids(ol_cnt), supply_w(ol_cnt), qtys(ol_cnt);
        int all_local = 1;
        for (int i = 0; i < ol_cnt; ++i) {
            item_ids[i] = bbItemId(rng);
            if (bbRand(1, 100, rng) > 1) {
                supply_w[i] = w_id;
            } else {
                do { supply_w[i] = bbRand(1, (int)warehouseCount, rng); }
                while (supply_w[i] == w_id && warehouseCount > 1);
                all_local = 0;
            }
            qtys[i] = bbRand(1, 10, rng);
        }
        // 1% intentional rollback via invalid item id
        bool intentional_rollback = (bbRand(1, 100, rng) == 1);
        if (intentional_rollback) item_ids[ol_cnt - 1] = 0;

        begin();
        try {
            exec("bb_no_get_cust", {p(w_id), p(d_id), p(c_id)});
            exec("bb_no_get_whse", {p(w_id)});

            auto dist   = exec("bb_no_get_dist", {p(w_id), p(d_id)});
            int d_next  = dist.ival(0, 0);

            exec("bb_no_upd_dist",      {p(w_id), p(d_id)});
            exec("bb_no_ins_oorder",    {p(d_next), p(d_id), p(w_id), p(c_id), p(ol_cnt), p(all_local)});
            exec("bb_no_ins_new_order", {p(d_next), p(d_id), p(w_id)});

            for (int i = 0; i < ol_cnt; ++i) {
                int ol_num  = i + 1;
                int ol_iid  = item_ids[i];
                int ol_sw   = supply_w[i];
                int ol_qty  = qtys[i];

                auto item = exec("bb_no_get_item", {p(ol_iid)});
                if (item.ntuples() == 0) { rollback(); return false; }
                double i_price  = item.dval(0, 0);
                double ol_amount = ol_qty * i_price;

                // s_dist_01..s_dist_10 are columns 2..11
                auto stock  = exec("bb_no_get_stock", {p(ol_iid), p(ol_sw)});
                int s_qty   = stock.ival(0, 0);
                std::string dist_info = stock.val(0, d_id + 1);

                // Client-side stock quantity adjustment (TPC-C 2.4.2.2)
                s_qty = (s_qty - ol_qty >= 10) ? s_qty - ol_qty : s_qty - ol_qty + 91;
                int s_remote = (ol_sw == w_id) ? 0 : 1;

                exec("bb_no_upd_stock", {p(s_qty), p(ol_qty), p(s_remote), p(ol_iid), p(ol_sw)});
                exec("bb_no_ins_ol",    {p(d_next), p(d_id), p(w_id), p(ol_num),
                                         p(ol_iid), p(ol_sw), p(ol_qty), p(ol_amount), dist_info});
            }
            commit();
            return true;
        } catch (...) {
            rollback();
            return false;
        }
    }

    bool doPayment() {
        int w_id = homeWarehouseId;
        int d_id = bbRand(1, TPCC_DIST_PER_WHSE, rng);
        double pay_amt = bbRand(100, 500000, rng) / 100.0;

        int cust_w, cust_d;
        if (bbRand(1, 100, rng) <= 85) {
            cust_w = w_id; cust_d = d_id;
        } else {
            do { cust_w = bbRand(1, (int)warehouseCount, rng); }
            while (cust_w == w_id && warehouseCount > 1);
            cust_d = bbRand(1, TPCC_DIST_PER_WHSE, rng);
        }

        begin();
        try {
            exec("bb_pay_upd_whse", {p(pay_amt), p(w_id)});
            auto whse = exec("bb_pay_get_whse", {p(w_id)});
            std::string w_name = whse.val(0, 0);

            exec("bb_pay_upd_dist", {p(pay_amt), p(w_id), p(d_id)});
            auto dist = exec("bb_pay_get_dist", {p(w_id), p(d_id)});
            std::string d_name = dist.val(0, 0);

            Customer cust;
            if (bbRand(1, 100, rng) <= 60) {
                cust = getCustomerByName(cust_w, cust_d, bbRandomLastName(rng));
            } else {
                cust = getCustomerById(cust_w, cust_d, bbCustomerId(rng));
            }

            // Client-side balance arithmetic (benchbase approach)
            cust.balance     -= pay_amt;
            cust.ytd_payment += pay_amt;
            cust.payment_cnt += 1;

            if (cust.credit == "BC") {
                auto cdata_res  = exec("bb_pay_get_cdata", {p(cust_w), p(cust_d), p(cust.id)});
                std::string c_data = cdata_res.val(0, 0);
                std::ostringstream prefix;
                prefix << cust.id << " " << cust_d << " " << cust_w << " "
                       << d_id << " " << w_id << " "
                       << std::fixed << std::setprecision(2) << pay_amt << " | ";
                c_data = prefix.str() + c_data;
                if (c_data.size() > 500) c_data.resize(500);
                exec("bb_pay_upd_cust_bc",
                     {p(cust.balance), p(cust.ytd_payment), p(cust.payment_cnt), c_data,
                      p(cust_w), p(cust_d), p(cust.id)});
            } else {
                exec("bb_pay_upd_cust_gc",
                     {p(cust.balance), p(cust.ytd_payment), p(cust.payment_cnt),
                      p(cust_w), p(cust_d), p(cust.id)});
            }

            if (w_name.size() > 10) w_name.resize(10);
            if (d_name.size() > 10) d_name.resize(10);
            std::string h_data = w_name + "    " + d_name;
            exec("bb_pay_ins_hist",
                 {p(cust_d), p(cust_w), p(cust.id), p(d_id), p(w_id), p(pay_amt), h_data});

            commit();
            return true;
        } catch (...) {
            rollback();
            return false;
        }
    }

    bool doOrderStatus() {
        int w_id = homeWarehouseId;
        int d_id = bbRand(1, TPCC_DIST_PER_WHSE, rng);

        begin();
        try {
            int c_id;
            if (bbRand(1, 100, rng) <= 60) {
                c_id = getCustomerByName(w_id, d_id, bbRandomLastName(rng)).id;
            } else {
                c_id = bbCustomerId(rng);
                exec("bb_cust_by_id", {p(w_id), p(d_id), p(c_id)});
            }

            auto order = exec("bb_os_get_order", {p(w_id), p(d_id), p(c_id)});
            if (order.ntuples() > 0) {
                int o_id = order.ival(0, 0);
                exec("bb_os_get_ol", {p(o_id), p(d_id), p(w_id)});
            }

            commit();
            return true;
        } catch (...) {
            rollback();
            return false;
        }
    }

    bool doDelivery() {
        int w_id       = homeWarehouseId;
        int o_carrier  = bbRand(1, 10, rng);

        begin();
        try {
            for (int d = 1; d <= TPCC_DIST_PER_WHSE; ++d) {
                auto no = exec("bb_del_get_no", {p(d), p(w_id)});
                if (no.ntuples() == 0) continue;
                int no_o_id = no.ival(0, 0);

                exec("bb_del_del_no",    {p(no_o_id), p(d), p(w_id)});
                auto ord = exec("bb_del_get_cust_id", {p(no_o_id), p(d), p(w_id)});
                int c_id = ord.ival(0, 0);

                exec("bb_del_upd_carrier", {p(o_carrier), p(no_o_id), p(d), p(w_id)});
                exec("bb_del_upd_ol",      {p(no_o_id), p(d), p(w_id)});

                auto total = exec("bb_del_sum_ol", {p(no_o_id), p(d), p(w_id)});
                double ol_total = total.dval(0, 0);

                exec("bb_del_upd_cust", {p(ol_total), p(w_id), p(d), p(c_id)});
            }
            commit();
            return true;
        } catch (...) {
            rollback();
            return false;
        }
    }

    bool doStockLevel() {
        int w_id      = homeWarehouseId;
        int d_id      = bbRand(1, TPCC_DIST_PER_WHSE, rng);
        int threshold = bbRand(10, 20, rng);

        begin();
        try {
            auto dist  = exec("bb_sl_get_dist", {p(w_id), p(d_id)});
            int o_id   = dist.ival(0, 0);
            exec("bb_sl_count", {p(w_id), p(d_id), p(o_id), p(o_id - 20), p(w_id), p(threshold)});
            commit();
            return true;
        } catch (...) {
            rollback();
            return false;
        }
    }

    unsigned pickKind() {
        int dice = bbRand(1, 100, rng);
        if (dice <= 43) return KPayment;
        dice -= 43;
        if (dice <= 4) return KOrderStatus;
        dice -= 4;
        if (dice <= 4) return KDelivery;
        dice -= 4;
        if (dice <= 4) return KStockLevel;
        return KNewOrder;
    }

    // -----------------------------------------------------------------------
    // Prepare
    // -----------------------------------------------------------------------
    void prepareImpl() override {
        stats.assign(KCount, TxStats{});
        c = connection.raw();

        // NewOrder
        connection.prepare("bb_no_get_cust",
            "SELECT c_discount,c_last,c_credit FROM customer"
            " WHERE c_w_id=$1 AND c_d_id=$2 AND c_id=$3", 3);
        connection.prepare("bb_no_get_whse",
            "SELECT w_tax FROM warehouse WHERE w_id=$1", 1);
        connection.prepare("bb_no_get_dist",
            "SELECT d_next_o_id,d_tax FROM district WHERE d_w_id=$1 AND d_id=$2", 2);
        connection.prepare("bb_no_upd_dist",
            "UPDATE district SET d_next_o_id=d_next_o_id+1 WHERE d_w_id=$1 AND d_id=$2", 2);
        connection.prepare("bb_no_ins_oorder",
            "INSERT INTO oorder(o_id,o_d_id,o_w_id,o_c_id,o_entry_d,o_ol_cnt,o_all_local)"
            " VALUES($1,$2,$3,$4,NOW(),$5,$6)", 6);
        connection.prepare("bb_no_ins_new_order",
            "INSERT INTO new_order(no_o_id,no_d_id,no_w_id) VALUES($1,$2,$3)", 3);
        connection.prepare("bb_no_get_item",
            "SELECT i_price,i_name,i_data FROM item WHERE i_id=$1", 1);
        connection.prepare("bb_no_get_stock",
            "SELECT s_quantity,s_data"
            ",s_dist_01,s_dist_02,s_dist_03,s_dist_04,s_dist_05"
            ",s_dist_06,s_dist_07,s_dist_08,s_dist_09,s_dist_10"
            " FROM stock WHERE s_i_id=$1 AND s_w_id=$2", 2);
        connection.prepare("bb_no_upd_stock",
            "UPDATE stock SET s_quantity=$1,s_ytd=s_ytd+$2"
            ",s_order_cnt=s_order_cnt+1,s_remote_cnt=s_remote_cnt+$3"
            " WHERE s_i_id=$4 AND s_w_id=$5", 5);
        connection.prepare("bb_no_ins_ol",
            "INSERT INTO order_line"
            "(ol_o_id,ol_d_id,ol_w_id,ol_number,ol_i_id,ol_supply_w_id"
            ",ol_quantity,ol_amount,ol_dist_info)"
            " VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9)", 9);

        // Shared customer lookup (Payment + OrderStatus)
        connection.prepare("bb_cust_by_name",
            "SELECT c_id,c_credit,c_balance,c_ytd_payment,c_payment_cnt"
            " FROM customer WHERE c_w_id=$1 AND c_d_id=$2 AND c_last=$3"
            " ORDER BY c_first", 3);
        connection.prepare("bb_cust_by_id",
            "SELECT c_credit,c_balance,c_ytd_payment,c_payment_cnt"
            " FROM customer WHERE c_w_id=$1 AND c_d_id=$2 AND c_id=$3", 3);

        // Payment
        connection.prepare("bb_pay_upd_whse",
            "UPDATE warehouse SET w_ytd=w_ytd+$1 WHERE w_id=$2", 2);
        connection.prepare("bb_pay_get_whse",
            "SELECT w_name,w_street_1,w_street_2,w_city,w_state,w_zip"
            " FROM warehouse WHERE w_id=$1", 1);
        connection.prepare("bb_pay_upd_dist",
            "UPDATE district SET d_ytd=d_ytd+$1 WHERE d_w_id=$2 AND d_id=$3", 3);
        connection.prepare("bb_pay_get_dist",
            "SELECT d_name,d_street_1,d_street_2,d_city,d_state,d_zip"
            " FROM district WHERE d_w_id=$1 AND d_id=$2", 2);
        connection.prepare("bb_pay_get_cdata",
            "SELECT c_data FROM customer WHERE c_w_id=$1 AND c_d_id=$2 AND c_id=$3", 3);
        connection.prepare("bb_pay_upd_cust_bc",
            "UPDATE customer SET c_balance=$1,c_ytd_payment=$2,c_payment_cnt=$3,c_data=$4"
            " WHERE c_w_id=$5 AND c_d_id=$6 AND c_id=$7", 7);
        connection.prepare("bb_pay_upd_cust_gc",
            "UPDATE customer SET c_balance=$1,c_ytd_payment=$2,c_payment_cnt=$3"
            " WHERE c_w_id=$4 AND c_d_id=$5 AND c_id=$6", 6);
        connection.prepare("bb_pay_ins_hist",
            "INSERT INTO history(h_c_d_id,h_c_w_id,h_c_id,h_d_id,h_w_id,h_date,h_amount,h_data)"
            " VALUES($1,$2,$3,$4,$5,NOW(),$6,$7)", 7);

        // OrderStatus
        connection.prepare("bb_os_get_order",
            "SELECT o_id,o_carrier_id,o_entry_d FROM oorder"
            " WHERE o_w_id=$1 AND o_d_id=$2 AND o_c_id=$3 ORDER BY o_id DESC LIMIT 1", 3);
        connection.prepare("bb_os_get_ol",
            "SELECT ol_i_id,ol_supply_w_id,ol_quantity,ol_amount,ol_delivery_d"
            " FROM order_line WHERE ol_o_id=$1 AND ol_d_id=$2 AND ol_w_id=$3", 3);

        // Delivery
        connection.prepare("bb_del_get_no",
            "SELECT no_o_id FROM new_order WHERE no_d_id=$1 AND no_w_id=$2"
            " ORDER BY no_o_id ASC LIMIT 1", 2);
        connection.prepare("bb_del_del_no",
            "DELETE FROM new_order WHERE no_o_id=$1 AND no_d_id=$2 AND no_w_id=$3", 3);
        connection.prepare("bb_del_get_cust_id",
            "SELECT o_c_id FROM oorder WHERE o_id=$1 AND o_d_id=$2 AND o_w_id=$3", 3);
        connection.prepare("bb_del_upd_carrier",
            "UPDATE oorder SET o_carrier_id=$1 WHERE o_id=$2 AND o_d_id=$3 AND o_w_id=$4", 4);
        connection.prepare("bb_del_upd_ol",
            "UPDATE order_line SET ol_delivery_d=NOW()"
            " WHERE ol_o_id=$1 AND ol_d_id=$2 AND ol_w_id=$3", 3);
        connection.prepare("bb_del_sum_ol",
            "SELECT SUM(ol_amount) FROM order_line"
            " WHERE ol_o_id=$1 AND ol_d_id=$2 AND ol_w_id=$3", 3);
        connection.prepare("bb_del_upd_cust",
            "UPDATE customer SET c_balance=c_balance+$1,c_delivery_cnt=c_delivery_cnt+1"
            " WHERE c_w_id=$2 AND c_d_id=$3 AND c_id=$4", 4);

        // StockLevel
        connection.prepare("bb_sl_get_dist",
            "SELECT d_next_o_id FROM district WHERE d_w_id=$1 AND d_id=$2", 2);
        connection.prepare("bb_sl_count",
            "SELECT COUNT(DISTINCT s_i_id) FROM order_line,stock"
            " WHERE ol_w_id=$1 AND ol_d_id=$2"
            " AND ol_o_id>=$3 AND ol_o_id<$4"
            " AND s_w_id=$5 AND s_i_id=ol_i_id AND s_quantity<$6", 6);
    }

    // -----------------------------------------------------------------------
    // Run
    // -----------------------------------------------------------------------
    void runImpl() override {
        // Per-thread rate cap: same formula as TpccClient but pipeline depth
        // is always 1 since each transaction executes statements sequentially.
        uint64_t intervalNs = 0;
        if (opt.targetRate) {
            intervalNs = static_cast<uint64_t>(
                1e9 * static_cast<double>(opt.scaleFactor) / opt.targetRate);
        }

        auto nextAt = std::chrono::steady_clock::now();

        while (!done.load(std::memory_order_acquire)) {
            unsigned kind = pickKind();

            auto start = std::chrono::steady_clock::now();
            bool ok;
            switch (kind) {
                case KPayment:     ok = doPayment();     break;
                case KOrderStatus: ok = doOrderStatus(); break;
                case KDelivery:    ok = doDelivery();    break;
                case KStockLevel:  ok = doStockLevel();  break;
                default:           ok = doNewOrder();    break;
            }

            if (ok) {
                auto ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start).count());
                maybeRecord(kind, ns);
                processed.fetch_add(1, std::memory_order_acq_rel);
            }

            if (intervalNs) {
                nextAt += std::chrono::nanoseconds(intervalNs);
                auto now = std::chrono::steady_clock::now();
                if (nextAt > now)
                    std::this_thread::sleep_until(nextAt);
                else
                    nextAt = now;
            }
        }
    }

public:
    TpccBenchbaseClient(const Options& o, const Scripts& s, unsigned index)
        : Client(o, s),
          homeWarehouseId(index + 1),
          warehouseCount(o.scaleFactor),
          rng(42u * (index + 1)) {}

    std::vector<std::string> labels() const override {
        return {"payment", "orderStatus", "delivery", "stockLevel", "newOrder"};
    }
    const char* category() const override { return "tpcc"; }
};

} // namespace chbench
