/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "GDAL"

#include <osgEarth/Registry>
#include <osgEarth/ImageUtils>
#include <osgEarth/URI>
#include <osgEarth/Progress>
#include <osgEarth/LandCover>
#include <osgEarth/Metrics>
#include <osgEarth/Color>
#include <osgEarth/Math>

#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>

#include <sstream>
#include <thread>

#include <gdal.h>
#include <gdalwarper.h>
#include <gdal_proxy.h>
#include <cpl_string.h>

using namespace osgEarth;

#ifndef OE_THREAD_LOCAL
//#define OE_THREAD_LOCAL
#define OE_THREAD_LOCAL static thread_local
#endif

#undef LC
#define LC "[GDAL] "

#define INDENT "    "

#define PIXEL_TO_GEO(X, Y, GEOX, GEOY) \
    GEOX = _geotransform[0] + _geotransform[1] * (X) + _geotransform[2] * (Y); \
    GEOY = _geotransform[3] + _geotransform[4] * (X) + _geotransform[5] * (Y)

#define GEO_TO_PIXEL(GEOX, GEOY, OUTX, OUTY) \
    OUTX = _invtransform[0] + _invtransform[1] * (GEOX) + _invtransform[2] * (GEOY); \
    OUTY = _invtransform[3] + _invtransform[4] * (GEOX) + _invtransform[5] * (GEOY); \
    if (equivalent(OUTX, 0.0, 0.0001)) OUTX = 0; \
    if (equivalent(OUTY, 0.0, 0.0001)) OUTY = 0; \
    if (equivalent(OUTX, (double)_warpedDS->GetRasterXSize(), 0.0001)) OUTX = _warpedDS->GetRasterXSize(); \
    if (equivalent(OUTY, (double)_warpedDS->GetRasterYSize(), 0.0001)) OUTY = _warpedDS->GetRasterYSize()


