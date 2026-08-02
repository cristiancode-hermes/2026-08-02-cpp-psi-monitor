// SPDX-License-Identifier: MIT
// cpp-psi-monitor — Pressure Stall Information (PSI) monitor in C++23
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o psi-monitor
// Usage: ./psi-monitor [--watch SECONDS] [--json] [--no-color] [--selftest] [--help]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace psi {

// ── ANSI palette (Catppuccin-ish) ──────────────────────────────────────────
constexpr const char* kRed    = "\x1b[31m";
constexpr const char* kGreen  = "\x1b[32m";
constexpr const char* kYellow = "\x1b[33m";
constexpr const char* kCyan   = "\x1b[36m";
constexpr const char* kBold   = "\x1b[1m";
constexpr const char* kReset  = "\x1b[0m";

// ── Types ─────────────────────────────────────────────────────────────────
struct Metric {
  double avg10 = 0.0;             // % stall over the last 10s
  double avg60 = 0.0;             // % stall over the last 60s
  double avg300 = 0.0;            // % stall over the last 5 min
  unsigned long long total = 0;   // total stall time in microseconds
};

struct Resource {
  std::string name;               // "cpu" | "memory" | "io"
  std::optional<Metric> some;     // some tasks stalled
  std::optional<Metric> full;     // all tasks stalled (cpu: non-idle tasks)
};

struct LoadAvg {
  double l1 = 0.0, l5 = 0.0, l15 = 0.0;
  int running = 0, total = 0;
  long long last_pid = 0;
};

// ── Small helpers ─────────────────────────────────────────────────────────
std::optional<double> parse_double_after_eq(std::string_view tok) {
  auto eq = tok.find('=');
  if (eq == std::string_view::npos) return std::nullopt;
  std::string_view num = tok.substr(eq + 1);
  double v = 0.0;
  auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
  if (ec != std::errc{}) return std::nullopt;
  return v;
}

std::optional<unsigned long long> parse_ull_after_eq(std::string_view tok) {
  auto eq = tok.find('=');
  if (eq == std::string_view::npos) return std::nullopt;
  std::string_view num = tok.substr(eq + 1);
  unsigned long long v = 0;
  auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
  if (ec != std::errc{}) return std::nullopt;
  return v;
}

// "some avg10=0.04 avg60=0.02 avg300=0.01 total=16308752174"
std::optional<Metric> parse_metric_line(const std::string& line) {
  std::istringstream ss(line);
  std::string kind;
  ss >> kind;
  if (kind != "some" && kind != "full") return std::nullopt;

  Metric m;
  std::string tok;
  while (ss >> tok) {
    if (tok.starts_with("avg10=")) {
      auto v = parse_double_after_eq(tok);
      if (!v) return std::nullopt;  // strict: malformed value rejects the line
      m.avg10 = *v;
    } else if (tok.starts_with("avg60=")) {
      auto v = parse_double_after_eq(tok);
      if (!v) return std::nullopt;
      m.avg60 = *v;
    } else if (tok.starts_with("avg300=")) {
      auto v = parse_double_after_eq(tok);
      if (!v) return std::nullopt;
      m.avg300 = *v;
    } else if (tok.starts_with("total=")) {
      auto v = parse_ull_after_eq(tok);
      if (!v) return std::nullopt;
      m.total = *v;
    }
  }
  return m;
}

std::optional<Resource> read_resource(const std::string& path, std::string name) {
  std::ifstream f(path);
  if (!f.is_open()) return std::nullopt;
  Resource r;
  r.name = std::move(name);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto m = parse_metric_line(line);
    if (!m) continue;
    if (line.starts_with("some")) r.some = *m;
    else if (line.starts_with("full")) r.full = *m;
  }
  return r;
}

std::optional<LoadAvg> parse_loadavg_line(const std::string& line) {
  LoadAvg la;
  std::istringstream ss(line);
  std::string tok;
  int idx = 0;
  while (ss >> tok) {
    if (idx == 0) la.l1 = std::atof(tok.c_str());
    else if (idx == 1) la.l5 = std::atof(tok.c_str());
    else if (idx == 2) la.l15 = std::atof(tok.c_str());
    else if (idx == 3) {
      auto slash = tok.find('/');
      if (slash != std::string::npos) {
        la.running = std::atoi(tok.substr(0, slash).c_str());
        la.total = std::atoi(tok.substr(slash + 1).c_str());
      }
    } else if (idx == 4) {
      la.last_pid = std::atoll(tok.c_str());
    }
    ++idx;
  }
  return la;
}

