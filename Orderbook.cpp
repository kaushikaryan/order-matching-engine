#include <iostream>
#include <map>
#include <set>
#include <list>
#include <numeric>
#include "Side.h"
#include "LevelInfo.h"
#include "Usings.h"
#include "OrderType.h"
#include "OrderBookLevelInfos.h"
#include "Order.h"
#include "OrderModify.h"
#include "TradeInfo.h"
#include "Trade.h"
#include "Orderbook.h"
#include <time.h>

bool Orderbook::CanMatch(Side side, Price price) const
{
    if (side == Side::Buy)
    {
        if (asks_.empty())
            return false;

        const auto& [bestAsk, _] = *asks_.begin();
        return price >= bestAsk;
    }
    if (bids_.empty())
        return false;

    const auto& [bestBid, _] = *bids_.begin();
    return price <= bestBid;
}

Trades Orderbook::AddOrder(OrderPointer order)
{
    if (orders_.contains(order->GetOrderId()))
        return {};

    if (order->GetOrderType() == OrderType::Market && !asks_.empty())
    {
        if (order->GetSide() == Side::Buy)
        {
            const auto& [worstAsk, _] = *asks_.begin();
            order->ToGoodTillCancel(worstAsk);
        }
        else if (order->GetSide() == Side::Sell && !bids_.empty())
        {
            const auto& [worstBid, _] = *bids_.begin();
            order->ToGoodTillCancel(worstBid);
        }
        else
            return {};
    }

    if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
        return {};

    OrderPointers::iterator iterator;

    if (order->GetSide() == Side::Buy)
    {
        auto& orders = bids_[order->GetPrice()];
        orders.push_back(order);
        iterator = std::next(orders.begin(), orders.size() - 1);
    }
    else
    {
        auto& orders = asks_[order->GetPrice()];
        orders.push_back(order);
        iterator = std::next(orders.begin(), orders.size() - 1);
    }

    orders_.insert({order->GetOrderId(), OrderEntry{order, iterator}});

    OnOrderAdded(order);

    return MatchOrders();
}

void Orderbook::CancelOrder(OrderId orderId)
{
    std::scoped_lock ordersLock{ordersMutex_};

    CancelOrderInternal(orderId);
}

Trades Orderbook::ModifyOrder(OrderModify order)
{
    if (!orders_.contains(order.GetOrderId()))
        return {};

    const auto& [existingOrder, _] = orders_.at(order.GetOrderId());
    CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrderPointer(existingOrder->GetOrderType()));
}

Trades Orderbook::MatchOrders()
{
    Trades trades;
    trades.reserve(orders_.size());

    while (true)
    {
        if (bids_.empty() || asks_.empty())
            break;

        auto& [bidPrice, bids] = *bids_.begin();
        auto& [askPrice, asks] = *asks_.begin();

        if (bidPrice < askPrice)
            break;

        while (!bids.empty() && !asks.empty())
        {
            auto bid = bids.front();
            auto ask = asks.front();

            Quantity quantity = std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

            if (bid->isFilled())
            {
                bids.pop_front();
                orders_.erase(bid->GetOrderId());
            }

            if (ask->isFilled())
            {
                asks.pop_front();
                orders_.erase(ask->GetOrderId());
            }

            if (bids.empty())
                bids_.erase(bidPrice);

            if (asks.empty())
                asks_.erase(askPrice);

            trades.emplace_back(
                TradeInfo{bid->GetOrderId(), bid->GetPrice(), quantity},
                TradeInfo{ask->GetOrderId(), ask->GetPrice(), quantity}
            );

            OnOrderMatched(bid->GetOrderId(), quantity, bid->isFilled());
            OnOrderMatched(ask->GetOrderId(), quantity, ask->isFilled());
        }
    }

    if (!bids_.empty())
    {
        auto& [_, bids] = *bids_.begin();
        auto& order = bids.front();
        if (order->GetOrderType() == OrderType::FillAndKill)
            CancelOrder(order->GetOrderId());
    }
    if (!asks_.empty())
    {
        auto& [_, asks] = *asks_.begin();
        auto& order = asks.front();
        if (order->GetOrderType() == OrderType::FillAndKill)
            CancelOrder(order->GetOrderId());
    }

    return trades;
}

OrderBookLevelInfos Orderbook::GetOrderInfos() const
{
    LevelInfos bidInfos, askInfos;
    bidInfos.reserve(orders_.size());
    askInfos.reserve(orders_.size());

    auto CreateLevelInfos = [](Price price, const OrderPointers& orders)
    {
        return LevelInfo{
            price, std::accumulate(orders.begin(), orders.end(), (Quantity)0,
                                   [](Quantity runningSum, const OrderPointer& order)
                                   {
                                       return runningSum + order->GetRemainingQuantity();
                                   })
        };
    };

    for (const auto& [price, orders] : bids_)
        bidInfos.push_back(CreateLevelInfos(price, orders));

    for (const auto& [price, orders] : asks_)
        askInfos.push_back(CreateLevelInfos(price, orders));

    return OrderBookLevelInfos{bidInfos, askInfos};
}

