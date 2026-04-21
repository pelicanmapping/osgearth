/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/Notify>
#include <osgEarth/Threading>
#include <osgEarth/Registry>
#include <osgEarth/TileKey>
#include <osgEarth/ImageUtils>
#include <osgDB/WriteFile>
#include <osgEarth/FileUtils>
#include <osgEarth/ImageLayer>
#include <iostream>
#include <vector>
#include <string>
#include <array>

#include "heatmap.h"

#include "colorschemes/gray.h"
#include "colorschemes/Blues.h"
#include "colorschemes/BrBG.h"
#include "colorschemes/BuGn.h"
#include "colorschemes/BuPu.h"
#include "colorschemes/GnBu.h"
#include "colorschemes/Greens.h"
#include "colorschemes/Greys.h"
#include "colorschemes/Oranges.h"
#include "colorschemes/OrRd.h"
#include "colorschemes/PiYG.h"
#include "colorschemes/PRGn.h"
#include "colorschemes/PuBuGn.h"
#include "colorschemes/PuBu.h"
#include "colorschemes/PuOr.h"
#include "colorschemes/PuRd.h"
#include "colorschemes/Purples.h"
#include "colorschemes/RdBu.h"
#include "colorschemes/RdGy.h"
#include "colorschemes/RdPu.h"
#include "colorschemes/RdYlBu.h"
#include "colorschemes/RdYlGn.h"
#include "colorschemes/Reds.h"
#include "colorschemes/Spectral.h"
#include "colorschemes/YlGnBu.h"
#include "colorschemes/YlGn.h"
#include "colorschemes/YlOrBr.h"
#include "colorschemes/YlOrRd.h"

