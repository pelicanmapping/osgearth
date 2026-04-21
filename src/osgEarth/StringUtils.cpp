/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/StringUtils>
#include <cctype>
#include <cstring>

using namespace osgEarth;
using namespace osgEarth::Util;

std::pair<double, int>
osgEarth::Util::parseDoubleAndIndex(const std::string& input)
{
    if (input.length() == 0)
        return std::make_pair(NAN, 0);

    auto* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    double value = std::strtod(str, &end);
    if (str == end || errno == ERANGE)
        return std::make_pair(NAN, 0);
    else
        return std::make_pair(value, (int)(end - str));
}

double
osgEarth::Util::parseDouble(const std::string& input)
{
    if (input.length() == 0)
        return NAN;

    auto* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    double value = std::strtod(str, &end);
    if (str == end || errno == ERANGE)
        return NAN;
    else
        return value;
}

std::pair<bool, double>
osgEarth::Util::isValidNumber(const std::string& input)
{
    auto a = parseDoubleAndIndex(input); auto& value = a.first; auto& index = a.second;
    // auto [value, index] = parseDoubleAndIndex(input); // C++17
    return std::make_pair(!std::isnan(value) && index == input.length(), value);
}

std::vector<std::string>
StringTokenizer::operator()(const std::string& input, bool* error) const
{
    if (error)
        *error = false;

    std::vector<std::string> output;

    std::stringstream buf;
    bool inside_quote = false;
    char quote_opener = '\0';
    char quote_closer = '\0';
    bool keep_quote_char = false;
    int quote_opener_offset = 0;

    for (std::size_t i = 0; i < input.size(); ++i)
    {
        char c = input[i];
        auto q = _quotes.find(c);

        if (inside_quote)
        {
            if (c == quote_closer)
            {
                inside_quote = false;

                if (keep_quote_char)
                    buf << c;
            }
            else
            {
                buf << c;
            }
        }
        else
        {
            if (q != _quotes.end())
            {
                // start a new quoted region
                inside_quote = true;
                quote_opener = c;
                quote_closer = q->second.first;
                keep_quote_char = q->second.second;
                quote_opener_offset = i;

                if (keep_quote_char)
                    buf << c;
            }
            else
            {
                bool is_delimiter = false;
                auto input_remaining = input.size() - i;
                for (auto& d : _delims)
                {
                    auto delim_size = d.first.size();
                    if (delim_size <= input_remaining && strncmp(&input[i], d.first.c_str(), delim_size) == 0)
                    {
                        is_delimiter = true;

                        // end the current token, clean it up, and push it
                        auto token = buf.str();
                        if (_trimTokens)
                            trim2(token);

                        if (_keepEmptyTokens || !token.empty())
                            output.push_back(token);

                        if (d.second == true) // keep the delimiter itself as a token?
                            output.push_back(d.first);

                        buf.str("");

                        // advance over the delimiter
                        i += d.first.size() - 1;
                        break;
                    }
                }

                if (!is_delimiter)
                {
                    buf << c;
                }
            }
        }
    }

#if 0

    for (auto& c : input)
    {
        ++offset;
        auto q = _quotes.find(c);

        if (inside_quote)
        {
            if (c == quote_closer)
            {
                inside_quote = false;
                if (keep_quote_char)
                    buf << c;
            }
            else
            {
                buf << c;
            }
        }
        else
        {
            if (q != _quotes.end())
            {
                // start a new quoted region
                inside_quote = true;
                quote_opener = c;
                quote_closer = q->second.first;
                keep_quote_char = q->second.second;
                quote_opener_offset = offset - 1;

                if (keep_quote_char)
                    buf << c;
            }
            else
            {
                auto d = _delims.find(c);
                if (d == _delims.end())
                {
                    buf << c;
                }
                else
                {
                    // found a delimiter. end the current token.
                    std::string token = buf.str();
                    if (_trimTokens)
                        trim2(token);

                    if (_allowEmpties || !token.empty())
                        output.push_back(token);

                    if (d->second == true) // keep the delimiter itself as a token?
                    {
                        output.push_back(std::string(1, c));
                    }

                    buf.str("");
                }
            }
        }
    }
#endif

    if (inside_quote && !_ignoreDanglingQuotes)
    {
        OE_WARN << "[Tokenizer] unterminated quote in string ("
            << quote_opener << " at offset "
            << quote_opener_offset << ") : " << input << std::endl;

        if (error)
            *error = true;
    }

    std::string bufstr = buf.str();
    if (_trimTokens)
        trim2(bufstr);
    if (!bufstr.empty())
        output.push_back(bufstr);

    return output;
}