namespace osgEarth
{
    // From easyrgb.com
    inline float Hue_2_RGB(float v1, float v2, float vH)
    {
        if (vH < 0.0f) vH += 1.0f;
        if (vH > 1.0f) vH -= 1.0f;
        if ((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
        if ((2.0f * vH) < 1.0f) return (v2);
        if ((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
        return (v1);
    }

    // This is simply the method GDALAutoCreateWarpedVRT() with the GDALSuggestedWarpOutput
    // logic replaced with something that will work properly for polar projections.
    // see: http://www.mail-archive.com/gdal-dev@lists.osgeo.org/msg01491.html
    inline GDALDatasetH GDALAutoCreateWarpedVRTforPolarStereographic(
        GDALDatasetH hSrcDS,
        const char* pszSrcWKT,
        const char* pszDstWKT,
        GDALResampleAlg eResampleAlg,
        double dfMaxError,
        const GDALWarpOptions* psOptionsIn)
    {
        GDALWarpOptions* psWO;
        int i;

        VALIDATE_POINTER1(hSrcDS, "GDALAutoCreateWarpedVRTForPolarStereographic", NULL);

        /* -------------------------------------------------------------------- */
        /*      Populate the warp options.                                      */
        /* -------------------------------------------------------------------- */
        if (psOptionsIn != NULL)
            psWO = GDALCloneWarpOptions(psOptionsIn);
        else
            psWO = GDALCreateWarpOptions();

        psWO->eResampleAlg = eResampleAlg;

        psWO->hSrcDS = hSrcDS;

        psWO->nBandCount = GDALGetRasterCount(hSrcDS);
        psWO->panSrcBands = (int*)CPLMalloc(sizeof(int) * psWO->nBandCount);
        psWO->panDstBands = (int*)CPLMalloc(sizeof(int) * psWO->nBandCount);

        for (i = 0; i < psWO->nBandCount; i++)
        {
            psWO->panSrcBands[i] = i + 1;
            psWO->panDstBands[i] = i + 1;
        }

        /* TODO: should fill in no data where available */

        /* -------------------------------------------------------------------- */
        /*      Create the transformer.                                         */
        /* -------------------------------------------------------------------- */
        psWO->pfnTransformer = GDALGenImgProjTransform;
        psWO->pTransformerArg =
            GDALCreateGenImgProjTransformer(psWO->hSrcDS, pszSrcWKT,
                NULL, pszDstWKT,
                TRUE, 1.0, 0);

        if (psWO->pTransformerArg == NULL)
        {
            GDALDestroyWarpOptions(psWO);
            return NULL;
        }

        /* -------------------------------------------------------------------- */
        /*      Figure out the desired output bounds and resolution.            */
        /* -------------------------------------------------------------------- */
        double adfDstGeoTransform[6];
        int    nDstPixels, nDstLines;
        CPLErr eErr;

        eErr =
            GDALSuggestedWarpOutput(hSrcDS, psWO->pfnTransformer,
                psWO->pTransformerArg,
                adfDstGeoTransform, &nDstPixels, &nDstLines);

        // override the suggestions:
        nDstPixels = GDALGetRasterXSize(hSrcDS) * 4;
        nDstLines = GDALGetRasterYSize(hSrcDS) / 2;
        adfDstGeoTransform[0] = -180.0;
        adfDstGeoTransform[1] = 360.0 / (double)nDstPixels;
        //adfDstGeoTransform[2] = 0.0;
        //adfDstGeoTransform[4] = 0.0;
        //adfDstGeoTransform[5] = (-90 -adfDstGeoTransform[3])/(double)nDstLines;

        /* -------------------------------------------------------------------- */
        /*      Update the transformer to include an output geotransform        */
        /*      back to pixel/line coordinates.                                 */
        /*                                                                      */
        /* -------------------------------------------------------------------- */
        GDALSetGenImgProjTransformerDstGeoTransform(
            psWO->pTransformerArg, adfDstGeoTransform);

        /* -------------------------------------------------------------------- */
        /*      Do we want to apply an approximating transformation?            */
        /* -------------------------------------------------------------------- */
        if (dfMaxError > 0.0)
        {
            psWO->pTransformerArg =
                GDALCreateApproxTransformer(psWO->pfnTransformer,
                    psWO->pTransformerArg,
                    dfMaxError);
            psWO->pfnTransformer = GDALApproxTransform;
        }

        /* -------------------------------------------------------------------- */
        /*      Create the VRT file.                                            */
        /* -------------------------------------------------------------------- */
        GDALDatasetH hDstDS;

        hDstDS = GDALCreateWarpedVRT(hSrcDS, nDstPixels, nDstLines,
            adfDstGeoTransform, psWO);

        GDALDestroyWarpOptions(psWO);

        if (pszDstWKT != NULL)
            GDALSetProjection(hDstDS, pszDstWKT);
        else if (pszSrcWKT != NULL)
            GDALSetProjection(hDstDS, pszDstWKT);
        else if (GDALGetGCPCount(hSrcDS) > 0)
            GDALSetProjection(hDstDS, GDALGetGCPProjection(hSrcDS));
        else
            GDALSetProjection(hDstDS, GDALGetProjectionRef(hSrcDS));

        return hDstDS;
    }

    /**
     * Gets the GeoExtent of the given filename.
     */
    GeoExtent getGeoExtent(std::string& filename)
    {
        GDALDataset* ds = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
        if (!ds)
        {
            return GeoExtent::INVALID;
        }

        // Get the geotransforms
        double geotransform[6];
        ds->GetGeoTransform(geotransform);

        double minX, minY, maxX, maxY;

        GDALApplyGeoTransform(geotransform, 0.0, ds->GetRasterYSize(), &minX, &minY);
        GDALApplyGeoTransform(geotransform, ds->GetRasterXSize(), 0.0, &maxX, &maxY);

        std::string srsString = ds->GetProjectionRef();
        const SpatialReference* srs = SpatialReference::create(srsString);

        GDALClose(ds);

        GeoExtent ext(srs, minX, minY, maxX, maxY);
        return ext;
    }
    /**
    * Finds a raster band based on color interpretation
    */
    GDALRasterBand* findBandByColorInterp(GDALDataset* ds, GDALColorInterp colorInterp)
    {
        for (int i = 1; i <= ds->GetRasterCount(); ++i)
        {
            if (ds->GetRasterBand(i)->GetColorInterpretation() == colorInterp) return ds->GetRasterBand(i);
        }
        return 0;
    }

    GDALRasterBand* findBandByDataType(GDALDataset* ds, GDALDataType dataType)
    {
        for (int i = 1; i <= ds->GetRasterCount(); ++i)
        {
            if (ds->GetRasterBand(i)->GetRasterDataType() == dataType) return ds->GetRasterBand(i);
        }
        return 0;
    }

    bool getPalleteIndexColor(GDALRasterBand* band, int index, osg::Vec4ub& color)
    {
        const GDALColorEntry* colorEntry = band->GetColorTable()->GetColorEntry(index);
        GDALPaletteInterp interp = band->GetColorTable()->GetPaletteInterpretation();
        if (!colorEntry)
        {
            //FIXME: What to do here?

            //OE_INFO << "NO COLOR ENTRY FOR COLOR " << rawImageData[i] << std::endl;
            color.r() = 255;
            color.g() = 0;
            color.b() = 0;
            color.a() = 1;
            return false;
        }
        else
        {
            if (interp == GPI_RGB)
            {
                color.r() = colorEntry->c1;
                color.g() = colorEntry->c2;
                color.b() = colorEntry->c3;
                color.a() = colorEntry->c4;
            }
            else if (interp == GPI_CMYK)
            {
                // from wikipedia.org
                short C = colorEntry->c1;
                short M = colorEntry->c2;
                short Y = colorEntry->c3;
                short K = colorEntry->c4;
                color.r() = 255 - C * (255 - K) - K;
                color.g() = 255 - M * (255 - K) - K;
                color.b() = 255 - Y * (255 - K) - K;
                color.a() = 255;
            }
            else if (interp == GPI_HLS)
            {
                // from easyrgb.com
                float H = colorEntry->c1;
                float S = colorEntry->c3;
                float L = colorEntry->c2;
                float R, G, B;
                if (S == 0)                       //HSL values = 0 - 1
                {
                    R = L;                      //RGB results = 0 - 1
                    G = L;
                    B = L;
                }
                else
                {
                    float var_2, var_1;
                    if (L < 0.5)
                        var_2 = L * (1 + S);
                    else
                        var_2 = (L + S) - (S * L);

                    var_1 = 2 * L - var_2;

                    R = Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f));
                    G = Hue_2_RGB(var_1, var_2, H);
                    B = Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
                }
                color.r() = static_cast<unsigned char>(R * 255.0f);
                color.g() = static_cast<unsigned char>(G * 255.0f);
                color.b() = static_cast<unsigned char>(B * 255.0f);
                color.a() = static_cast<unsigned char>(255.0f);
            }
            else if (interp == GPI_Gray)
            {
                color.r() = static_cast<unsigned char>(colorEntry->c1 * 255.0f);
                color.g() = static_cast<unsigned char>(colorEntry->c1 * 255.0f);
                color.b() = static_cast<unsigned char>(colorEntry->c1 * 255.0f);
                color.a() = static_cast<unsigned char>(255.0f);
            }
            else
            {
                return false;
            }
            return true;
        }
    }

    template<typename T>
    inline void applyScaleAndOffset(void* data, int count, double scale, double offset)
    {
        T* f = (T*)data;
        for (int i = 0; i < count; ++i)
        {
            double value = static_cast<double>(*f) * scale + offset;
            *f++ = static_cast<T>(value);
        }
    }

    void applyScaleAndOffset(GDALRasterBand* band, void* pData, GDALDataType eBufType, int nBufXSize, int nBufYSize)
    {
        double scale = band->GetScale();
        double offset = band->GetOffset();

        if (scale != 1.0 || offset != 0.0)
        {
            int count = nBufXSize * nBufYSize;

            if (eBufType == GDT_Float32)
                applyScaleAndOffset<float>(pData, count, scale, offset);
            else if (eBufType == GDT_Float64)
                applyScaleAndOffset<double>(pData, count, scale, offset);
            else if (eBufType == GDT_Int16)
                applyScaleAndOffset<short>(pData, count, scale, offset);
            else if (eBufType == GDT_Int32)
                applyScaleAndOffset<int>(pData, count, scale, offset);
            else if (eBufType == GDT_Byte)
                applyScaleAndOffset<char>(pData, count, scale, offset);
        }
    }

    // GDALRasterBand::RasterIO helper method
    bool rasterIO(
        GDALRasterBand* band,
        GDALRWFlag eRWFlag,
        double dXOff,
        double dYOff,
        double dXSize,
        double dYSize,
        void* pData,
        int nBufXSize,
        int nBufYSize,
        GDALDataType eBufType,
        GSpacing nPixelSpace,
        GSpacing nLineSpace,
        RasterInterpolation interpolation = INTERP_NEAREST
    )
    {
        GDALRasterIOExtraArg psExtraArg;

        // defaults to GRIORA_NearestNeighbour
        INIT_RASTERIO_EXTRA_ARG(psExtraArg);

        switch (interpolation)
        {
        case INTERP_AVERAGE:
            //psExtraArg.eResampleAlg = GRIORA_Average;
            // for some reason gdal's average resampling produces artifacts occasionally for imagery at higher levels.
            // for now we'll just use bilinear interpolation under the hood until we can understand what is going on.
            psExtraArg.eResampleAlg = GRIORA_Bilinear;
            break;
        case INTERP_BILINEAR:
            psExtraArg.eResampleAlg = GRIORA_Bilinear;
            break;
        case INTERP_CUBIC:
            psExtraArg.eResampleAlg = GRIORA_Cubic;
            break;
        case INTERP_CUBICSPLINE:
            psExtraArg.eResampleAlg = GRIORA_CubicSpline;
            break;
        }

        // pass in double extents instead of int
        psExtraArg.bFloatingPointWindowValidity = TRUE;
        psExtraArg.dfXOff = dXOff;
        psExtraArg.dfYOff = dYOff;
        psExtraArg.dfXSize = dXSize;
        psExtraArg.dfYSize = dYSize;

        CPLErr err = band->RasterIO(eRWFlag, floor(dXOff), floor(dYOff), ceil(dXSize), ceil(dYSize), pData, nBufXSize, nBufYSize, eBufType, nPixelSpace, nLineSpace, &psExtraArg);

        if (err != CE_None)
        {
            //OE_WARN << LC << "RasterIO failed.\n";
        }
        else
        {
            applyScaleAndOffset(band, pData, eBufType, nBufXSize, nBufYSize);
        }

        return (err == CE_None);
    }
} // namespace osgEarth

//...................................................................


GDAL_detail::Driver::~Driver()
{
    if (_warpedDS)
        GDALClose(_warpedDS);
    else if (_srcDS)
        GDALClose(_srcDS);
}

void
GDAL_detail::Driver::setExternalDataset(GDAL_detail::ExternalDataset* value)
{
    _externalDataset = value;
}

// Open the data source and prepare it for reading
Status
GDAL_detail::Driver::open(
    const std::string& name,
    const GDAL_detail::Options& options,
    unsigned tileSize,
    const Profile* fallback_profile,
    DataExtentList* layerDataExtents,
    const osgDB::Options* readOptions,
    bool verbose)
{
    _name = name;
    _gdalOptions = options;

    // Is a valid external GDAL dataset specified ?
    bool useExternalDataset = false;
    if (_externalDataset.valid() && _externalDataset->dataset() != NULL)
    {
        useExternalDataset = true;
    }

    if (useExternalDataset == false &&
        (!gdalOptions().url().isSet() || gdalOptions().url()->empty()) &&
        (!gdalOptions().connection().isSet() || gdalOptions().connection()->empty()))
    {
        return Status::Error(Status::ConfigurationError, "No URL, directory, or connection string specified");
    }

    // source connection:
    std::string source;
    bool isFile = true;

    if (gdalOptions().url().isSet())
    {
        // Use the base instead of the full if this is a gdal virtual file system
        if (startsWith(gdalOptions().url()->base(), "/vsi"))
        {
            source = gdalOptions().url()->base();
        }
        else
        {
            source = gdalOptions().url()->full();
        }
    }
    else if (gdalOptions().connection().isSet())
    {
        source = gdalOptions().connection().get();
        isFile = false;
    }

    if (useExternalDataset == false)
    {
        std::string input = source;

        if (input.empty())
        {
            return Status::Error(Status::ResourceUnavailable, "Could not find any valid input.");
        }

        // Resolve the pathname...
        if (isFile && !osgDB::fileExists(input))
        {
            std::string found = osgDB::findDataFile(input);
            if (!found.empty())
                input = found;
        }

        // Create the source dataset:
        _srcDS = (GDALDataset*)GDALOpen(input.c_str(), GA_ReadOnly);
        if (_srcDS)
        {
            // Use auto for compatibility: older GDAL returns char**, newer GDAL returns CSLConstList (const char* const*)
            auto subDatasets = _srcDS->GetMetadata("SUBDATASETS");
            int numSubDatasets = CSLCount(subDatasets);

            if (numSubDatasets > 0)
            {
                int subDataset = gdalOptions().subDataSet().isSet() ? *gdalOptions().subDataSet() : 1;
                if (subDataset < 1 || subDataset > numSubDatasets) subDataset = 1;
                std::stringstream buf;
                buf << "SUBDATASET_" << subDataset << "_NAME";
                char *pszSubdatasetName = CPLStrdup(CSLFetchNameValue(subDatasets, buf.str().c_str()));
                GDALClose(_srcDS);
                _srcDS = (GDALDataset*)GDALOpen(pszSubdatasetName, GA_ReadOnly);
                CPLFree(pszSubdatasetName);
            }
        }

        if (!_srcDS)
        {
            return Status::Error(Status::ResourceUnavailable, Stringify() << "Failed to open " << input);
        }
    }
    else
    {
        _srcDS = _externalDataset->dataset();
    }

    // Establish the source spatial reference:
    osg::ref_ptr<const SpatialReference> src_srs;

    std::string srcProj = _srcDS->GetProjectionRef();

    // If the projection is empty and we have GCP's then use the GCP projection.
    if (srcProj.empty() && _srcDS->GetGCPCount() > 0)
    {
        srcProj = _srcDS->GetGCPProjection();
    }

    if (!srcProj.empty())
    {
        src_srs = SpatialReference::create(srcProj);
    }

    // still no luck? (for example, an ungeoreferenced file lika jpeg?)
    // try to read a .prj file:
    if (!src_srs.valid())
    {
        // not found in the dataset; try loading a .prj file
        std::string prjLocation = osgDB::getNameLessExtension(source) + std::string(".prj");

        ReadResult r = URI(prjLocation).readString(readOptions);
        if (r.succeeded())
        {
            src_srs = SpatialReference::create(Strings::trim(r.getString()));
        }
    }

    if (!src_srs.valid())
    {
        if (fallback_profile)
        {
            _profile = fallback_profile;
            src_srs = fallback_profile->getSRS();
            if (verbose)
                OE_INFO << LC << source << ": using fallback profile " << fallback_profile->toString() << std::endl;
        }
        else
        {
            return Status::Error(Status::ResourceUnavailable,
                "Dataset has no spatial reference information (" + source + ")");
        }
    }

    // These are the actual extents of the data:
    bool hasGCP = false;
    bool isRotated = false;
    bool requiresReprojection = false;
    
    bool hasGeoTransform = (_srcDS->GetGeoTransform(_geotransform) == CE_None);

    hasGCP = _srcDS->GetGCPCount() > 0 && _srcDS->GetGCPProjection();
    isRotated = hasGeoTransform && (_geotransform[2] != 0.0 || _geotransform[4] != 0.0);
    requiresReprojection = hasGCP || isRotated;

    // For a geographic SRS, use the whole-globe profile for performance.
    // Otherwise, collect information and make the profile later.
    if (src_srs->isGeographic())
    {
        if (verbose)
            OE_DEBUG << LC << source << ": creating Profile from source's geographic SRS: " << src_srs->getName() << std::endl;

        if (!_profile)
        {
            _profile = Profile::create(src_srs.get());
        }

        if (!_profile.valid())
        {
            return Status::Error(Status::ResourceUnavailable,
                "Cannot create geographic Profile from dataset's spatial reference information: " + src_srs->getName());
        }

        // no xform an geographic? Match the profile.
        if (!hasGeoTransform)
        {
            _geotransform[0] = _profile->getExtent().xMin();
            _geotransform[1] = _profile->getExtent().width() / (double)_srcDS->GetRasterXSize();
            _geotransform[2] = 0;
            _geotransform[3] = _profile->getExtent().yMax();
            _geotransform[4] = 0;
            _geotransform[5] = -_profile->getExtent().height() / (double)_srcDS->GetRasterYSize();
            hasGeoTransform = true;
        }
    }

    // Handle some special cases.
    std::string warpedSRSWKT;

    if (requiresReprojection || (_profile.valid() && !_profile->getSRS()->isEquivalentTo(src_srs.get())))
    {
        if (_profile.valid() && _profile->getSRS()->isGeographic() && (src_srs->isNorthPolar() || src_srs->isSouthPolar()))
        {
            _warpedDS = (GDALDataset*)GDALAutoCreateWarpedVRTforPolarStereographic(
                _srcDS,
                src_srs->getWKT().c_str(),
                _profile->getSRS()->getWKT().c_str(),
                GRA_NearestNeighbour,
                5.0,
                nullptr);
        }
        else
        {
            std::string destWKT = _profile.valid() ? _profile->getSRS()->getWKT() : src_srs->getWKT();
            _warpedDS = (GDALDataset*)GDALAutoCreateWarpedVRT(
                _srcDS,
                src_srs->getWKT().c_str(),
                destWKT.c_str(),
                GRA_NearestNeighbour,
                5.0,
                0);
        }

        if (_warpedDS)
        {
            warpedSRSWKT = _warpedDS->GetProjectionRef();
            _warpedDS->GetGeoTransform(_geotransform);
        }
    }
    else
    {
        _warpedDS = _srcDS;
        warpedSRSWKT = src_srs->getWKT();
    }

    if (!_warpedDS)
    {
        return Status::Error("Failed to create a final sampling dataset");
    }

    // calcluate the inverse of the geotransform:
    if (GDALInvGeoTransform(_geotransform, _invtransform) == FALSE)
    {
        OE_DEBUG << LC << "GDALInvGeoTransform failed" << std::endl;
    }

    int ds_ysize = _warpedDS->GetRasterYSize();
    int ds_xsize = _warpedDS->GetRasterXSize();

    double minX, minY, maxX, maxY;
    PIXEL_TO_GEO(0.0, ds_ysize, minX, minY);
    PIXEL_TO_GEO(ds_xsize, 0.0, maxX, maxY);

    // record the AREA_OR_POINT metadata if available (default to AREA)
    auto* pora = _warpedDS->GetMetadataItem("AREA_OR_POINT");
    _pixelIsArea = (pora == nullptr) || toLower(std::string(pora)) == "area";

    //OE_INFO << LC << INDENT << gdalOptions().url()->full() << ": pixelToGeo: " << minX << ", " << minY << " -> " << maxX << ", " << maxY << std::endl;

    // If we don't have a profile yet, that means this is a projected dataset
    // so we will create the profile from the actual data extents.
    if (!_profile.valid())
    {
        _profile = Profile::create(
            warpedSRSWKT,
            minX, minY, maxX, maxY);

        if (!_profile.valid())
        {
            return Status::Error("Cannot create projected Profile from dataset's warped spatial reference WKT: " + warpedSRSWKT);
        }
        
        if (verbose)
            OE_DEBUG << LC << gdalOptions().url()->base() << source << " is projected, SRS = " << warpedSRSWKT << std::endl;
    }

    OE_HARD_ASSERT(_profile.valid());

    //Compute the min and max data levels
    double resolutionX = (maxX - minX) / (double)ds_xsize;
    double resolutionY = (maxY - minY) / (double)ds_ysize;
    double maxResolution = osg::minimum(resolutionX, resolutionY);

    if (verbose)
        OE_INFO << LC << source << ": resolution= " << resolutionX << "x" << resolutionY << " max=" << maxResolution << std::endl;

    if (_maxDataLevel.isSet())
    {
        if (verbose)
            OE_DEBUG << LC << source << ": override max data level= " << _maxDataLevel.get() << std::endl;
    }
    else if (!std::isnan(maxResolution) && maxResolution > 0.0)
    {
        _maxDataLevel = 0u;
        double w, h;
        _profile->getTileDimensions(0, w, h);
        w /= (double)tileSize, h /= (double)tileSize;
        while(w >= maxResolution && h >= maxResolution)
        {
            _maxDataLevel = _maxDataLevel.value() + 1;
            w *= 0.5, h *= 0.5;
        }

        if (verbose)
            OE_INFO << LC << source << ": max data level= " << _maxDataLevel.get() << std::endl;
    }

    // If the input dataset is a VRT, then get the individual files in the dataset and use THEM for the DataExtents.
    // A VRT will create a potentially very large virtual dataset from sparse datasets, so using the extents from the underlying files
    // will allow osgEarth to only create tiles where there is actually data.
    DataExtentList dataExtents;

    osg::ref_ptr< SpatialReference > srs = SpatialReference::create(warpedSRSWKT);

    // record the data extent in profile space:
    _bounds = Bounds(minX, minY, 0.0, maxX, maxY, 0.0);

    bool clamped = false;
    if (srs->isGeographic())
    {
        if (_pixelIsArea && (_bounds.xMin() < -180.0 || _bounds.xMax() > 180.0))
        {
            _bounds.xMin() += resolutionX * 0.5;
            _bounds.xMax() -= resolutionX * 0.5;
        }

        if ((_bounds.xMax() - _bounds.xMin()) > 360.0)
        {
            _bounds.xMin() = -180;
            _bounds.xMax() = 180;
            clamped = true;
        }

        if (_pixelIsArea && (_bounds.yMin() < -90.0 || _bounds.yMax() > 90.0))
        {
            _bounds.yMin() += resolutionY * 0.5;
            _bounds.yMax() -= resolutionY * 0.5;
        }

        if ((_bounds.yMax() - _bounds.yMin()) > 180)
        {
            _bounds.yMin() = -90;
            _bounds.yMax() = 90;
            clamped = true;
        }
        if (clamped)
        {
            if (verbose)
                OE_DEBUG << LC << source << ": clamped out-of-range geographic extents" << std::endl;
        }
    }
    _extents = GeoExtent(srs.get(), _bounds);

    if (verbose)
        OE_DEBUG << LC << source << ": GeoExtent = " << _extents.toString() << std::endl;

    if (layerDataExtents)
    {
        GeoExtent profile_extent = _extents.transform(_profile->getSRS());
        if (dataExtents.empty())
        {
            // Use the extents of the whole file.
            if (_maxDataLevel.isSet())
                layerDataExtents->push_back(DataExtent(profile_extent, 0, _maxDataLevel.get()));
            else
                layerDataExtents->push_back(DataExtent(profile_extent));
        }
        else
        {
            // Use the DataExtents from the subfiles of the VRT.
            layerDataExtents->insert(layerDataExtents->end(), dataExtents.begin(), dataExtents.end());
        }
    }

    // Get the linear units of the SRS for scaling elevation values
    _linearUnits = srs->getReportedLinearUnits();

    if (verbose)
        OE_DEBUG << LC << source << ": set Profile to " << _profile->toString() << std::endl;

    return STATUS_OK;
}

bool
GDAL_detail::Driver::isValidValue(float v, float noDataValue) const
{
    //Check to see if the value is equal to the bands specified no data
    if (noDataValue == v)
        return false;

    //Check to see if the value is equal to the user specified nodata value
    if (_noDataValue.isSetTo(v))
        return false;

    //Check to see if the user specified a custom min/max
    if (_minValidValue.isSet() && v < _minValidValue.get())
        return false;

    if (_maxValidValue.isSet() && v > _maxValidValue.get())
        return false;

    return true;
}

bool
GDAL_detail::Driver::isValidValue(float v, GDALRasterBand* band) const
{
    float bandNoData = -32767.0f;
    int success;
    float value = band->GetNoDataValue(&success);
    if (success)
    {
        bandNoData = value;
    }

    //Check to see if the value is equal to the bands specified no data
    if (bandNoData == v)
        return false;

    //Check to see if the value is equal to the user specified nodata value
    if (_noDataValue.isSetTo(v))
        return false;

    //Check to see if the user specified a custom min/max
    if (_minValidValue.isSet() && v < _minValidValue.get())
        return false;

    if (_maxValidValue.isSet() && v > _maxValidValue.get())
        return false;

    return true;
}

// pre-GDAL 3.10 path:
// Superceded by the Band::InterpolateAtPoint function that was introduced in GDAL 3.10
// https://github.com/OSGeo/gdal/commit/3b089b2f789a7d1e8af162397767ffc8adf71b21
float
GDAL_detail::Driver::getInterpolatedDEMValue(GDALRasterBand* band, double x, double y, bool applyOffset)
{
    double r, c;
    GEO_TO_PIXEL(x, y, c, r);

    if (applyOffset)
    {
        //Apply half pixel offset
        r -= 0.5;
        c -= 0.5;

        //Account for the half pixel offset in the geotransform.  If the pixel value is -0.5 we are still technically in the dataset
        //since 0,0 is now the center of the pixel.  So, if are within a half pixel above or a half pixel below the dataset just use
        //the edge values
        if (c < 0 && c >= -0.5)
        {
            c = 0;
        }
        else if (c > _warpedDS->GetRasterXSize() - 1 && c <= _warpedDS->GetRasterXSize() - 0.5)
        {
            c = _warpedDS->GetRasterXSize() - 1;
        }

        if (r < 0 && r >= -0.5)
        {
            r = 0;
        }
        else if (r > _warpedDS->GetRasterYSize() - 1 && r <= _warpedDS->GetRasterYSize() - 0.5)
        {
            r = _warpedDS->GetRasterYSize() - 1;
        }
    }

    float result = 0.0f;

    //If the location is outside of the pixel values of the dataset, just return 0
    if (c < 0 || r < 0 || c > _warpedDS->GetRasterXSize() - 1 || r > _warpedDS->GetRasterYSize() - 1)
        return NO_DATA_VALUE;


    if (gdalOptions().interpolation() == INTERP_NEAREST)
    {
        rasterIO(band, GF_Read, osg::round(c), osg::round(r), 1, 1, &result, 1, 1, GDT_Float32, 0, 0);
        if (!isValidValue(result, band))
        {
            return NO_DATA_VALUE;
        }
    }
    else
    {
        int rowMin = osg::maximum((int)floor(r), 0);
        int rowMax = osg::maximum(osg::minimum((int)ceil(r), (int)(_warpedDS->GetRasterYSize() - 1)), 0);
        int colMin = osg::maximum((int)floor(c), 0);
        int colMax = osg::maximum(osg::minimum((int)ceil(c), (int)(_warpedDS->GetRasterXSize() - 1)), 0);

        if (rowMin > rowMax) rowMin = rowMax;
        if (colMin > colMax) colMin = colMax;

        float urHeight, llHeight, ulHeight, lrHeight;

        rasterIO(band, GF_Read, colMin, rowMin, 1, 1, &llHeight, 1, 1, GDT_Float32, 0, 0);
        rasterIO(band, GF_Read, colMin, rowMax, 1, 1, &ulHeight, 1, 1, GDT_Float32, 0, 0);
        rasterIO(band, GF_Read, colMax, rowMin, 1, 1, &lrHeight, 1, 1, GDT_Float32, 0, 0);
        rasterIO(band, GF_Read, colMax, rowMax, 1, 1, &urHeight, 1, 1, GDT_Float32, 0, 0);

        if ((!isValidValue(urHeight, band)) || (!isValidValue(llHeight, band)) || (!isValidValue(ulHeight, band)) || (!isValidValue(lrHeight, band)))
        {
            return NO_DATA_VALUE;
        }

        if (gdalOptions().interpolation() == INTERP_AVERAGE)
        {
            double x_rem = c - (int)c;
            double y_rem = r - (int)r;

            double w00 = (1.0 - y_rem) * (1.0 - x_rem) * (double)llHeight;
            double w01 = (1.0 - y_rem) * x_rem * (double)lrHeight;
            double w10 = y_rem * (1.0 - x_rem) * (double)ulHeight;
            double w11 = y_rem * x_rem * (double)urHeight;

            result = (float)(w00 + w01 + w10 + w11);
        }
        else // if (gdalOptions().interpolation() == INTERP_BILINEAR)
        {
            //Check for exact value
            if ((colMax == colMin) && (rowMax == rowMin))
            {
                result = llHeight;
            }
            else if (colMax == colMin)
            {
                result = ((float)rowMax - r) * llHeight + (r - (float)rowMin) * ulHeight;
            }
            else if (rowMax == rowMin)
            {
                result = ((float)colMax - c) * llHeight + (c - (float)colMin) * lrHeight;
            }
            else
            {
                float r1 = ((float)colMax - c) * llHeight + (c - (float)colMin) * lrHeight;
                float r2 = ((float)colMax - c) * ulHeight + (c - (float)colMin) * urHeight;
                result = ((float)rowMax - r) * r1 + (r - (float)rowMin) * r2;
            }
        }
    }

    return result;
}

bool
GDAL_detail::Driver::intersects(const TileKey& key)
{
    return key.getExtent().intersects(_extents);
}

osg::Image*
GDAL_detail::Driver::createImage(const TileKey& key,
    unsigned tileSize,
    bool isCoverage,
    ProgressCallback* progress)
{
    if (_maxDataLevel.isSet() && key.getLevelOfDetail() > _maxDataLevel.get())
    {
        OE_DEBUG << LC << "Reached maximum data resolution key="
            << key.getLevelOfDetail() << " max=" << _maxDataLevel.get() << std::endl;
        return NULL;
    }

    if (progress && progress->isCanceled())
    {
        return NULL;
    }

    osg::ref_ptr<osg::Image> image;

    //Get the extents of the tile
    double xmin, ymin, xmax, ymax;
    key.getExtent().getBounds(xmin, ymin, xmax, ymax);

    // Compute the intersection of the incoming key with the data extents of the dataset
    osgEarth::GeoExtent intersection = key.getExtent().intersectionSameSRS(_extents);
    if (!intersection.isValid())
    {
        return 0;
    }

    double west = intersection.xMin();
    double east = intersection.xMax();
    double north = intersection.yMax();
    double south = intersection.yMin();

    // The extents and the intersection will be normalized between -180 and 180 longitude if they are geographic.
    // However, the georeferencing will expect the coordinates to be in the same longitude frame as the original dataset,
    // so the intersection bounds are adjusted here if necessary so that the values line up with the georeferencing.
    if (_extents.getSRS()->isGeographic())
    {
        while (west < _bounds.xMin())
        {
            west += 360.0;
            east = west + intersection.width();
        }
        while (west > _bounds.xMax())
        {
            west -= 360.0;
            east = west + intersection.width();
        }
    }

    // Determine the read window
    double src_min_x, src_min_y, src_max_x, src_max_y;
    // Get the pixel coordiantes of the intersection
    GEO_TO_PIXEL(west, intersection.yMax(), src_min_x, src_min_y);
    GEO_TO_PIXEL(east, intersection.yMin(), src_max_x, src_max_y);

    double src_width = src_max_x - src_min_x;
    double src_height = src_max_y - src_min_y;

    int rasterWidth = _warpedDS->GetRasterXSize();
    int rasterHeight = _warpedDS->GetRasterYSize();

    if (src_min_x + src_width > (double)rasterWidth)
    {
        src_width = (double)rasterWidth - src_min_x;
    }

    if (src_min_y + src_height > (double)rasterHeight)
    {
        src_height = (double)rasterHeight - src_min_y;
    }

    // Determine the destination window

    // Compute the offsets in geo coordinates of the intersection from the TileKey
    double offset_left = intersection.xMin() - xmin;
    double offset_top = ymax - intersection.yMax();

    int target_width = (int)ceil((intersection.width() / key.getExtent().width())*(double)tileSize);
    int target_height = (int)ceil((intersection.height() / key.getExtent().height())*(double)tileSize);
    int tile_offset_left = (int)floor((offset_left / key.getExtent().width()) * (double)tileSize);
    int tile_offset_top = (int)floor((offset_top / key.getExtent().height()) * (double)tileSize);

    // Compute spacing
    double dx = (xmax - xmin) / (double)(tileSize - 1);
    double dy = (ymax - ymin) / (double)(tileSize - 1);

    //OE_DEBUG << LC << "ReadWindow " << off_x << "," << off_y << " " << width << "x" << height << std::endl;
    //OE_DEBUG << LC << "DestWindow " << tile_offset_left << "," << tile_offset_top << " " << target_width << "x" << target_height << std::endl;


    //Return if parameters are out of range.
    if (src_width <= 0.0 || src_height <= 0.0 || target_width <= 0 || target_height <= 0)
    {
        return 0;
    }


    GDALRasterBand* bandRed = findBandByColorInterp(_warpedDS, GCI_RedBand);
    GDALRasterBand* bandGreen = findBandByColorInterp(_warpedDS, GCI_GreenBand);
    GDALRasterBand* bandBlue = findBandByColorInterp(_warpedDS, GCI_BlueBand);
    GDALRasterBand* bandAlpha = findBandByColorInterp(_warpedDS, GCI_AlphaBand);

    GDALRasterBand* bandGray = findBandByColorInterp(_warpedDS, GCI_GrayIndex);

    GDALRasterBand* bandPalette = findBandByColorInterp(_warpedDS, GCI_PaletteIndex);

    if (!bandRed && !bandGreen && !bandBlue && !bandAlpha && !bandGray && !bandPalette)
    {
        OE_DEBUG << LC << "Could not determine bands based on color interpretation, using band count" << std::endl;
        //We couldn't find any valid bands based on the color interp, so just make an educated guess based on the number of bands in the file
        //RGB = 3 bands
        if (_warpedDS->GetRasterCount() == 3)
        {
            bandRed = _warpedDS->GetRasterBand(1);
            bandGreen = _warpedDS->GetRasterBand(2);
            bandBlue = _warpedDS->GetRasterBand(3);
        }
        //RGBA = 4 bands
        else if (_warpedDS->GetRasterCount() == 4)
        {
            bandRed = _warpedDS->GetRasterBand(1);
            bandGreen = _warpedDS->GetRasterBand(2);
            bandBlue = _warpedDS->GetRasterBand(3);
            bandAlpha = _warpedDS->GetRasterBand(4);
        }
        //Gray = 1 band
        else if (_warpedDS->GetRasterCount() == 1)
        {
            bandGray = _warpedDS->GetRasterBand(1);
        }
        //Gray + alpha = 2 bands
        else if (_warpedDS->GetRasterCount() == 2)
        {
            bandGray = _warpedDS->GetRasterBand(1);
            bandAlpha = _warpedDS->GetRasterBand(2);
        }
    }



    //The pixel format is always RGBA to support transparency
    GLenum pixelFormat = GL_RGBA;


    if (bandRed && bandGreen && bandBlue)
    {
        auto channelSize = target_width * target_height;

        OE_THREAD_LOCAL std::vector<unsigned char> red, green, blue, alpha;
        if (red.size() < channelSize) red.resize(channelSize);
        if (green.size() < channelSize) green.resize(channelSize);
        if (blue.size() < channelSize) blue.resize(channelSize);
        if (alpha.size() < channelSize) alpha.resize(channelSize);

        //Initialize the alpha values to 255.
        memset(alpha.data(), 255, target_width * target_height);

        image = new osg::Image;
        image->allocateImage(tileSize, tileSize, 1, pixelFormat, GL_UNSIGNED_BYTE);
        memset(image->data(), 0, image->getImageSizeInBytes());

        rasterIO(bandRed, GF_Read, src_min_x, src_min_y, src_width, src_height, red.data(), target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation().get());
        rasterIO(bandGreen, GF_Read, src_min_x, src_min_y, src_width, src_height, green.data(), target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation().get());
        rasterIO(bandBlue, GF_Read, src_min_x, src_min_y, src_width, src_height, blue.data(), target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation().get());

        if (bandAlpha)
        {
            rasterIO(bandAlpha, GF_Read, src_min_x, src_min_y, src_width, src_height, alpha.data(), target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation().get());
        }

        for (int src_row = 0, dst_row = tile_offset_top;
            src_row < target_height;
            src_row++, dst_row++)
        {
            unsigned int flippedRow = tileSize - dst_row - 1;
            for (int src_col = 0, dst_col = tile_offset_left;
                src_col < target_width;
                ++src_col, ++dst_col)
            {
                unsigned char r = red[src_col + src_row * target_width];
                unsigned char g = green[src_col + src_row * target_width];
                unsigned char b = blue[src_col + src_row * target_width];
                unsigned char a = alpha[src_col + src_row * target_width];
                *(image->data(dst_col, flippedRow) + 0) = r;
                *(image->data(dst_col, flippedRow) + 1) = g;
                *(image->data(dst_col, flippedRow) + 2) = b;
                if (!isValidValue(r, bandRed) ||
                    !isValidValue(g, bandGreen) ||
                    !isValidValue(b, bandBlue) ||
                    (bandAlpha && !isValidValue(a, bandAlpha)))
                {
                    a = 0.0f;
                }
                *(image->data(dst_col, flippedRow) + 3) = a;
            }
        }
    }
    else if (bandGray)
    {
        if (isCoverage)
        {
            GDALDataType gdalDataType = bandGray->GetRasterDataType();

            int gdalSampleSize =
                (gdalDataType == GDT_Byte) ? 1 :
                (gdalDataType == GDT_UInt16 || gdalDataType == GDT_Int16) ? 2 :
                4;

            // Create an un-normalized image to hold coverage values.
            image = LandCover::createImage(tileSize);

            ImageUtils::PixelWriter write(image.get());

            // initialize all coverage texels to NODATA. -gw
            write.assign(Color(NO_DATA_VALUE));

            // coverage data; one channel data that is not subject to interpolated values
            OE_THREAD_LOCAL std::vector<unsigned char> data;
            if (data.size() < target_width * target_height * gdalSampleSize)
                data.resize(target_width * target_height * gdalSampleSize);

            memset(data.data(), 0, target_width * target_height * gdalSampleSize);

            osg::Vec4 temp;

            int success;
            float nodata = bandGray->GetNoDataValue(&success);
            if (!success)
                nodata = NO_DATA_VALUE;

            if (rasterIO(bandGray, GF_Read, src_min_x, src_min_y, src_width, src_height, data.data(), target_width, target_height, gdalDataType, 0, 0, INTERP_NEAREST))
            {
                // copy from data to image.
                for (int src_row = 0, dst_row = tile_offset_top; src_row < target_height; src_row++, dst_row++)
                {
                    unsigned int flippedRow = tileSize - dst_row - 1;
                    for (int src_col = 0, dst_col = tile_offset_left; src_col < target_width; ++src_col, ++dst_col)
                    {
                        unsigned char* ptr = &data[(src_col + src_row * target_width)*gdalSampleSize];

                        float value =
                            gdalSampleSize == 1 ? (float)(*ptr) :
                            gdalSampleSize == 2 ? (float)*(unsigned short*)ptr :
                            gdalSampleSize == 4 ? *(float*)ptr :
                            NO_DATA_VALUE;

                        if (!isValidValue(value, bandGray))
                            value = NO_DATA_VALUE;

                        temp.r() = value;
                        write(temp, dst_col, flippedRow);
                    }
                }
            }
            else // err != CE_None
            {
                OE_WARN << LC << "RasterIO failed.\n";
                // TODO - handle error condition
            }
        }

        else // greyscale image (not a coverage)
        {
            auto channelSize = target_width * target_height;

            OE_THREAD_LOCAL std::vector<unsigned char> gray, alpha;
            if (gray.size() < channelSize) gray.resize(channelSize);
            if (alpha.size() < channelSize) alpha.resize(channelSize);

            //Initialize the alpha values to 255.
            memset(alpha.data(), 255, target_width * target_height);

            image = new osg::Image;
            image->allocateImage(tileSize, tileSize, 1, pixelFormat, GL_UNSIGNED_BYTE);
            memset(image->data(), 0, image->getImageSizeInBytes());


            rasterIO(bandGray, GF_Read, src_min_x, src_min_y, src_width, src_height, gray.data(), target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation().get());

            if (bandAlpha)
            {
                rasterIO(bandAlpha, GF_Read, src_min_x, src_min_y, src_width, src_height, alpha.data(), target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation().get());
            }

            for (int src_row = 0, dst_row = tile_offset_top;
                src_row < target_height;
                src_row++, dst_row++)
            {
                unsigned int flippedRow = tileSize - dst_row - 1;
                for (int src_col = 0, dst_col = tile_offset_left;
                    src_col < target_width;
                    ++src_col, ++dst_col)
                {
                    unsigned char g = gray[src_col + src_row * target_width];
                    unsigned char a = alpha[src_col + src_row * target_width];
                    *(image->data(dst_col, flippedRow) + 0) = g;
                    *(image->data(dst_col, flippedRow) + 1) = g;
                    *(image->data(dst_col, flippedRow) + 2) = g;
                    if (!isValidValue(g, bandGray) ||
                        (bandAlpha && !isValidValue(a, bandAlpha)))
                    {
                        a = 0.0f;
                    }
                    *(image->data(dst_col, flippedRow) + 3) = a;
                }
            }
        }
    }
    else if (bandPalette)
    {
        auto channelSize = target_width * target_height;

        OE_THREAD_LOCAL std::vector<unsigned char> palette;
        if (palette.size() < channelSize) palette.resize(channelSize);

        //Palette indexed imagery doesn't support interpolation currently and only uses nearest
        //b/c interpolating palette indexes doesn't make sense.

        if (isCoverage == true)
        {
            image = LandCover::createImage(tileSize);

            // initialize all coverage texels to NODATA. -gw
            ImageUtils::PixelWriter write(image.get());
            write.assign(Color(NO_DATA_VALUE));
        }
        else
        {
            image = new osg::Image();
            image->allocateImage(tileSize, tileSize, 1, pixelFormat, GL_UNSIGNED_BYTE);
            memset(image->data(), 0, image->getImageSizeInBytes());
        }

        rasterIO(bandPalette, GF_Read, src_min_x, src_min_y, src_width, src_height, palette.data(), target_width, target_height, GDT_Byte, 0, 0, INTERP_NEAREST);

        ImageUtils::PixelWriter write(image.get());

        osg::Vec4f pixel;

        for (int src_row = 0, dst_row = tile_offset_top;
            src_row < target_height;
            src_row++, dst_row++)
        {
            unsigned int flippedRow = tileSize - dst_row - 1;
            for (int src_col = 0, dst_col = tile_offset_left;
                src_col < target_width;
                ++src_col, ++dst_col)
            {
                unsigned char p = palette[src_col + src_row * target_width];

                if (isCoverage)
                {
                    if (_gdalOptions.coverageUsesPaletteIndex() == true)
                    {
                        pixel.r() = (float)p;
                    }
                    else
                    {
                        osg::Vec4ub color;
                        if (getPalleteIndexColor(bandPalette, p, color) &&
                            isValidValue((float)color.r(), bandPalette)) // need this?
                        {
                            pixel.r() = (float)color.r();
                        }
                        else
                        {
                            pixel.r() = NO_DATA_VALUE;
                        }
                    }

                    write(pixel, dst_col, flippedRow);
                }
                else
                {
                    osg::Vec4ub color;
                    if (!getPalleteIndexColor(bandPalette, p, color))
                    {
                        color.a() = 0.0f;
                    }
                    else if (!isValidValue((float)color.r(), bandPalette)) // is this applicable for palettized data?
                    {
                        color.a() = 0.0f;
                    }

                    *(image->data(dst_col, flippedRow) + 0) = color.r();
                    *(image->data(dst_col, flippedRow) + 1) = color.g();
                    *(image->data(dst_col, flippedRow) + 2) = color.b();
                    *(image->data(dst_col, flippedRow) + 3) = color.a();
                }
            }
        }
    }
    else
    {
        OE_WARN
            << LC << "Could not find red, green and blue bands or gray bands in "
            << gdalOptions().url()->full()
            << ".  Cannot create image. " << std::endl;

        return NULL;
    }

    return image.release();
}

osg::HeightField*
GDAL_detail::Driver::createHeightField(const TileKey& key, unsigned tileSize, ProgressCallback* progress)
{
    if (_maxDataLevel.isSet() && key.getLevelOfDetail() > _maxDataLevel.get())
    {
        //OE_NOTICE << "Reached maximum data resolution key=" << key.getLevelOfDetail() << " max=" << _maxDataLevel <<  std::endl;
        return NULL;
    }

    // Allocate the heightfield
    osg::ref_ptr<osg::HeightField> hf = new osg::HeightField;
    hf->allocate(tileSize, tileSize);

    if (intersects(key))
    {
        // Extract the extents of the tile
        double tile_xmin, tile_ymin, tile_xmax, tile_ymax;
        key.getExtent().getBounds(tile_xmin, tile_ymin, tile_xmax, tile_ymax);

        // Sampling intervals:
        double dx = (tile_xmax - tile_xmin) / (tileSize - 1);
        double dy = (tile_ymax - tile_ymin) / (tileSize - 1);

        // Assume the first band contains our data
        auto* band = _warpedDS->GetRasterBand(1);

        // Raw pointer to the height data output block:
        float* hf_raw = (float*)hf->getFloatArray()->getDataPointer();

        int hasNoDataValue = 0;
        float noDataValue = band->GetNoDataValue(&hasNoDataValue);
        if (!hasNoDataValue) noDataValue = NO_DATA_VALUE;

        // If the interpolation is not nearest neighbor, we will use the
        // high-res sampling path. This is not terribly fast but it's accurate.
        if (gdalOptions().interpolation() != INTERP_NEAREST)
        {
            bool done = false;

#if GDAL_VERSION_NUM >= 3100000 // 3.10+

            if (!hasNoDataValue)
            {
                double ri, ci;

                GDALRIOResampleAlg alg =
                    gdalOptions().interpolation() == INTERP_AVERAGE ? GRIORA_Average : // note: broken
                    gdalOptions().interpolation() == INTERP_BILINEAR ? GRIORA_Bilinear :
                    gdalOptions().interpolation() == INTERP_CUBIC ? GRIORA_Cubic :
                    gdalOptions().interpolation() == INTERP_CUBICSPLINE ? GRIORA_CubicSpline :
                    GRIORA_NearestNeighbour;

                for (unsigned r = 0; r < tileSize; ++r)
                {
                    double y = tile_ymin + (dy * (double)r);
                    for (unsigned c = 0; c < tileSize; ++c)
                    {
                        double x = tile_xmin + (dx * (double)c);
                        GEO_TO_PIXEL(x, y, ci, ri);

                        // this function applies the 1/2 pixel offset for us for DEMs
                        double realPart = 0.0;
                        auto err = band->InterpolateAtPoint(ci, ri, alg, &realPart, nullptr);
                        if (err == CE_None)
                        {
                            hf->setHeight(c, r, (float)realPart * _linearUnits);
                        }
                    }
                }
                done = true;
            }
#endif

            if (!done)
            {
                // fallback in case either InterpolateAtPoint is not available, or there's a nodata value
                // (which is not supported by InterpolateAtPoint)
                for (unsigned r = 0; r < tileSize; ++r)
                {
                    double y = tile_ymin + (dy * (double)r);
                    for (unsigned c = 0; c < tileSize; ++c)
                    {
                        double x = tile_xmin + (dx * (double)c);
                        auto h = getInterpolatedDEMValue(band, x, y, true);
                        if (isValidValue(h, noDataValue))
                            h *= _linearUnits;
                        else
                            h = NO_DATA_VALUE;

                        hf->setHeight(c, r, h);
                    }
                }
                done = true;
            }
        }

        else // NEAREST NEIGHBOR fast path
        {
            // Calculate and clamp the pixel extents of the tile
            double col_min, col_max, row_min, row_max;
            GEO_TO_PIXEL(tile_xmin, tile_ymin, col_min, row_max);
            GEO_TO_PIXEL(tile_xmax, tile_ymax, col_max, row_min);

            col_min = clamp(col_min, 0.0, (double)band->GetXSize() - 1.0);
            col_max = clamp(col_max, 0.0, (double)band->GetXSize() - 1.0);
            row_min = clamp(row_min, 0.0, (double)band->GetYSize() - 1.0);
            row_max = clamp(row_max, 0.0, (double)band->GetYSize() - 1.0);

            auto read_error = band->RasterIO(GF_Read,
                (int)col_min, (int)row_min,
                (int)col_max - (int)col_min + 1, (int)row_max - (int)row_min + 1,
                (void*)hf_raw, tileSize, tileSize,
                GDT_Float32, 0, 0);

            int gotNoDataValue = 0;
            float noDataValue = band->GetNoDataValue(&gotNoDataValue);
            if (!gotNoDataValue) noDataValue = NO_DATA_VALUE;

            if (read_error != CE_None)
            {
                //OE_WARN << LC << "RasterIO failed.\n";
                return nullptr;
            }

            // flip the raster, and scale by linear units
            int halfHeight = tileSize / 2;
            for (int t = 0; t < (int)tileSize; ++t)
            {
                for (int s = 0; s < (int)tileSize; ++s)
                {
                    if (t < halfHeight)
                        std::swap(hf_raw[t * tileSize + s], hf_raw[(tileSize - t - 1) * tileSize + s]);

                    if (isValidValue(hf_raw[t * tileSize + s], noDataValue))
                        hf_raw[t * tileSize + s] *= _linearUnits; // apply linear units
                    else
                        hf_raw[t * tileSize + s] = NO_DATA_VALUE;
                }
            }
        }

        // Apply any scale/offset found in the source:
        applyScaleAndOffset(band, (void*)hf_raw, GDT_Float32, tileSize, tileSize);
    }

    else // does not intersect - fill with no-data values
    {
        std::vector<float>& heightList = hf->getHeightList();
        std::fill(heightList.begin(), heightList.end(), NO_DATA_VALUE);
    }

    return hf.release();
}

osg::HeightField*
GDAL_detail::Driver::createHeightFieldWithVRT(const TileKey& key,
    unsigned tileSize,
    ProgressCallback* progress)
{
    if (_maxDataLevel.isSet() && key.getLevelOfDetail() > _maxDataLevel.get())
    {
        return NULL;
    }

    //Allocate the heightfield
    osg::ref_ptr<osg::HeightField> hf = new osg::HeightField;
    hf->allocate(tileSize, tileSize);
    for (unsigned int i = 0; i < hf->getHeightList().size(); ++i) hf->getHeightList()[i] = NO_DATA_VALUE;

    if (intersects(key))
    {
        GDALResampleAlg resampleAlg = GRA_NearestNeighbour;
        switch (*_gdalOptions.interpolation())
        {
        case INTERP_NEAREST:
            resampleAlg = GRA_NearestNeighbour;
            break;
        case INTERP_AVERAGE:
            resampleAlg = GRA_Average;
            break;
        case INTERP_BILINEAR:
            resampleAlg = GRA_Bilinear;
            break;
        case INTERP_CUBIC:
            resampleAlg = GRA_Cubic;
            break;
        case INTERP_CUBICSPLINE:
            resampleAlg = GRA_CubicSpline;
            break;
        }

        // Create warp options
        GDALWarpOptions* psWarpOptions = GDALCreateWarpOptions();
        psWarpOptions->eResampleAlg = resampleAlg;
        psWarpOptions->hSrcDS = _srcDS;
        psWarpOptions->nBandCount = _srcDS->GetRasterCount();
        psWarpOptions->panSrcBands = (int*)CPLMalloc(sizeof(int) * psWarpOptions->nBandCount);
        psWarpOptions->panDstBands = (int*)CPLMalloc(sizeof(int) * psWarpOptions->nBandCount);

        for (short unsigned int i = 0; i < psWarpOptions->nBandCount; ++i) {
            psWarpOptions->panDstBands[i] = psWarpOptions->panSrcBands[i] = i + 1;
        }

        void* transformerArg = GDALCreateGenImgProjTransformer2(_srcDS, NULL, NULL);
        if (transformerArg == NULL) {
            GDALDestroyWarpOptions(psWarpOptions);
            // ERROR;
            return 0;
        }

        // Expand the geotransform by half a pixel since we want to neighboring tiles to share edges
        double resolution = key.getExtent().width() / ((double)tileSize - 1);
        double adfGeoTransform[6];
        adfGeoTransform[0] = key.getExtent().xMin() - resolution / 2.0;
        adfGeoTransform[1] = resolution;
        adfGeoTransform[2] = 0;
        adfGeoTransform[3] = key.getExtent().yMax() + resolution / 2.0;
        adfGeoTransform[4] = 0;
        adfGeoTransform[5] = -resolution;

        GDALSetGenImgProjTransformerDstGeoTransform(transformerArg, adfGeoTransform);

        psWarpOptions->pTransformerArg = transformerArg;
        psWarpOptions->pfnTransformer = GDALGenImgProjTransform;

        GDALDatasetH tileDS = GDALCreateWarpedVRT(_srcDS, tileSize, tileSize, adfGeoTransform, psWarpOptions);
        if (GDALSetProjection(tileDS, key.getProfile()->getSRS()->getWKT().c_str()) != CE_None)
        {
            OE_DEBUG << LC << "GDALSetProjection failed" << std::endl;
        }

        OE_THREAD_LOCAL std::vector<float> heights;
        if (heights.size() < tileSize * tileSize)
            heights.resize(tileSize * tileSize);

        GDALRasterBand* band = static_cast<GDALRasterBand*>(GDALGetRasterBand(tileDS, 1));
        if (band->RasterIO(GF_Read, 0, 0, tileSize, tileSize, heights.data(), tileSize, tileSize, GDT_Float32, 0, 0) != CE_None)
        {
            OE_DEBUG << LC << "RasterIO failure" << std::endl;
        }

        for (unsigned int c = 0; c < tileSize; c++)
        {
            for (unsigned int r = 0; r < tileSize; r++)
            {
                unsigned inv_r = tileSize - r - 1;
                float h = heights[r * tileSize + c];
                if (!isValidValue(h, band))
                {
                    h = NO_DATA_VALUE;
                }
                hf->setHeight(c, inv_r, h);
            }
        }

        // Close the dataset
        if (tileDS != NULL)
        {
            GDALClose(tileDS);
        }

        // Destroy the warp options
        if (psWarpOptions != NULL)
        {
            GDALDestroyWarpOptions(psWarpOptions);
        }
    }
    return hf.release();
}

//...................................................................

GDAL_detail::Options::Options(const ConfigOptions& input)
{
    readFrom(input.getConfig());
}

void
GDAL_detail::Options::readFrom(const Config& conf)
{
    conf.get("url", _url);
    conf.get("connection", _connection);
    conf.get("subdataset", _subDataSet);
    conf.get("interpolation", "nearest", _interpolation, osgEarth::INTERP_NEAREST);
    conf.get("interpolation", "average", _interpolation, osgEarth::INTERP_AVERAGE);
    conf.get("interpolation", "bilinear", _interpolation, osgEarth::INTERP_BILINEAR);
    conf.get("interpolation", "cubic", _interpolation, osgEarth::INTERP_CUBIC);
    conf.get("interpolation", "cubicspline", _interpolation, osgEarth::INTERP_CUBICSPLINE);
    conf.get("coverage_uses_palette_index", coverageUsesPaletteIndex());
    conf.get("single_threaded", singleThreaded());
    conf.get("use_vrt", useVRT());
    conf.get("fallback_profile", fallbackProfile());

    // report on deprecated usage
    const std::string deprecated_keys[] = {
        "use_vrt",
        "warp_profile"
    };
    for (const auto& key : deprecated_keys)
        if (conf.hasValue(key))
            OE_DEBUG << LC << "Deprecated property \"" << key << "\" ignored" << std::endl;
}

void
GDAL_detail::Options::writeTo(Config& conf) const
{
    conf.set("url", _url);
    conf.set("connection", _connection);
    conf.set("subdataset", _subDataSet);
    conf.set("interpolation", "nearest", _interpolation, osgEarth::INTERP_NEAREST);
    conf.set("interpolation", "average", _interpolation, osgEarth::INTERP_AVERAGE);
    conf.set("interpolation", "bilinear", _interpolation, osgEarth::INTERP_BILINEAR);
    conf.set("interpolation", "cubic", _interpolation, osgEarth::INTERP_CUBIC);
    conf.set("interpolation", "cubicspline", _interpolation, osgEarth::INTERP_CUBICSPLINE);
    conf.set("coverage_uses_palette_index", coverageUsesPaletteIndex());
    conf.set("single_threaded", singleThreaded());
    conf.set("fallback_profile", fallbackProfile());
}

//......................................................................

#undef LC
#define LC "[GDAL] \"" << getName() << "\" "

namespace
{
    template<typename T>
    Status openOnThisThread(
        const T* layer,
        GDAL_detail::Driver::Ptr& driver,
        osg::ref_ptr<const Profile>* in_out_profile,
        DataExtentList* out_dataExtents,
        bool verbose)
    {
        driver = std::make_shared<GDAL_detail::Driver>();

        if (layer->options().noDataValue().isSet())
            driver->setNoDataValue(layer->options().noDataValue().get());
        if (layer->options().minValidValue().isSet())
            driver->setMinValidValue(layer->options().minValidValue().get());
        if (layer->options().maxValidValue().isSet())
            driver->setMaxValidValue(layer->options().maxValidValue().get());
        if (layer->options().maxDataLevel().isSet())
            driver->setMaxDataLevel(layer->options().maxDataLevel().get());

        Status status = driver->open(
            layer->getName(),
            layer->options(),
            layer->options().tileSize().get(),
            in_out_profile ? in_out_profile->get() : nullptr,
            out_dataExtents,
            layer->getReadOptions(),
            verbose);

        if (status.isError())
            return status;

        if (driver->getProfile() && in_out_profile != nullptr)
        {
            *in_out_profile = driver->getProfile();
        }

        return Status::NoError;
    }
}

//......................................................................

Config
GDALImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    writeTo(conf);
    return conf;
}

void
GDALImageLayer::Options::fromConfig(const Config& conf)
{
    readFrom(conf);
}

//......................................................................

REGISTER_OSGEARTH_LAYER(gdalimage, GDALImageLayer);

OE_LAYER_PROPERTY_IMPL(GDALImageLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(GDALImageLayer, std::string, Connection, connection);
OE_LAYER_PROPERTY_IMPL(GDALImageLayer, unsigned, SubDataSet, subDataSet);
OE_LAYER_PROPERTY_IMPL(GDALImageLayer, RasterInterpolation, Interpolation, interpolation);


void GDALImageLayer::setSingleThreaded(bool value) { options().singleThreaded() = value; }
bool GDALImageLayer::getSingleThreaded() const { return options().singleThreaded().get(); }


void
GDALImageLayer::init()
{
    // If no name is set, default it to the value of the URL
    if (!options().name().isSet() && options().url().isSet())
        options().name().setDefault(options().url()->base());

    // Initialize the image layer
    ImageLayer::init();
}

Status
GDALImageLayer::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    osg::ref_ptr<const Profile> profile;

    if (options().fallbackProfile().isSet())
    {
        profile = Profile::create(options().fallbackProfile().get());
        if (!profile.valid())
        {
            return Status(Status::ConfigurationError, "Override profile is not valid");
        }
    }

    // GDAL thread-safety requirement: each thread requires a separate GDALDataSet.
    // So we just encapsulate the entire setup once per thread.
    // https://trac.osgeo.org/gdal/wiki/FAQMiscellaneous#IstheGDALlibrarythread-safe

    // Note: no need to mutex the _driverSingleThreaded instance since we are in open
    // and open is single-threaded by definition.
    GDAL_detail::Driver::Ptr& driver = getSingleThreaded() ? _driverSingleThreaded : _driverPerThread.get();

    DataExtentList dataExtents;

    Status s = openOnThisThread(
        this,
        driver,
        &profile,
        &dataExtents,
        true);              //verbose

    if (s.isError())
        return s;

    // if the driver generated a valid profile, set it.
    if (profile.valid())
    {
        setProfile(profile.get());
    }

    setDataExtents(dataExtents);

    return s;
}

Status
GDALImageLayer::closeImplementation()
{
    // safely shut down all per-thread handles.
    Util::ScopedWriteLock unique_lock(_createCloseMutex);
    _driverPerThread.clear();
    _driverSingleThreaded = nullptr;

    return ImageLayer::closeImplementation();
}

GeoImage
GDALImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (getStatus().isError())
        return GeoImage::INVALID;