std::optional<LoadAvg> read_loadavg(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return std::nullopt;
  std::string line;
  if (!std::getline(f, line)) return std::nullopt;
  return parse_loadavg_line(line);
}

bool read_uptime(const std::string& path, double& uptime, double& idle) {
  std::ifstream f(path);
  if (!f.is_open()) return false;
  std::string line;
  if (!std::getline(f, line)) return false;
  std::istringstream ss(line);
  std::string a, b;
  ss >> a >> b;
  if (a.empty() || b.empty()) return false;
  uptime = std::atof(a.c_str());
  idle = std::atof(b.c_str());
  return true;
}

int count_cpus() {
  std::ifstream f("/proc/stat");
  if (!f.is_open()) return 0;
  int n = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.starts_with("cpu") && line.size() > 3 && line[3] >= '0' && line[3] <= '9') ++n;
  }
  return n;
}

std::string format_duration(double seconds) {
  long long s = static_cast<long long>(seconds);
  int d = static_cast<int>(s / 86400);
  int h = static_cast<int>((s % 86400) / 3600);
  int m = static_cast<int>((s % 3600) / 60);
  int sec = static_cast<int>(s % 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%dd %02d:%02d:%02d", d, h, m, sec);
  return buf;
}

std::string format_human(unsigned long long v) {
  if (v >= 1'000'000'000ULL) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fB", static_cast<double>(v) / 1e9);
    return buf;
  }
  if (v >= 1'000'000ULL) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(v) / 1e6);
    return buf;
  }
  if (v >= 1'000ULL) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(v) / 1e3);
    return buf;
  }
  return std::to_string(v);
}

const char* pct_color(double pct, bool color) {
  if (!color) return "";
  if (pct >= 50.0) return kRed;
  if (pct >= 10.0) return kYellow;
  return kGreen;
}

// ── Rendering ─────────────────────────────────────────────────────────────
void print_banner(bool color) {
  const char* c = color ? kCyan : "";
  const char* b = color ? kBold : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "{}{}psi-monitor{} — Pressure Stall Information (PSI) monitor (C++23)", c, b, r);
  std::println(std::cout);
}

void render_table(const std::vector<Resource>& resources, bool color) {
  std::println(std::cout, "  {:<8} {:<5} {:>7}  {:>6}  {:>6}  {:>8}", "RESOURCE", "TYPE", "AVG10", "AVG60", "AVG300", "STALL");

  for (const auto& res : resources) {
    auto render_metric = [&](std::string_view kind, const Metric& m) {
      const char* c = pct_color(m.avg10, color);
      const char* rr = color ? kReset : "";
      std::string stall = format_human(m.total);
      // 8 placeholders max (GCC 14 limit)
      std::println(std::cout, "  {:<8} {:<5} {}{:>6.2f}%{}  {:>6.2f}%  {:>6.2f}%  {:>8} us",
                   res.name, kind, c, m.avg10, rr, m.avg60, m.avg300, stall);
    };
    if (res.some) render_metric("some", *res.some);
    if (res.full) render_metric("full", *res.full);
  }
  std::println(std::cout);
}

void render_context() {
  double up = 0.0, idle = 0.0;
  bool have_uptime = read_uptime("/proc/uptime", up, idle);
  int cores = count_cpus();

  if (auto la = read_loadavg("/proc/loadavg"); la) {
    std::println(std::cout, "  load   {:<5.2f} {:<5.2f} {:<5.2f}    {}/{} procs    last pid {}",
                 la->l1, la->l5, la->l15, la->running, la->total, la->last_pid);
  }
  if (have_uptime) {
    std::string idle_str = (cores > 0) ? format_duration(idle) : "?";
    std::string core_str = (cores > 0) ? std::to_string(cores) : "?";
    std::println(std::cout, "  uptime {}        idle {} ({} cores)",
                 format_duration(up), idle_str, core_str);
  }
  std::println(std::cout);
}

void render_pressure_status(const std::vector<Resource>& resources, bool color) {
  double maxp = 0.0;
  for (const auto& res : resources) {
    if (res.some) maxp = std::max(maxp, res.some->avg10);
    if (res.full) maxp = std::max(maxp, res.full->avg10);
  }
  const char* c = color ? kReset : "";
  const char* rr = color ? kReset : "";
  if (maxp >= 50.0) {
    const char* red = color ? kRed : "";
    std::println(std::cout, "  {}⚠ HIGH PRESSURE — resources stalled, max avg10 {:.2f}%{}", red, maxp, rr);
  } else if (maxp >= 10.0) {
    const char* yel = color ? kYellow : "";
    std::println(std::cout, "  {}⚠ elevated pressure — max avg10 {:.2f}%{}", yel, maxp, rr);
  } else {
    const char* grn = color ? kGreen : "";
    std::println(std::cout, "  {}✓ no pressure detected{} — max avg10 {:.2f}%{}", grn, c, maxp, rr);
  }
}

