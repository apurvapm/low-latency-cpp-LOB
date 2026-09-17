#pragma once

#include <array>
#include <cstddef> //size_t, ptrdiff
#include <cstdint>
#include <memory> //ptrs
#include <string_view>
#include <span> //for trades
#include <optional> //for OrderView
#include <vector> //used only for OrderView, LevelView which are used by the cli

#include "lob/types.hpp"
#include "lob/order.hpp"
#include "lob/price_level.hpp"
#include "lob/object_pool.hpp"
#include "lob/flat_id_map.hpp"
#include "lob/level_bitmap.hpp"

namespace lob{
    inline constexpr std::size_t kOrderCapacity = 1u<<20; // ~1M orders

//these two needed for the cli 
    struct LevelView{
        Price price;
        Quantity total_qty;
        std::uint32_t order_count;
    };

    //resting orders are limit orders, maket orders that do not get fuolfilled are discarded
    struct OrderView{
        OrderId id;
        Side side;
        Price price;
        Quantity quantity;
    };
//these two needed for the cli
//end

    class MatchingEngine{
        public : 
        MatchingEngine(std::string_view symbol, Price max_price); 
        
        std::size_t addLimitOrder(OrderId id, Side side, Price price, Quantity qty, std::span<Trade> trades);
        std::size_t addMarketOrder(OrderId id, Side side, Quantity qty, std::span<Trade> trades);

        bool cancelOrder(OrderId id);

        Price bestBid() const noexcept{return best_bid_;}
        Price bestAsk() const noexcept{return best_ask_;}

        //non-allocating single level read,
        //topBidLevels/topAskLevels return vectors
        //which the MD path cannot afford per message
        //caller must ensure 0<= oprice < max_price
        [[nodiscard]] LevelView levelAt(Side side, Price price) const noexcept{
            const PriceLevel& level =
                (side == Side::BUY ? *bid_levels_ : *ask_levels_)
                    [static_cast<std::size_t>(price)];
            return LevelView{price, level.total_quantity, level.order_count};
        }

        std::string_view symbol()const noexcept{return symbol_;}
        std::size_t restingOrderCount() const noexcept{return id_to_index_.size();}
        // Count of addLimitOrder/addMarketOrder-driven resting inserts rejected
        // because the id was already live (P10): the order is dropped, not
        // merged or overwritten.
        std::uint64_t duplicateIdCount() const noexcept{return duplicate_id_count_;}

        //for cli display, these use vector
        [[nodiscard]]std::vector<LevelView> topBidLevels(std::size_t depth) const;
        [[nodiscard]]std::vector<LevelView> topAskLevels(std::size_t depth) const ;
        [[nodiscard]]std::optional<OrderView> getOrder(OrderId id) const;
        [[nodiscard]]std::vector<OrderView> liveOrders() const;
        //for cli display end
        private : 
        std::size_t match(OrderId taker_id, Side side, Price limit_price, Quantity& qty, bool isMarkey, std::span<Trade> trades);
        void unlink(PriceLevel& level, PoolIndex idx);
        void insertResting(OrderId id, Side side, Price price, Quantity qty);
        void advanceBestAsk();
        void advanceBestBid();

        std::string_view symbol_;
        Price max_price_;
        Price best_bid_ = kInvalidPrice;
        Price best_ask_ = kInvalidPrice;

        std::unique_ptr<std::array<PriceLevel, kMaxPriceTicks>> bid_levels_; //dont want the matching engine to own the enormous array directly, also might get stack overflow
        std::unique_ptr<std::array<PriceLevel, kMaxPriceTicks>> ask_levels_;
        std::unique_ptr<LevelBitmap> bid_bitmap_; //bit set <=> the level at that tick is non-empty
        std::unique_ptr<LevelBitmap> ask_bitmap_;
        FlatIdMap id_to_index_; //preallocated open-addressing map, allocates once at construction, never on the hot path
        std::unique_ptr<ObjectPool<Order, kOrderCapacity>> pool_;
        std::uint64_t duplicate_id_count_ = 0;
    };
}