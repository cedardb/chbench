// Standalone CH-benchmark driver against a Postgres/CedarDB connection string.
// Assumes the TPC-C + CH schema (see ddl-postgres-*.sql) is already populated.
// Prepares stored procedures from sql/functions.sql and runs TPC-C client
// threads plus optional CH analytical threads driving sql/queries/*.sql.

#include <libpq-fe.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace chbench {

using namespace std::string_view_literals;

//---------------------------------------------------------------------------
// Options
//---------------------------------------------------------------------------
struct Options {
   std::string connString;
   std::string scriptDir;
   unsigned scaleFactor = 0;       // == warehouseCount == clientThreads
   unsigned olapThreads = 0;
   unsigned pipelineDepth = 1;
   unsigned warmupTime = 5;
   unsigned measureTime = 30;
   unsigned targetRate = 0;        // per-thread TPS cap, 0 = uncapped
   std::string csvPath = "chbench-stats.csv";
   std::string targetSystem = "cedar";  // "cedar" or "postgres"
};

static void printHelp(const char* argv0) {
   std::cout
      << "Usage: " << argv0 << " [options]\n"
      << "  --conn <str>          Postgres connection string (required)\n"
      << "  --scale <n>           Scale factor = warehouse count = client thread count (required)\n"
      << "  --olap-threads <n>    Number of analytical clients (default 0)\n"
      << "  --pipeline-depth <n>  Transactions per libpq pipeline batch (default 1)\n"
      << "  --warmup <sec>        Warmup duration (default 5)\n"
      << "  --measure <sec>       Measurement duration (default 30)\n"
      << "  --target-rate <n>     Global TPC-C transaction rate cap, tx/s (default 0 = uncapped)\n"
      << "  --csv <path>          Latency stats output path (default chbench-stats.csv)\n"
      << "  --target-system <s>   cedar | postgres (default cedar) — picks functions_<s>.sql\n"
      << "  --script-dir <path>   SQL script dir (default: <binary-dir>/sql)\n"
      << "  -h, --help            Show help\n";
}

static bool parseArgs(int argc, char** argv, Options& opt) {
   auto need = [&](int& i) -> const char* {
      if (i + 1 >= argc) { std::cerr << "missing value for " << argv[i] << "\n"; return nullptr; }
      return argv[++i];
   };
   for (int i = 1; i < argc; ++i) {
      std::string_view a(argv[i]);
      if (a == "-h" || a == "--help") { printHelp(argv[0]); std::exit(0); }
      else if (a == "--conn") { auto v = need(i); if (!v) return false; opt.connString = v; }
      else if (a == "--scale") { auto v = need(i); if (!v) return false; opt.scaleFactor = std::stoul(v); }
      else if (a == "--olap-threads") { auto v = need(i); if (!v) return false; opt.olapThreads = std::stoul(v); }
      else if (a == "--pipeline-depth") { auto v = need(i); if (!v) return false; opt.pipelineDepth = std::stoul(v); }
      else if (a == "--warmup") { auto v = need(i); if (!v) return false; opt.warmupTime = std::stoul(v); }
      else if (a == "--measure") { auto v = need(i); if (!v) return false; opt.measureTime = std::stoul(v); }
      else if (a == "--target-rate") { auto v = need(i); if (!v) return false; opt.targetRate = std::stoul(v); }
      else if (a == "--csv") { auto v = need(i); if (!v) return false; opt.csvPath = v; }
      else if (a == "--target-system") { auto v = need(i); if (!v) return false; opt.targetSystem = v; }
      else if (a == "--script-dir") { auto v = need(i); if (!v) return false; opt.scriptDir = v; }
      else { std::cerr << "unknown option: " << a << "\n"; return false; }
   }
   if (opt.connString.empty()) { std::cerr << "--conn is required\n"; return false; }
   if (!opt.scaleFactor) { std::cerr << "--scale is required and must be > 0\n"; return false; }
   if (opt.targetSystem != "cedar" && opt.targetSystem != "postgres") {
      std::cerr << "--target-system must be 'cedar' or 'postgres'\n";
      return false;
   }
   return true;
}

//---------------------------------------------------------------------------
// SQL script cache: loaded once on the main thread so client threads never
// race on file I/O during prepare.
//---------------------------------------------------------------------------
struct Scripts {
   std::string functions;
   std::string session;
   std::vector<std::string> queries;  // indexed 0..21, corresponds to q1..q22