std::map<std::string, const heatmap_colorscheme_t*> g_schemes = {
    {"b2w", heatmap_cs_b2w},
    {"b2w_opaque", heatmap_cs_b2w_opaque},
    {"w2b", heatmap_cs_w2b},
    {"w2b_opaque", heatmap_cs_w2b_opaque},
    {"Blues_discrete", heatmap_cs_Blues_discrete},
    {"Blues_soft", heatmap_cs_Blues_soft},
    {"Blues_mixed", heatmap_cs_Blues_mixed},
    {"Blues_mixed_exp", heatmap_cs_Blues_mixed_exp},
    {"BrBG_discrete", heatmap_cs_BrBG_discrete},
    {"BrBG_soft", heatmap_cs_BrBG_soft},
    {"BrBG_mixed", heatmap_cs_BrBG_mixed},
    {"BrBG_mixed_exp", heatmap_cs_BrBG_mixed_exp},
    {"BuGn_discrete", heatmap_cs_BuGn_discrete},
    {"BuGn_soft", heatmap_cs_BuGn_soft},
    {"BuGn_mixed", heatmap_cs_BuGn_mixed},
    {"BuGn_mixed_exp", heatmap_cs_BuGn_mixed_exp},
    {"BuPu_discrete", heatmap_cs_BuPu_discrete},
    {"BuPu_soft", heatmap_cs_BuPu_soft},
    {"BuPu_mixed", heatmap_cs_BuPu_mixed},
    {"BuPu_mixed_exp", heatmap_cs_BuPu_mixed_exp},
    {"GnBu_discrete", heatmap_cs_GnBu_discrete},
    {"GnBu_soft", heatmap_cs_GnBu_soft},
    {"GnBu_mixed", heatmap_cs_GnBu_mixed},
    {"GnBu_mixed_exp", heatmap_cs_GnBu_mixed_exp},
    {"Greens_discrete", heatmap_cs_Greens_discrete},
    {"Greens_soft", heatmap_cs_Greens_soft},
    {"Greens_mixed", heatmap_cs_Greens_mixed},
    {"Greens_mixed_exp", heatmap_cs_Greens_mixed_exp},
    {"Greys_discrete", heatmap_cs_Greys_discrete},
    {"Greys_soft", heatmap_cs_Greys_soft},
    {"Greys_mixed", heatmap_cs_Greys_mixed},
    {"Greys_mixed_exp", heatmap_cs_Greys_mixed_exp},
    {"Oranges_discrete", heatmap_cs_Oranges_discrete},
    {"Oranges_soft", heatmap_cs_Oranges_soft},
    {"Oranges_mixed", heatmap_cs_Oranges_mixed},
    {"Oranges_mixed_exp", heatmap_cs_Oranges_mixed_exp},
    {"OrRd_discrete", heatmap_cs_OrRd_discrete},
    {"OrRd_soft", heatmap_cs_OrRd_soft},
    {"OrRd_mixed", heatmap_cs_OrRd_mixed},
    {"OrRd_mixed_exp", heatmap_cs_OrRd_mixed_exp},
    {"PiYG_discrete", heatmap_cs_PiYG_discrete},
    {"PiYG_soft", heatmap_cs_PiYG_soft},
    {"PiYG_mixed", heatmap_cs_PiYG_mixed},
    {"PiYG_mixed_exp", heatmap_cs_PiYG_mixed_exp},
    {"PRGn_discrete", heatmap_cs_PRGn_discrete},
    {"PRGn_soft", heatmap_cs_PRGn_soft},
    {"PRGn_mixed", heatmap_cs_PRGn_mixed},
    {"PRGn_mixed_exp", heatmap_cs_PRGn_mixed_exp},
    {"PuBuGn_discrete", heatmap_cs_PuBuGn_discrete},
    {"PuBuGn_soft", heatmap_cs_PuBuGn_soft},
    {"PuBuGn_mixed", heatmap_cs_PuBuGn_mixed},
    {"PuBuGn_mixed_exp", heatmap_cs_PuBuGn_mixed_exp},
    {"PuBu_discrete", heatmap_cs_PuBu_discrete},
    {"PuBu_soft", heatmap_cs_PuBu_soft},
    {"PuBu_mixed", heatmap_cs_PuBu_mixed},
    {"PuBu_mixed_exp", heatmap_cs_PuBu_mixed_exp},
    {"PuOr_discrete", heatmap_cs_PuOr_discrete},
    {"PuOr_soft", heatmap_cs_PuOr_soft},
    {"PuOr_mixed", heatmap_cs_PuOr_mixed},
    {"PuOr_mixed_exp", heatmap_cs_PuOr_mixed_exp},
    {"PuRd_discrete", heatmap_cs_PuRd_discrete},
    {"PuRd_soft", heatmap_cs_PuRd_soft},
    {"PuRd_mixed", heatmap_cs_PuRd_mixed},
    {"PuRd_mixed_exp", heatmap_cs_PuRd_mixed_exp},
    {"Purples_discrete", heatmap_cs_Purples_discrete},
    {"Purples_soft", heatmap_cs_Purples_soft},
    {"Purples_mixed", heatmap_cs_Purples_mixed},
    {"Purples_mixed_exp", heatmap_cs_Purples_mixed_exp},
    {"RdBu_discrete", heatmap_cs_RdBu_discrete},
    {"RdBu_soft", heatmap_cs_RdBu_soft},
    {"RdBu_mixed", heatmap_cs_RdBu_mixed},
    {"RdBu_mixed_exp", heatmap_cs_RdBu_mixed_exp},
    {"RdGy_discrete", heatmap_cs_RdGy_discrete},
    {"RdGy_soft", heatmap_cs_RdGy_soft},
    {"RdGy_mixed", heatmap_cs_RdGy_mixed},
    {"RdGy_mixed_exp", heatmap_cs_RdGy_mixed_exp},
    {"RdPu_discrete", heatmap_cs_RdPu_discrete},
    {"RdPu_soft", heatmap_cs_RdPu_soft},
    {"RdPu_mixed", heatmap_cs_RdPu_mixed},
    {"RdPu_mixed_exp", heatmap_cs_RdPu_mixed_exp},
    {"RdYlBu_discrete", heatmap_cs_RdYlBu_discrete},
    {"RdYlBu_soft", heatmap_cs_RdYlBu_soft},
    {"RdYlBu_mixed", heatmap_cs_RdYlBu_mixed},
    {"RdYlBu_mixed_exp", heatmap_cs_RdYlBu_mixed_exp},
    {"RdYlGn_discrete", heatmap_cs_RdYlGn_discrete},
    {"RdYlGn_soft", heatmap_cs_RdYlGn_soft},
    {"RdYlGn_mixed", heatmap_cs_RdYlGn_mixed},
    {"RdYlGn_mixed_exp", heatmap_cs_RdYlGn_mixed_exp},
    {"Reds_discrete", heatmap_cs_Reds_discrete},
    {"Reds_soft", heatmap_cs_Reds_soft},
    {"Reds_mixed", heatmap_cs_Reds_mixed},
    {"Reds_mixed_exp", heatmap_cs_Reds_mixed_exp},
    {"Spectral_discrete", heatmap_cs_Spectral_discrete},
    {"Spectral_soft", heatmap_cs_Spectral_soft},
    {"Spectral_mixed", heatmap_cs_Spectral_mixed},
    {"Spectral_mixed_exp", heatmap_cs_Spectral_mixed_exp},
    {"YlGnBu_discrete", heatmap_cs_YlGnBu_discrete},
    {"YlGnBu_soft", heatmap_cs_YlGnBu_soft},
    {"YlGnBu_mixed", heatmap_cs_YlGnBu_mixed},
    {"YlGnBu_mixed_exp", heatmap_cs_YlGnBu_mixed_exp},
    {"YlGn_discrete", heatmap_cs_YlGn_discrete},
    {"YlGn_soft", heatmap_cs_YlGn_soft},
    {"YlGn_mixed", heatmap_cs_YlGn_mixed},
    {"YlGn_mixed_exp", heatmap_cs_YlGn_mixed_exp},
    {"YlOrBr_discrete", heatmap_cs_YlOrBr_discrete},
    {"YlOrBr_soft", heatmap_cs_YlOrBr_soft},
    {"YlOrBr_mixed", heatmap_cs_YlOrBr_mixed},
    {"YlOrBr_mixed_exp", heatmap_cs_YlOrBr_mixed_exp},
    {"YlOrRd_discrete", heatmap_cs_YlOrRd_discrete},
    {"YlOrRd_soft", heatmap_cs_YlOrRd_soft},
    {"YlOrRd_mixed", heatmap_cs_YlOrRd_mixed},
    {"YlOrRd_mixed_exp", heatmap_cs_YlOrRd_mixed_exp},
};