    Util::ScopedReadLock shared_lock(_createCloseMutex);

    // check while locked to ensure we may continue
    if (!isOpen())
        return GeoImage::INVALID;

    GDAL_detail::Driver::Ptr& driver = getSingleThreaded() ? _driverSingleThreaded : _driverPerThread.get();

    if (driver == nullptr)
    {
        // calling openImpl with NULL params limits the setup
        // since we already called this during openImplementation
        osg::ref_ptr<const Profile> profile = getProfile();
        openOnThisThread(this, driver, &profile, nullptr, false);
    }

    if (driver != nullptr)
    {
        OE_PROFILING_ZONE;

        // serialize acccess if we're in single-threaded mode
        Util::scoped_lock_if lock(_singleThreadingMutex, getSingleThreaded());

        osg::ref_ptr<osg::Image> image = driver->createImage(
            key,
            options().tileSize().get(),
            options().coverage() == true,
            progress);

        return GeoImage(image.get(), key.getExtent());
    }

    return GeoImage::INVALID;
}

//......................................................................

Config
GDALElevationLayer::Options::getConfig() const
{
    Config conf = ElevationLayer::Options::getConfig();
    writeTo(conf);
    return conf;
}

void
GDALElevationLayer::Options::fromConfig(const Config& conf)
{
    readFrom(conf);
}