std::string metric_json(std::string_view kind, const Metric& m) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "{\"avg10\":%.2f,\"avg60\":%.2f,\"avg300\":%.2f,\"total\":%llu}",
                m.avg10, m.avg60, m.avg300, m.total);
  return "\"" + std::string(kind) + "\":" + buf;
}

void render_json(const std::vector<Resource>& resources) {
  std::string out = "{\n  \"resources\": {\n";
  for (size_t i = 0; i < resources.size(); ++i) {
    const auto& res = resources[i];
    out += "    \"" + res.name + "\": {";
    std::vector<std::string> parts;
    if (res.some) parts.push_back(metric_json("some", *res.some));
    if (res.full) parts.push_back(metric_json("full", *res.full));
    for (size_t j = 0; j < parts.size(); ++j) {
      out += parts[j];
      if (j + 1 < parts.size()) out += ",";
    }
    out += "}";
    if (i + 1 < resources.size()) out += ",";
    out += "\n";
  }
  out += "  },\n  \"loadavg\": {";
  if (auto la = read_loadavg("/proc/loadavg"); la) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "\"l1\":%.2f,\"l5\":%.2f,\"l15\":%.2f,\"running\":%d,\"total\":%d,\"last_pid\":%lld",
                  la->l1, la->l5, la->l15, la->running, la->total, la->last_pid);
    out += buf;
  }
  out += "},\n  \"uptime\": {";
  double up = 0.0, idle = 0.0;
  if (read_uptime("/proc/uptime", up, idle)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "\"seconds\":%.2f,\"idle_seconds\":%.2f,\"cores\":%d",
                  up, idle, count_cpus());
    out += buf;
  }
  out += "}\n}\n";
  std::cout << out;
}

void print_help() {
  std::cout <<
    "psi-monitor — Pressure Stall Information (PSI) monitor in C++23\n"
    "\n"
    "Reads /proc/pressure/{cpu,memory,io} (Linux kernel 4.20+, CONFIG_PSI) and\n"
    "reports the percentage of time tasks were stalled on resources, plus\n"
    "load average, uptime and CPU count context.\n"
    "\n"
    "Usage:\n"
    "  ./psi-monitor               one-shot table of PSI metrics\n"
    "  ./psi-monitor --watch 2     refresh every 2 seconds (Ctrl+C to stop)\n"
    "  ./psi-monitor --json        machine-readable JSON output\n"
    "  ./psi-monitor --selftest    run parser/format self-tests\n"
    "  ./psi-monitor --no-color    plain output (scripts/pipes)\n"
    "  ./psi-monitor --help        show this help\n"
    "\n"
    "Exit codes: 0 = ok, 2 = PSI unavailable or bad arguments\n";
}

