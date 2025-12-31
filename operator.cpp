#include <boost/asio.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>

using boost::asio::ip::tcp;

static inline std::string trim(std::string s) {
  auto notSpace = [](int ch){ return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

struct AmiMessage {
  std::unordered_map<std::string,std::string> kv;
};

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
  return s;
}

class AmiClient {
public:
  AmiClient(boost::asio::io_context& io, std::string host, int port)
    : io_(io), socket_(io), host_(std::move(host)), port_(port) {}

  void connect() {
    tcp::resolver resolver(io_);
    auto endpoints = resolver.resolve(host_, std::to_string(port_));
    boost::asio::connect(socket_, endpoints);
    // best-effort banner drain
    drain_some();
  }

  bool login(const std::string& user, const std::string& secret) {
    std::ostringstream oss;
    oss << "Action: Login\r\n"
        << "Username: " << user << "\r\n"
        << "Secret: " << secret << "\r\n"
        << "Events: on\r\n\r\n";
    write_raw(oss.str());
    auto msg = read_message_blocking();
    if (!msg) return false;
    return toLower(get(*msg,"Response")) == "success";
  }

  void logoff() { write_raw("Action: Logoff\r\n\r\n"); }

  // Fire-and-forget action; caller can read responses from event loop if desired.
  void send_action(const std::vector<std::pair<std::string,std::string>>& headers) {
    std::ostringstream oss;
    for (auto& kv : headers) {
      oss << kv.first << ": " << kv.second << "\r\n";
    }
    oss << "\r\n";
    write_raw(oss.str());
  }

  // Read next AMI message (blocking). Used by reader thread.
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
      std::string k = trim(line.substr(0,pos));
      std::string v = trim(line.substr(pos+1));
      msg.kv[k] = v;
    }
  }

  static std::string get(const AmiMessage& m, const std::string& k) {
    auto it = m.kv.find(k);
    return it==m.kv.end() ? "" : it->second;
  }

private:
  void write_raw(const std::string& s) {
    boost::asio::write(socket_, boost::asio::buffer(s));
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

  void drain_some() {
    boost::system::error_code ec;
    socket_.non_blocking(true, ec);
    if (ec) return;
    for (int i=0;i<5;i++) {
      char buf[1024];
      socket_.read_some(boost::asio::buffer(buf), ec);
      if (ec) break;
    }
    socket_.non_blocking(false, ec);
  }

  boost::asio::io_context& io_;
  tcp::socket socket_;
  std::string host_;
  int port_;
};

struct ChannelInfo {
  std::string channel;
  std::string uniqueid;
  std::string linkedid;
  std::string bridgeid;      // CoreShowChannel provides BridgeId
  std::string state;
  std::string context;
  std::string exten;
  std::string callerNum, callerName;
  std::string connNum, connName;
  int durationSec = 0;       // channel existence duration
  std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
};

struct BridgeInfo {
  std::string bridgeid;
  std::unordered_set<std::string> members; // channel names
  std::chrono::steady_clock::time_point firstSeen = std::chrono::steady_clock::now();
};

struct ClassificationRules {
  // Very practical heuristics for “inbound vs outbound”
  // 1) contexts that should be treated as inbound
  // 2) channel name prefixes that indicate trunks/outbound legs (e.g., "PJSIP/mytrunk-")
  std::vector<std::string> inboundContexts = {"from-external","from-trunk","inbound"};
  std::vector<std::string> outboundChannelPrefixes = {"PJSIP/outbound","PJSIP/mytrunk","PJSIP/siptrunk"};
};

static bool startsWithAny(const std::string& s, const std::vector<std::string>& prefixes) {
  for (const auto& p : prefixes) {
    if (s.rfind(p, 0) == 0) return true;
  }
  return false;
}

static bool equalsAnyCI(const std::string& s, const std::vector<std::string>& vals) {
  auto sl = toLower(s);
  for (const auto& v : vals) {
    if (sl == toLower(v)) return true;
  }
  return false;
}

static std::string classifyBridge(const BridgeInfo& b,
                                  const std::unordered_map<std::string,ChannelInfo>& chans,
                                  const ClassificationRules& rules) {
  // If any member context matches inboundContexts -> inbound
  // Else if any member channel name matches outbound prefixes -> outbound
  bool inbound = false;
  bool outbound = false;

  for (const auto& chName : b.members) {
    auto it = chans.find(chName);
    if (it == chans.end()) continue;
    const auto& c = it->second;
    if (equalsAnyCI(c.context, rules.inboundContexts)) inbound = true;
    if (startsWithAny(c.channel, rules.outboundChannelPrefixes)) outbound = true;
  }

  if (inbound && outbound) return "mixed";
  if (inbound) return "inbound";
  if (outbound) return "outbound";
  return "unknown";
}