//......................................................................

REGISTER_OSGEARTH_LAYER(gdalelevation, GDALElevationLayer);

OE_LAYER_PROPERTY_IMPL(GDALElevationLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(GDALElevationLayer, std::string, Connection, connection);
OE_LAYER_PROPERTY_IMPL(GDALElevationLayer, unsigned, SubDataSet, subDataSet);
OE_LAYER_PROPERTY_IMPL(GDALElevationLayer, RasterInterpolation, Interpolation, interpolation);
OE_LAYER_PROPERTY_IMPL(GDALElevationLayer, bool, UseVRT, useVRT);

void GDALElevationLayer::setSingleThreaded(bool value) { options().singleThreaded() = value; }
bool GDALElevationLayer::getSingleThreaded() const { return options().singleThreaded().get(); }

void
GDALElevationLayer::setExternalDataset(GDAL_detail::ExternalDataset* value)
{
    //_driver->setExternalDataset(value);
    OE_WARN << LC << "setExternalDataset NOT IMPLEMENTED" << std::endl;
}

void
GDALElevationLayer::init()
{
    ElevationLayer::init();
}

Status
GDALElevationLayer::openImplementation()
{
    Status parent = ElevationLayer::openImplementation();
    if (parent.isError())
        return parent;

    osg::ref_ptr<const Profile> profile;

    // GDAL thread-safety requirement: each thread requires a separate GDALDataSet.
    // So we just encapsulate the entire setup once per thread.
    // https://trac.osgeo.org/gdal/wiki/FAQMiscellaneous#IstheGDALlibrarythread-safe

    // Open the dataset temporarily to query the profile and extents.
    GDAL_detail::Driver::Ptr& driver = getSingleThreaded() ? _driverSingleThreaded :  _driverPerThread.get();

    DataExtentList dataExtents;

    Status s = openOnThisThread(
        this,
        driver,
        &profile,
        &dataExtents,
        true);              //verbose

    if (s.isError())
        return s;

    if (profile.valid())
        setProfile(profile.get());

    setDataExtents(dataExtents);


    return s;
}

Status
GDALElevationLayer::closeImplementation()
{
    // safely shut down all per-thread handles. The mutex prevents closing
    // while the layer is working on a create call.
    {
        Util::ScopedWriteLock unique_lock(_createCloseMutex);
        _driverPerThread.clear();
        _driverSingleThreaded = nullptr;
    }

    return ElevationLayer::closeImplementation();
}

GeoHeightField
GDALElevationLayer::createHeightFieldImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (getStatus().isError())
        return GeoHeightField(getStatus());

    Util::ScopedReadLock shared_lock(_createCloseMutex);

    // check while locked to ensure we may continue
    if (!isOpen())
        return GeoHeightField::INVALID;

    GDAL_detail::Driver::Ptr& driver = getSingleThreaded() ? _driverSingleThreaded : _driverPerThread.get();

    if (driver == nullptr)
    {
        // calling openImpl with NULL params limits the setup
        // since we already called this during openImplementation
        osg::ref_ptr<const Profile> profile = getProfile();
        openOnThisThread(this, driver, &profile, nullptr, false);
    }

    if (driver != nullptr)
    {
        OE_PROFILING_ZONE;

        Util::scoped_lock_if lock(_singleThreadingMutex, getSingleThreaded());

        osg::ref_ptr<osg::HeightField> heightfield;

        if (*_options->useVRT())
        {
            heightfield = driver->createHeightFieldWithVRT(
                key,
                options().tileSize().get(),
                progress);
        }
        else
        {         
            heightfield = driver->createHeightField(
                key,
                options().tileSize().get(),
                progress);
        }

        return GeoHeightField(heightfield.get(), key.getExtent());
    }

    return GeoHeightField::INVALID;
}