   static std::string slurp(const std::filesystem::path& p) {
      std::ifstream ifs(p, std::ios::binary);
      if (!ifs) throw std::runtime_error("cannot open " + p.string());
      std::stringstream buf;
      buf << ifs.rdbuf();
      return buf.str();
   }

   void load(const std::string& dir, const std::string& targetSystem) {
      std::filesystem::path d(dir);
      functions = slurp(d / ("functions_" + targetSystem + ".sql"));
      session   = slurp(d / ("session_" + targetSystem + ".sql"));
      queries.reserve(22);
      for (unsigned i = 1; i <= 22; ++i)
         queries.push_back(slurp(d / "queries" / (std::to_string(i) + ".sql")));
   }
};

//---------------------------------------------------------------------------
// libpq helpers
//---------------------------------------------------------------------------
[[noreturn]] static void die(const std::string& what) {
   std::cerr << what << "\n";
   std::exit(1);
}

static std::string defaultScriptDir(const char* argv0) {
   try {
      auto p = std::filesystem::canonical(argv0).parent_path() / "sql";
      if (std::filesystem::exists(p)) return p.string();
   } catch (...) {}
   return "sql";
}

class Connection {
   PGconn* conn = nullptr;

   public:
   Connection() = default;
   Connection(const Connection&) = delete;
   Connection& operator=(const Connection&) = delete;
   ~Connection() { if (conn) PQfinish(conn); }

   void open(const std::string& connStr) {
      conn = PQconnectdb(connStr.c_str());
      if (PQstatus(conn) != CONNECTION_OK) {
         std::string msg = PQerrorMessage(conn);
         PQfinish(conn); conn = nullptr;
         throw std::runtime_error("connection failed: " + msg);
      }
      // Suppress server NOTICE messages (e.g. "serialization failure") — they
      // are part of normal TPC-C behavior and would spam the console.
      PQsetNoticeProcessor(conn, [](void*, const char*) {}, nullptr);
   }

   PGconn* raw() const { return conn; }

   // Execute a single command, expect success (any non-FATAL/non-ERROR result).
   void exec(const std::string& sql) {
      PGresult* res = PQexec(conn, sql.c_str());
      auto status = PQresultStatus(res);
      if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
         std::string msg = PQerrorMessage(conn);
         PQclear(res);
         throw std::runtime_error("exec failed: " + sql.substr(0, 100) + "\n  -> " + msg);
      }
      PQclear(res);
   }

   // Read entire SQL file and pass to PQexec; server processes multiple
   // statements in a single simple-query message.
   void execFile(const std::filesystem::path& path) {
      std::ifstream ifs(path, std::ios::binary);
      if (!ifs) throw std::runtime_error("cannot open " + path.string());
      std::stringstream buf;
      buf << ifs.rdbuf();
      exec(buf.str());
   }

   // Fetch a single scalar (text) from a simple query.
   std::string scalar(const std::string& sql) {
      PGresult* res = PQexec(conn, sql.c_str());
      if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
         std::string msg = PQerrorMessage(conn);
         PQclear(res);
         throw std::runtime_error("scalar failed: " + sql + "\n  -> " + msg);
      }
      std::string out = PQgetvalue(res, 0, 0);
      PQclear(res);
      return out;
   }

   // Ask the backend to abort any in-flight query on this connection, then
   // forcibly half-shut the socket so that any worker thread blocked in
   // PQgetResult wakes up immediately even if the server doesn't process the
   // cancel signal promptly (e.g. mid-OLAP-query). PQcancel uses a separate
   // connection, PQsocket+shutdown is thread-safe with respect to the worker
   // thread's libpq calls. Once shut, the connection is unusable — only call
   // from a stop path.
   void cancel() {
      if (!conn) return;
      PGcancel* cn = PQgetCancel(conn);
      if (cn) {
         char err[256];
         PQcancel(cn, err, sizeof(err));
         PQfreeCancel(cn);
      }
      int fd = PQsocket(conn);
      if (fd >= 0)
         ::shutdown(fd, SHUT_RDWR);
   }

   // Prepare a named statement.
   void prepare(const std::string& name, const std::string& sql, int nParams) {
      PGresult* res = PQprepare(conn, name.c_str(), sql.c_str(), nParams, nullptr);
      if (PQresultStatus(res) != PGRES_COMMAND_OK) {
         std::string msg = PQerrorMessage(conn);
         PQclear(res);
         throw std::runtime_error("prepare failed for " + name + ": " + msg);
      }
      PQclear(res);
   }
};

