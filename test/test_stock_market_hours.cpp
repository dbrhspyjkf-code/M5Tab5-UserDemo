#include "app/apps/app_stocks/stock_market_hours.h"
#include <cassert>
#include <iostream>

int main()
{
    using stocks_refresh::isAshareTradingTime;

    assert(!isAshareTradingTime(0));           // 未校时
    assert(!isAshareTradingTime(1782091799));  // 周一 09:29:59 CST
    assert( isAshareTradingTime(1782091800));  // 周一 09:30:00 CST
    assert( isAshareTradingTime(1782098999));  // 周一 11:29:59 CST
    assert(!isAshareTradingTime(1782099000));  // 周一 11:30:00 CST
    assert( isAshareTradingTime(1782104400));  // 周一 13:00:00 CST
    assert( isAshareTradingTime(1782111599));  // 周一 14:59:59 CST
    assert(!isAshareTradingTime(1782111600));  // 周一 15:00:00 CST
    assert(!isAshareTradingTime(1782007200));  // 周日 10:00:00 CST

    std::cout << "stock market hours tests passed\n";
    return 0;
}
