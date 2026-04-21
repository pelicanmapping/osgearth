/* osgEarth
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include <osgEarth/DateTime>
#include <osgEarth/StringUtils>

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    // from RFC 1123, RFC 850

    const char* rfc_wkday[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

    const char* rfc_weekday[7] = {
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
    };

    const char* rfc_month[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
}

//------------------------------------------------------------------------

DateTime::DateTime()
{
    ::time( &_time_t );
    tm* temp = ::gmtime( &_time_t );
    if ( temp ) _tm = *temp;
    else memset( &_tm, 0, sizeof(tm) );
}

DateTime::DateTime(TimeStamp utc)
{
    _time_t = utc;
    tm* temp = ::gmtime( &_time_t );
    if ( temp ) _tm = *temp;
    else memset( &_tm, 0, sizeof(tm) );
}

DateTime::DateTime(const ::tm& in_tm)
{
    tm temptm = in_tm;
    _time_t = ::mktime( &temptm ); // assumes in_tm is in local time
    tm* temp = ::gmtime( &_time_t );
    if ( temp ) _tm = *temp;
    else memset( &_tm, 0, sizeof(tm) );
    _time_t = this->timegm(&_tm); // back to UTC
}

DateTime::DateTime(int year, int month, int day, double hour)
{
    _tm.tm_year = year - 1900;
    _tm.tm_mon  = month - 1;
    _tm.tm_mday = day;

    double hour_whole = ::floor(hour);
    _tm.tm_hour = (int)hour_whole;
    double frac = hour - (double)_tm.tm_hour;
    double min = frac*60.0;
    _tm.tm_min = (int)::floor(min);
    frac = min - (double)_tm.tm_min;
    _tm.tm_sec = (int)(frac*60.0);

    // now go to time_t, and back to tm, to populate the rest of the fields.
    _time_t =  this->timegm( &_tm );
    //_time_t =  ::mktime( &_tm );
    tm* temp = ::gmtime( &_time_t );
    if ( temp ) _tm = *temp;
    else memset( &_tm, 0, sizeof(tm) );
}

DateTime::DateTime(int year, double dayOfYear)
{
    TimeStamp utc = DateTime(year,1,1,0).asTimeStamp() + (int)((dayOfYear-1.0)*24.0*3600.0);
    _time_t = utc;
    tm* temp = ::gmtime( &_time_t );
    if ( temp ) _tm = *temp;
    else memset( &_tm, 0, sizeof(tm) );    
}

DateTime::DateTime(const std::string& input) :
_time_t(0)
{
    bool ok = false;
    int year, month, day, hour, min, sec;
    char smonth[4];
    char swdky[4];
    
    ::memset( &_tm, 0, sizeof(tm) );

    if (sscanf(input.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        _tm.tm_year = year - 1900;
        _tm.tm_mon  = month - 1;
        _tm.tm_mday = day;
        _tm.tm_hour = hour;
        _tm.tm_min  = min;
        _tm.tm_sec  = sec;
        ok = true;
    }
    else if (sscanf(input.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        _tm.tm_year = year - 1900;
        _tm.tm_mon  = month - 1;
        _tm.tm_mday = day;
        _tm.tm_hour = hour;
        _tm.tm_min  = min;
        _tm.tm_sec  = sec;
        ok = true;
    }
    else if (sscanf(input.c_str(), "%4d%2d%2dT%2d%2d%2d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        _tm.tm_year = year - 1900;
        _tm.tm_mon  = month - 1;
        _tm.tm_mday = day;
        _tm.tm_hour = hour;
        _tm.tm_min  = min;
        _tm.tm_sec  = sec;
        ok = true;
    }
    else if (sscanf(input.c_str(), "%4d%2d%2d%2d%2d%2d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        _tm.tm_year = year - 1900;
        _tm.tm_mon  = month - 1;
        _tm.tm_mday = day;
        _tm.tm_hour = hour;
        _tm.tm_min  = min;
        _tm.tm_sec  = sec;
        ok = true;
    }
    else if (sscanf(input.c_str(), "%3s, %2d %3s %4d %2d:%2d:%2d", swdky, &day, smonth, &year, &hour, &min, &sec) == 7)
    {
        // try RFC 1123 format: "Sun, 06 Nov 1994 08:49:37 GMT"
        _tm.tm_year = year - 1900;
        _tm.tm_mon = -1;
        for (int i = 0; i < 12; ++i)
        {
            if (strncmp(smonth, rfc_month[i], 3) == 0)
            {
                _tm.tm_mon = i;
                break;
            }
        }
        if (_tm.tm_mon >= 0)
        {
            _tm.tm_mday = day;
            _tm.tm_hour = hour;
            _tm.tm_min = min;
            _tm.tm_sec = sec;
            ok = true;
        }
    }

    if ( ok )
    {
        // now go to time_t, and back to tm, to populate the rest of the fields.
        _time_t =  this->timegm( &_tm );
        tm* temp = ::gmtime( &_time_t );
        if ( temp ) _tm = *temp;
    }
}

DateTime::DateTime(const DateTime& rhs) :
_tm    ( rhs._tm ),
_time_t( rhs._time_t )
{
    //nop
}

int 
DateTime::year() const 
{ 
    return _tm.tm_year + 1900;
}

int
DateTime::month() const
{
    return _tm.tm_mon + 1;
}

int
DateTime::day() const
{
    return _tm.tm_mday;
}

double
DateTime::hours() const
{
    return (double)_tm.tm_hour + ((double)_tm.tm_min)/60. + ((double)_tm.tm_sec)/3600.;
}

const std::string
DateTime::asRFC1123() const
{
    return Stringify()
        << rfc_wkday[_tm.tm_wday] << ", "
        << std::setfill('0') << std::setw(2) << _tm.tm_mday << ' '
        << rfc_month[_tm.tm_mon] << ' '
        << std::setw(4) << (1900 + _tm.tm_year) << ' '
        << std::setw(2) << _tm.tm_hour << ':'
        << std::setw(2) << _tm.tm_min << ':'
        << std::setw(2) << _tm.tm_sec << ' '
        << "GMT";
}

const std::string
DateTime::asISO8601() const
{
    return Stringify()
        << std::setw(4) << (_tm.tm_year + 1900) << '-'
        << std::setfill('0') << std::setw(2) << (_tm.tm_mon + 1) << '-'
        << std::setfill('0') << std::setw(2) << (_tm.tm_mday)
        << 'T'
        << std::setfill('0') << std::setw(2) << _tm.tm_hour << ':'
        << std::setfill('0') << std::setw(2) << _tm.tm_min << ':'
        << std::setfill('0') << std::setw(2) << _tm.tm_sec
        << 'Z';
}

const std::string
DateTime::asCompactISO8601() const
{
    return Stringify()
        << std::setw(4) << (_tm.tm_year + 1900)
        << std::setfill('0') << std::setw(2) << (_tm.tm_mon + 1)
        << std::setfill('0') << std::setw(2) << (_tm.tm_mday)
        << 'T'
        << std::setfill('0') << std::setw(2) << _tm.tm_hour
        << std::setfill('0') << std::setw(2) << _tm.tm_min
        << std::setfill('0') << std::setw(2) << _tm.tm_sec
        << 'Z';
}


// https://en.wikipedia.org/wiki/Julian_day#Converting_Gregorian_calendar_date_to_Julian_Day_Number
double
DateTime::getJulianDay() const
{
    int d = (1461 * (year() + 4800 + (month() - 14) / 12)) / 4 + (367 * (month() - 2 - 12 * ((month() - 14) / 12))) / 12 - (3 * ((year() + 4900 + (month()-14)/12)/100))/4 + day() - 32075;
    return (double)(d-0.5) + hours()/24.0;
}

//------------------------------------------------------------------------

/*
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

namespace
{
    /* Number of days per month (except for February in leap years). */
    static const int monoff[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    static int is_leap_year(int year)
    {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    static int leap_days(int y1, int y2)
    {
        --y1;
        --y2;
        return (y2/4 - y1/4) - (y2/100 - y1/100) + (y2/400 - y1/400);
    }
}

/*
* Code adapted from Python 2.4.1 sources (Lib/calendar.py).
*/
::time_t DateTime::timegm(const struct tm* tm) const
{
    int year;
    time_t days;
    time_t hours;
    time_t minutes;
    time_t seconds;

    year = 1900 + tm->tm_year;
    days = 365 * (year - 1970) + leap_days(1970, year);
    days += monoff[tm->tm_mon];

    if (tm->tm_mon > 1 && is_leap_year(year))
        ++days;
    days += tm->tm_mday - 1;

    hours = days * 24 + tm->tm_hour;
    minutes = hours * 60 + tm->tm_min;
    seconds = minutes * 60 + tm->tm_sec;

    return seconds;
}

DateTime
DateTime::operator+(double hours) const
{
    double seconds = (double)asTimeStamp() + (hours*3600.0);
    return DateTime((TimeStamp)seconds);
}

void
DateTimeExtent::expandBy(const DateTime& value)
{
    if (!_valid || value < _start)
        _start = value;
    if (!_valid || value > _end)
        _end = value;
    _valid = true;
}