static int safeToInt(const std::string& s) {
  try { return std::stoi(s); } catch (...) { return 0; }
}

static std::string readLine(const std::string& prompt) {
  std::cout << prompt;
  std::string s;
  std::getline(std::cin, s);
  return trim(s);
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 5038;

  if (argc >= 3) {
    host = argv[1];
    port = std::stoi(argv[2]);
  }

  std::string user = (argc >= 4) ? argv[3] : readLine("AMI Username: ");
  std::string secret = (argc >= 5) ? argv[4] : readLine("AMI Secret: ");

  boost::asio::io_context io;
  AmiClient ami(io, host, port);

  try {
    ami.connect();
    if (!ami.login(user, secret)) {
      std::cerr << "AMI login failed.\n";
      return 1;
    }
  } catch (const std::exception& ex) {
    std::cerr << "Connect/login error: " << ex.what() << "\n";
    return 1;
  }

  std::mutex mtx;
  std::unordered_map<std::string, ChannelInfo> channels; // key=Channel
  std::unordered_map<std::string, BridgeInfo> bridges;   // key=BridgeId
  ClassificationRules rules;
  std::atomic<bool> running{true};

  auto upsertChannel = [&](const AmiMessage& msg) {
    ChannelInfo c;
    c.channel   = AmiClient::get(msg,"Channel");
    if (c.channel.empty()) return;

    c.uniqueid  = AmiClient::get(msg,"Uniqueid");
    if (c.uniqueid.empty()) c.uniqueid = AmiClient::get(msg,"UniqueID");

    c.linkedid  = AmiClient::get(msg,"Linkedid");
    c.bridgeid  = AmiClient::get(msg,"BridgeId");
    c.state     = AmiClient::get(msg,"ChannelStateDesc");
    c.context   = AmiClient::get(msg,"Context");
    c.exten     = AmiClient::get(msg,"Exten");
    c.callerNum = AmiClient::get(msg,"CallerIDNum");
    c.callerName= AmiClient::get(msg,"CallerIDName");
    c.connNum   = AmiClient::get(msg,"ConnectedLineNum");
    c.connName  = AmiClient::get(msg,"ConnectedLineName");

    auto dur = AmiClient::get(msg,"Duration");
    if (!dur.empty()) c.durationSec = safeToInt(dur);

    c.lastSeen = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(mtx);
    auto& ref = channels[c.channel];
    // merge, preserving anything already known
    if (!c.uniqueid.empty()) ref.uniqueid = c.uniqueid;
    if (!c.linkedid.empty()) ref.linkedid = c.linkedid;
    if (!c.bridgeid.empty()) ref.bridgeid = c.bridgeid;
    if (!c.state.empty()) ref.state = c.state;
    if (!c.context.empty()) ref.context = c.context;
    if (!c.exten.empty()) ref.exten = c.exten;
    if (!c.callerNum.empty()) ref.callerNum = c.callerNum;
    if (!c.callerName.empty()) ref.callerName = c.callerName;
    if (!c.connNum.empty()) ref.connNum = c.connNum;
    if (!c.connName.empty()) ref.connName = c.connName;
    if (c.durationSec > 0) ref.durationSec = c.durationSec;
    ref.channel = c.channel;
    ref.lastSeen = c.lastSeen;

    // Update bridge membership if we have a bridge ID
    if (!ref.bridgeid.empty()) {
      auto& b = bridges[ref.bridgeid];
      b.bridgeid = ref.bridgeid;
      b.members.insert(ref.channel);
    }
  };

  auto removeChannel = [&](const std::string& ch) {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = channels.find(ch);
    if (it != channels.end()) {
      auto bid = it->second.bridgeid;
      channels.erase(it);
      if (!bid.empty()) {
        auto bit = bridges.find(bid);
        if (bit != bridges.end()) {
          bit->second.members.erase(ch);
          if (bit->second.members.empty()) bridges.erase(bit);
        }
      }
    }
  };

  auto bridgeEnter = [&](const AmiMessage& msg) {
    std::string bid = AmiClient::get(msg,"BridgeUniqueid");
    if (bid.empty()) bid = AmiClient::get(msg,"BridgeId");
    std::string ch = AmiClient::get(msg,"Channel");
    if (bid.empty() || ch.empty()) return;

    std::lock_guard<std::mutex> lk(mtx);
    auto& b = bridges[bid];
    b.bridgeid = bid;
    b.members.insert(ch);

    auto& c = channels[ch];
    c.channel = ch;
    c.bridgeid = bid;
    c.callerNum = AmiClient::get(msg,"CallerIDNum");
    c.callerName = AmiClient::get(msg,"CallerIDName");
    c.connNum = AmiClient::get(msg,"ConnectedLineNum");
    c.connName = AmiClient::get(msg,"ConnectedLineName");
    c.context = AmiClient::get(msg,"Context");
    c.state = AmiClient::get(msg,"ChannelStateDesc");
    c.lastSeen = std::chrono::steady_clock::now();
  };

  auto bridgeLeave = [&](const AmiMessage& msg) {
    std::string bid = AmiClient::get(msg,"BridgeUniqueid");
    if (bid.empty()) bid = AmiClient::get(msg,"BridgeId");
    std::string ch = AmiClient::get(msg,"Channel");
    if (bid.empty() || ch.empty()) return;

    std::lock_guard<std::mutex> lk(mtx);
    auto bit = bridges.find(bid);
    if (bit != bridges.end()) {
      bit->second.members.erase(ch);
      if (bit->second.members.empty()) bridges.erase(bit);
    }
    auto cit = channels.find(ch);
    if (cit != channels.end()) cit->second.bridgeid.clear();
  };

  // Reader thread: consumes AMI events and responses
  std::thread reader([&](){
    while (running.load()) {
      try {
        auto msgOpt = ami.read_message_blocking();
        if (!msgOpt) continue;
        auto& msg = *msgOpt;

        const std::string event = AmiClient::get(msg,"Event");
        const std::string response = AmiClient::get(msg,"Response");

        if (!event.empty()) {
          if (event == "CoreShowChannel") {
            upsertChannel(msg);
          } else if (event == "Newchannel" || event == "Newstate" || event == "Rename") {
            upsertChannel(msg);
          } else if (event == "BridgeEnter") {
            bridgeEnter(msg);
          } else if (event == "BridgeLeave") {
            bridgeLeave(msg);
          } else if (event == "Hangup") {
            std::string ch = AmiClient::get(msg,"Channel");
            if (!ch.empty()) removeChannel(ch);
          }
        } else if (!response.empty()) {
          // You can optionally log action responses here if you start using ActionID tracking.
        }
      } catch (...) {
        // If the socket dies, stop cleanly.
        running.store(false);
      }
    }
  });

  // Initial sync: request current channels (CoreShowChannels triggers CoreShowChannel events) :contentReference[oaicite:7]{index=7}
  ami.send_action({{"Action","CoreShowChannels"}});

  auto printBridges = [&](){
    std::lock_guard<std::mutex> lk(mtx);
    std::vector<std::string> keys;
    keys.reserve(bridges.size());
    for (auto& kv : bridges) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    std::cout << "\nActive Calls (Bridges): " << keys.size() << "\n";
    std::cout << "-----------------------------------------------------------------------------\n";
    std::cout << "Idx | Type     | BridgeId                         | Members | MaxLegDuration\n";
    std::cout << "-----------------------------------------------------------------------------\n";
    int idx = 1;
    for (auto& bid : keys) {
      const auto& b = bridges.at(bid);
      int maxDur = 0;
      for (auto& ch : b.members) {
        auto it = channels.find(ch);
        if (it != channels.end()) maxDur = std::max(maxDur, it->second.durationSec);
      }
      std::string typ = classifyBridge(b, channels, rules);

      std::string shortId = bid;
      if (shortId.size() > 32) shortId = shortId.substr(0, 29) + "...";

      std::cout << idx++ << "   | "
                << typ << std::string(std::max(1, 8 - (int)typ.size()), ' ')
                << " | " << shortId << std::string(std::max(1, 32 - (int)shortId.size()), ' ')
                << " | " << b.members.size()
                << "       | " << maxDur << "s\n";
    }
    std::cout << "-----------------------------------------------------------------------------\n";
  };

  auto printBridgeDetails = [&](const std::string& bridgeId){
    std::lock_guard<std::mutex> lk(mtx);
    auto bit = bridges.find(bridgeId);
    if (bit == bridges.end()) {
      std::cout << "Bridge not found.\n";
      return;
    }
    const auto& b = bit->second;
    std::cout << "\nBridge: " << bridgeId << "\nMembers:\n";
    int i=1;
    for (const auto& ch : b.members) {
      auto it = channels.find(ch);
      if (it == channels.end()) {
        std::cout << "  " << (i++) << ") " << ch << " (no details)\n";
        continue;
      }
      const auto& c = it->second;
      std::ostringstream a, z;
      a << (c.callerName.empty() ? "" : c.callerName + " ")
        << "<" << (c.callerNum.empty() ? "unknown" : c.callerNum) << ">";
      z << (c.connName.empty() ? "" : c.connName + " ")
        << "<" << (c.connNum.empty() ? "unknown" : c.connNum) << ">";
      std::cout << "  " << (i++) << ") " << c.channel
                << " | " << c.durationSec << "s"
                << " | " << c.state
                << " | " << a.str() << " -> " << z.str()
                << " | ctx=" << c.context
                << "\n";
    }
  };

  auto hangupChannel = [&](const std::string& ch){
    ami.send_action({{"Action","Hangup"},{"Channel",ch}});
  };

  auto kickFromBridge = [&](const std::string& bridgeId, const std::string& ch){
    // BridgeKick removes channel from bridge :contentReference[oaicite:8]{index=8}
    ami.send_action({{"Action","BridgeKick"},{"BridgeUniqueid",bridgeId},{"Channel",ch}});
  };

  auto destroyBridge = [&](const std::string& bridgeId){
    // BridgeDestroy destroys the bridge :contentReference[oaicite:9]{index=9}
    ami.send_action({{"Action","BridgeDestroy"},{"BridgeUniqueid",bridgeId}});
  };

  auto hangupAll = [&](){
    std::lock_guard<std::mutex> lk(mtx);
    for (auto& kv : channels) {
      ami.send_action({{"Action","Hangup"},{"Channel",kv.first}});
    }
  };

  auto configureRules = [&](){
    std::cout << "\nCurrent inbound contexts:\n";
    for (auto& c : rules.inboundContexts) std::cout << "  - " << c << "\n";
    std::cout << "Current outbound channel prefixes:\n";
    for (auto& p : rules.outboundChannelPrefixes) std::cout << "  - " << p << "\n";

    std::cout << "\nEnter comma-separated inbound contexts (blank to keep): ";
    std::string in = trim(readLine(""));
    if (!in.empty()) {
      rules.inboundContexts.clear();
      std::stringstream ss(in);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) rules.inboundContexts.push_back(tok);
      }
    }

    std::cout << "Enter comma-separated outbound channel prefixes (blank to keep): ";
    std::string out = trim(readLine(""));
    if (!out.empty()) {
      rules.outboundChannelPrefixes.clear();
      std::stringstream ss(out);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) rules.outboundChannelPrefixes.push_back(tok);
      }
    }
    std::cout << "Rules updated.\n";
  };

  while (running.load()) {
    std::cout << "\n=== Asterisk 20 AMI Call Control Console (Bridge-aware) ===\n"
              << "1) List active calls (bridges)\n"
              << "2) Show call (bridge) details\n"
              << "3) Hang up a channel\n"
              << "4) Kick a channel from a bridge\n"
              << "5) Destroy a bridge\n"
              << "6) Hang up ALL channels\n"
              << "7) Configure inbound/outbound classification rules\n"
              << "8) Refresh snapshot (CoreShowChannels)\n"
              << "9) Exit\n";

    std::string choice = readLine("Select an option: ");

    if (choice == "1") {
      printBridges();
    } else if (choice == "2") {
      std::string bid = readLine("Enter BridgeId: ");
      printBridgeDetails(bid);
    } else if (choice == "3") {
      std::string ch = readLine("Enter Channel name to hang up: ");
      if (!ch.empty()) hangupChannel(ch);
    } else if (choice == "4") {
      std::string bid = readLine("Enter BridgeId: ");
      std::string ch  = readLine("Enter Channel to kick: ");
      if (!bid.empty() && !ch.empty()) kickFromBridge(bid, ch);
    } else if (choice == "5") {
      std::string bid = readLine("Enter BridgeId to destroy: ");
      if (!bid.empty()) destroyBridge(bid);
    } else if (choice == "6") {
      hangupAll();
      std::cout << "Hangup ALL sent.\n";
    } else if (choice == "7") {
      configureRules();
    } else if (choice == "8") {
      ami.send_action({{"Action","CoreShowChannels"}});
      std::cout << "Refresh requested.\n";
    } else if (choice == "9") {
      running.store(false);
      break;
    } else {
      std::cout << "Unknown option.\n";
    }
  }

  try {
    ami.logoff();
  } catch (...) {}

  if (reader.joinable()) reader.join();
  return 0;
}