std::size_t Orderbook::Size() const { return orders_.size(); }

Orderbook::Orderbook() : ordersPruneThread_{[this] { PruneGoodForDayOrders(); }}
{
}

Orderbook::~Orderbook()
{
    shutdown_.store(true, std::memory_order_release);
    shutdownConditionVariable_.notify_one();
    ordersPruneThread_.join();
}

void Orderbook::PruneGoodForDayOrders()
{
    using namespace std::chrono;
    const auto end = hours(16);

    while (true)
    {
        const auto now = system_clock::now();
        const auto now_c = system_clock::to_time_t(now);
        std::tm now_parts;
        localtime_r(&now_c, &now_parts);

        if (now_parts.tm_hour >= end.count())
            now_parts.tm_mday += 1;

        now_parts.tm_hour = end.count();
        now_parts.tm_min = 0;
        now_parts.tm_sec = 0;

        auto next = system_clock::from_time_t(mktime(&now_parts));
        auto till = next - now + milliseconds(100);

        {
            std::unique_lock ordersLock{ordersMutex_};

            if (shutdown_.load(std::memory_order_acquire) || shutdownConditionVariable_.wait_for(ordersLock, till) ==
                std::cv_status::no_timeout)
                return;
        }

        OrderIds orderIds;

        {
            std::scoped_lock ordersLock{ordersMutex_};

            for (const auto& [_, entry] : orders_)
            {
                const auto& [order, _] = entry;

                if (entry.order_->GetOrderType() == OrderType::GoodForDay)
                    continue;

                orderIds.push_back(order->GetOrderId());
            }
        }

        CancelOrders(orderIds);
    }
}

void Orderbook::CancelOrders(OrderIds orderIds)
{
    std::scoped_lock ordersLock{ordersMutex_};

    for (const auto& orderId : orderIds)
        CancelOrderInternal(orderId);
}

void Orderbook::CancelOrderInternal(OrderId orderId)
{
    if (!orders_.contains(orderId))
        return;

    const auto [order, iterator] = orders_.at(orderId);
    orders_.erase(orderId);

    auto price = order->GetPrice();
    if (order->GetSide() == Side::Sell)
    {
        auto& orders = asks_.at(price);
        orders.erase(iterator);
        if (orders.empty())
            asks_.erase(price);
    }
    else
    {
        auto& orders = bids_.at(price);
        orders.erase(iterator);
        if (orders.empty())
            bids_.erase(price);
    }

    OnOrderCancelled(order);
}

void Orderbook::OnOrderAdded(OrderPointer order)
{
    UpdateLevelData(order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Add);
}

void Orderbook::OnOrderCancelled(OrderPointer order)
{
    UpdateLevelData(order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled)
{
    UpdateLevelData(price, quantity, isFullyFilled ? LevelData::Action::Remove : LevelData::Action::Match);
}

void Orderbook::UpdateLevelData(Price price, Quantity quantity, LevelData::Action action)
{
    auto& data = data_[price];

    data.count_ += action == LevelData::Action::Remove ? -1 : action == LevelData::Action::Add ? 1 : 0;

    if (action == LevelData::Action::Remove || action == LevelData::Action::Match)
    {
        data.quantity_ -= quantity;
    }
    else
    {
        data.quantity_ += quantity;
    }

    if (data.count_ == 0)
        data_.erase(price);
}

bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
    if (!CanMatch(side, price))
        return false;

    std::optional<Price> threshold;

    if (side == Side::Buy)
    {
        const auto [askPrice, _] = *asks_.begin();
        threshold = askPrice;
    }
    else
    {
        const auto [bidPrice, _] = *bids_.begin();
        threshold = bidPrice;
    }

    for (const auto& [levelPrice, levelData] : data_)
    {
        if (threshold.has_value() &&
            (side == Side::Buy && threshold.value() > levelPrice) ||
            (side == Side::Sell && threshold.value() < levelPrice))
            continue;

        if ((side == Side::Buy && levelPrice > price) ||
            (side == Side::Sell && levelPrice < price))
            continue;

        if (quantity <= levelData.quantity_)
            return true;

        quantity -= levelData.quantity_;
    }

    return false;
}


int main()
{
    Orderbook orderbook;
    constexpr OrderId orderId = 1;
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10));
    std::cout << orderbook.Size() << std::endl; // 1
    orderbook.CancelOrder(orderId);
    std::cout << orderbook.Size() << std::endl; // 0
    return 0;
}
