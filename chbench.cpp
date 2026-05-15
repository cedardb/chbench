// Standalone CH-benchmark driver against a Postgres/CedarDB connection string.
// Assumes the TPC-C + CH schema (see ddl-postgres-*.sql) is already populated.
// Runs TPC-C client threads plus optional CH analytical threads.
// Two modes: stored-procedure calls (--mode sproc, default) and
// individual-statement execution matching benchbase behaviour (--mode benchbase).

#include "chbench_base.h"
#include "tpcc_benchbase.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace chbench {

//---------------------------------------------------------------------------
// Argument parsing
//---------------------------------------------------------------------------
static void printHelp(const char* argv0) {
   std::cout
      << "Usage: " << argv0 << " [options]\n"
      << "  --conn <str>          Postgres connection string (required)\n"
      << "  --scale <n>           Scale factor = warehouse count = client thread count (required)\n"
      << "  --olap-threads <n>    Number of analytical clients (default 0)\n"
      << "  --pipeline-depth <n>  Transactions per libpq pipeline batch (default 1, sproc mode only)\n"
      << "  --warmup <sec>        Warmup duration (default 5)\n"
      << "  --measure <sec>       Measurement duration (default 30)\n"
      << "  --target-rate <n>     Global TPC-C transaction rate cap, tx/s (default 0 = uncapped)\n"
      << "  --csv <path>          Latency stats output path (default chbench-stats.csv)\n"
      << "  --target-system <s>   cedar | postgres (default cedar) — picks functions_<s>.sql\n"
      << "  --script-dir <path>   SQL script dir (default: <binary-dir>/sql)\n"
      << "  --mode <s>            sproc | benchbase (default sproc)\n"
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
      else if (a == "--conn")          { auto v = need(i); if (!v) return false; opt.connString = v; }
      else if (a == "--scale")         { auto v = need(i); if (!v) return false; opt.scaleFactor = std::stoul(v); }
      else if (a == "--olap-threads")  { auto v = need(i); if (!v) return false; opt.olapThreads = std::stoul(v); }
      else if (a == "--pipeline-depth"){ auto v = need(i); if (!v) return false; opt.pipelineDepth = std::stoul(v); }
      else if (a == "--warmup")        { auto v = need(i); if (!v) return false; opt.warmupTime = std::stoul(v); }
      else if (a == "--measure")       { auto v = need(i); if (!v) return false; opt.measureTime = std::stoul(v); }
      else if (a == "--target-rate")   { auto v = need(i); if (!v) return false; opt.targetRate = std::stoul(v); }
      else if (a == "--csv")           { auto v = need(i); if (!v) return false; opt.csvPath = v; }
      else if (a == "--target-system") { auto v = need(i); if (!v) return false; opt.targetSystem = v; }
      else if (a == "--script-dir")    { auto v = need(i); if (!v) return false; opt.scriptDir = v; }
      else if (a == "--mode")          { auto v = need(i); if (!v) return false; opt.mode = v; }
      else { std::cerr << "unknown option: " << a << "\n"; return false; }
   }
   if (opt.connString.empty()) { std::cerr << "--conn is required\n"; return false; }
   if (!opt.scaleFactor) { std::cerr << "--scale is required and must be > 0\n"; return false; }
   if (opt.targetSystem != "cedar" && opt.targetSystem != "postgres") {
      std::cerr << "--target-system must be 'cedar' or 'postgres'\n";
      return false;
   }
   if (opt.mode != "sproc" && opt.mode != "benchbase") {
      std::cerr << "--mode must be 'sproc' or 'benchbase'\n";
      return false;
   }
   return true;
}

//---------------------------------------------------------------------------
// Helpers used only in this translation unit
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

//---------------------------------------------------------------------------
// TPC-C stored-procedure client
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

   std::cout << "preparing " << clientCount << " TPC-C clients ("
             << opt.mode << ") + " << olapCount << " OLAP clients...\n\n";

   std::vector<std::unique_ptr<Client>> clients;
   clients.reserve(clientCount);
   for (unsigned i = 0; i < clientCount; ++i) {
      if (opt.mode == "benchbase")
         clients.emplace_back(std::make_unique<TpccBenchbaseClient>(opt, scripts, i));
      else
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
      scripts.load(opt.scriptDir, opt.targetSystem, opt.mode == "benchbase");
      if (opt.mode == "sproc")
         chbench::preparePopulationIndependent(opt, scripts);
      chbench::runBenchmark(opt, scripts);
   } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 1;
   }
   return 0;
}
