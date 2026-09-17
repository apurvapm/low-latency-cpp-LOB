#include "lob/matching_engine.hpp"

#include <algorithm>
#include <cassert>

namespace lob{

    MatchingEngine::MatchingEngine(std::string_view symbol, Price max_price, std::size_t order_capacity):
    max_price_(max_price), //
    bid_levels_(std::make_unique<std::array<PriceLevel, kMaxPriceTicks>>()), //this will be allocated once
    ask_levels_(std::make_unique<std::array<PriceLevel, kMaxPriceTicks>>()), //can go further and touch all allocated pages, so that there are no page faults later
    bid_bitmap_(std::make_unique<LevelBitmap>()),
    ask_bitmap_(std::make_unique<LevelBitmap>()),
    id_to_index_(order_capacity),
    pool_(std::make_unique<ObjectPool<Order>>(order_capacity))
    {
        assert(max_price > 0 && static_cast<std::size_t>(max_price_) <= kMaxPriceTicks);
        std::size_t n = std::min(symbol.size(), symbol_.size());
        std::copy_n(symbol.begin(), n, symbol_.begin());
    }

    std::size_t MatchingEngine::addLimitOrder(OrderId id, Side side, Price price, Quantity qty, std::span<Trade> trades)
    {
        if(price < 0 || price >= max_price_ || qty == 0){
            ++rejected_count_; //P3: an invalid order is rejected, not asserted away
            return 0;
        }
        //match it with resting orders
        Quantity remaining = qty;
        std::size_t n_ordersFilled = match(id, side, price, remaining, /*is_market*/false, trades);
        if(remaining > 0)insertResting(id, side, price, remaining);
        return n_ordersFilled;
    }

    std::size_t MatchingEngine::addMarketOrder(OrderId id, Side side, Quantity qty, std::span<Trade> trades){
        if(qty == 0){
            ++rejected_count_;
            return 0;
        }
        Quantity remaining = qty;
        return match(id, side, /*limit_price*/0,remaining, /*is_market*/true, trades);
    }

    std::size_t MatchingEngine::match(OrderId taker_id, Side side, Price limit_price, Quantity& qty, bool is_market, std::span<Trade> trades)
    {
        std::size_t n_ordersFilled=0;
        if(side==Side::BUY){
            //can go accross multiple levels
            while(qty>0 && best_ask_!=kInvalidPrice && (is_market || best_ask_<= limit_price)){
                PriceLevel& level = (*ask_levels_)[static_cast<std::size_t>(best_ask_)];
                //in the same level
                while(qty>0 && level.head!=kInvalidIndex){
                    Order& maker = (*pool_)[level.head];
                    Quantity qty_traded = std::min(qty, maker.quantity);
                    if(n_ordersFilled < trades.size()) trades[n_ordersFilled++] = Trade{ maker.id,taker_id, best_ask_, qty_traded};
                    else ++truncated_trade_count_; //P5: this fill still executes but isn't recorded
                    qty-= qty_traded;
                    maker.quantity -= qty_traded;
                    level.total_quantity-= qty_traded;
                    if(maker.quantity==0){
                        // PoolIndex filledOrder = maker.id;
                        PoolIndex filledOrderIdx = level.head; 
                        OrderId maker_id = maker.id;
                        unlink(level, filledOrderIdx);
                        id_to_index_.erase(maker_id);
                        pool_->release(filledOrderIdx);
                    }
                }
                if(level.empty()){
                    ask_bitmap_->clear(best_ask_);
                    advanceBestAsk();
                }
            }
        }
        else {
            //sell order
            //across levels
            while(qty>0 && best_bid_!=kInvalidPrice &&(is_market || best_bid_>=limit_price)){

                PriceLevel& level = (*bid_levels_)[static_cast<std::size_t>(best_bid_)];
                while(qty>0 && level.head!= kInvalidIndex){
                    Order& maker = (*pool_)[level.head];
                    Quantity qty_traded = std::min(maker.quantity, qty);
                    if(n_ordersFilled < trades.size())trades[n_ordersFilled++] = Trade{ maker.id, taker_id,best_bid_, qty_traded};
                    else ++truncated_trade_count_;
                    maker.quantity-= qty_traded;
                    level.total_quantity -= qty_traded;
                    qty-= qty_traded;

                    if(maker.quantity ==0){
                        PoolIndex filledOrderIdx = level.head;
                        OrderId maker_id = maker.id;
                        unlink(level , filledOrderIdx);
                        pool_->release(filledOrderIdx);
                        id_to_index_.erase(maker_id);
                    }
                }
                if(level.empty()){
                    bid_bitmap_->clear(best_bid_);
                    advanceBestBid();
                }
            }
        }
        return n_ordersFilled;
    }


