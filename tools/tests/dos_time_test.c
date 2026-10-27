/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "../../fs/dos_time.h"

/* The host calendar library is an independent oracle, never production code. */
int main(void)
{
    unsigned dates=0;
    const unsigned hours[]={0,13,23};
    for(unsigned year=1980;year<=2107;++year)
        for(unsigned month=1;month<=12;++month)
            for(unsigned day=1;day<=31;++day) {
                struct tm calendar={.tm_year=(int)year-1900,.tm_mon=(int)month-1,.tm_mday=(int)day};
                time_t midnight=timegm(&calendar);
                if(calendar.tm_mon!=(int)month-1 || calendar.tm_mday!=(int)day)continue;
                ++dates;
                for(unsigned i=0;i<sizeof(hours)/sizeof(hours[0]);++i) {
                    int64_t original=(int64_t)midnight+hours[i]*3600+59*60+59;
                    struct storage_dos_time encoded={0};
                    assert(!storage_dos_encode(original,987654321,&encoded));
                    assert(encoded.date==(uint16_t)(((year-1980)<<9)|(month<<5)|day));
                    assert(encoded.clock==(uint16_t)((hours[i]<<11)|(59<<5)|29) && encoded.increment==198);
                    int64_t decoded=-1;uint32_t fraction=1;
                    assert(!storage_dos_decode(encoded.date,encoded.clock,encoded.increment,&decoded,&fraction));
                    assert(decoded==original && fraction==980000000);
                }
            }
    assert(dates==46751);
    int64_t seconds=1;uint32_t fraction=1;
    assert(!storage_dos_decode(0,0,0,&seconds,&fraction) && !seconds && !fraction);
    const uint16_t bad_dates[]={1,(uint16_t)(13<<5|1),(uint16_t)(((2100-1980)<<9)|(2<<5)|29)};
    for(unsigned i=0;i<sizeof(bad_dates)/sizeof(bad_dates[0]);++i) {
        seconds=123;fraction=456;
        assert(storage_dos_decode(bad_dates[i],0,0,&seconds,&fraction)==-LINUX_EINVAL && !seconds && !fraction);
    }
    uint16_t date=(uint16_t)(1<<5|1);
    assert(storage_dos_decode(date,24<<11,0,&seconds,&fraction)==-LINUX_EINVAL);
    assert(storage_dos_decode(date,60<<5,0,&seconds,&fraction)==-LINUX_EINVAL);
    assert(storage_dos_decode(date,30,0,&seconds,&fraction)==-LINUX_EINVAL);
    assert(storage_dos_decode(date,0,200,&seconds,&fraction)==-LINUX_EINVAL);
    assert(storage_dos_decode(date,0,0,NULL,&fraction)==-LINUX_EINVAL);
    struct storage_dos_time encoded={0};
    assert(!storage_dos_encode(INT64_MIN,0,&encoded) && encoded.date==(1<<5|1) && !encoded.clock && !encoded.increment);
    assert(!storage_dos_encode(INT64_MAX,999999999,&encoded));
    assert(!storage_dos_decode(encoded.date,encoded.clock,encoded.increment,&seconds,&fraction));
    assert(seconds==4354819199LL && fraction==990000000);
    encoded=(struct storage_dos_time){1,2,3};
    assert(storage_dos_encode(0,1000000000,&encoded)==-LINUX_EINVAL && !encoded.date && !encoded.clock && !encoded.increment);
    assert(storage_dos_encode(0,0,NULL)==-LINUX_EINVAL);
    puts("PASS DOS timestamp codec: 46751 independently checked dates, 2100/leap rules, fractions, bounds and invalid fields");
}
