#include "lob/matching_engine.hpp"

#include <algorithm>
#include <cassert>

namespace lob{

    MatchingEngine::MatchingEngine(std::string_view symbol, Price max_price):
    symbol_(symbol), 
    max_price_(max_price), //
    bid_levels_(std::make_unique<std::array<PriceLevel, kMaxPriceTicks>>()), //this will be allocated once
    ask_levels_(std::make_unique<std::array<PriceLevel, kMaxPriceTicks>>()), //can go further and touch all allocated pages, so that there are no page faults later
    pool_(std::make_unique<ObjectPool<Order, kOrderCapacity>>())
    {
        assert(max_price > 0 && static_cast<std::size_t>(max_price_) <= kMaxPriceTicks);
        id_to_index_.reserve(kOrderCapacity); 
    }

    std::size_t MatchingEngine::addLimitOrder(OrderId id, Side side, Price price, Quantity qty, std::span<Trade> trades)
    {
        assert(0<= price && price< max_price_);
        //match it with resting orders
        Quantity remaining = qty;
        std::size_t n_ordersFilled = match(id, side, price, remaining, /*is_market*/false, trades);
        if(remaining > 0)insertResting(id, side, price, remaining);
        return n_ordersFilled;
    }

    std::size_t MatchingEngine::addMarketOrder(OrderId id, Side side, Quantity qty, std::span<Trade> trades){
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
                if(level.empty())advanceBestAsk();   
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
                if(level.empty())advanceBestBid();
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
        PoolIndex idx = pool_->acquire();
        if(idx==kInvalidIndex) return ; //pool exhausted

        Order& o = (*pool_)[idx];
        o.id = id;
        o.price = price;
        o.quantity = qty;
        o.side = side;
        o.prev = kInvalidIndex;
        o.next = kInvalidIndex;

        id_to_index_[id] = idx;

        std::array<PriceLevel, kMaxPriceTicks>& levels = (side==Side::BUY)? *bid_levels_ : *ask_levels_;
        PriceLevel& level = levels[static_cast<std::size_t>(price)];
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

        if(side== Side::BUY){
            //check if best_bid_ can be increased
            if(best_bid_==kInvalidPrice || price > best_bid_) best_bid_ = price;
        }else {
            if(best_ask_==kInvalidPrice || best_ask_ > price) best_ask_ = price;
        }
    }
    void MatchingEngine::advanceBestAsk()
    {
        //this order was matched, next best_ask_ can be higher
        for(Price p = best_ask_+1; p <= max_price_; p++){
            if(!(*ask_levels_)[static_cast<std::size_t>(p)].empty()){
                best_ask_ = p;
                return ;
            }
            best_ask_ = kInvalidPrice;
        }
    }
    void MatchingEngine::advanceBestBid()
    {
        //this(best_bid_) entire level was matched, next best bid can be lower 
        for(Price p = best_bid_-1; p>=0; --p){
            //if that level is not empty
            if(!(*bid_levels_)[static_cast<std::size_t>(p)].empty()){
                best_bid_ = p;
                return;
            }
        }
        best_bid_ = kInvalidPrice;

    }

    bool MatchingEngine::cancelOrder(OrderId id)
    {
        //remove from PriceLevel and change best_ask_ or best_bid
        auto it = id_to_index_.find(id);
        if(it==id_to_index_.end()) return false;

        PoolIndex idx = it->second;
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
        auto it = id_to_index_.find(id);
        if(it==id_to_index_.end()) return std::nullopt;
        const Order& order = (*pool_)[it->second];
        return OrderView{order.id, order.side, order.price, order.quantity};
    }
    std::vector<OrderView> MatchingEngine::liveOrders() const{
        std::vector<OrderView>out;
        for( auto [id, idx] : id_to_index_){
            const Order& order = (*pool_)[idx];
            out.push_back(OrderView{id, order.side, order.price, order.quantity});
        }
        return out;
    }

}