//...................................................................


#undef LC
#define LC "[GDAL] "

namespace
{
    osg::Image* createImageFromDataset(GDALDataset* ds)
    {
        // called internally -- GDAL lock not required

        int numBands = ds->GetRasterCount();
        if (numBands < 1)
            return 0L;

        GLenum dataType;
        int    sampleSize;
        GLint  internalFormat;

        switch (ds->GetRasterBand(1)->GetRasterDataType())
        {
        case GDT_Byte:
            dataType = GL_UNSIGNED_BYTE;
            sampleSize = 1;
            internalFormat = GL_R8;
            break;
        case GDT_UInt16:
        case GDT_Int16:
            dataType = GL_UNSIGNED_SHORT;
            sampleSize = 2;
            internalFormat = GL_R16;
            break;
        default:
            dataType = GL_FLOAT;
            sampleSize = 4;
            internalFormat = GL_R32F; // GL_LUMINANCE32F_ARB;
        }

        GLenum pixelFormat =
            numBands == 1 ? GL_RED :
            numBands == 2 ? GL_RG :
            numBands == 3 ? GL_RGB :
            GL_RGBA;

        int pixelBytes = sampleSize * numBands;

        //Allocate the image
        osg::Image *image = new osg::Image;
        image->allocateImage(ds->GetRasterXSize(), ds->GetRasterYSize(), 1, pixelFormat, dataType);

        CPLErr err = ds->RasterIO(
            GF_Read,
            0, 0,
            image->s(), image->t(),
            (void*)image->data(),
            image->s(), image->t(),
            ds->GetRasterBand(1)->GetRasterDataType(),
            numBands,
            NULL,
            pixelBytes,
            pixelBytes * image->s(),
            1);
        if (err != CE_None)
        {
            OE_WARN << LC << "RasterIO failed.\n";
        }

        ds->FlushCache();

        image->flipVertical();

        return image;
    }

