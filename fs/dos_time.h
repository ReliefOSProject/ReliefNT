/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_DOS_TIME_H
#define RELIEFNT_FS_DOS_TIME_H
#include <stdbool.h>
#include <stdint.h>
#include <linux/errno.h>

/* Decoded host fields, never cast onto a packed on-disk directory entry. */
struct storage_dos_time {uint16_t date,clock;uint8_t increment;};

/** @brief Decide a Gregorian leap year without a timezone or clock dependency.
 * @param year Calendar year. @return True for Gregorian leap years.
 * Pure arithmetic; no allocation, I/O, locking or ownership transfer.
 */
static inline bool storage_dos_leap(unsigned year)
{
    return year%4==0 && (year%100!=0 || year%400==0);
}
/** @brief Return the length of a validated Gregorian calendar month.
 * @param year Calendar year. @param month Validated one-based month 1..12.
 * @return Number of days, including February's century/leap rule.
 * Pure arithmetic over static constants, safe in any execution context.
 */
static inline unsigned storage_dos_month_days(unsigned year,unsigned month)
{
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1]+(month==2 && storage_dos_leap(year));
}
/** @brief Decode native FAT/exFAT calendar fields as UTC without aliasing disk bytes.
 * @param date Seven-bit year since1980, four-bit month and five-bit day.
 * @param clock Five-bit hour, six-bit minute and five-bit double-seconds.
 * @param increment Optional 0..199 hundredths since the even second; zero for atime.
 * @param seconds Required signed Unix-seconds output, cleared on failure.
 * @param nanoseconds Required fractional output, cleared on failure.
 * @return Zero or -EINVAL for missing output or impossible calendar/clock fields.
 * All-zero fields preserve an unspecified legacy timestamp as zero. Timezone
 * offsets are applied separately by the exFAT codec. Pure bounded arithmetic;
 * no allocation, clock access, reference ownership or lock requirements.
 */
static inline int storage_dos_decode(uint16_t date,uint16_t clock,uint8_t increment,
                                       int64_t *seconds,uint32_t *nanoseconds)
{
    if(seconds)*seconds=0;
    if(nanoseconds)*nanoseconds=0;
    if(!seconds || !nanoseconds)return -LINUX_EINVAL;
    if(!date && !clock && !increment)return 0;
    unsigned year=1980+(date>>9),month=(date>>5)&15,day=date&31;
    unsigned hour=clock>>11,minute=(clock>>5)&63,double_second=clock&31;
    if(!month || month>12 || !day || day>storage_dos_month_days(year,month) ||
       hour>23 || minute>59 || double_second>29 || increment>199)return -LINUX_EINVAL;
    int64_t days=3652; /* Exact Gregorian days from the Unix epoch to1980-01-01. */
    for(unsigned y=1980;y<year;++y)days+=365+storage_dos_leap(y);
    for(unsigned m=1;m<month;++m)days+=storage_dos_month_days(year,m);
    *seconds=(days+day-1)*86400+hour*3600+minute*60+double_second*2+increment/100;
    *nanoseconds=(increment%100)*10000000u;
    return 0;
}
/** @brief Encode a Unix timestamp using the native DOS date and10ms increment.
 * @param seconds Signed UTC Unix seconds; clamped to the format's1980..2107 range.
 * @param nanoseconds Fractional second0..999999999, truncated to10ms.
 * @param out Required writable calendar fields, cleared on validation failure.
 * @return Zero or -EINVAL for missing output/invalid fractional input. Input
 * outside the native date range is clamped in seconds; an earlier input
 * has zero fractional time. No allocation, I/O, lock or ownership transfer.
 */
static inline int storage_dos_encode(int64_t seconds,uint32_t nanoseconds,struct storage_dos_time *out)
{
    if(!out)return -LINUX_EINVAL;
    *out=(struct storage_dos_time){0};
    if(nanoseconds>=1000000000)return -LINUX_EINVAL;
    if(seconds<315532800){seconds=315532800;nanoseconds=0;}
    if(seconds>4354819199LL)seconds=4354819199LL;
    unsigned year=1980,month=1;
    int64_t days=(seconds-315532800)/86400;
    while(days>=365+storage_dos_leap(year)){days-=365+storage_dos_leap(year);++year;}
    while(days>=storage_dos_month_days(year,month)){days-=storage_dos_month_days(year,month);++month;}
    unsigned second_of_day=(unsigned)(seconds%86400);
    unsigned hour=second_of_day/3600,minute=second_of_day%3600/60,second=second_of_day%60;
    out->date=(uint16_t)(((year-1980)<<9)|(month<<5)|(unsigned)(days+1));
    out->clock=(uint16_t)((hour<<11)|(minute<<5)|(second>>1));
    out->increment=(uint8_t)((second&1)*100+nanoseconds/10000000);
    return 0;
}
#endif
