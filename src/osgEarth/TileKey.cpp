/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/TileKey>
#include <osgEarth/Math>
#include <charconv>
#include <limits>

using namespace osgEarth;

namespace
{
    constexpr std::size_t MAX_UNSIGNED_DIGITS = std::numeric_limits<unsigned int>::digits10 + 1u;
    constexpr std::size_t MAX_TILEKEY_STRING_SIZE = 3u * MAX_UNSIGNED_DIGITS + 2u;

    char* appendUnsigned(char* out, char* end, unsigned int value)
    {
        return std::to_chars(out, end, value).ptr;
    }
}

//------------------------------------------------------------------------

TileKey TileKey::INVALID( 0, 0, 0, 0L );

//------------------------------------------------------------------------

TileKey::TileKey(unsigned int lod, unsigned int tile_x, unsigned int tile_y, const Profile* profile)
{
    _x = tile_x;
    _y = tile_y;
    _lod = lod;
    _profile = profile;
    rehash();
}

TileKey::TileKey(const TileKey& rhs) :
    _lod(rhs._lod),
    _x(rhs._x),
    _y(rhs._y),
    _profile(rhs._profile.get()),
    _hash(rhs._hash)
{
    //NOP
}

void
TileKey::rehash()
{
    _hash = valid() ?
        osgEarth::hash_value_unsigned(
            (std::size_t)_lod, 
            (std::size_t)_x,
            (std::size_t)_y,
            _profile->hash()) :
        0ULL;
}

const Profile*
TileKey::getProfile() const
{
    return _profile.get();
}

void
TileKey::getTileXY(unsigned int& out_tile_x,
                   unsigned int& out_tile_y) const
{
    out_tile_x = _x;
    out_tile_y = _y;
}

const GeoExtent
TileKey::getExtent() const
{
    if (!valid())
        return GeoExtent::INVALID;

    double width, height;
    _profile->getTileDimensions(_lod, width, height);
    double xmin = _profile->getExtent().xMin() + (width * (double)_x);
    double ymax = _profile->getExtent().yMax() - (height * (double)_y);
    double xmax = xmin + width;
    double ymin = ymax - height;

    return GeoExtent( _profile->getSRS(), xmin, ymin, xmax, ymax );
}

const std::string
TileKey::str() const
{
    if (valid())
    {
        char buf[MAX_TILEKEY_STRING_SIZE];
        char* out = buf;
        char* end = buf + sizeof(buf);

        out = appendUnsigned(out, end, _lod);
        *out++ = '/';
        out = appendUnsigned(out, end, _x);
        *out++ = '/';
        out = appendUnsigned(out, end, _y);

        return std::string(buf, out);
    }
    else return "invalid";
}

unsigned
TileKey::getQuadrant() const
{
    if ( _lod == 0 )
        return 0;
    bool xeven = (_x & 1) == 0;
    bool yeven = (_y & 1) == 0;
    return 
        xeven && yeven ? 0 :
        xeven          ? 2 :
        yeven          ? 1 : 3;
}

void
TileKey::getPixelExtents(unsigned int& xmin,
                         unsigned int& ymin,
                         unsigned int& xmax,
                         unsigned int& ymax,
                         const unsigned int &tile_size) const
{
    xmin = _x * tile_size;
    ymin = _y * tile_size;
    xmax = xmin + tile_size;
    ymax = ymin + tile_size; 
}

std::pair<double,double>
TileKey::getResolution(unsigned tileSize) const
{
    double width, height;
    _profile->getTileDimensions(_lod, width, height);
    return std::make_pair(
        width/(double)(tileSize-1),
        height/(double)(tileSize-1));
}

TileKey
TileKey::createChildKey( unsigned int quadrant ) const
{
    unsigned int lod = _lod + 1;
    unsigned int x = _x * 2;
    unsigned int y = _y * 2;

    if (quadrant == 1)
    {
        x+=1;
    }
    else if (quadrant == 2)
    {
        y+=1;
    }
    else if (quadrant == 3)
    {
        x+=1;
        y+=1;
    }
    return TileKey( lod, x, y, _profile.get());
}


TileKey
TileKey::createParentKey() const
{
    if (_lod == 0) return TileKey::INVALID;

    unsigned int lod = _lod - 1;
    unsigned int x = _x / 2;
    unsigned int y = _y / 2;
    return TileKey( lod, x, y, _profile.get());
}

bool
TileKey::makeParent()
{
    if (_lod == 0)
    {
        _profile = NULL; // invalidate
        return false;
    }

    _lod--;
    _x >>= 1;
    _y >>= 1;
    rehash();
    return true;
}

TileKey
TileKey::createAncestorKey( int ancestorLod ) const
{
    if ( ancestorLod > (int)_lod ) return TileKey::INVALID;

    unsigned int x = _x, y = _y;
    for( int i=_lod; i > ancestorLod; i-- )
    {
        x /= 2;
        y /= 2;
    }
    return TileKey( ancestorLod, x, y, _profile.get() );
}

TileKey
TileKey::createNeighborKey( int xoffset, int yoffset ) const
{
    unsigned tx, ty;
    getProfile()->getNumTiles( _lod, tx, ty );

    int sx = (int)_x + xoffset;
    unsigned x =
        sx < 0        ? (unsigned)((int)tx + sx) :
        sx >= (int)tx ? (unsigned)sx - tx :
        (unsigned)sx;

    int sy = (int)_y + yoffset;
    unsigned y =
        sy < 0        ? (unsigned)((int)ty + sy) :
        sy >= (int)ty ? (unsigned)sy - ty :
        (unsigned)sy;

    //OE_NOTICE << "Returning neighbor " << x << ", " << y << " for tile " << str() << " offset=" << xoffset << ", " << yoffset << std::endl;

    return TileKey( _lod, x % tx, y % ty, _profile.get() );
}

TileKey
TileKey::mapResolution(unsigned targetSize,
                       unsigned sourceSize,
                       unsigned minimumLOD) const
{
    // This only works when falling back; i.e. targetSize is smaller than sourceSize.
    if ( getLOD() == 0 || targetSize >= sourceSize )
        return *this;

    // Minimum target tile size.
    if ( targetSize < 2 )
        targetSize = 2;

    int lod = (int)getLOD();
    int targetSizePOT = nextPowerOf2((int)targetSize);

    while(true)
    {
        if (targetSizePOT >= (int)sourceSize)
        {
            return createAncestorKey(lod);
        }

        if ( lod == (int)minimumLOD )
        {
            return createAncestorKey(lod);
        }

        lod--;
        targetSizePOT *= 2;        
    }
}