    GDALDataset* createMemDS(int width, int height, int numBands, GDALDataType dataType, double minX, double minY, double maxX, double maxY, const std::string &projection)
    {
        //Get the MEM driver
        GDALDriver* memDriver = (GDALDriver*)GDALGetDriverByName("MEM");
        if (!memDriver)
        {
            OE_WARN << LC << "Could not get MEM driver" << std::endl;
            return NULL;
        }

        //Create the in memory dataset.
        GDALDataset* ds = memDriver->Create("", width, height, numBands, dataType, 0);
        if (!ds)
        {
            OE_WARN << LC << "memDriver.create failed" << std::endl;
            return NULL;
        }

        //Initialize the color interpretation
        if (numBands == 1)
        {
            ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
        }
        else
        {
            if (numBands >= 1)
                ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
            if (numBands >= 2)
                ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
            if (numBands >= 3)
                ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
            if (numBands >= 4)
                ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
        }

        //Initialize the geotransform
        double geotransform[6];
        double x_units_per_pixel = (maxX - minX) / (double)width;
        double y_units_per_pixel = (maxY - minY) / (double)height;
        geotransform[0] = minX;
        geotransform[1] = x_units_per_pixel;
        geotransform[2] = 0;
        geotransform[3] = maxY;
        geotransform[4] = 0;
        geotransform[5] = -y_units_per_pixel;
        ds->SetGeoTransform(geotransform);
        ds->SetProjection(projection.c_str());

        return ds;
    }

