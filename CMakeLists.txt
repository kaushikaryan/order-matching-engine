cmake_minimum_required(VERSION 3.30)
project(orderbook)

set(CMAKE_CXX_STANDARD 20)

add_executable(orderbook orderbook.cpp
        Side.h
        LevelInfo.h
        Usings.h
        OrderType.h
        OrderBookLevelInfos.h
        Order.h
        OrderModify.h
        TradeInfo.h
        Trade.h
        Orderbook.h
        Constants.h)