    void MatchingEngine::unlink(PriceLevel& level, PoolIndex idx)
    {
        //assume that idx is valid??
        Order& o = (*pool_)[idx];
        if(o.prev!=kInvalidIndex){
            (*pool_)[o.prev].next = o.next;
        }
        else{
            //there was no prev , the head should become the next
            level.head = o.next;
        }
        if(o.next!=kInvalidIndex){
            (*pool_)[o.next].prev = o.prev;
        }
        else {
            level.tail = o.prev;
        }
        level.order_count-=1;
    }
    void MatchingEngine::insertResting(OrderId id, Side side, Price price, Quantity qty){
        if(id_to_index_.find(id) != kInvalidIndex){
            ++duplicate_id_count_; //P10: a duplicate id must not orphan the first order
            return;
        }
        PoolIndex idx = pool_->acquire();
        if(idx==kInvalidIndex){
            ++dropped_count_; //P4: the resting remainder is dropped, not silently lost
            return;
        }

        Order& o = (*pool_)[idx];
        o.id = id;
        o.price = price;
        o.quantity = qty;
        o.side = side;
        o.prev = kInvalidIndex;
        o.next = kInvalidIndex;

        id_to_index_.insert(id, idx);

        std::array<PriceLevel, kMaxPriceTicks>& levels = (side==Side::BUY)? *bid_levels_ : *ask_levels_;
        PriceLevel& level = levels[static_cast<std::size_t>(price)];
        bool wasEmpty = level.empty();
        if(level.tail == kInvalidIndex){
            level.head = idx;
            level.tail = idx;
        }
        else {
            (*pool_)[level.tail].next = idx;
            o.prev = level.tail;
            level.tail = idx;
        }
        level.total_quantity += qty;
        level.order_count += 1;

        if(wasEmpty){
            (side==Side::BUY ? *bid_bitmap_ : *ask_bitmap_).set(price);
        }

        if(side== Side::BUY){
            //check if best_bid_ can be increased
            if(best_bid_==kInvalidPrice || price > best_bid_) best_bid_ = price;
        }else {
            if(best_ask_==kInvalidPrice || best_ask_ > price) best_ask_ = price;
        }
    }
    void MatchingEngine::advanceBestAsk()
    {
        //this level was emptied; the next best ask is the next set bit above it,
        //found in O(1) via the bitmap instead of an O(gap) tick-by-tick scan (P2)
        Price next = ask_bitmap_->nextSetAtOrAbove(best_ask_ + 1);
        best_ask_ = (next != kInvalidPrice && next < max_price_) ? next : kInvalidPrice;
    }
    void MatchingEngine::advanceBestBid()
    {
        //this level was emptied; the next best bid is the next set bit below it
        best_bid_ = bid_bitmap_->prevSetAtOrBelow(best_bid_ - 1);
    }

    bool MatchingEngine::cancelOrder(OrderId id)
    {
        //remove from PriceLevel and change best_ask_ or best_bid
        PoolIndex idx = id_to_index_.find(id);
        if(idx==kInvalidIndex) return false;

        Order& o = (*pool_)[idx];
        Price price = o.price;
        Quantity qty = o.quantity;
        Side side = o.side;

        std::array<PriceLevel, kMaxPriceTicks>& levels = (side ==Side::BUY)? *bid_levels_ : *ask_levels_;
        PriceLevel& level = levels[static_cast<std::size_t>(price)];
        level.total_quantity -= qty;
        unlink(level, idx);

        id_to_index_.erase(id);
        pool_->release(idx);

        if(level.empty()){
            (side==Side::BUY ? *bid_bitmap_ : *ask_bitmap_).clear(price);
            if(side==Side::BUY && best_bid_ == price)advanceBestBid();
            else if(side == Side::SELL && best_ask_==price)advanceBestAsk();
        }
        //successfully cancelled
        return true;
    }

    std::vector<LevelView> MatchingEngine::topBidLevels(std::size_t depth) const{
        std::vector<LevelView>out;
        if(best_bid_==kInvalidPrice) return out;
        for(Price p = best_bid_; p >=0 && out.size()< depth; p--){
            const PriceLevel& level = (*bid_levels_)[static_cast<std::size_t>(p)];
            if(!level.empty())out.push_back(LevelView{p, level.total_quantity, level.order_count});
        }
        return out;
    }
    std::vector<LevelView> MatchingEngine::topAskLevels(std::size_t depth) const{
        std::vector<LevelView>out;
        if(best_ask_==kInvalidPrice) return out;
        for(Price p = best_ask_; p < max_price_ && out.size()< depth; p++){
            const PriceLevel& level = (*ask_levels_)[static_cast<std::size_t>(p)];
            if(!level.empty())out.push_back(LevelView{p, level.total_quantity, level.order_count});
        }
        return out;
    }
    
    std::optional<OrderView> MatchingEngine::getOrder(OrderId id) const{
        PoolIndex idx = id_to_index_.find(id);
        if(idx==kInvalidIndex) return std::nullopt;
        const Order& order = (*pool_)[idx];
        return OrderView{order.id, order.side, order.price, order.quantity};
    }
    std::vector<OrderView> MatchingEngine::liveOrders() const{
        std::vector<OrderView>out;
        id_to_index_.forEach([&](OrderId id, PoolIndex idx){
            const Order& order = (*pool_)[idx];
            out.push_back(OrderView{id, order.side, order.price, order.quantity});
        });
        return out;
    }

}