//---------------------------------------------------------------------------
// Per-transaction-type latency stats (nanoseconds).
//---------------------------------------------------------------------------
// Log-linear histogram for percentile estimation: 16 sub-buckets per power-of-2
// octave covers latencies from ~16ns up to ~2^44 ns (~4h) with ~6% resolution.
// Chosen over a flat sample buffer so memory stays bounded regardless of tps.
struct TxStats {
   static constexpr unsigned SUB_BITS = 4;
   static constexpr unsigned SUB = 1u << SUB_BITS;  // 16
   static constexpr unsigned OCTAVES = 40;
   static constexpr unsigned BUCKETS = OCTAVES * SUB;

   uint64_t count = 0;
   uint64_t sumNs = 0;
   uint64_t minNs = std::numeric_limits<uint64_t>::max();
   uint64_t maxNs = 0;
   std::vector<uint64_t> hist;  // size BUCKETS, lazily allocated

   static unsigned bucketOf(uint64_t ns) {
      uint64_t v = ns < SUB ? SUB : ns;  // clamp to smallest octave
      unsigned leading = static_cast<unsigned>(__builtin_clzll(v));
      unsigned octave = 63u - leading;   // >= SUB_BITS
      unsigned sub = static_cast<unsigned>((v >> (octave - SUB_BITS)) & (SUB - 1));
      unsigned idx = (octave - SUB_BITS) * SUB + sub;
      if (idx >= BUCKETS) idx = BUCKETS - 1;
      return idx;
   }

   // Representative (midpoint) ns value for a bucket — for reporting.
   static uint64_t bucketMidpoint(unsigned idx) {
      unsigned octaveOffset = idx / SUB;
      unsigned sub = idx % SUB;
      unsigned realOct = octaveOffset + SUB_BITS;
      uint64_t base = 1ull << realOct;
      uint64_t step = base >> SUB_BITS;
      return base + sub * step + step / 2;
   }

   void record(uint64_t ns) {
      count++;
      sumNs += ns;
      if (ns < minNs) minNs = ns;
      if (ns > maxNs) maxNs = ns;
      if (hist.empty()) hist.assign(BUCKETS, 0);
      hist[bucketOf(ns)]++;
   }

   void merge(const TxStats& o) {
      count += o.count;
      sumNs += o.sumNs;
      if (o.count && o.minNs < minNs) minNs = o.minNs;
      if (o.maxNs > maxNs) maxNs = o.maxNs;
      if (!o.hist.empty()) {
         if (hist.empty()) hist.assign(BUCKETS, 0);
         for (unsigned i = 0; i < BUCKETS; ++i) hist[i] += o.hist[i];
      }
   }

   // Approximate percentile in ns (p in [0,1]). Returns 0 if no samples.
   uint64_t percentile(double p) const {
      if (!count || hist.empty()) return 0;
      uint64_t target = static_cast<uint64_t>(p * count);
      if (target >= count) target = count - 1;
      uint64_t cum = 0;
      for (unsigned i = 0; i < BUCKETS; ++i) {
         cum += hist[i];
         if (cum > target) return bucketMidpoint(i);
      }
      return maxNs;
   }
};

//---------------------------------------------------------------------------
// Bound prepared statement (name + pre-computed text parameter values).
//---------------------------------------------------------------------------
struct BoundStmt {
   std::string name;
   std::vector<std::string> paramStorage;
   std::vector<const char*> paramPtrs;

   void setParams(std::vector<std::string> params) {
      paramStorage = std::move(params);
      paramPtrs.clear();
      paramPtrs.reserve(paramStorage.size());
      for (auto& s : paramStorage) paramPtrs.push_back(s.c_str());
   }
};

//---------------------------------------------------------------------------
// Client base (thread, prepare/run/stop lifecycle)
//---------------------------------------------------------------------------
class Client {
   protected:
   const Options& opt;
   const Scripts& scripts;
   Connection connection;
   std::atomic<uint64_t> processed{0};
   std::atomic<bool> recordStats{false};
   // Stats per transaction kind (populated by prepareImpl via setStatsSize).
   std::vector<TxStats> stats;

   private:
   std::thread thread;
   std::atomic<bool> preparedFlag{false};
   std::atomic<bool> started{false};
   std::atomic<bool> failed{false};

   protected:
   std::atomic<bool> done{false};

   virtual void prepareImpl() = 0;
   virtual void runImpl() = 0;

   // Record a latency observation if stats recording is currently enabled.
   void maybeRecord(unsigned kind, uint64_t ns) {
      if (recordStats.load(std::memory_order_acquire))
         stats[kind].record(ns);
   }

