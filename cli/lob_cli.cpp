#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/matching_engine.hpp"

namespace {

using namespace lob;

constexpr const char* kColorReset = "\033[0m";
constexpr const char* kColorRed = "\033[31m";     // asks
constexpr const char* kColorGreen = "\033[32m";   // bids
constexpr const char* kColorYellow = "\033[33m";  // fills
constexpr const char* kColorCyan = "\033[36m";    // spread

bool colorEnabled() {
  static const bool enabled = ::isatty(fileno(stdout)) != 0;
  return enabled;
}

std::string colorize(const std::string& text, const char* color) {
  if (!colorEnabled()) return text;
  return std::string(color) + text + kColorReset;
}

// 1 tick = $0.01, so a Price of 100'000 ticks means prices up to $999.99.
constexpr Price kMaxPrice = 100'000;
constexpr Price kTicksPerUnit = 100;
constexpr std::size_t kTradeBufferSize = 256;
constexpr std::size_t kDefaultBookDepth = 10;
constexpr std::size_t kDefaultTradeCount = 10;

const char* sideName(Side side) { return side == Side::BUY ? "BUY" : "SELL"; }

std::optional<long long> parseInt(const std::string& token) {
  try {
    std::size_t pos = 0;
    long long value = std::stoll(token, &pos);
    if (pos != token.size()) return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

bool allDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Parses a decimal dollar amount ("100", "100.25", ".5") into an exact tick
// count, using integer arithmetic throughout so there is no floating-point
// rounding. Rejects negative amounts and precision finer than the $0.01 tick.
std::optional<Price> parsePrice(const std::string& token) {
  if (token.empty() || token[0] == '-') return std::nullopt;

  auto dot = token.find('.');
  std::string int_part = (dot == std::string::npos) ? token : token.substr(0, dot);
  std::string frac_part = (dot == std::string::npos) ? "" : token.substr(dot + 1);

  if (int_part.empty() && frac_part.empty()) return std::nullopt;
  if (!int_part.empty() && !allDigits(int_part)) return std::nullopt;
  if (!frac_part.empty() && !allDigits(frac_part)) return std::nullopt;
  if (frac_part.size() > 2) return std::nullopt;  // finer than the $0.01 tick

  while (frac_part.size() < 2) frac_part.push_back('0');

  long long dollars = int_part.empty() ? 0 : std::stoll(int_part);
  long long cents = frac_part.empty() ? 0 : std::stoll(frac_part);
  long long ticks = dollars * kTicksPerUnit + cents;
  if (ticks < 0 || ticks >= kMaxPrice) return std::nullopt;
  return static_cast<Price>(ticks);
}

std::string formatPrice(Price ticks) {
  std::ostringstream oss;
  oss << (ticks / kTicksPerUnit) << '.' << std::setw(2) << std::setfill('0') << (ticks % kTicksPerUnit);
  return oss.str();
}

std::string formatPriceOrDash(Price price) { return price == kInvalidPrice ? "-" : formatPrice(price); }

void printBanner() {
  const std::string kTitle = "ORDER BOOK";
  constexpr std::size_t kPad = 3;
  const std::size_t inner_width = kTitle.size() + kPad * 2;

  std::string top = "╔";     // ╔
  std::string bottom = "╚";  // ╚
  for (std::size_t i = 0; i < inner_width; ++i) {
    top += "═";     // ═
    bottom += "═";  // ═
  }
  top += "╗";     // ╗
  bottom += "╝";  // ╝
  std::string mid = "║" + std::string(kPad, ' ') + kTitle + std::string(kPad, ' ') + "║";  // ║ ... ║

  std::cout << colorize(top + "\n" + mid + "\n" + bottom, kColorCyan) << "\n\n";
}

void printHelp() {
  std::cout << "Commands:\n"
               "  buy <price> <qty>        submit a limit buy order, e.g. buy 100.25 10\n"
               "  sell <price> <qty>       submit a limit sell order\n"
               "  buy market <qty>         submit a market buy order\n"
               "  sell market <qty>        submit a market sell order\n"
               "  cancel <id>              cancel a resting order\n"
               "  book [depth]             show the order book (default depth 10)\n"
               "  orders                   list live resting orders\n"
               "  trades [n]               show the last n trades (default 10)\n"
               "  quit | exit              exit\n"
               "Prices are dollars with cents (tick size $0.01), e.g. 100.25.\n";
}

class Cli {
 public:
  Cli() : engine_("CLI", kMaxPrice) {}

  void run() {
    printBanner();
    printHelp();
    std::string line;
    while (true) {
      std::cout << "\n> " << std::flush;
      if (!std::getline(std::cin, line)) break;

      std::istringstream iss(line);
      std::vector<std::string> tokens;
      std::string tok;
      while (iss >> tok) tokens.push_back(tok);
      if (tokens.empty()) continue;

      const std::string& cmd = tokens[0];
      if (cmd == "quit" || cmd == "exit") {
        break;
      } else if (cmd == "help") {
        printHelp();
      } else if (cmd == "buy") {
        handleOrder(tokens, Side::BUY);
      } else if (cmd == "sell") {
        handleOrder(tokens, Side::SELL);
      } else if (cmd == "cancel") {
        handleCancel(tokens);
      } else if (cmd == "book") {
        handleBook(tokens);
      } else if (cmd == "orders") {
        handleOrders();
      } else if (cmd == "trades") {
        handleTrades(tokens);
      } else {
        std::cout << "Unknown command '" << cmd << "'. Type 'help' for a list of commands.\n";
      }
    }
  }

 private:
  void handleOrder(const std::vector<std::string>& tokens, Side side) {
    if (tokens.size() >= 2 && tokens[1] == "market") {
      if (tokens.size() != 3) {
        std::cout << "Usage: " << tokens[0] << " market <qty>\n";
        return;
      }
      auto qty = parseInt(tokens[2]);
      if (!qty || *qty <= 0) {
        std::cout << "Invalid quantity.\n";
        return;
      }
      OrderId id = next_id_++;
      std::array<Trade, kTradeBufferSize> trades;
      std::size_t n = engine_.addMarketOrder(id, side, static_cast<Quantity>(*qty), trades);
      logTrades(trades, n);
      printOrderHeader(id, side, static_cast<Quantity>(*qty), /*price=*/std::nullopt);
      printFills(trades, n);
      Quantity filled = sumFilled(trades, n);
      if (filled < static_cast<Quantity>(*qty)) {
        std::cout << "  unfilled " << (static_cast<Quantity>(*qty) - filled)
                   << " discarded (market orders do not rest)\n";
      }
      return;
    }

    if (tokens.size() != 3) {
      std::cout << "Usage: " << tokens[0] << " <price> <qty>  (or '" << tokens[0] << " market <qty>')\n";
      return;
    }
    auto price = parsePrice(tokens[1]);
    auto qty = parseInt(tokens[2]);
    if (!price) {
      std::cout << "Invalid price: must be dollars.cents in [0, " << formatPrice(kMaxPrice - 1)
                 << "], to the nearest $0.01 tick, e.g. 100.25.\n";
      return;
    }
    if (!qty || *qty <= 0) {
      std::cout << "Invalid quantity.\n";
      return;
    }
    OrderId id = next_id_++;
    std::array<Trade, kTradeBufferSize> trades;
    std::size_t n = engine_.addLimitOrder(id, side, *price, static_cast<Quantity>(*qty), trades);
    logTrades(trades, n);
    printOrderHeader(id, side, static_cast<Quantity>(*qty), *price);
    printFills(trades, n);
    if (auto resting = engine_.getOrder(id)) {
      std::cout << "  resting " << resting->quantity << " @ " << formatPrice(resting->price) << "\n";
    }
  }

  void handleCancel(const std::vector<std::string>& tokens) {
    if (tokens.size() != 2) {
      std::cout << "Usage: cancel <id>\n";
      return;
    }
    auto id = parseInt(tokens[1]);
    if (!id || *id < 0) {
      std::cout << "Invalid order id.\n";
      return;
    }
    bool ok = engine_.cancelOrder(static_cast<OrderId>(*id));
    std::cout << (ok ? "Cancelled order #" + tokens[1] + "\n" : "No live order #" + tokens[1] + "\n");
  }

  void handleBook(const std::vector<std::string>& tokens) {
    std::size_t depth = kDefaultBookDepth;
    if (tokens.size() == 2) {
      auto d = parseInt(tokens[1]);
      if (!d || *d <= 0) {
        std::cout << "Invalid depth.\n";
        return;
      }
      depth = static_cast<std::size_t>(*d);
    } else if (tokens.size() > 2) {
      std::cout << "Usage: book [depth]\n";
      return;
    }

    auto asks = engine_.topAskLevels(depth);
    auto bids = engine_.topBidLevels(depth);

    std::cout << "ASKS (price  qty  orders)\n";
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
      std::ostringstream row;
      row << "  " << formatPrice(it->price) << "  " << it->total_qty << "  " << it->order_count;
      std::cout << colorize(row.str(), kColorRed) << "\n";
    }
    std::ostringstream spread;
    spread << "  ---- spread: " << formatPriceOrDash(engine_.bestBid()) << " / "
           << formatPriceOrDash(engine_.bestAsk()) << " ----";
    std::cout << colorize(spread.str(), kColorCyan) << "\n";
    std::cout << "BIDS (price  qty  orders)\n";
    for (const auto& lvl : bids) {
      std::ostringstream row;
      row << "  " << formatPrice(lvl.price) << "  " << lvl.total_qty << "  " << lvl.order_count;
      std::cout << colorize(row.str(), kColorGreen) << "\n";
    }
  }

  void handleOrders() {
    auto orders = engine_.liveOrders();
    if (orders.empty()) {
      std::cout << "No live orders.\n";
      return;
    }
    std::sort(orders.begin(), orders.end(), [](const OrderView& a, const OrderView& b) {
      if (a.side != b.side) return a.side < b.side;
      return a.price != b.price ? a.price < b.price : a.id < b.id;
    });
    std::cout << "id  side  price  qty\n";
    for (const auto& o : orders) {
      std::cout << "#" << o.id << "  " << sideName(o.side) << "  " << formatPrice(o.price) << "  " << o.quantity
                 << "\n";
    }
  }

  void handleTrades(const std::vector<std::string>& tokens) {
    std::size_t n = kDefaultTradeCount;
    if (tokens.size() == 2) {
      auto v = parseInt(tokens[1]);
      if (!v || *v <= 0) {
        std::cout << "Invalid count.\n";
        return;
      }
      n = static_cast<std::size_t>(*v);
    } else if (tokens.size() > 2) {
      std::cout << "Usage: trades [n]\n";
      return;
    }
    if (trade_log_.empty()) {
      std::cout << "No trades yet.\n";
      return;
    }
    std::size_t start = trade_log_.size() > n ? trade_log_.size() - n : 0;
    for (std::size_t i = start; i < trade_log_.size(); ++i) {
      const Trade& t = trade_log_[i];
      std::ostringstream row;
      row << "  taker #" << t.taker_id << "  maker #" << t.maker_id << "  " << t.quantity << " @ "
          << formatPrice(t.price);
      std::cout << colorize(row.str(), kColorYellow) << "\n";
    }
  }

  static void printOrderHeader(OrderId id, Side side, Quantity qty, std::optional<Price> price) {
    std::cout << "Order #" << id << ": " << sideName(side) << " " << qty;
    if (price) {
      std::cout << " @ " << formatPrice(*price) << "\n";
    } else {
      std::cout << " @ MARKET\n";
    }
  }

  static void printFills(const std::array<Trade, kTradeBufferSize>& trades, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      std::ostringstream row;
      row << "  filled " << trades[i].quantity << " @ " << formatPrice(trades[i].price) << " (maker #"
          << trades[i].maker_id << ")";
      std::cout << colorize(row.str(), kColorYellow) << "\n";
    }
  }

  static Quantity sumFilled(const std::array<Trade, kTradeBufferSize>& trades, std::size_t n) {
    Quantity total = 0;
    for (std::size_t i = 0; i < n; ++i) total += trades[i].quantity;
    return total;
  }

  void logTrades(const std::array<Trade, kTradeBufferSize>& trades, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) trade_log_.push_back(trades[i]);
  }

  MatchingEngine engine_;
  OrderId next_id_ = 1;
  std::vector<Trade> trade_log_;
};

}  // namespace

int main() {
  Cli cli;
  cli.run();
  return 0;
}
