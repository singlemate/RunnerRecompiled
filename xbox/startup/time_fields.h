#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>

// Xbox TIME_FIELDS order; UTC conversion only. Weekday is Sunday=0.
struct MeTimeFields {
    using Fields=std::array<uint16_t,8>;
    static constexpr uint64_t day_ticks=864000000000ull;
    static constexpr auto epoch=std::chrono::sys_days{std::chrono::year{1601}/1/1};
    static bool decode(uint64_t ticks,Fields& fields){
        if(ticks>uint64_t(INT64_MAX))return false;
        using namespace std::chrono;
        const auto date=epoch+days{int64_t(ticks/day_ticks)};
        const year_month_day ymd{date};
        const auto remainder=ticks%day_ticks;
        fields={uint16_t(int(ymd.year())),uint16_t(unsigned(ymd.month())),uint16_t(unsigned(ymd.day())),
            uint16_t(remainder/36000000000ull),uint16_t(remainder/600000000ull%60),
            uint16_t(remainder/10000000ull%60),uint16_t(remainder/10000ull%1000),uint16_t(weekday{date}.c_encoding())};
        return true;
    }
    static bool encode(const Fields& fields,uint64_t& ticks){
        using namespace std::chrono;
        if(fields[0]<1601 || fields[0]>32767 || fields[3]>23 || fields[4]>59 || fields[5]>59 || fields[6]>999)return false;
        // Check before constructing narrow chrono month/day values.
        if(fields[1]<1 || fields[1]>12 || fields[2]<1 || fields[2]>31)return false;
        const year_month_day date{year{fields[0]},month{fields[1]},day{fields[2]}};
        if(!date.ok())return false;
        const uint64_t result=uint64_t((sys_days{date}-epoch).count())*day_ticks+
            uint64_t(fields[3])*36000000000ull+uint64_t(fields[4])*600000000ull+
            uint64_t(fields[5])*10000000ull+uint64_t(fields[6])*10000ull;
        if(result>uint64_t(INT64_MAX))return false;
        ticks=result;return true;
    }
};