   public:
   Client(const Options& o, const Scripts& s) : opt(o), scripts(s) {}
   virtual ~Client() = default;

   bool isFailed() const { return failed.load(std::memory_order_acquire); }
   uint64_t countProcessed() const { return processed.load(std::memory_order_acquire); }
   void setRecording(bool on) { recordStats.store(on, std::memory_order_release); }
   const std::vector<TxStats>& getStats() const { return stats; }
   /// Human-readable label per stats index.
   virtual std::vector<std::string> labels() const = 0;
   /// "tpcc" or "olap" — CSV category column.
   virtual const char* category() const = 0;

   void prepare() {
      thread = std::thread([this]() {
         try {
            connection.open(opt.connString);
            prepareImpl();
         } catch (const std::exception& e) {
            std::cerr << "[prepare] " << e.what() << "\n";
            preparedFlag.store(true, std::memory_order_release);
            failed.store(true, std::memory_order_release);
            return;
         }
         preparedFlag.store(true, std::memory_order_release);

         while (!started.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

         if (done.load(std::memory_order_acquire)) return;

         try {
            runImpl();
         } catch (const std::exception& e) {
            std::cerr << "[run] " << e.what() << "\n";
            failed.store(true, std::memory_order_release);
         }
      });
   }

   void waitUntilPrepared() {
      while (!preparedFlag.load(std::memory_order_acquire))
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }

   void run() { started.store(true, std::memory_order_release); }

   // Signal the client to stop and cancel any in-flight query so the worker
   // thread wakes from PQgetResult. Does not join — call join() separately.
   void requestStop() {
      done.store(true, std::memory_order_release);
      started.store(true, std::memory_order_release);
      connection.cancel();
   }

   void join() {
      if (thread.joinable()) thread.join();
   }

   void stop() {
      requestStop();
      join();
   }
};

//---------------------------------------------------------------------------
// Shared pipeline loop: send pipelineDepth prepared calls, receive, repeat.
//---------------------------------------------------------------------------
/// pick() returns {statement, kindIndex}. record(kindIndex, ns) is invoked
/// once per completed statement with its latency measured from batch send
/// to the NULL terminator of that statement's result stream.
template <typename PickFn, typename RecordFn>
static void runPipeline(Connection& connection, std::atomic<uint64_t>& processed,
                        std::atomic<bool>& done, unsigned pipelineDepth,
                        uint64_t targetIntervalNs,
                        PickFn pick, RecordFn record,
                        bool tolerateQueryErrors = false) {
   PGconn* c = connection.raw();
   if (!PQenterPipelineMode(c))
      throw std::runtime_error(std::string("enterPipelineMode: ") + PQerrorMessage(c));

   // Track kinds + start time for each in-flight batch separately. The driver
   // pipelines one batch ahead of consume(), so when we drain batch A we must
   // reference batch A's start time, not the just-sent batch B's.
   std::vector<unsigned> kindsA(pipelineDepth);
   std::vector<unsigned> kindsB(pipelineDepth);
   std::chrono::steady_clock::time_point startA, startB;
   std::vector<unsigned>* curKinds = &kindsA;
   std::chrono::steady_clock::time_point* curStart = &startA;
   std::vector<unsigned>* drainKinds = &kindsA;
   std::chrono::steady_clock::time_point* drainStart = &startA;

   auto sendBatch = [&]() {
      for (unsigned i = 0; i < pipelineDepth; ++i) {
         auto [sp, kind] = pick();
         (*curKinds)[i] = kind;
         const BoundStmt& s = *sp;
         int n = static_cast<int>(s.paramPtrs.size());
         if (PQsendQueryPrepared(c, s.name.c_str(), n,
                                 n ? s.paramPtrs.data() : nullptr,
                                 nullptr, nullptr, 0) != 1)
            throw std::runtime_error(std::string("sendQueryPrepared: ") + PQerrorMessage(c));
      }
      if (PQpipelineSync(c) != 1)
         throw std::runtime_error(std::string("pipelineSync: ") + PQerrorMessage(c));
      PQflush(c);
      *curStart = std::chrono::steady_clock::now();
   };

   // Drain results for the *previously dispatched* batch (kept in
   // drainKinds/drainStart). Per statement: {result(s), NULL terminator}, then
   // a trailing PGRES_PIPELINE_SYNC marks the synced group's end.
   auto consume = [&]() {
      // libpq tends to buffer all results of a pipelined batch before
      // returning the first one, so per-NULL-terminator deltas don't give us
      // real per-query timing. Instead, measure the batch's total wall-clock
      // turnaround and record (total / pipelineDepth) for each query kind in
      // the drained batch. With pipelineDepth=1 this is identical to the
      // straight per-query latency.
      bool batchFailed = false;
      while (true) {
         PGresult* r = PQgetResult(c);
         if (!r) {
            // Either inter-statement boundary, or the connection went away
            // (post-shutdown on stop). The latter would otherwise loop forever
            // since PIPELINE_SYNC will never arrive.
            if (PQstatus(c) != CONNECTION_OK) break;
            continue;
         }
         auto st = PQresultStatus(r);
         PQclear(r);
         if (st == PGRES_PIPELINE_SYNC) break;
         if (st == PGRES_PIPELINE_ABORTED) continue;
         if (st == PGRES_FATAL_ERROR || st == PGRES_BAD_RESPONSE) {
            if (done.load(std::memory_order_acquire)) continue;
            if (tolerateQueryErrors) {
               // Log and keep draining until PIPELINE_SYNC; the connection
               // remains usable for the next batch.
               std::cerr << "[run] query failed: " << PQerrorMessage(c);
               batchFailed = true;
               continue;
            }
            throw std::runtime_error(std::string("query failed: ") + PQerrorMessage(c));
         }
      }
      if (batchFailed) return;  // skip latency record + processed bump for failed batch
      auto now = std::chrono::steady_clock::now();
      auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *drainStart).count();
      uint64_t perTxNs = static_cast<uint64_t>(totalNs) / pipelineDepth;
      for (unsigned i = 0; i < pipelineDepth; ++i)
         record((*drainKinds)[i], perTxNs);
      processed.fetch_add(pipelineDepth, std::memory_order_acq_rel);
   };