    GDALDataset* createDataSetFromImage(const osg::Image* image, double minX, double minY, double maxX, double maxY, const std::string &projection)
    {
        //Clone the incoming image
        osg::ref_ptr<osg::Image> clonedImage = new osg::Image(*image);

        //Flip the image
        clonedImage->flipVertical();

        GDALDataType gdalDataType =
            image->getDataType() == GL_UNSIGNED_BYTE ? GDT_Byte :
            image->getDataType() == GL_UNSIGNED_SHORT ? GDT_UInt16 :
            image->getDataType() == GL_FLOAT ? GDT_Float32 :
            GDT_Byte;

        int numBands = osg::Image::computeNumComponents(image->getPixelFormat());

        if (numBands == 0)
        {
            OE_WARN << LC << "Failure in createDataSetFromImage: unsupported pixel format\n";
            return nullptr;
        }

        int pixelBytes =
            gdalDataType == GDT_Byte ? numBands :
            gdalDataType == GDT_UInt16 ? 2 * numBands :
            4 * numBands;

        GDALDataset* srcDS = createMemDS(image->s(), image->t(), numBands, gdalDataType, minX, minY, maxX, maxY, projection);

        if (srcDS)
        {
            CPLErr err = srcDS->RasterIO(
                GF_Write,
                0, 0,
                clonedImage->s(), clonedImage->t(),
                (void*)clonedImage->data(),
                clonedImage->s(),
                clonedImage->t(),
                gdalDataType,
                numBands,
                NULL,
                pixelBytes,
                pixelBytes * image->s(),
                1);
            if (err != CE_None)
            {
                OE_WARN << LC << "RasterIO failed.\n";
            }

            srcDS->FlushCache();
        }

        return srcDS;
    }
}

