// ami_callmon_tui.cpp
// Asterisk 20 AMI event-driven call/bridge monitor + control TUI (ncurses)
// Features:
// - Live bridges ("calls") with participants
// - Inbound/Outbound/Internal classification (heuristic + optional CALL_DIR variable)
// - Duration from BridgeEnter timestamps
// - Actions: hangup channel, hangup bridge, kick channel from bridge, originate supervisor monitor (ChanSpy)
// Notes:
// - This program does NOT capture audio. Supervisor monitoring is done by Originate to a supervisor endpoint
//   into a dialplan context that runs ChanSpy.
//
// Build (Debian/Ubuntu):
//   g++ -std=c++17 -O2 -pthread ami_callmon_tui.cpp -o ami-callmon -lboost_system -lncursesw
//
// Run:
//   ./ami-callmon 127.0.0.1 5038 callmon 'secret'
//
// Optional env/config can be applied by systemd EnvironmentFile in the installer.

#include <boost/asio.hpp>
#include <ncursesw/ncurses.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using boost::asio::ip::tcp;

static std::atomic_bool g_running{true};

static inline std::string trim(std::string s) {
  auto notSpace = [](int ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

static inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

static inline int to_int_safe(const std::string& s) {
  try {
    return std::stoi(s);
  } catch (...) {
    return 0;
  }
}

static inline std::string now_ts() {
  auto t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

struct AmiMessage {
  std::unordered_map<std::string, std::string> kv;
};

struct ChannelInfo {
  std::string channel;
  std::string uniqueid;
  std::string linkedid;
  std::string caller_num;
  std::string caller_name;
  std::string connected_num;
  std::string connected_name;
  std::string context;
  std::string exten;
  std::string state_desc;
  std::string channelstate;
  std::string tech;      // PJSIP, Local, etc (best effort)
  std::string peer;      // endpoint/trunk best effort
  std::string call_dir;  // inbound/outbound/internal/unknown (optional from dialplan var)
  std::string bridge_id;

  std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();
};

struct BridgeInfo {
  std::string bridge_id;
  std::string bridge_type;
  std::set<std::string> channels; // channel names
  std::chrono::steady_clock::time_point first_enter = std::chrono::steady_clock::time_point::min();
  std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();
};

struct AppConfig {
  std::string ami_host = "127.0.0.1";
  int ami_port = 5038;
  std::string ami_user;
  std::string ami_secret;

  // Supervisor originate (optional)
  std::string supervisor_endpoint = ""; // e.g. "PJSIP/9000"
  std::string supervisor_context  = "supervisor-monitor";
  std::string supervisor_prefix   = "*55"; // dialplan expects *55<target>
  int originate_timeout_ms = 20000;

  // Heuristic trunk/extension detection
  std::vector<std::string> trunk_prefixes = {"PJSIP/trunk", "PJSIP/siptrunk", "PJSIP/provider"};
};

class AmiClient {
public:
  AmiClient(boost::asio::io_context& io, AppConfig cfg)
      : io_(io), socket_(io), cfg_(std::move(cfg)) {}

  void connect() {
    tcp::resolver resolver(io_);
    auto endpoints = resolver.resolve(cfg_.ami_host, std::to_string(cfg_.ami_port));
    boost::asio::connect(socket_, endpoints);

    // Drain banner (non-blocking small read)
    boost::system::error_code ec;
    socket_.non_blocking(true, ec);
    if (!ec) {
      for (int i = 0; i < 5; i++) {
        char buf[1024];
        socket_.read_some(boost::asio::buffer(buf), ec);
        if (ec) break;
      }
    }
    socket_.non_blocking(false, ec);
  }

  bool login() {
    std::ostringstream oss;
    oss << "Action: Login\r\n"
        << "Username: " << cfg_.ami_user << "\r\n"
        << "Secret: " << cfg_.ami_secret << "\r\n"
        << "Events: on\r\n"
        << "\r\n";
    write_raw(oss.str());

    auto msg = read_message_blocking();
    if (!msg) return false;
    auto it = msg->kv.find("Response");
    if (it == msg->kv.end()) return false;
    return lower(it->second) == "success";
  }

  void logoff() {
    write_raw("Action: Logoff\r\n\r\n");
  }

  // Read loop: pushes parsed AMI messages into the queue
  void start_reader(std::deque<AmiMessage>* out_queue, std::mutex* out_mu) {
    reader_thread_ = std::thread([this, out_queue, out_mu]() {
      while (g_running.load()) {
        try {
          auto msgOpt = read_message_blocking();
          if (!msgOpt) continue;
          {
            std::lock_guard<std::mutex> lk(*out_mu);
            out_queue->push_back(std::move(*msgOpt));
            if (out_queue->size() > 20000) out_queue->pop_front();
          }
        } catch (...) {
          // socket error, break to allow restart logic (not implemented in this minimal version)
          break;
        }
      }
    });
  }

  void stop_reader() {
    if (reader_thread_.joinable()) reader_thread_.join();
  }

  // Actions
  bool hangup_channel(const std::string& channel) {
    std::ostringstream oss;
    oss << "Action: Hangup\r\n"
        << "Channel: " << channel << "\r\n\r\n";
    write_raw(oss.str());
    auto msg = read_message_blocking();
    return msg && lower(msg->kv["Response"]) == "success";
  }

  bool bridge_kick(const std::string& bridge_id, const std::string& channel) {
    // Asterisk 20 supports BridgeKick
    std::ostringstream oss;
    oss << "Action: BridgeKick\r\n"
        << "BridgeUniqueid: " << bridge_id << "\r\n"
        << "Channel: " << channel << "\r\n\r\n";
    write_raw(oss.str());
    auto msg = read_message_blocking();
    return msg && lower(msg->kv["Response"]) == "success";
  }

  bool bridge_destroy(const std::string& bridge_id) {
    // More deterministic than hanging up one channel when you want the entire bridge ended
    std::ostringstream oss;
    oss << "Action: BridgeDestroy\r\n"
        << "BridgeUniqueid: " << bridge_id << "\r\n\r\n";
    write_raw(oss.str());
    auto msg = read_message_blocking();
    return msg && lower(msg->kv["Response"]) == "success";
  }

  bool originate_supervisor_chanspy(const std::string& target_channel) {
    if (cfg_.supervisor_endpoint.empty()) return false;

    // Dialplan expects extension like *55<target>, in supervisor-monitor context.
    // Example: exten "*55PJSIP/1001-0000002a"
    std::string exten = cfg_.supervisor_prefix + target_channel;

    std::ostringstream oss;
    oss << "Action: Originate\r\n"
        << "Channel: " << cfg_.supervisor_endpoint << "\r\n"
        << "Context: " << cfg_.supervisor_context << "\r\n"
        << "Exten: " << exten << "\r\n"
        << "Priority: 1\r\n"
        << "Timeout: " << cfg_.originate_timeout_ms << "\r\n"
        << "Async: true\r\n\r\n";
    write_raw(oss.str());
    auto msg = read_message_blocking();
    return msg && lower(msg->kv["Response"]) == "success";
  }

private:
  void write_raw(const std::string& s) {
    boost::asio::write(socket_, boost::asio::buffer(s));
  }

  std::optional<AmiMessage> read_message_blocking() {
    AmiMessage msg;
    while (true) {
      std::string line = read_line_crlf();
      if (line.empty()) {
        if (!msg.kv.empty()) return msg;
        continue;
      }
      auto pos = line.find(':');
      if (pos == std::string::npos) continue;
      std::string k = trim(line.substr(0, pos));
      std::string v = trim(line.substr(pos + 1));
      msg.kv[k] = v;
    }
  }

  std::string read_line_crlf() {
    boost::asio::streambuf sb;
    boost::asio::read_until(socket_, sb, "\r\n");
    std::istream is(&sb);
    std::string line;
    std::getline(is, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
  }

  boost::asio::io_context& io_;
  tcp::socket socket_;
  AppConfig cfg_;
  std::thread reader_thread_;
};

// --- State Store ---
struct StateStore {
  std::unordered_map<std::string, ChannelInfo> channels_by_name; // key=Channel
  std::unordered_map<std::string, std::string> chan_by_uniqueid; // uniqueid -> Channel
  std::unordered_map<std::string, BridgeInfo> bridges;           // bridge_id -> bridge info
  std::deque<std::string> audit_log;                             // last N actions/events of interest
  std::string filter = "all"; // all|inbound|outbound|internal
  int selected_bridge_index = 0;
  int selected_member_index = 0;

  void log_line(const std::string& s) {
    audit_log.push_back(now_ts() + "  " + s);
    while (audit_log.size() > 2000) audit_log.pop_front();
  }
};

static void parse_tech_peer(const std::string& channel, std::string& tech, std::string& peer) {
  // Examples:
  // PJSIP/1001-0000002a -> tech=PJSIP peer=1001
  // PJSIP/provider-0000001b -> tech=PJSIP peer=provider
  auto slash = channel.find('/');
  if (slash == std::string::npos) return;
  tech = channel.substr(0, slash);
  std::string rest = channel.substr(slash + 1);
  auto dash = rest.find('-');
  peer = (dash == std::string::npos) ? rest : rest.substr(0, dash);
}

static std::string classify_dir_heuristic(const ChannelInfo& c, const AppConfig& cfg) {
  // If dialplan sets CALL_DIR, prefer it
  if (!c.call_dir.empty()) return lower(c.call_dir);

  // Heuristic:
  // - Trunk-like channel names are inbound/outbound depending on context/exten and connected/caller
  // - Internal if peer looks numeric extension and context suggests internal
  // This is best-effort. For accuracy, set __CALL_DIR in dialplan at entry.

  std::string ch = c.channel;
  std::string lch = lower(ch);

  bool is_trunk = false;
  for (const auto& p : cfg.trunk_prefixes) {
    if (lch.find(lower(p)) != std::string::npos) { is_trunk = true; break; }
  }

  // Internal extension guess: peer is digits and not obviously trunk
  bool peer_digits = !c.peer.empty() && std::all_of(c.peer.begin(), c.peer.end(), ::isdigit);

  if (is_trunk) {
    // If caller looks like PSTN and connected looks like extension, likely inbound
    bool conn_is_ext = !c.connected_num.empty() && std::all_of(c.connected_num.begin(), c.connected_num.end(), ::isdigit) && c.connected_num.size() <= 6;
    bool caller_is_ext = !c.caller_num.empty() && std::all_of(c.caller_num.begin(), c.caller_num.end(), ::isdigit) && c.caller_num.size() <= 6;
    if (conn_is_ext && !caller_is_ext) return "inbound";
    if (caller_is_ext && !conn_is_ext) return "outbound";
    return "unknown";
  }

  if (peer_digits) return "internal";
  return "unknown";
}

static int secs_since(std::chrono::steady_clock::time_point t0) {
  if (t0 == std::chrono::steady_clock::time_point::min()) return 0;
  auto now = std::chrono::steady_clock::now();
  return (int)std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
}

static void apply_event(StateStore& st, const AppConfig& cfg, const AmiMessage& m) {
  auto get = [&](const char* k) -> std::string {
    auto it = m.kv.find(k);
    return it == m.kv.end() ? "" : it->second;
  };

  const std::string event = get("Event");
  if (event.empty()) return;

  // Channel lifecycle and metadata
  if (event == "Newchannel") {
    ChannelInfo ci;
    ci.channel = get("Channel");
    ci.uniqueid = get("Uniqueid");
    ci.linkedid = get("Linkedid");
    ci.caller_num = get("CallerIDNum");
    ci.caller_name = get("CallerIDName");
    ci.context = get("Context");
    ci.exten = get("Exten");
    ci.channelstate = get("ChannelState");
    ci.state_desc = get("ChannelStateDesc");
    parse_tech_peer(ci.channel, ci.tech, ci.peer);

    ci.last_update = std::chrono::steady_clock::now();
    st.channels_by_name[ci.channel] = ci;
    if (!ci.uniqueid.empty()) st.chan_by_uniqueid[ci.uniqueid] = ci.channel;
    st.log_line("Newchannel: " + ci.channel);
    return;
  }

  if (event == "Rename") {
    std::string oldn = get("Oldname");
    std::string newn = get("Newname");
    if (!oldn.empty() && !newn.empty()) {
      auto it = st.channels_by_name.find(oldn);
      if (it != st.channels_by_name.end()) {
        ChannelInfo ci = it->second;
        st.channels_by_name.erase(it);
        ci.channel = newn;
        parse_tech_peer(ci.channel, ci.tech, ci.peer);
        st.channels_by_name[newn] = ci;

        // Update any bridge memberships
        for (auto& [bid, b] : st.bridges) {
          if (b.channels.erase(oldn)) b.channels.insert(newn);
        }
        st.log_line("Rename: " + oldn + " -> " + newn);
      }
    }
    return;
  }

  if (event == "Newstate") {
    std::string ch = get("Channel");
    auto it = st.channels_by_name.find(ch);
    if (it != st.channels_by_name.end()) {
      it->second.channelstate = get("ChannelState");
      it->second.state_desc = get("ChannelStateDesc");
      it->second.last_update = std::chrono::steady_clock::now();
    }
    return;
  }

  if (event == "NewCallerid") {
    std::string ch = get("Channel");
    auto it = st.channels_by_name.find(ch);
    if (it != st.channels_by_name.end()) {
      it->second.caller_num = get("CallerIDNum");
      it->second.caller_name = get("CallerIDName");
      it->second.last_update = std::chrono::steady_clock::now();
    }
    return;
  }

  if (event == "VarSet") {
    std::string ch = get("Channel");
    std::string var = get("Variable");
    std::string val = get("Value");
    auto it = st.channels_by_name.find(ch);
    if (it != st.channels_by_name.end()) {
      if (var == "CALL_DIR" || var == "__CALL_DIR") {
        it->second.call_dir = val;
      }
      it->second.last_update = std::chrono::steady_clock::now();
    }
    return;
  }

  if (event == "Hangup") {
    std::string ch = get("Channel");
    // remove from bridges
    for (auto& [bid, b] : st.bridges) b.channels.erase(ch);
    st.channels_by_name.erase(ch);
    st.log_line("Hangup: " + ch);
    return;
  }

  // Bridge lifecycle
  if (event == "BridgeCreate") {
    BridgeInfo bi;
    bi.bridge_id = get("BridgeUniqueid");
    bi.bridge_type = get("BridgeType");
    bi.last_update = std::chrono::steady_clock::now();
    st.bridges[bi.bridge_id] = bi;
    st.log_line("BridgeCreate: " + bi.bridge_id);
    return;
  }

  if (event == "BridgeDestroy") {
    std::string bid = get("BridgeUniqueid");
    st.bridges.erase(bid);
    st.log_line("BridgeDestroy: " + bid);
    return;
  }

  if (event == "BridgeEnter") {
    std::string bid = get("BridgeUniqueid");
    std::string ch = get("Channel");
    auto& b = st.bridges[bid];
    b.bridge_id = bid;
    b.bridge_type = get("BridgeType");
    b.channels.insert(ch);
    b.last_update = std::chrono::steady_clock::now();
    if (b.first_enter == std::chrono::steady_clock::time_point::min()) {
      b.first_enter = std::chrono::steady_clock::now();
    }
    auto it = st.channels_by_name.find(ch);
    if (it != st.channels_by_name.end()) it->second.bridge_id = bid;
    return;
  }

  if (event == "BridgeLeave") {
    std::string bid = get("BridgeUniqueid");
    std::string ch = get("Channel");
    auto bit = st.bridges.find(bid);
    if (bit != st.bridges.end()) {
      bit->second.channels.erase(ch);
      bit->second.last_update = std::chrono::steady_clock::now();
    }
    auto it = st.channels_by_name.find(ch);
    if (it != st.channels_by_name.end()) it->second.bridge_id.clear();
    return;
  }

  // Optional: DialBegin/DialEnd could be used to refine direction and ring time if desired.
}

// --- TUI ---
struct BridgeRow {
  std::string bridge_id;
  std::string dir;
  int duration_sec = 0;
  int participants = 0;
  std::vector<std::string> member_channels;
  std::string summary;
};

static std::vector<BridgeRow> build_bridge_rows(StateStore& st, const AppConfig& cfg) {
  std::vector<BridgeRow> rows;
  rows.reserve(st.bridges.size());

  for (const auto& [bid, b] : st.bridges) {
    if (b.channels.empty()) continue;

    BridgeRow r;
    r.bridge_id = bid;
    r.duration_sec = secs_since(b.first_enter);
    r.participants = (int)b.channels.size();
    r.member_channels.assign(b.channels.begin(), b.channels.end());

    // Determine direction by looking at member channels classifications
    std::map<std::string, int> counts;
    for (const auto& ch : r.member_channels) {
      auto it = st.channels_by_name.find(ch);
      if (it == st.channels_by_name.end()) continue;
      std::string d = classify_dir_heuristic(it->second, cfg);
      counts[d]++;
    }
    std::string dir = "unknown";
    int best = 0;
    for (auto& kv : counts) {
      if (kv.second > best) { best = kv.second; dir = kv.first; }
    }
    r.dir = dir;

    // Apply filter
    if (lower(st.filter) != "all" && lower(st.filter) != lower(r.dir)) continue;

    // Build human summary: try to pick 1-2 legs with caller->connected
    std::ostringstream sum;
    int shown = 0;
    for (const auto& ch : r.member_channels) {
      auto it = st.channels_by_name.find(ch);
      if (it == st.channels_by_name.end()) continue;
      const auto& c = it->second;

      std::string caller = c.caller_num.empty() ? "unknown" : c.caller_num;
      std::string conn = c.connected_num.empty() ? "unknown" : c.connected_num;
      if (caller == "unknown" && conn == "unknown") continue;

      sum << c.tech << "/" << c.peer << " " << caller << "->" << conn << "  ";
      if (++shown >= 2) break;
    }
    r.summary = sum.str();
    rows.push_back(std::move(r));
  }

  // Stable ordering: longest duration first (more relevant)
  std::sort(rows.begin(), rows.end(), [](const BridgeRow& a, const BridgeRow& b) {
    return a.duration_sec > b.duration_sec;
  });

  return rows;
}

static void tui_draw(StateStore& st, const AppConfig& cfg) {
  erase();
  int maxy, maxx;
  getmaxyx(stdscr, maxy, maxx);

  mvprintw(0, 0, "Asterisk AMI Call Monitor (Asterisk 20 / PJSIP)  Filter: %s  Time: %s",
           st.filter.c_str(), now_ts().c_str());

  mvprintw(1, 0, "Keys: [Up/Down]=Select Call  [Tab]=Select Member  [F]=Filter  [H]=Hangup Member  [K]=Kick Member  [B]=Destroy Bridge");
  mvprintw(2, 0, "      [M]=Monitor (Originate supervisor to ChanSpy)  [L]=Logs  [Q]=Quit");

  auto rows = build_bridge_rows(st, cfg);

  int list_start = 4;
  mvprintw(list_start - 1, 0, "Calls (bridges): %d", (int)rows.size());
  mvhline(list_start, 0, ACS_HLINE, maxx);

  int y = list_start + 1;
  int idx = 0;
  for (; idx < (int)rows.size() && y < maxy - 8; idx++, y++) {
    const auto& r = rows[idx];
    bool sel = (idx == st.selected_bridge_index);
    if (sel) attron(A_REVERSE);

    std::ostringstream line;
    line << std::setw(3) << idx + 1 << "  "
         << std::setw(8) << (std::to_string(r.duration_sec) + "s") << "  "
         << std::setw(9) << r.dir << "  "
         << "parts=" << r.participants << "  "
         << r.bridge_id.substr(0, 12) << "…  "
         << r.summary;

    std::string s = line.str();
    if ((int)s.size() > maxx - 1) s.resize(maxx - 1);
    mvprintw(y, 0, "%s", s.c_str());

    if (sel) attroff(A_REVERSE);
  }

  // Selected call details
  int detail_y = maxy - 7;
  mvhline(detail_y - 1, 0, ACS_HLINE, maxx);
  mvprintw(detail_y, 0, "Selected Call Details:");

  if (!rows.empty()) {
    st.selected_bridge_index = std::max(0, std::min(st.selected_bridge_index, (int)rows.size() - 1));
    const auto& sel = rows[st.selected_bridge_index];

    mvprintw(detail_y + 1, 0, "BridgeUniqueid: %s", sel.bridge_id.c_str());
    mvprintw(detail_y + 2, 0, "Direction: %s   Duration: %ds   Participants: %d",
             sel.dir.c_str(), sel.duration_sec, sel.participants);

    mvprintw(detail_y + 3, 0, "Members:");
    int my = detail_y + 4;

    int mindex = 0;
    st.selected_member_index = std::max(0, std::min(st.selected_member_index, (int)sel.member_channels.size() - 1));

    for (; mindex < (int)sel.member_channels.size() && my < maxy - 1; mindex++, my++) {
      const std::string& ch = sel.member_channels[mindex];
      auto it = st.channels_by_name.find(ch);

      std::ostringstream ml;
      ml << (mindex == st.selected_member_index ? " > " : "   ")
         << ch;

      if (it != st.channels_by_name.end()) {
        const auto& c = it->second;
        std::string d = classify_dir_heuristic(c, cfg);
        ml << "  [" << d << "]"
           << "  CID:" << (c.caller_num.empty() ? "?" : c.caller_num)
           << "  CONN:" << (c.connected_num.empty() ? "?" : c.connected_num)
           << "  STATE:" << (c.state_desc.empty() ? "?" : c.state_desc);
      }

      std::string ms = ml.str();
      if ((int)ms.size() > maxx - 1) ms.resize(maxx - 1);
      mvprintw(my, 0, "%s", ms.c_str());
    }
  } else {
    mvprintw(detail_y + 1, 0, "No active bridges detected. Ensure calls are being bridged and AMI events are enabled.");
  }

  refresh();
}

static void tui_show_logs(StateStore& st) {
  erase();
  int maxy, maxx;
  getmaxyx(stdscr, maxy, maxx);

  mvprintw(0, 0, "Audit / Event Log (press any key to return)");
  mvhline(1, 0, ACS_HLINE, maxx);

  int start = std::max(0, (int)st.audit_log.size() - (maxy - 3));
  int y = 2;
  for (int i = start; i < (int)st.audit_log.size() && y < maxy; i++, y++) {
    std::string s = st.audit_log[i];
    if ((int)s.size() > maxx - 1) s.resize(maxx - 1);
    mvprintw(y, 0, "%s", s.c_str());
  }
  refresh();
  getch();
}

static void signal_handler(int) {
  g_running.store(false);
}

static AppConfig read_config_from_env_and_args(int argc, char** argv) {
  AppConfig cfg;

  // CLI: host port user secret
  if (argc >= 3) { cfg.ami_host = argv[1]; cfg.ami_port = std::stoi(argv[2]); }
  if (argc >= 4) cfg.ami_user = argv[3];
  if (argc >= 5) cfg.ami_secret = argv[4];

  auto getenv_s = [](const char* k) -> std::string {
    const char* v = std::getenv(k);
    return v ? std::string(v) : "";
  };

  // Allow env overrides (used by systemd EnvironmentFile)
  if (!getenv_s("AMI_HOST").empty()) cfg.ami_host = getenv_s("AMI_HOST");
  if (!getenv_s("AMI_PORT").empty()) cfg.ami_port = std::stoi(getenv_s("AMI_PORT"));
  if (!getenv_s("AMI_USER").empty()) cfg.ami_user = getenv_s("AMI_USER");
  if (!getenv_s("AMI_SECRET").empty()) cfg.ami_secret = getenv_s("AMI_SECRET");

  if (!getenv_s("SUPERVISOR_ENDPOINT").empty()) cfg.supervisor_endpoint = getenv_s("SUPERVISOR_ENDPOINT");
  if (!getenv_s("SUPERVISOR_CONTEXT").empty()) cfg.supervisor_context = getenv_s("SUPERVISOR_CONTEXT");
  if (!getenv_s("SUPERVISOR_PREFIX").empty()) cfg.supervisor_prefix = getenv_s("SUPERVISOR_PREFIX");
  if (!getenv_s("ORIGINATE_TIMEOUT_MS").empty()) cfg.originate_timeout_ms = std::stoi(getenv_s("ORIGINATE_TIMEOUT_MS"));

  return cfg;
}

int main(int argc, char** argv) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  AppConfig cfg = read_config_from_env_and_args(argc, argv);
  if (cfg.ami_user.empty() || cfg.ami_secret.empty()) {
    std::cerr << "Usage: " << argv[0] << " <host> <port> <user> <secret>\n"
              << "Or set AMI_HOST/AMI_PORT/AMI_USER/AMI_SECRET in environment.\n";
    return 1;
  }

  boost::asio::io_context io;
  AmiClient ami(io, cfg);

  std::deque<AmiMessage> q;
  std::mutex q_mu;

  StateStore st;
  st.log_line("Starting...");

  try {
    ami.connect();
    if (!ami.login()) {
      std::cerr << "AMI login failed.\n";
      return 1;
    }
    st.log_line("AMI login success");
  } catch (const std::exception& ex) {
    std::cerr << "Connection/login error: " << ex.what() << "\n";
    return 1;
  }

  ami.start_reader(&q, &q_mu);

  // Init TUI
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE); // non-blocking
  curs_set(0);

  bool show_logs = false;
  while (g_running.load()) {
    // Drain event queue
    {
      std::lock_guard<std::mutex> lk(q_mu);
      while (!q.empty()) {
        auto msg = std::move(q.front());
        q.pop_front();
        apply_event(st, cfg, msg);
      }
    }

    tui_draw(st, cfg);

    int ch = getch();
    if (ch == ERR) {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      continue;
    }

    if (ch == 'q' || ch == 'Q') {
      g_running.store(false);
      break;
    }

    if (ch == 'l' || ch == 'L') {
      // Temporarily turn off nodelay so log view can block
      nodelay(stdscr, FALSE);
      tui_show_logs(st);
      nodelay(stdscr, TRUE);
      continue;
    }

    if (ch == 'f' || ch == 'F') {
      // cycle filters
      std::string f = lower(st.filter);
      if (f == "all") st.filter = "inbound";
      else if (f == "inbound") st.filter = "outbound";
      else if (f == "outbound") st.filter = "internal";
      else st.filter = "all";