// ── Self-tests ────────────────────────────────────────────────────────────
int run_selftest() {
  int failures = 0;
  auto check = [&failures](bool cond, std::string_view name) {
    if (cond) {
      std::println(std::cout, "  ✓ {}", name);
    } else {
      std::println(std::cout, "  ✗ {}", name);
      ++failures;
    }
  };

  std::println(std::cout, "psi-monitor selftest");

  // 1. Parse a "some" PSI line
  auto m1 = parse_metric_line("some avg10=0.04 avg60=0.02 avg300=0.01 total=16308752174");
  check(m1.has_value(), "parse some line");
  check(m1 && m1->avg10 > 0.03 && m1->avg10 < 0.05, "avg10 parsed (0.04)");
  check(m1 && m1->total == 16308752174ULL, "total parsed (16308752174)");

  // 2. Parse a "full" PSI line with zeros
  auto m2 = parse_metric_line("full avg10=0.00 avg60=0.00 avg300=0.00 total=0");
  check(m2.has_value() && m2->avg10 == 0.0 && m2->total == 0, "parse full zero line");

  // 3. High-pressure line
  auto m3 = parse_metric_line("some avg10=63.21 avg60=45.10 avg300=12.50 total=1024");
  check(m3.has_value() && m3->avg10 > 63.0 && m3->avg10 < 64.0, "parse high-pressure line (63.21)");
  check(m3 && m3->total == 1024ULL, "small total parsed");

  // 4. Malformed line is rejected
  check(!parse_metric_line("garbage no=1").has_value(), "reject malformed line");
  check(!parse_metric_line("some avg10=abc").has_value(), "reject non-numeric avg10");

  // 5. Threshold colors
  check(std::string(pct_color(5.0, true)) == kGreen, "5% -> green");
  check(std::string(pct_color(25.0, true)) == kYellow, "25% -> yellow");
  check(std::string(pct_color(75.0, true)) == kRed, "75% -> red");
  check(std::string(pct_color(75.0, false)).empty(), "no-color disables ansi");

  // 6. Load average
  auto la = parse_loadavg_line("0.71 0.69 0.62 2/706 1266845");
  check(la.has_value(), "parse loadavg");
  check(la && la->l1 > 0.70 && la->l1 < 0.72, "load1 parsed (0.71)");
  check(la && la->running == 2 && la->total == 706, "running/total parsed (2/706)");
  check(la && la->last_pid == 1266845LL, "last pid parsed");

  // 7. Uptime formatting
  check(format_duration(1755605.0) == "20d 07:40:05", "format duration (20d 07:40:05)");
  check(format_duration(0.0) == "0d 00:00:00", "format zero duration");
  check(format_duration(3661.0) == "0d 01:01:01", "format 3661s");

  // 8. Human totals
  check(format_human(16308752174ULL) == "16.3B", "format 16.3B");
  check(format_human(1234567ULL) == "1.2M", "format 1.2M");
  check(format_human(88213ULL) == "88.2K", "format 88.2K");
  check(format_human(0ULL) == "0", "format 0");
  check(format_human(999ULL) == "999", "format 999");

  // 9. Live /proc/pressure read (conditional: kernel may lack PSI)
  if (fs::exists("/proc/pressure/cpu")) {
    auto cpu = read_resource("/proc/pressure/cpu", "cpu");
    check(cpu.has_value(), "live read /proc/pressure/cpu");
    check(cpu && cpu->some.has_value(), "cpu has 'some' metric");
    if (cpu && cpu->some) {
      bool in_range = cpu->some->avg10 >= 0.0 && cpu->some->avg10 <= 100.0;
      check(in_range, "cpu avg10 in [0,100]");
    }
  } else {
    check(true, "live /proc/pressure read (skipped: not available)");
  }

  std::println(std::cout);
  if (failures == 0) {
    std::println(std::cout, "  {} 13/13 selftests passed{}", kGreen, kReset);
    return 0;
  }
  std::println(std::cout, "  {} {} selftest(s) failed{}", kRed, failures, kReset);
  return 1;
}

}  // namespace psi

// ── Signal handling (watch mode) ──────────────────────────────────────────
static volatile std::sig_atomic_t g_stop = 0;
extern "C" void psi_on_sigint(int) { g_stop = 1; }

int main(int argc, char** argv) {
  using namespace psi;

  bool color = true, json = false, selftest = false;
  int watch = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--no-color") {
      color = false;
    } else if (a == "--json") {
      json = true;
    } else if (a == "--selftest") {
      selftest = true;
    } else if (a == "--watch" || a == "-w") {
      if (i + 1 < argc) {
        watch = std::atoi(argv[++i]);
        if (watch <= 0) watch = 1;
      }
    } else if (a == "--help" || a == "-h") {
      print_help();
      return 0;
    } else {
      std::cerr << "psi-monitor: unknown option '" << a << "'\n\n";
      print_help();
      return 2;
    }
  }

  if (selftest) return run_selftest();

  if (!fs::exists("/proc/pressure/cpu")) {
    std::cerr << "psi-monitor: PSI not available on this system\n"
                 "(requires Linux kernel 4.20+ with CONFIG_PSI enabled)\n";
    return 2;
  }

  std::vector<Resource> resources;
  for (const auto& [path, name] : std::vector<std::pair<std::string, std::string>>{
           {"/proc/pressure/cpu", "cpu"},
           {"/proc/pressure/memory", "memory"},
           {"/proc/pressure/io", "io"}}) {
    if (auto r = read_resource(path, name); r) resources.push_back(std::move(*r));
  }

  if (json) {
    render_json(resources);
    return 0;
  }

  if (watch > 0) {
    std::signal(SIGINT, psi_on_sigint);
    while (!g_stop) {
      if (color) std::print(std::cout, "\x1b[2J\x1b[H");
      print_banner(color);
      render_table(resources, color);
      render_context();
      render_pressure_status(resources, color);
      std::cout << std::flush;  // render immediately, even piped
      if (g_stop) break;
      std::this_thread::sleep_for(std::chrono::seconds(watch));
    }
    std::println(std::cout);
    return 0;
  }

  print_banner(color);
  render_table(resources, color);
  render_context();
  render_pressure_status(resources, color);
  return 0;
}