   // Pace each batch so the per-thread mean interval matches targetIntervalNs.
   // Absolute scheduling avoids cumulative drift from transient slow batches.
   auto nextBatchAt = std::chrono::steady_clock::now();

   // Prime: send batch A.
   curKinds = &kindsA; curStart = &startA;
   sendBatch();

   while (!done.load(std::memory_order_acquire)) {
      // Send the next batch into slot B; consume() then drains slot A.
      curKinds   = (curKinds == &kindsA) ? &kindsB : &kindsA;
      curStart   = (curStart == &startA) ? &startB : &startA;
      drainKinds = (curKinds == &kindsA) ? &kindsB : &kindsA;
      drainStart = (curStart == &startA) ? &startB : &startA;
      sendBatch();
      consume();
      if (targetIntervalNs) {
         nextBatchAt += std::chrono::nanoseconds(targetIntervalNs);
         auto now = std::chrono::steady_clock::now();
         if (nextBatchAt > now)
            std::this_thread::sleep_until(nextBatchAt);
         else
            nextBatchAt = now;  // behind schedule: don't build up credit
      }
   }

   // Drain the last in-flight batch (the one curKinds/curStart point at).
   drainKinds = curKinds;
   drainStart = curStart;
   consume();
   PQexitPipelineMode(c);
}

//---------------------------------------------------------------------------
// TPC-C client
//---------------------------------------------------------------------------
class TpccClient final : public Client {
   public:
   enum Kind : unsigned { KPayment = 0, KOrderStatus, KDelivery, KStockLevel, KNewOrder, KCount };

   private:
   unsigned homeWarehouseId;
   unsigned warehouseCount = 0;
   std::mt19937 rng;

   BoundStmt delivery, newOrder, orderStatus, payment, stockLevel;

   std::pair<const BoundStmt*, unsigned> pickTransaction() {
      std::uniform_int_distribution<int> dist(1, 100);
      int dice = dist(rng);
      if (dice <= 43) return {&payment, KPayment};
      dice -= 43;
      if (dice <= 4) return {&orderStatus, KOrderStatus};
      dice -= 4;
      if (dice <= 4) return {&delivery, KDelivery};
      dice -= 4;
      if (dice <= 4) return {&stockLevel, KStockLevel};
      return {&newOrder, KNewOrder};
   }

