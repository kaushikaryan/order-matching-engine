#pragma once
#include <map>
#include <thread>
#include "Order.h"
#include "OrderBookLevelInfos.h"
#include "Trade.h"

class Orderbook
{
private:
    struct OrderEntry
    {
        OrderPointer order_{nullptr};
        OrderPointers::iterator location_;
    };

    struct LevelData
    {
        Quantity quantity_{};
        std::size_t count_{};

        enum class Action
        {
            Add,
            Remove,
            Match
        };
    };

    std::map<Price, OrderPointers, std::greater<>> bids_;
    std::map<Price, OrderPointers, std::less<>> asks_;
    std::unordered_map<OrderId, OrderEntry> orders_;
    std::unordered_map<Price, LevelData> data_;

    mutable std::mutex ordersMutex_;
    std::thread ordersPruneThread_;
    std::condition_variable shutdownConditionVariable_;
    std::atomic<bool> shutdown_{false};

    void PruneGoodForDayOrders();

    void CancelOrders(OrderIds orderIds);
    void CancelOrderInternal(OrderId orderID);

    void OnOrderCancelled(OrderPointer order);
    void OnOrderAdded(OrderPointer order);
    void OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled);
    void UpdateLevelData(Price price, Quantity quantity, LevelData::Action action);

    bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    bool CanMatch(Side side, Price price) const;
    Trades MatchOrders();

public:
    Trades AddOrder(OrderPointer order);
    void CancelOrder(OrderId orderId);
    Trades ModifyOrder(OrderModify order);
    Trades MatchOrders(OrderModify order);
    OrderBookLevelInfos GetOrderInfos() const;
    std::size_t Size() const;
    Orderbook();
    ~Orderbook();
};