using namespace osgEarth;

int usage(const char* name)
{
    std::cout
        << "Generates a heatmap tiled dataset from a series of points.\n\n"
        << "osgearth_heatmap < points.txt  where points.txt contains a series of lat lon points separated by a space"
        << name
        << "\n    --weighted                          : If set the incoming points have a third component which represents the weight of the point"
        << "\n    --min-level [level]                 : The minimum zoom level to generate map image layer.  Heat map points are aggregated together for lower lods."
        << "\n    --max-level [level]                 : The maximum zoom level to generate map image layer, higher levels take longer"
        << "\n    --max-heat [maxHeat]                : The maximum heat value to scale the color ramp to."
        << "\n    --buffer [buffer]                   : The buffer size used to create neighboring tiles.  Default 30."
        << "\n    --list-color-schemes                : Lists all available color schemes"
        << "\n    --color-scheme [color-scheme]       : The color scheme to use."
        << "\n    --osg-options [OSG options string]  : options to pass to OSG readers/writers"
        << "\n    --out [prop_name] [prop_value]      : set an output property"
        << std::endl;

    return 0;
}


typedef std::unordered_map<unsigned short, float> CellIndex;
typedef std::unordered_map<osgEarth::TileKey, CellIndex> TileKeyMap;

static int numRead = 0;