   protected:
   void prepareImpl() override {
      stats.assign(KCount, TxStats{});
      connection.exec(scripts.session);
      warehouseCount = opt.scaleFactor;  // client count == warehouse count

      connection.prepare("delivery",    "CALL delivery($1::INTEGER)", 1);
      connection.prepare("newOrder",    "CALL newOrder($1::INTEGER, $2::INTEGER)", 2);
      connection.prepare("orderStatus", "CALL orderStatus($1::INTEGER)", 1);
      connection.prepare("payment",     "CALL payment($1::INTEGER, $2::INTEGER)", 2);
      connection.prepare("stockLevel",  "CALL stockLevel($1::INTEGER)", 1);

      auto w = std::to_string(homeWarehouseId);
      auto wc = std::to_string(warehouseCount);
      delivery.name    = "delivery";    delivery.setParams({w});
      newOrder.name    = "newOrder";    newOrder.setParams({w, wc});
      orderStatus.name = "orderStatus"; orderStatus.setParams({w});
      payment.name     = "payment";     payment.setParams({w, wc});
      stockLevel.name  = "stockLevel";  stockLevel.setParams({w});
   }

   void runImpl() override {
      // Global TPC-C rate cap split evenly across client threads. Convert to
      // per-thread inter-batch interval in nanoseconds: a thread sending
      // pipelineDepth tx per batch needs one batch every
      // (pipelineDepth * scaleFactor / targetRate) seconds.
      uint64_t intervalNs = 0;
      if (opt.targetRate) {
         intervalNs = static_cast<uint64_t>(
            1e9 * opt.pipelineDepth * opt.scaleFactor / opt.targetRate);
      }
      runPipeline(connection, processed, done, opt.pipelineDepth, intervalNs,
                  [&]() { return pickTransaction(); },
                  [&](unsigned kind, uint64_t ns) { maybeRecord(kind, ns); });
   }

   public:
   TpccClient(const Options& o, const Scripts& s, unsigned index)
      : Client(o, s), homeWarehouseId(index + 1), rng(42u * (index + 1)) {}

   std::vector<std::string> labels() const override {
      return {"payment", "orderStatus", "delivery", "stockLevel", "newOrder"};
   }
   const char* category() const override { return "tpcc"; }
};

//---------------------------------------------------------------------------
// CH analytical client
//---------------------------------------------------------------------------
class ChClient final : public Client {
   static constexpr unsigned queries = 22;
   std::mt19937 rng;
   std::vector<BoundStmt> statements;

   std::pair<const BoundStmt*, unsigned> pickTransaction() {
      std::uniform_int_distribution<unsigned> dist(0, queries - 1);
      unsigned k = dist(rng);
      return {&statements[k], k};
   }

   protected:
   void prepareImpl() override {
      stats.assign(queries, TxStats{});
      statements.reserve(queries);
      for (unsigned i = 1; i <= queries; ++i) {
         auto name = "q" + std::to_string(i);
         connection.prepare(name, scripts.queries[i - 1], 0);
         BoundStmt s;
         s.name = name;
         statements.push_back(std::move(s));
      }
   }

   void runImpl() override {
      // OLAP queries use pipeline depth 1 (matches tools/oltp ChClient). OLAP
      // is not affected by --target-rate; that flag caps TPC-C only.
      runPipeline(connection, processed, done, 1, /*targetIntervalNs=*/0,
                  [&]() { return pickTransaction(); },
                  [&](unsigned kind, uint64_t ns) { maybeRecord(kind, ns); },
                  /*tolerateQueryErrors=*/true);
   }

   public:
   ChClient(const Options& o, const Scripts& s, unsigned index)
      : Client(o, s), rng(42u * (index + 1)) {}

   std::vector<std::string> labels() const override {
      std::vector<std::string> r;
      r.reserve(queries);
      for (unsigned i = 1; i <= queries; ++i) r.push_back("q" + std::to_string(i));
      return r;
   }
   const char* category() const override { return "olap"; }
};

//---------------------------------------------------------------------------
// Driver
//---------------------------------------------------------------------------
static std::string formatTime(unsigned seconds) {
   std::stringstream out;
   out << std::setfill('0') << std::setw(2) << (seconds / 60) << ":"
       << std::setfill('0') << std::setw(2) << (seconds % 60);
   return out.str();
}

static void preparePopulationIndependent(const Options& opt, const Scripts& scripts) {
   Connection conn;
   conn.open(opt.connString);
   // Use the *last* procedure defined in functions.sql as the sentinel so we
   // only skip the install when a prior run completed the whole script. If a
   // previous attempt was interrupted mid-install, stockLevel won't exist and
   // we'll re-run (which will surface the duplicate-object error from the
   // earlier procedures, prompting the user to clean up).
   PGresult* res = PQexec(conn.raw(),
      "SELECT 1 FROM pg_proc WHERE lower(proname) = 'stocklevel' LIMIT 1");
   bool installed = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
   PQclear(res);
   if (installed) {
      std::cout << "functions already installed, skipping " << opt.scriptDir
                << "/functions_" << opt.targetSystem << ".sql\n";
      return;
   }
   std::cout << "installing functions from " << opt.scriptDir
             << "/functions_" << opt.targetSystem << ".sql...\n";
   conn.exec(scripts.functions);
}