osg::Image* osgEarth::GDAL_detail::reprojectImage(
    const osg::Image* srcImage,
    const std::string srcWKT,
    double srcMinX, double srcMinY, double srcMaxX, double srcMaxY,
    const std::string destWKT,
    double destMinX, double destMinY, double destMaxX, double destMaxY,
    int width,
    int height,
    bool useBilinearInterpolation)
{
    OE_PROFILING_ZONE;

    // Unnessecary since this is totally self-contained with thread-safe DataSets
    //GDAL_SCOPED_LOCK;

    osg::Timer_t start = osg::Timer::instance()->tick();

    //Create a dataset from the source image
    GDALDataset* srcDS = createDataSetFromImage(srcImage, srcMinX, srcMinY, srcMaxX, srcMaxY, srcWKT);

    if (srcDS == nullptr)
        return nullptr;

    OE_DEBUG << LC << "Source image is " << srcImage->s() << "x" << srcImage->t() << " in " << srcWKT << std::endl;


    if (width == 0 || height == 0)
    {
        double outgeotransform[6];
        double extents[4];
        void* transformer = GDALCreateGenImgProjTransformer(srcDS, srcWKT.c_str(), NULL, destWKT.c_str(), 1, 0, 0);
        GDALSuggestedWarpOutput2(srcDS,
            GDALGenImgProjTransform, transformer,
            outgeotransform,
            &width,
            &height,
            extents,
            0);
        GDALDestroyGenImgProjTransformer(transformer);
    }
    OE_DEBUG << "Creating warped output of " << width << "x" << height << " in " << destWKT << std::endl;

    int numBands = srcDS->GetRasterCount();
    GDALDataType dataType = srcDS->GetRasterBand(1)->GetRasterDataType();

    GDALDataset* destDS = createMemDS(width, height, numBands, dataType, destMinX, destMinY, destMaxX, destMaxY, destWKT);

    if (useBilinearInterpolation == true)
    {
        GDALReprojectImage(srcDS, NULL,
            destDS, NULL,
            GRA_Bilinear,
            0, 0, 0, 0, 0);
    }
    else
    {
        GDALReprojectImage(srcDS, NULL,
            destDS, NULL,
            GRA_NearestNeighbour,
            0, 0, 0, 0, 0);
    }

    osg::Image* result = createImageFromDataset(destDS);

    delete srcDS;
    delete destDS;

    osg::Timer_t end = osg::Timer::instance()->tick();

    OE_DEBUG << "Reprojected image in " << osg::Timer::instance()->delta_m(start, end) << std::endl;

    return result;
}

std::string
osgEarth::GDAL_detail::heightFieldToTiff(const osg::HeightField* hf)
{
    std::string vsimem_url = Stringify() << "/vsimem/" << std::this_thread::get_id() << "_heightFieldToTiff.tif";

    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");

    int width = hf->getNumColumns();
    int height = hf->getNumRows();

    char** options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "DEFLATE");
    options = CSLSetNameValue(options, "PREDICTOR", "3");

    // Write to virtual memory first
    GDALDataset* dataset = driver->Create(vsimem_url.c_str(), width, height, 1, GDT_Float32, options);
    CSLDestroy(options);

    std::vector<float> heights;
    heights.reserve(width * height);
    // Flip the heightfield to match the GDAL orientation
    for (int r = 0; r < height; ++r)
    {
        int inv_r = height - r - 1;
        for (int c = 0; c < width; ++c)
        {
            heights.push_back(hf->getHeight(c, inv_r));
        }
    }

    unsigned int numBands = 1;
    int pixelBytes = sizeof(float) * numBands;

    if (dataset->RasterIO(GF_Write,
        0, 0,
        width, height,
        (void*)heights.data(),
        width, height,
        GDT_Float32,
        numBands,
        NULL,
        pixelBytes,
        pixelBytes * width,
        1) != CE_None)
    {
        OE_WARN << CPLGetLastErrorMsg() << std::endl;
        GDALClose(dataset);
        VSIUnlink(vsimem_url.c_str());    
    }

    dataset->FlushCache();
    GDALClose(dataset);

    std::string result;

    // Read the bytes from vsimem
    vsi_l_offset length = 0;
    GByte* data = VSIGetMemFileBuffer(vsimem_url.c_str(), &length, FALSE);
    if (data && length > 0)
    {
        result.reserve(length);
        result.assign(reinterpret_cast<char*>(data), length);       
    }
    VSIUnlink(vsimem_url.c_str());
    return result;
}
