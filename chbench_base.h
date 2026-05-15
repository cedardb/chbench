#pragma once

// Shared infrastructure for the CH-benchmark driver.
// Included by chbench.cpp and tpcc_benchbase.h.

#include <libpq-fe.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
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
   std::string mode = "sproc";          // "sproc" or "benchbase"
};

//---------------------------------------------------------------------------
// SQL script cache
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

   void load(const std::string& dir, const std::string& targetSystem, bool skipProcs = false) {
      std::filesystem::path d(dir);
      if (!skipProcs) {
         functions = slurp(d / ("functions_" + targetSystem + ".sql"));
         session   = slurp(d / ("session_" + targetSystem + ".sql"));
      }
      queries.reserve(22);
      for (unsigned i = 1; i <= 22; ++i)
         queries.push_back(slurp(d / "queries" / (std::to_string(i) + ".sql")));
   }
};

//---------------------------------------------------------------------------
// Connection
//---------------------------------------------------------------------------
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

} // namespace chbench