osg::ref_ptr<const Profile> wgs84 = Profile::create(Profile::GLOBAL_GEODETIC);

static TileKeyMap s_keys;

unsigned int minLevel = 0;
unsigned int maxLevel = 8;
float maxHeat = 100.0;
// Buffer with neighbor tiles to do some simple metatiling to prevent inconsistent edges along tile boundaries.
unsigned int buffer = 30;

inline void addPoint(double lon, double lat, float weight)
{
    if (lon >= -180.0 && lon <= 180.0 &&
        lat >= -90 && lat <= 90.0 &&
        weight >= 0.0)
    {
        for (unsigned int level = minLevel; level <= maxLevel; ++level)
        {
            TileKey key = wgs84->createTileKey(lon, lat, level);

            GeoExtent extent = key.getExtent();

            unsigned int x = osg::clampBetween((unsigned int)(256.0 * (lon - extent.xMin()) / extent.width()), 0u, 255u);
            unsigned int y = osg::clampBetween((unsigned int)(256.0 * (lat - extent.yMin()) / extent.height()), 0u, 255u);
            unsigned short index = (unsigned short)(y * 256 + x);
            s_keys[key][index] += weight;
        }
    }
}

void ReadFile(std::istream& in, bool weighted)
{
    double lat, lon;
    float weight;
    if (weighted)
    {
        std::cout << "Reading weighted points..." << std::endl;
        while (in >> lat >> lon >> weight)
        {
            ++numRead;
            if (numRead % 50000 == 0) std::cout << "Read " << numRead << std::endl;

            addPoint(lon, lat, weight);
        }
    }
    else
    {
        std::cout << "Reading non-weighted points..." << std::endl;
        while (in >> lat >> lon)
        {
            ++numRead;
            if (numRead % 50000 == 0) std::cout << "Read " << numRead << std::endl;

            addPoint(lon, lat, 1.0);
        }
    }
}

void WriteKeys(ImageLayer* layer, const heatmap_colorscheme_t* colorScheme)
{
    unsigned int numKeys = s_keys.size();
    unsigned int numProcessed = 0;
    for (const auto& key : s_keys)
    {
        unsigned int w = 256;
        unsigned int h = 256;

        // Create the heatmap object with the given dimensions (in pixel).
        heatmap_t* hm = heatmap_new(w + buffer * 2, h + buffer * 2);

        for (auto& cell : key.second)
        {
            unsigned int index = cell.first;

            int r = index / 256;
            int c = index % 256;
            auto hitCount = cell.second;

            int xOffset = 0;
            int yOffset = 0;

            // Add the point but weighted to the number of hits in the cell to get a nice clustered view of the points of the lower levels.
            heatmap_add_weighted_point(hm, c + buffer, r + buffer, hitCount);
        }

        if (buffer > 0)
        {
            for (int i = -1; i <= 1; ++i)
            {
                for (int j = -1; j <= 1; ++j)
                {
                    if (!(i == 0 && j == 0))
                    {
                        TileKey neighborKey = key.first.createNeighborKey(i, j);

                        TileKeyMap::iterator neighborItr = s_keys.find(neighborKey);
                        if (neighborItr != s_keys.end())
                        {
                            int xOffset = 0;
                            if (neighborKey.getTileX() < key.first.getTileX())
                            {
                                xOffset = -256;
                            }
                            if (neighborKey.getTileX() > key.first.getTileX())
                            {
                                xOffset = 256;
                            }
                            int yOffset = 0;
                            if (neighborKey.getTileY() > key.first.getTileY())
                            {
                                yOffset = -256;
                            }
                            if (neighborKey.getTileY() < key.first.getTileY())
                            {
                                yOffset = 256;
                            }

                            for (auto& cell : neighborItr->second)
                            {
                                unsigned short index = cell.first;

                                int r = index / 256;
                                int c = index % 256;
                                auto hitCount = cell.second;

                                c += xOffset;
                                r += yOffset;

                                // Add the point but weighted to the number of hits in the cell to get a nice clustered view of the points of the lower levels.
                                heatmap_add_weighted_point(hm, c + buffer, r + buffer, hitCount);
                            }
                        }
                    }
                }
            }
        }

        unsigned int imageSize = hm->w * hm->h * 4;
        unsigned char* imageData = new unsigned char[imageSize];
        heatmap_render_saturated_to(hm, colorScheme, maxHeat, imageData);
        osg::ref_ptr< osg::Image > image = new osg::Image;
        image->setImage(hm->w, hm->h, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, imageData, osg::Image::USE_NEW_DELETE);

        osg::ref_ptr < osg::Image > cropped = ImageUtils::cropImage(image.get(), buffer, buffer, w, h);
        layer->writeImage(key.first, cropped.get());

        heatmap_free(hm);

        ++numProcessed;
        if (numProcessed % 100 == 0)
        {
            std::cout << "Processed " << numProcessed << " of " << numKeys << " keys" << std::endl;
        }
    }
}