//--------------------------------------------------------------------------

const std::string osgEarth::Util::EMPTY_STRING;

std::string
osgEarth::Util::toLegalFileName(const std::string& input, bool allowSubdirs, const char* replacementChar)
{
    // See: http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap03.html#tag_03_282
    // We omit '-' so we can use it for the HEX identifier.
    static const std::string legalWithoutSubdirs("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.");
    static const std::string legalWithDirs("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./");


    std::size_t pos = input.find("://");
    pos = pos == std::string::npos ? 0 : pos + 3;

    const std::string& legal = allowSubdirs ? legalWithDirs : legalWithoutSubdirs;

    std::stringstream buf;
    for (; pos < input.size(); ++pos)
    {
        std::string::const_reference c = input[pos];
        if (legal.find(c) != std::string::npos)
        {
            buf << c;
        }
        else
        {
            if (replacementChar)
                buf << (char)(*replacementChar);
            else
                buf << "-" << std::hex << static_cast<unsigned>(c) << "-";
        }
    }

    std::string result;
    result = buf.str();

    return result;
}

// Public domain / CC0; based on Austin Appleby's MurmurHash3.
// Produces a 64-bit hash by folding the 128-bit x64 variant (h1 ^ h2).

namespace
{
    inline uint64_t rotl64(uint64_t x, int r) {
        return (x << r) | (x >> (64 - r));
    }

    // fmix64 finalizer
    inline uint64_t fmix64(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }
}

std::uint64_t
osgEarth::Util::hashString(const std::string& input)
{
    const std::uint64_t seed = 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(input.c_str());
    const auto len = input.length();
    const int nblocks = static_cast<int>(len / 16);

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    //----------
    // body

    for (int i = 0; i < nblocks; ++i) {
        uint64_t k1, k2;
        // safe unaligned loads
        std::memcpy(&k1, data + i * 16 + 0, 8);
        std::memcpy(&k2, data + i * 16 + 8, 8);

        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729ULL;

        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5ULL;
    }

    //----------
    // tail

    const uint8_t* tail = data + nblocks * 16;
    uint64_t k1 = 0, k2 = 0;

    switch (len & 15) {
    case 15: k2 ^= (uint64_t)tail[14] << 48;
    case 14: k2 ^= (uint64_t)tail[13] << 40;
    case 13: k2 ^= (uint64_t)tail[12] << 32;
    case 12: k2 ^= (uint64_t)tail[11] << 24;
    case 11: k2 ^= (uint64_t)tail[10] << 16;
    case 10: k2 ^= (uint64_t)tail[9] << 8;
    case  9: k2 ^= (uint64_t)tail[8];
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
    case  8: k1 ^= (uint64_t)tail[7] << 56;
    case  7: k1 ^= (uint64_t)tail[6] << 48;
    case  6: k1 ^= (uint64_t)tail[5] << 40;
    case  5: k1 ^= (uint64_t)tail[4] << 32;
    case  4: k1 ^= (uint64_t)tail[3] << 24;
    case  3: k1 ^= (uint64_t)tail[2] << 16;
    case  2: k1 ^= (uint64_t)tail[1] << 8;
    case  1: k1 ^= (uint64_t)tail[0];
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
    }

    //----------
    // finalization

    h1 ^= len;
    h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    // Fold to 64 bits (you can also just return h1)
    return h1 ^ h2;
}


std::string
osgEarth::Util::hashToString(const std::string& input)
{
    return Stringify() << std::hex << std::setw(8) << std::setfill('0') << hashString(input);
}