static uint64_t countProcessed(const std::vector<std::unique_ptr<Client>>& clients) {
   uint64_t r = 0;
   for (auto& c : clients) r += c->countProcessed();
   return r;
}

// Merge per-kind stats across a homogeneous group of clients (same labels).
static std::vector<std::pair<std::string, TxStats>> mergeStats(
   const std::vector<std::unique_ptr<Client>>& clients) {
   std::vector<std::pair<std::string, TxStats>> out;
   if (clients.empty()) return out;
   auto labels = clients.front()->labels();
   out.resize(labels.size());
   for (size_t i = 0; i < labels.size(); ++i) out[i].first = labels[i];
   for (auto& c : clients) {
      const auto& s = c->getStats();
      for (size_t i = 0; i < s.size() && i < out.size(); ++i)
         out[i].second.merge(s[i]);
   }
   return out;
}

static void writeCsv(const std::string& path,
                     const std::vector<std::unique_ptr<Client>>& tpcc,
                     const std::vector<std::unique_ptr<Client>>& olap) {
   std::ofstream ofs(path);
   if (!ofs) {
      std::cerr << "warning: cannot open " << path << " for writing\n";
      return;
   }
   ofs << "category,name,count,min_ms,median_ms,p95_ms,p99_ms\n";

   constexpr double NS_PER_MS = 1'000'000.0;
   auto dump = [&](const char* category,
                   const std::vector<std::pair<std::string, TxStats>>& rows) {
      for (auto& [name, s] : rows) {
         double minMs    = s.count ? s.minNs / NS_PER_MS : 0.0;
         double medianMs = s.count ? s.percentile(0.50) / NS_PER_MS : 0.0;
         double p95Ms    = s.count ? s.percentile(0.95) / NS_PER_MS : 0.0;
         double p99Ms    = s.count ? s.percentile(0.99) / NS_PER_MS : 0.0;
         ofs << category << ',' << name << ',' << s.count << ','
             << std::fixed << std::setprecision(3)
             << minMs << ',' << medianMs << ',' << p95Ms << ',' << p99Ms << '\n';
         ofs.unsetf(std::ios::floatfield);
      }
   };

   if (!tpcc.empty()) dump(tpcc.front()->category(), mergeStats(tpcc));
   if (!olap.empty()) dump(olap.front()->category(), mergeStats(olap));

   std::cout << "wrote latency stats to " << path << "\n";

   // Also print a short per-line summary to stdout.
   auto printGroup = [](const char* cat,
                        const std::vector<std::pair<std::string, TxStats>>& rows) {
      if (rows.empty()) return;
      std::cout << "\n" << cat << " latencies (ms):\n";
      std::cout << std::left << std::setw(14) << "  name"
                << std::right << std::setw(10) << "count"
                << std::setw(12) << "min"
                << std::setw(12) << "median"
                << std::setw(12) << "p95"
                << std::setw(12) << "p99" << "\n";
      constexpr double NS_PER_MS = 1'000'000.0;
      for (auto& [name, s] : rows) {
         double minMs    = s.count ? s.minNs / NS_PER_MS : 0.0;
         double medianMs = s.count ? s.percentile(0.50) / NS_PER_MS : 0.0;
         double p95Ms    = s.count ? s.percentile(0.95) / NS_PER_MS : 0.0;
         double p99Ms    = s.count ? s.percentile(0.99) / NS_PER_MS : 0.0;
         std::cout << "  " << std::left << std::setw(12) << name
                   << std::right << std::setw(10) << s.count
                   << std::fixed << std::setprecision(3)
                   << std::setw(12) << minMs
                   << std::setw(12) << medianMs
                   << std::setw(12) << p95Ms
                   << std::setw(12) << p99Ms << "\n";
         std::cout.unsetf(std::ios::floatfield);
      }
   };
   if (!tpcc.empty()) printGroup(tpcc.front()->category(), mergeStats(tpcc));
   if (!olap.empty()) printGroup(olap.front()->category(), mergeStats(olap));
}