int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);

    // help?
    if (arguments.read("--help"))
        return usage(argv[0]);

    if (arguments.read("--list-color-schemes"))
    {
        for (auto& scheme : g_schemes) {
            std::cout << "  " << scheme.first << std::endl;
        }
        return 0;
    }

    bool weighted = false;

    auto startTime = std::chrono::high_resolution_clock::now();

    arguments.read("--min-level", minLevel);
    arguments.read("--max-level", maxLevel);
    arguments.read("--max-heat", maxHeat);
    arguments.read("--buffer", buffer);
    weighted = arguments.read("--weighted");

    const heatmap_colorscheme_t* colorScheme = heatmap_cs_default;
    std::string colorSchemeName;
    if (arguments.read("--color-scheme", colorSchemeName))
    {
        auto itr = g_schemes.find(colorSchemeName);
        if (itr == g_schemes.end())
        {
            std::cout << "Can't find color scheme named " << colorSchemeName << ", using default" << std::endl;
        }
        else
        {
            colorScheme = itr->second;
        }
    }

    // collect output configuration:
    Config outConf;
    std::string key, value;
    while (arguments.read("--out", key, value))
        outConf.set(key, value);
    outConf.key() = outConf.value("driver");
    outConf.add("profile", "global-geodetic");

    osg::ref_ptr<osgDB::Options> dbo = new osgDB::Options();

    // plugin options, if the user passed them in:
    std::string str;
    while (arguments.read("--osg-options", str) || arguments.read("-O", str))
    {
        dbo->setOptionString(str);
    }

    // open the output tile source:
    auto layer = Layer::create(ConfigOptions(outConf));
    osg::ref_ptr<ImageLayer> output = dynamic_cast<ImageLayer*>(layer.get());
    if (!output.valid())
    {
        return usage("Failed to create output layer");
    }

    output->setReadOptions(dbo.get());
    Status outputStatus = output->openForWriting();
    if (outputStatus.isError())
    {        
        OE_WARN << "Error initializing output: " << outputStatus.message() << std::endl;
        return -1;
    }

    OE_NOTICE << "Writing tiles to:\n"
        << outConf.toJSON(true)
        << std::endl;

    std::cout << "Generating heatmap to max level of " << maxLevel << " with max heat " << maxHeat << std::endl;

    ReadFile(std::cin, weighted);

    std::cout << "Read " << numRead << " points" << std::endl;

    // Write out all the keys
    std::cout << "Writing " << s_keys.size() << " keys" << std::endl;


    WriteKeys(output.get(), colorScheme);

    auto endTime = std::chrono::high_resolution_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::cout << "Complete in " << ms << " ms " << std::endl;

    return 0;
}