/** Parses an HTML color ("#rrggbb" or "#rrggbbaa") into an OSG color. */
osg::Vec4f
osgEarth::Util::htmlColorToVec4f(const std::string& html)
{
    std::string t = osgEarth::Util::toLower(html);
    osg::Vec4ub c(0, 0, 0, 255);
    if (t.length() >= 7) {
        c.r() |= t[1] <= '9' ? (t[1] - '0') << 4 : (10 + (t[1] - 'a')) << 4;
        c.r() |= t[2] <= '9' ? (t[2] - '0') : (10 + (t[2] - 'a'));
        c.g() |= t[3] <= '9' ? (t[3] - '0') << 4 : (10 + (t[3] - 'a')) << 4;
        c.g() |= t[4] <= '9' ? (t[4] - '0') : (10 + (t[4] - 'a'));
        c.b() |= t[5] <= '9' ? (t[5] - '0') << 4 : (10 + (t[5] - 'a')) << 4;
        c.b() |= t[6] <= '9' ? (t[6] - '0') : (10 + (t[6] - 'a'));
        if (t.length() == 9) {
            c.a() = 0;
            c.a() |= t[7] <= '9' ? (t[7] - '0') << 4 : (10 + (t[7] - 'a')) << 4;
            c.a() |= t[8] <= '9' ? (t[8] - '0') : (10 + (t[8] - 'a'));
        }
    }
    return osg::Vec4f(((float)c.r()) / 255.0f, ((float)c.g()) / 255.0f, ((float)c.b()) / 255.0f, ((float)c.a()) / 255.0f);
}

/** Makes an HTML color ("#rrggbb" or "#rrggbbaa") from an OSG color. */
std::string
osgEarth::Util::vec4fToHtmlColor(const osg::Vec4f& c)
{
    std::stringstream buf;
    buf << "#";
    buf << std::hex << std::setw(2) << std::setfill('0') << (int)(c.r() * 255.0f);
    buf << std::hex << std::setw(2) << std::setfill('0') << (int)(c.g() * 255.0f);
    buf << std::hex << std::setw(2) << std::setfill('0') << (int)(c.b() * 255.0f);
    if (c.a() < 1.0f)
        buf << std::hex << std::setw(2) << std::setfill('0') << (int)(c.a() * 255.0f);
    std::string ssStr;
    ssStr = buf.str();
    return ssStr;
}

/** Parses a color string in the form "255 255 255 255" (r g b a [0..255]) into an OSG color. */
osg::Vec4ub
osgEarth::Util::stringToColor(const std::string& str, osg::Vec4ub default_value)
{
    osg::Vec4ub color = default_value;
    std::istringstream strin(str);
    int r, g, b, a;
    if (strin >> r && strin >> g && strin >> b && strin >> a)
    {
        color.r() = (unsigned char)r;
        color.g() = (unsigned char)g;
        color.b() = (unsigned char)b;
        color.a() = (unsigned char)a;
    }
    return color;
}

/** Creates a string in the form "255 255 255 255" (r g b a [0..255]) from a color */
std::string
osgEarth::Util::colorToString(const osg::Vec4ub& c)
{
    std::stringstream ss;
    ss << (int)c.r() << " " << (int)c.g() << " " << (int)c.b() << " " << (int)c.a();
    std::string ssStr;
    ssStr = ss.str();
    return ssStr;
}

/** Converts a string to a vec3f */
osg::Vec3f
osgEarth::Util::stringToVec3f(const std::string& str, const osg::Vec3f& default_value)
{
    std::stringstream buf(str);
    osg::Vec3f out = default_value;
    buf >> out.x();
    if (!buf.eof()) {
        buf >> out.y() >> out.z();
    }
    else {
        out.y() = out.x();
        out.z() = out.x();
    }
    return out;
}

/** Converts a vec3f to a string */
std::string
osgEarth::Util::vec3fToString(const osg::Vec3f& v)
{
    std::stringstream buf;
    buf << std::setprecision(6)
        << v.x() << " " << v.y() << " " << v.z()
        << std::endl;
    std::string result;
    result = buf.str();
    return result;
}


/** Replaces all the instances of "sub" with "other" in "s". */
std::string&
osgEarth::Util::replaceIn(std::string& s, const std::string& sub, const std::string& other)
{
    if (sub.empty()) return s;
    size_t b = 0;
    for (; ; )
    {
        b = s.find(sub, b);
        if (b == s.npos) break;
        s.replace(b, sub.size(), other);
        b += other.size();
    }
    return s;
}