static void runBenchmark(const Options& opt, const Scripts& scripts) {
   unsigned clientCount = opt.scaleFactor;
   unsigned olapCount = opt.olapThreads;

   std::cout << "preparing " << clientCount << " TPC-C clients + "
             << olapCount << " OLAP clients...\n\n";

   std::vector<std::unique_ptr<Client>> clients;
   clients.reserve(clientCount);
   for (unsigned i = 0; i < clientCount; ++i) {
      clients.emplace_back(std::make_unique<TpccClient>(opt, scripts, i));
      clients.back()->prepare();
   }

   std::vector<std::unique_ptr<Client>> olapClients;
   olapClients.reserve(olapCount);
   for (unsigned i = 0; i < olapCount; ++i) {
      olapClients.emplace_back(std::make_unique<ChClient>(opt, scripts, i));
      olapClients.back()->prepare();
   }

   auto checkFailure = [&](const char* msg) {
      bool anyFailed = false;
      for (auto& c : clients) if (c->isFailed()) anyFailed = true;
      for (auto& c : olapClients) if (c->isFailed()) anyFailed = true;
      if (anyFailed) {
         for (auto& c : clients) c->stop();
         for (auto& c : olapClients) c->stop();
         die(msg);
      }
   };

   for (auto& c : clients) c->waitUntilPrepared();
   for (auto& c : olapClients) c->waitUntilPrepared();
   checkFailure("error while preparing benchmark run");

   for (auto& c : clients) c->run();
   for (auto& c : olapClients) c->run();

   auto runPhase = [&](const char* label, unsigned duration) {
      if (!duration) return std::pair<uint64_t, uint64_t>{0, 0};
      auto begin = std::chrono::steady_clock::now();
      auto processed = countProcessed(clients);
      auto olap = countProcessed(olapClients);
      auto beginProcessed = processed;
      auto beginOlap = olap;
      for (unsigned i = 1; i <= duration; ++i) {
         std::this_thread::sleep_until(begin + std::chrono::seconds(i));
         auto next = countProcessed(clients);
         auto nextOlap = countProcessed(olapClients);
         std::cout << label << " [ " << formatTime(i) << " / " << formatTime(duration) << " ]: "
                   << (next - processed) << " tx/s (" << next << " processed)";
         if (olapCount)
            std::cout << ", " << (nextOlap - olap) << " queries/s (" << nextOlap << " queries)";
         std::cout << "\n";
         processed = next;
         olap = nextOlap;
      }
      return std::pair<uint64_t, uint64_t>{processed - beginProcessed, olap - beginOlap};
   };

   runPhase("WARMUP ", opt.warmupTime);

   // Enable latency recording only during the measure phase so warmup latencies
   // don't pollute min/max/avg.
   for (auto& c : clients) c->setRecording(true);
   for (auto& c : olapClients) c->setRecording(true);
   auto [txs, qs] = runPhase("MEASURE", opt.measureTime);
   for (auto& c : clients) c->setRecording(false);
   for (auto& c : olapClients) c->setRecording(false);

   if (opt.measureTime) {
      std::cout << "\nMEASURE THROUGHPUT: "
                << (static_cast<double>(txs) / opt.measureTime) << " tx/s ("
                << txs << " processed)";
      if (olapCount)
         std::cout << ", " << (static_cast<double>(qs) / opt.measureTime) << " queries/s ("
                   << qs << " queries)";
      std::cout << "\n";
   }

   // Signal stop to every client first (including query cancellation) so their
   // threads wake concurrently; then join. Otherwise total shutdown would be
   // the SUM of per-client in-flight query durations instead of the MAX.
   std::cout << "\nGathering statistics...\n" << std::flush;
   for (auto& c : clients) c->requestStop();
   for (auto& c : olapClients) c->requestStop();
   for (auto& c : clients) c->join();
   for (auto& c : olapClients) c->join();
   checkFailure("error while executing benchmark run");

   writeCsv(opt.csvPath, clients, olapClients);
}

} // namespace chbench

int main(int argc, char** argv) {
   chbench::Options opt;
   if (!chbench::parseArgs(argc, argv, opt)) return 1;
   if (opt.scriptDir.empty()) opt.scriptDir = chbench::defaultScriptDir(argv[0]);

   try {
      chbench::Scripts scripts;
      scripts.load(opt.scriptDir, opt.targetSystem);
      chbench::preparePopulationIndependent(opt, scripts);
      chbench::runBenchmark(opt, scripts);
   } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 1;
   }
   return 0;
}