std::string&
osgEarth::Util::ciReplaceIn(std::string& s, const std::string& pattern, const std::string& replacement)
{
    if (pattern.empty()) return s;

    std::string upperSource = s;
    std::transform(upperSource.begin(), upperSource.end(), upperSource.begin(), (int(*)(int))std::toupper);

    std::string upperPattern = pattern;
    std::transform(upperPattern.begin(), upperPattern.end(), upperPattern.begin(), (int(*)(int))std::toupper);

    for (size_t b = 0; ; )
    {
        b = upperSource.find(upperPattern, b);
        if (b == s.npos) break;
        s.replace(b, pattern.size(), replacement);
        upperSource.replace(b, upperPattern.size(), replacement);
        b += replacement.size();
    }

    return s;
}

/**
* Trims whitespace from the ends of a string.
* by Rodrigo C F Dias
* http://www.codeproject.com/KB/stl/stdstringtrim.aspx
*/
void
osgEarth::Util::trim2(std::string& str)
{
    static const std::string whitespace(" \t\f\v\n\r");
    std::string::size_type pos = str.find_last_not_of(whitespace);
    if (pos != std::string::npos) {
        str.erase(pos + 1);
        pos = str.find_first_not_of(whitespace);
        if (pos != std::string::npos) str.erase(0, pos);
    }
    else str.erase(str.begin(), str.end());
}

/**
* Trims whitespace from the ends of a string, returning a
* copy of the string with whitespace removed.
*/
std::string
osgEarth::Util::trim(const std::string& in)
{
    std::string str = in;
    trim2(str);
    return str;
}

std::string
osgEarth::Util::trimAndCompress(const std::string& in)
{
    bool inwhite = true;
    std::stringstream buf;
    for (unsigned i = 0; i < in.length(); ++i)
    {
        char c = in[i];
        if (::isspace(c))
        {
            if (!inwhite)
            {
                buf << ' ';
                inwhite = true;
            }
        }
        else
        {
            inwhite = false;
            buf << c;
        }
    }
    std::string r;
    r = buf.str();
    trim2(r);
    return r;
}

std::string
osgEarth::Util::joinStrings(const StringVector& input, char delim)
{
    std::stringstream buf;
    for (StringVector::const_iterator i = input.begin(); i != input.end(); ++i)
    {
        buf << *i;
        if ((i + 1) != input.end()) buf << delim;
    }
    std::string result;
    result = buf.str();
    return result;
}

std::string
osgEarth::Util::prettyPrintTime(double seconds)
{
    int hours = (int)floor(seconds / (3600.0));
    seconds -= hours * 3600.0;

    int minutes = (int)floor(seconds / 60.0);
    seconds -= minutes * 60.0;

    std::stringstream buf;
    buf << hours << ":" << minutes << ":" << seconds;
    return buf.str();
}

std::string
osgEarth::Util::prettyPrintSize(double mb)
{
    std::stringstream buf;
    // Convert to terabytes
    if (mb > 1024 * 1024)
    {
        buf << (mb / (1024.0 * 1024.0)) << " TB";
    }
    else if (mb > 1024)
    {
        buf << (mb / 1024.0) << " GB";
    }
    else
    {
        buf << mb << " MB";
    }
    return buf.str();
}

std::string
osgEarth::Util::getToken(const std::string& input, unsigned i, char delim)
{
    auto tokens = StringTokenizer()
        .delim(std::string(1, delim))
        .standardQuotes()
        .tokenize(input);

    return i < tokens.size() ? tokens[i] : "";
}

std::string
osgEarth::Util::unquote(const std::string& input)
{
    auto trimmed = trim(input);
    if (trimmed.size() < 2)
        return trimmed;
    if (trimmed.front() == '\'' && trimmed.back() == '\'')
        return unquote(trimmed.substr(1, trimmed.size() - 2));
    if (trimmed.front() == '\"' && trimmed.back() == '\"')
        return unquote(trimmed.substr(1, trimmed.size() - 2));
    return trimmed;
}
