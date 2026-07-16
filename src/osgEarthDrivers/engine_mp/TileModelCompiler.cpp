/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2018 Pelican Mapping
* MIT License
*/
#include "TileModelCompiler"
#include "MPGeometry"

#include <osgEarth/Locators>
#include <osgEarth/BuildConfig>
#ifdef OSGEARTH_USE_TINYBVH
#include <osgEarth/TinyBVHShape>
#endif
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include "MapFrame"
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/ImageUtils>
#include <osgEarth/Utils>
#include <osgEarth/ECEF>
#include <osgEarth/ObjectIndex>
#include <osgEarthSymbology/Geometry>
#include <osgEarthSymbology/MeshConsolidator>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/GL2Extensions>
#include <osg/ComputeBoundsVisitor>
#include <osgUtil/DelaunayTriangulator>
#include <osgUtil/Optimizer>
#include <osgUtil/MeshOptimizers>
#include <osgText/Text>

using namespace osgEarth::Drivers::MPTerrainEngine;
using namespace osgEarth;
using namespace osgEarth::Drivers;
using namespace osgEarth::Symbology;

#define LC "[TileModelCompiler] "

//#define USE_TEXCOORD_CACHE 1

//------------------------------------------------------------------------

osg::ref_ptr<osg::Vec2Array>&
CompilerCache::TexCoordArrayCache::get(const osg::Vec4d& mat,
                                       unsigned          cols,
                                       unsigned          rows)
{
    for( iterator i = begin(); i != end(); ++i )
    {
        CompilerCache::TexCoordTableKey& key = i->first;
        if ( key._mat == mat && key._cols == cols && key._rows == rows )
        {
            return i->second;
        }
    }
    
    CompilerCache::TexCoordTableKey newKey;
    newKey._mat     = mat;
    newKey._cols    = cols;
    newKey._rows    = rows;
    this->push_back( std::make_pair(newKey, (osg::Vec2Array*)0L) );
    return this->back().second;
}

//------------------------------------------------------------------------


#define MATCH_TOLERANCE 0.000001

namespace
{    
    // Data for a single renderable color layer
    struct RenderLayer
    {
        TileModel::ColorData           _layer;
        TileModel::ColorData           _layerParent;
        osg::ref_ptr<const GeoLocator> _locator;
        osg::ref_ptr<osg::Vec2Array>   _texCoords;
        osg::ref_ptr<osg::Vec2Array>   _stitchTexCoords;
        bool _ownsTexCoords;
        RenderLayer() : 
            _ownsTexCoords( false ) { }
    };

    typedef std::vector< RenderLayer > RenderLayerVector;


    // Record that stores the data for a single masking region.
    struct MaskRecord
    {
        osg::ref_ptr<osg::Vec3dArray> _boundary;
        osg::Vec3d                    _ndcMin, _ndcMax;
        osg::ref_ptr<MPGeometry>      _geom;
        osg::ref_ptr<osg::Vec3Array>  _internal;

        MaskRecord(osg::Vec3dArray* boundary, osg::Vec3d& ndcMin, osg::Vec3d& ndcMax, MPGeometry* geom) 
            : _boundary(boundary), _ndcMin(ndcMin), _ndcMax(ndcMax), _geom(geom), _internal(new osg::Vec3Array()) { }
    };

    typedef std::vector<MaskRecord> MaskRecordVector;
    typedef std::vector<int> Indices;


    struct Data
    {
        Data(const TileModel* in_model, 
             const MapFrame&  in_frame,
             const MaskLayerVector& in_maskLayers,
             const ModelLayerVector& in_modelLayers)

            : model     ( in_model ), 
              frame     ( in_frame ),
              maskLayers( in_maskLayers ),
              modelLayers( in_modelLayers )
        {
            surfaceGeode     = 0L;
            surface          = 0L;
            heightScale      = 1.0f;
            heightOffset     = 0.0f;
            createSkirt      = false;
            i_sampleFactor   = 1.0f;
            j_sampleFactor   = 1.0f;
            useVBOs          = true; 
            textureImageUnit = 0;
            renderTileCoords = 0L;
            ownsTileCoords   = false;
            stitchTileCoords = 0L;
            installParentData = false;
            usePatches       = false;
            numVerticesInSkirt = 0;
            numVerticesInSurface = 0;
            numRows = 0;
            numCols = 0;
            originalNumRows = 0;
            originalNumCols = 0;
            useUInt = false;
        }

        osg::Matrixd local2world, world2local;

        const MapFrame& frame;

        bool                     useVBOs;
        int                      textureImageUnit;

        const TileModel*              model;                   // the tile's data model
        osg::ref_ptr<const TileModel> parentModel;             // parent model reference
        bool                          installParentData;       // whether to install parent colors/normals for blending

        const MaskLayerVector&   maskLayers;                    // map-global masking layer set
        const ModelLayerVector&  modelLayers;                   // model layers with masks set
        osg::ref_ptr<GeoLocator> geoLocator;                    // tile locator adjusted to geographic
        osg::Vec3d               centerModel;                   // tile center in model (world) coords

        RenderLayerVector            renderLayers;
        osg::ref_ptr<osg::Vec2Array> renderTileCoords;
        bool                         ownsTileCoords;

        // tile coords for masked areas; always owned (never shared)
        osg::ref_ptr<osg::Vec2Array> stitchTileCoords;

        // surface data:
        osg::ref_ptr<osg::Geode>      surfaceGeode;
        osg::ref_ptr<MPGeometry>      surface;
        osg::ref_ptr<osg::Vec3Array>  surfaceVerts;
        osg::ref_ptr<osg::Vec3Array>  normals;
        osg::ref_ptr<osg::Vec4Array>  surfaceAttribs;
        osg::ref_ptr<osg::Vec4Array>  surfaceAttribs2;
        unsigned                      numVerticesInSurface;
        osg::ref_ptr<osg::FloatArray> elevations;
        Indices                       indices;
        osg::BoundingSphere           surfaceBound;

        // skirt data:
        unsigned                 numVerticesInSkirt;
        bool                     createSkirt;

        // sampling grid parameters:
        unsigned                 numRows;
        unsigned                 numCols;
        double                   i_sampleFactor;
        double                   j_sampleFactor;
        double                   heightScale;
        double                   heightOffset;
        unsigned                 originalNumRows;
        unsigned                 originalNumCols;
        
        // for masking/stitching:
        MaskRecordVector         maskRecords;
        //MPGeometry*              stitchGeom;

        bool                     usePatches;

        bool useUInt;
        osg::DrawElements* newDrawElements(GLenum mode) {
            osg::DrawElements* de = 0L;
            if ( useUInt )
                de = new osg::DrawElementsUInt(mode);
            else
                de = new osg::DrawElementsUShort(mode);
            de->setName("TMC");
            return de;
        }
    };


    /**
     * Set up an single masking geometry. Called by setupMaskRecords
     */
    void setupMaskRecord(Data& d, osg::Vec3dArray* boundary)
    {
        if ( boundary )
        {
            osg::Vec3d min, max;
            min = max = boundary->front();

            for (osg::Vec3dArray::iterator it = boundary->begin(); it != boundary->end(); ++it)
            {
                if (it->x() < min.x())
                min.x() = it->x();

                if (it->y() < min.y())
                min.y() = it->y();

                if (it->x() > max.x())
                max.x() = it->x();

                if (it->y() > max.y())
                max.y() = it->y();
            }

            osg::Vec3d min_ndc, max_ndc;
            d.geoLocator->modelToUnit(min, min_ndc);
            d.geoLocator->modelToUnit(max, max_ndc);

            bool x_match = ((min_ndc.x() >= 0.0 && max_ndc.x() <= 1.0) ||
                            (min_ndc.x() <= 0.0 && max_ndc.x() > 0.0) ||
                            (min_ndc.x() < 1.0 && max_ndc.x() >= 1.0));

            bool y_match = ((min_ndc.y() >= 0.0 && max_ndc.y() <= 1.0) ||
                            (min_ndc.y() <= 0.0 && max_ndc.y() > 0.0) ||
                            (min_ndc.y() < 1.0 && max_ndc.y() >= 1.0));

            if (x_match && y_match)
            {
                osg::ref_ptr<MPGeometry> stitchGeom = new MPGeometry( d.model->_tileKey, d.frame, d.textureImageUnit );
                stitchGeom->setName("stitchGeom");
                d.maskRecords.push_back( MaskRecord(boundary, min_ndc, max_ndc, stitchGeom.get()) );
            }
        }
    }

    
    /**
     * Set up the masking records for this build. Here we check all the map's mask layer
     * boundary geometries and find any that intersect the current tile. For an intersection
     * we create a MaskRecord that we'll use later in the process.
     */
    void setupMaskRecords(Data& d)
    {
        // When displaying Plate Carre, Heights have to be converted from meters to degrees.
        // This is also true for mask feature
        // TODO: adjust this calculation based on the actual EllipsoidModel.
        float scale = d.heightScale;
        if (d.model->_tileLocator->getCoordinateSystemType() == osgEarth::GeoLocator::GEOGRAPHIC)
        {
            scale = d.heightScale / 111319.0f;
        }      

        for(MaskLayerVector::const_iterator it = d.maskLayers.begin();
            it != d.maskLayers.end(); 
            ++it)
        {
            MaskLayer* layer = it->get();
            if ( layer->getMinLevel() <= d.model->_tileKey.getLevelOfDetail() )
            {
                setupMaskRecord( d, layer->getOrCreateMaskBoundary(
                    scale,
                    d.model->_tileLocator->getDataExtent().getSRS(),
                    (ProgressCallback*)0L ) );
            }
        }

        for(ModelLayerVector::const_iterator it = d.modelLayers.begin();
            it != d.modelLayers.end();
            ++it)
        {
            ModelLayer* layer = it->get();
            if (layer->getMaskSource() &&
                layer->getMaskMinLevel() <= d.model->_tileKey.getLevelOfDetail() )
            {
                setupMaskRecord( d, layer->getOrCreateMaskBoundary(
                    scale,
                    d.model->_tileLocator->getDataExtent().getSRS(),
                    (ProgressCallback*)0L ) );
            }
        }
    }

    /**
     * Calculates the sample rate and allocates all the vertex, normal, and color
     * arrays for the tile.
     */
    void setupGeometryAttributes( Data& d, int tileSize )
    {
        d.numRows = 17;
        d.numCols = 17;
        d.originalNumRows = 17;
        d.originalNumCols = 17;        

        // read the row/column count and skirt size from the model:
        osg::HeightField* hf = d.model->_elevationData.getHeightField();
        if ( hf )
        {
            d.numCols = hf->getNumColumns();
            d.numRows = hf->getNumRows();
            d.originalNumCols = d.numCols;
            d.originalNumRows = d.numRows;
        }

        // calculate the elevation sampling factors that we'll use to step though
        // the tile's NDC space.
        d.i_sampleFactor = 1.0f;
        d.j_sampleFactor = 1.0f;

        if ( tileSize > 0 )
        {
            d.numCols = tileSize;
            d.numRows = tileSize;

            d.i_sampleFactor = double(d.originalNumCols-1)/double(d.numCols-1);
            d.j_sampleFactor = double(d.originalNumRows-1)/double(d.numRows-1);
        }

        // calculate the total number of verts:
        d.numVerticesInSkirt   = d.createSkirt ? (2 * (d.numCols*2 + d.numRows*2 - 4)) : 0;
        d.numVerticesInSurface = d.numCols * d.numRows + d.numVerticesInSkirt;

        // allocate and assign vertices
        d.surfaceVerts = new osg::Vec3Array();
        d.surfaceVerts->reserve( d.numVerticesInSurface );
        d.surface->setVertexArray( d.surfaceVerts.get() );

        // allocate and assign normals
        d.normals = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX);
        d.normals->reserve( d.numVerticesInSurface );
        d.surface->setNormalArray( d.normals.get() );

        // vertex attribution
        // for each vertex, a vec4 containing a unit extrusion vector in [0..2] and the raw elevation in [3]
        d.surfaceAttribs = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
        d.surfaceAttribs->setNormalize(false);
        d.surfaceAttribs->reserve( d.numVerticesInSurface );
        d.surface->setVertexAttribArray( osg::Drawable::ATTRIBUTE_6, d.surfaceAttribs.get() );

        // for each vertex, index 0 holds the interpolated elevation from the lower lod (for morphing)
        d.surfaceAttribs2 = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
        d.surfaceAttribs2->setNormalize(false);
        d.surfaceAttribs2->reserve( d.numVerticesInSurface );
        d.surface->setVertexAttribArray( osg::Drawable::ATTRIBUTE_7, d.surfaceAttribs2.get() );
        
        // temporary data structures for triangulation support
        d.elevations = new osg::FloatArray();
        d.elevations->reserve( d.numVerticesInSurface );
        d.indices.resize( d.numVerticesInSurface, -1 );

        // Uint required?
        d.useUInt = d.numVerticesInSurface > 0xFFFF;
    }


    /**
     * Generates the texture coordinate arrays for each layer.
     */
    void setupTextureAttributes( Data& d, CompilerCache& cache )
    {
        // Any color entries that have the same Locator will share a texcoord
        // array, saving on memory.
        d.renderLayers.reserve( d.model->_colorData.size() );

        if ( d.maskRecords.size() > 0 )
        {
            if ( !d.stitchTileCoords.valid() )
            {
                d.stitchTileCoords = new osg::Vec2Array();
            }
        }

#ifdef USE_TEXCOORD_CACHE
        // unit tile coords - [0..1] always across the tile.
        osg::Vec4d idmat;
        idmat[0] = 0.0;
        idmat[1] = 0.0;
        idmat[2] = 1.0;
        idmat[3] = 1.0;

        osg::ref_ptr<osg::Vec2Array>& tileCoords = cache._surfaceTexCoordArrays.get( idmat, d.numCols, d.numRows );
        if ( !tileCoords.valid() )
        {
            // Note: anything in the cache must have its own VBO. No sharing!
            tileCoords = new osg::Vec2Array();
            tileCoords->setVertexBufferObject( new osg::VertexBufferObject() );
            tileCoords->reserve( d.numVerticesInSurface );
            d.ownsTileCoords = true;
        }
        d.renderTileCoords = tileCoords.get();

#else // not USE_TEXCOORD_CACHE
        d.renderTileCoords = new osg::Vec2Array();
#if OSG_MIN_VERSION_REQUIRED(3,5,6)
        if(osg::DisplaySettings::instance()->getVertexBufferHint() == osg::DisplaySettings::VERTEX_ARRAY_OBJECT) d.renderTileCoords->setVertexBufferObject( new osg::VertexBufferObject() );
#endif
        d.renderTileCoords->reserve( d.numVerticesInSurface );
        d.ownsTileCoords = true;
#endif


        // build a list of "render layers", in rendering order, sharing texture coordinate
        // arrays wherever possible.
        for( TileModel::ColorDataByUID::const_iterator i = d.model->_colorData.begin(); i != d.model->_colorData.end(); ++i )
        {
            const TileModel::ColorData& colorLayer = i->second;
            RenderLayer r;
            r._layer = colorLayer;

            const GeoLocator* locator = r._layer.getLocator();
            if ( locator )
            {
                // if we have no mask records, we can use the texture coordinate array cache.
                if ( d.maskLayers.size() == 0 && locator->isLinear() )
                {
                    const GeoExtent& locex = locator->getDataExtent();
                    const GeoExtent& keyex = d.model->_tileKey.getExtent();

                    osg::Vec4d mat;
                    mat[0] = (keyex.xMin() - locex.xMin())/locex.width();
                    mat[1] = (keyex.yMin() - locex.yMin())/locex.height();
                    mat[2] = (keyex.width() / locex.width());
                    mat[3] = (keyex.height() / locex.height());

                    //OE_DEBUG << "key=" << d.model->_tileKey.str() << ": off=[" <<mat[0]<< ", " <<mat[1] << "] scale=["
                    //    << mat[2]<< ", " << mat[3] << "]" << std::endl;

#ifdef USE_TEXCOORD_CACHE
                    osg::ref_ptr<osg::Vec2Array>& surfaceTexCoords = cache._surfaceTexCoordArrays.get( mat, d.numCols, d.numRows );
                    if ( !surfaceTexCoords.valid() )
                    {
                        // Note: anything in the cache must have its own VBO. No sharing!
                        surfaceTexCoords = new osg::Vec2Array();
                        surfaceTexCoords->setVertexBufferObject( new osg::VertexBufferObject() );
                        surfaceTexCoords->reserve( d.numVerticesInSurface );
                        r._ownsTexCoords = true;
                    }
                    r._texCoords = surfaceTexCoords.get();

#else // not USE_TEXCOORD_CACHE
                    r._texCoords = new osg::Vec2Array();
#if OSG_MIN_VERSION_REQUIRED(3,5,6)
                    if(osg::DisplaySettings::instance()->getVertexBufferHint() == osg::DisplaySettings::VERTEX_ARRAY_OBJECT) r._texCoords->setVertexBufferObject( new osg::VertexBufferObject() );
#endif
                    r._texCoords->reserve( d.numVerticesInSurface );
                    r._ownsTexCoords = true;
#endif
                }

                else
                {
                    // cannot use the tex coord array cache if there are masking records.
                    r._texCoords = new osg::Vec2Array();
                    r._texCoords->reserve( d.numVerticesInSurface );
                    r._ownsTexCoords = true;

                    if ( d.maskRecords.size() > 0 )
                    {
                        r._stitchTexCoords = new osg::Vec2Array();                        
                    }
                }

                // install the locator:
                r._locator = locator;
                if ( locator->getCoordinateSystemType() == osgTerrain::Locator::GEOCENTRIC )
                {
                    r._locator = locator->getGeographicFromGeocentric();
                }

                // install the parent color data layer if necessary.
                if ( d.installParentData )
                {
                    if ( d.parentModel.valid() )
                    {                    
                        if (!d.parentModel->getColorData( r._layer.getUID(), r._layerParent ))
                        {
                            // If we can't get the color data from the parent that means it doesn't exist, perhaps b/c of a min level setting
                            // So we create a false layer parent with a transparent image so it will fade into the real data.
                            r._layerParent = r._layer;
                            r._layerParent._texture = new osg::Texture2D(ImageUtils::createEmptyImage());
                            r._layerParent._hasAlpha = true;
                        }
                    }
                    else
                    {
                        r._layerParent = r._layer;
                    }
                }

                d.renderLayers.push_back( r );

                // Note that we don't actually assign the tex coord arrays to the geometry yet.
                // That must wait until the end. See the comments in assignTextureArrays()
                // to understand why.
            }
            else
            {
                OE_WARN << LC << "Found a Locator, but it wasn't a GeoLocator." << std::endl;
            }
        }
    }


    /**
     * Iterate over the sampling grid and calculate the vertex positions and normals
     * for each sampling point.
     */
    void createSurfaceGeometry( Data& d, bool debug )
    {
        d.surfaceBound.init();

        //osgTerrain::HeightFieldLayer* elevationLayer = d.model->_elevationData.getHFLayer();

        osg::HeightField* hf            = d.model->_elevationData.getHeightField();
        GeoLocator*       hfLocator     = d.model->_elevationData.getLocator();

        if ( debug )
        {
            // Debugging code to help identify all zero heightfields.
            bool allZero = true;
            // Check for an all 0 heightfield
            for (unsigned int c = 0; c < hf->getNumColumns(); ++c)
            {
                for (unsigned int r = 0; r < hf->getNumRows(); ++r)
                {
                    float h = hf->getHeight(c, r);
                    if (h != 0)
                    {
                        allZero = false;
                        break;
                    }
                }
            }

            if (allZero)
            {
                OE_DEBUG << "ALL ZERO HEIGHTFIELD " << d.model->_tileKey.str() << std::endl;
            }
        }


        // populate vertex and tex coord arrays    
        for(unsigned j=0; j < d.numRows; ++j)
        {
            for(unsigned i=0; i < d.numCols; ++i)
            {
                unsigned int iv = j*d.numCols + i;
                osg::Vec3d ndc( ((double)i)/(double)(d.numCols-1), ((double)j)/(double)(d.numRows-1), 0.0);

                // raw height:
                float heightValue = 0.0f;
                bool  validValue  = true;

                if ( hf )
                {
                    validValue = d.model->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), heightValue, INTERP_TRIANGULATE );
                }

                ndc.z() = heightValue * d.heightScale + d.heightOffset;

                if ( !validValue )
                {
                    d.indices[iv] = -1;
                }

                // First check whether the sampling point falls within a mask's bounding box.
                // If so, skip the sampling and mark it as a mask location
                if ( validValue && d.maskRecords.size() > 0 )
                {
					double minndcx = d.maskRecords[0]._ndcMin.x();
					double minndcy = d.maskRecords[0]._ndcMin.y();
					double maxndcx = d.maskRecords[0]._ndcMax.x();
					double maxndcy = d.maskRecords[0]._ndcMax.y();
					for (int mrs = 1; mrs < d.maskRecords.size(); ++mrs)
					{
						if ( d.maskRecords[mrs]._ndcMin.x()< minndcx)
						{
							minndcx = d.maskRecords[mrs]._ndcMin.x();
						}
						if ( d.maskRecords[mrs]._ndcMin.y()< minndcy)
						{
							minndcy = d.maskRecords[mrs]._ndcMin.y();
						}
						if ( d.maskRecords[mrs]._ndcMax.x()> maxndcx)
						{
							maxndcx = d.maskRecords[mrs]._ndcMax.x();
						}
						if ( d.maskRecords[mrs]._ndcMax.y()> maxndcy)
						{
							maxndcy = d.maskRecords[mrs]._ndcMax.y();
						}


					}
					if(ndc.x() >= (minndcx) && ndc.x() <= (maxndcx) &&
						        ndc.y() >= (minndcy) && ndc.y() <= (maxndcy))
				    {
				        validValue = false;
				        d.indices[iv] = -2;
				    
				    }
                }
                
                if ( validValue )
                {
                    d.indices[iv] = d.surfaceVerts->size();

                    osg::Vec3d model;
                    d.model->_tileLocator->unitToModel( ndc, model );
                    osg::Vec3d modelLTP = model * d.world2local;
                    (*d.surfaceVerts).push_back(modelLTP);

                    // grow the bounding sphere:
                    d.surfaceBound.expandBy( (*d.surfaceVerts).back() );

                    // the separate texture space requires separate transformed texcoords for each layer.
                    for( RenderLayerVector::const_iterator r = d.renderLayers.begin(); r != d.renderLayers.end(); ++r )
                    {
                        if ( r->_ownsTexCoords )
                        {
                            if ( !r->_locator->isEquivalentTo( *d.geoLocator.get() ) )
                            {
                                osg::Vec3d color_ndc;
                                osgTerrain::Locator::convertLocalCoordBetween( *d.geoLocator.get(), ndc, *r->_locator.get(), color_ndc );
                                r->_texCoords->push_back( osg::Vec2( color_ndc.x(), color_ndc.y() ) );
                            }
                            else
                            {
                                r->_texCoords->push_back( osg::Vec2( ndc.x(), ndc.y() ) );
                            }
                        }
                    }

                    if ( d.ownsTileCoords )
                    {
                        d.renderTileCoords->push_back( osg::Vec2(ndc.x(), ndc.y()) );
                    }

                    // record the raw elevation value in our float array for later
                    (*d.elevations).push_back(ndc.z());

                    // compute the local normal (up vector)
                    osg::Vec3d ndc_plus_one(ndc.x(), ndc.y(), ndc.z() + 1.0);
                    osg::Vec3d model_up;
                    d.model->_tileLocator->unitToModel(ndc_plus_one, model_up);
                    model_up = (model_up*d.world2local) - modelLTP;
                    model_up.normalize();
                    (*d.normals).push_back(model_up);

                    // Calculate and store the "old height", i.e the height value from
                    // the parent LOD.
                    float     oldHeightValue = heightValue;
                    osg::Vec3 oldNormal;

                    // This only works if the tile size is an odd number in both directions.
                    if (d.model->_tileKey.getLOD() > 0 && (d.numCols&1) && (d.numRows&1) && d.parentModel.valid())
                    {
                        d.parentModel->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), oldHeightValue, INTERP_TRIANGULATE );
                        d.parentModel->_elevationData.getNormal( ndc, d.model->_tileLocator.get(), oldNormal, INTERP_TRIANGULATE );
                    }
                    else
                    {
                        d.model->_elevationData.getNormal(ndc, d.model->_tileLocator.get(), oldNormal, INTERP_TRIANGULATE );
                    }

                    // first attribute set has the unit extrusion vector and the
                    // raw height value.
                    (*d.surfaceAttribs).push_back( osg::Vec4f(
                        model_up.x(),
                        model_up.y(),
                        model_up.z(),
                        heightValue) );

                    // second attribute set has the old height value in "w"
                    (*d.surfaceAttribs2).push_back( osg::Vec4f(
                        oldNormal.x(),
                        oldNormal.y(),
                        oldNormal.z(),
                        oldHeightValue ) );
                }
            }
        }

        //if ( d.renderLayers[0]._texCoords->size() < d.surfaceVerts->size() )
        //{
        //    OE_WARN << LC << "not good. mask error." << std::endl;
        //}
    }


    /**
     * If there are masking records, calculate the vertices to bound the masked area
     * and the internal vertices to populate it. Then build a triangulation of the
     * area inside the masking bounding box and add this to the surface geode.
     */
    void createMaskGeometry( Data& d )
    {
        bool hasElev = d.model->hasElevation();

        osg::ref_ptr<osgUtil::DelaunayTriangulator> trig=new osgUtil::DelaunayTriangulator();

        std::vector<osg::ref_ptr<osgUtil::DelaunayConstraint> > alldcs;

        osg::ref_ptr<osg::Vec3Array> coordsArray = new osg::Vec3Array;

        double minndcx = d.maskRecords[0]._ndcMin.x();
        double minndcy = d.maskRecords[0]._ndcMin.y();
        double maxndcx = d.maskRecords[0]._ndcMax.x();
        double maxndcy = d.maskRecords[0]._ndcMax.y();
        for (int mrs = 1; mrs < d.maskRecords.size(); ++mrs)
        {
            if ( d.maskRecords[mrs]._ndcMin.x()< minndcx)
            {
                minndcx = d.maskRecords[mrs]._ndcMin.x();
            }
            if ( d.maskRecords[mrs]._ndcMin.y()< minndcy)
            {
                minndcy = d.maskRecords[mrs]._ndcMin.y();
            }
            if ( d.maskRecords[mrs]._ndcMax.x()> maxndcx)
            {
                maxndcx = d.maskRecords[mrs]._ndcMax.x();
            }
            if ( d.maskRecords[mrs]._ndcMax.y()> maxndcy)
            {
                maxndcy = d.maskRecords[mrs]._ndcMax.y();
            }			
        }

        int min_i = (int)floor(minndcx * (double)(d.numCols-1));
        if (min_i < 0) min_i = 0;
        if (min_i >= (int)d.numCols) min_i = d.numCols - 1;

        int min_j = (int)floor(minndcy * (double)(d.numRows-1));
        if (min_j < 0) min_j = 0;
        if (min_j >= (int)d.numRows) min_j = d.numRows - 1;

        int max_i = (int)ceil(maxndcx * (double)(d.numCols-1));
        if (max_i < 0) max_i = 0;
        if (max_i >= (int)d.numCols) max_i = d.numCols - 1;

        int max_j = (int)ceil(maxndcy * (double)(d.numRows-1));
        if (max_j < 0) max_j = 0;
        if (max_j >= (int)d.numRows) max_j = d.numRows - 1;

        if (min_i >= 0 && max_i >= 0 && min_j >= 0 && max_j >= 0)
        {
            int num_i = max_i - min_i + 1;
            int num_j = max_j - min_j + 1;

            osg::ref_ptr<osgEarth::Polygon> maskSkirtPoly = new osgEarth::Polygon();
            maskSkirtPoly->resize(num_i * 2 + num_j * 2 - 4);

            for (int i = 0; i < num_i; i++)
            {
                //int index = indices[min_j*numColumns + i + min_i];
                {
                    osg::Vec3d ndc( ((double)(i + min_i))/(double)(d.numCols-1), ((double)min_j)/(double)(d.numRows-1), 0.0);

                    //if (elevationLayer)
                    if ( hasElev )
                    {
                        float value = 0.0f;
                        if ( d.model->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), value, INTERP_BILINEAR ) )
                            ndc.z() = value * d.heightScale + d.heightOffset;
                    }

                    (*maskSkirtPoly)[i] = ndc;
                }

                //index = indices[max_j*numColumns + i + min_i];
                {
                    osg::Vec3d ndc( ((double)(i + min_i))/(double)(d.numCols-1), ((double)max_j)/(double)(d.numRows-1), 0.0);

                    if ( hasElev )
                    {
                        float value = 0.0f;
                        if ( d.model->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), value, INTERP_BILINEAR ) )
                            ndc.z() = value * d.heightScale + d.heightOffset;
                    }

                    (*maskSkirtPoly)[i + (2 * num_i + num_j - 3) - 2 * i] = ndc;
                }
            }
            for (int j = 0; j < num_j - 2; j++)
            {
                //int index = indices[(min_j + j + 1)*numColumns + max_i];
                {
                    osg::Vec3d ndc( ((double)max_i)/(double)(d.numCols-1), ((double)(min_j + j + 1))/(double)(d.numRows-1), 0.0);

                    if ( hasElev )
                    {
                        float value = 0.0f;
                        if ( d.model->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), value, INTERP_BILINEAR ) )
                            ndc.z() = value * d.heightScale + d.heightOffset;
                    }

                    (*maskSkirtPoly)[j + num_i] = ndc;
                }

                //index = indices[(min_j + j + 1)*numColumns + min_i];
                {
                    osg::Vec3d ndc( ((double)min_i)/(double)(d.numCols-1), ((double)(min_j + j + 1))/(double)(d.numRows-1), 0.0);

                    if ( hasElev )
                    {
                        float value = 0.0f;
                        if ( d.model->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), value, INTERP_BILINEAR ) )
                            ndc.z() = value * d.heightScale + d.heightOffset;
                    }

                    (*maskSkirtPoly)[j + (2 * num_i + 2 * num_j - 5) - 2 * j] = ndc;
                }
            }

            for (int j = 0; j < num_j; j++)
            {
                for (int i = 0; i < num_i; i++)
                {
                    //int index = indices[min_j*numColumns + i + min_i];
                    {
                        osg::Vec3d ndc( ((double)(i + min_i))/(double)(d.numCols-1), ((double)(j+min_j))/(double)(d.numRows-1), 0.0);

                        //if (elevationLayer)
                        if ( hasElev )
                        {
                            float value = 0.0f;
                            if ( d.model->_elevationData.getHeight( ndc, d.model->_tileLocator.get(), value, INTERP_BILINEAR ) )
                                ndc.z() = value * d.heightScale + d.heightOffset;
                        }

                        coordsArray->push_back(ndc) ;
                    }						
                }
            }


            // Use delaunay triangulation for stitching:
            for (MaskRecordVector::iterator mr = d.maskRecords.begin();mr != d.maskRecords.end();mr++)
            {

                // Add the outter stitching bounds to the collection of vertices to be used for triangulation
                //	coordsArray->insert(coordsArray->end(), (*mr)._internal->begin(), (*mr)._internal->end());
                //Create local polygon representing mask
                osg::ref_ptr<Polygon> maskPoly = new osgEarth::Polygon();
                for (osg::Vec3dArray::iterator it = (*mr)._boundary->begin(); it != (*mr)._boundary->end(); ++it)
                {
                    osg::Vec3d local;
                    d.geoLocator->convertModelToLocal(*it, local);
                    maskPoly->push_back(local);
                }
                // Add mask bounds as a triangulation constraint

                osg::ref_ptr<osgUtil::DelaunayConstraint> newdc=new osgUtil::DelaunayConstraint;
                osg::ref_ptr<osg::Vec3Array> maskConstraint = new osg::Vec3Array();
                newdc->setVertexArray(maskConstraint.get());

                //Crop the mask to the stitching poly (for case where mask crosses tile edge)
                osg::ref_ptr<Geometry> maskCrop;
                maskPoly->crop(maskSkirtPoly.get(), maskCrop);

                GeometryIterator i( maskCrop.get(), false );
                while( i.hasMore() )
                {
                    Geometry* part = i.next();
                    if (!part)
                        continue;

                    if (part->getType() == Geometry::TYPE_POLYGON)
                    {
                        osg::ref_ptr<osg::Vec3Array> partVerts = part->createVec3Array();
                        maskConstraint->insert(maskConstraint->end(), partVerts->begin(), partVerts->end());
                        newdc->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINE_LOOP, maskConstraint->size() - partVerts->size(), partVerts->size()));
                    }
                }

                // Cropping strips z-values so need reassign
                std::vector<int> isZSet;
                for (osg::Vec3Array::iterator it = maskConstraint->begin(); it != maskConstraint->end(); ++it)
                {
                    int zSet = 0;

                    //Look for verts that belong to the original mask skirt polygon
                    for (Polygon::iterator mit = maskSkirtPoly->begin(); mit != maskSkirtPoly->end(); ++mit)
                    {
                        if (osg::absolute((*mit).x() - (*it).x()) < MATCH_TOLERANCE && osg::absolute((*mit).y() - (*it).y()) < MATCH_TOLERANCE)
                        {
                            (*it).z() = (*mit).z();
                            zSet += 1;

                            // Remove duplicate point from coordsArray to avoid duplicate point warnings
                            osg::Vec3Array::iterator caIt;
                            for (caIt = coordsArray->begin(); caIt != coordsArray->end(); ++caIt)
                            {
                                if (osg::absolute((*caIt).x() - (*it).x()) < MATCH_TOLERANCE && osg::absolute((*caIt).y() - (*it).y()) < MATCH_TOLERANCE)
                                    break;
                            }
                            if (caIt != coordsArray->end())
                                coordsArray->erase(caIt);

                            break;
                        }
                    }

                    //Look for verts that belong to the mask polygon
                    for (Polygon::iterator mit = maskPoly->begin(); mit != maskPoly->end(); ++mit)
                    {
                        if (osg::absolute((*mit).x() - (*it).x()) < MATCH_TOLERANCE && osg::absolute((*mit).y() - (*it).y()) < MATCH_TOLERANCE)
                        {
                            (*it).z() = (*mit).z();
                            zSet += 2;
                            break;
                        }
                    }

                    isZSet.push_back(zSet);
                }

                //Any mask skirt verts that are still unset are newly created verts where the skirt
                //meets the mask. Find the mask segment the point lies along and calculate the
                //appropriate z value for the point.
                int count = 0;
                for (osg::Vec3Array::iterator it = maskConstraint->begin(); it != maskConstraint->end(); ++it)
                {
                    //If the z-value was set from a mask vertex there is no need to change it.  If
                    //it was set from a vertex from the stitching polygon it may need overridden if
                    //the vertex lies along a mask edge.  Or if it is unset, it will need to be set.
                    //if (isZSet[count] < 2)
                    if (!isZSet[count])
                    {
                        osg::Vec3d p2 = *it;
                        double closestZ = 0.0;
                        double closestRatio = DBL_MAX;
                        for (Polygon::iterator mit = maskPoly->begin(); mit != maskPoly->end(); ++mit)
                        {
                            osg::Vec3d p1 = *mit;
                            osg::Vec3d p3 = mit == --maskPoly->end() ? maskPoly->front() : (*(mit + 1));

                            //Truncated vales to compensate for accuracy issues
                            double p1x = ((int)(p1.x() * 1000000)) / 1000000.0L;
                            double p3x = ((int)(p3.x() * 1000000)) / 1000000.0L;
                            double p2x = ((int)(p2.x() * 1000000)) / 1000000.0L;

                            double p1y = ((int)(p1.y() * 1000000)) / 1000000.0L;
                            double p3y = ((int)(p3.y() * 1000000)) / 1000000.0L;
                            double p2y = ((int)(p2.y() * 1000000)) / 1000000.0L;

                            if ((p1x < p3x ? p2x >= p1x && p2x <= p3x : p2x >= p3x && p2x <= p1x) &&
                                (p1y < p3y ? p2y >= p1y && p2y <= p3y : p2y >= p3y && p2y <= p1y))
                            {
                                double l1 =(osg::Vec2d(p2.x(), p2.y()) - osg::Vec2d(p1.x(), p1.y())).length();
                                double lt = (osg::Vec2d(p3.x(), p3.y()) - osg::Vec2d(p1.x(), p1.y())).length();
                                double zmag = p3.z() - p1.z();

                                double foundZ = (l1 / lt) * zmag + p1.z();

                                double mRatio = 1.0;
                                if (osg::absolute(p1x - p3x) < MATCH_TOLERANCE)
                                {
                                    if (osg::absolute(p1x-p2x) < MATCH_TOLERANCE)
                                        mRatio = 0.0;
                                }
                                else
                                {
                                    double m1 = p1x == p2x ? 0.0 : (p2y - p1y) / (p2x - p1x);
                                    double m2 = p1x == p3x ? 0.0 : (p3y - p1y) / (p3x - p1x);
                                    mRatio = m2 == 0.0 ? m1 : osg::absolute(1.0L - m1 / m2);
                                }

                                if (mRatio < 0.01)
                                {
                                    (*it).z() = foundZ;
                                    isZSet[count] = 2;
                                    break;
                                }
                                else if (mRatio < closestRatio)
                                {
                                    closestRatio = mRatio;
                                    closestZ = foundZ;
                                }
                            }
                        }

                        if (!isZSet[count] && closestRatio < DBL_MAX)
                        {
                            (*it).z() = closestZ;
                            isZSet[count] = 2;
                        }
                    }

                    if (!isZSet[count])
                        OE_WARN << LC << "Z-value not set for mask constraint vertex" << std::endl;

                    count++;
                }

                alldcs.push_back(newdc);
            }


            //coordsArray->insert(coordsArray->end(),maskSkirtPoly->begin(),maskSkirtPoly->end());
            trig->setInputPointArray(coordsArray.get());


            for (int dcnum =0; dcnum < alldcs.size();dcnum++)
            {
                trig->addInputConstraint(alldcs[dcnum].get());
            }

            // Create array to hold vertex normals
            osg::ref_ptr<osg::Vec3Array> norms = new osg::Vec3Array;
            trig->setOutputNormalArray(norms.get());


            // Triangulate vertices and remove triangles that lie within the contraint loop
            trig->triangulate();
            for (int dcnum =0; dcnum < alldcs.size();dcnum++)
            {

                trig->removeInternalTriangles(alldcs[dcnum].get());
            }             


            MaskRecordVector::iterator mr = d.maskRecords.begin();
            // Set up new arrays to hold final vertices and normals
            osg::ref_ptr<osg::Geometry> stitch_geom = (*mr)._geom;

            osg::ref_ptr<osg::Vec3Array> stitch_verts = new osg::Vec3Array();
            stitch_verts->reserve(trig->getInputPointArray()->size());
            stitch_geom->setVertexArray(stitch_verts.get());

            osg::ref_ptr<osg::Vec3Array> stitch_norms = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, trig->getInputPointArray()->size());
            stitch_geom->setNormalArray( stitch_norms.get() );


            // vertex attribution
            // for each vertex, a vec4 containing a unit extrusion vector in [0..2] and the raw elevation in [3]
            osg::ref_ptr<osg::Vec4Array> surfaceAttribs = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
            surfaceAttribs->setNormalize(false);
            surfaceAttribs->reserve( trig->getInputPointArray()->size() );
            stitch_geom->setVertexAttribArray( osg::Drawable::ATTRIBUTE_6, surfaceAttribs.get() );

            // for each vertex, index 0 holds the interpolated elevation from the lower lod (for morphing)
            osg::ref_ptr<osg::Vec4Array> surfaceAttribs2 = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
            surfaceAttribs2->setNormalize(false);
            surfaceAttribs2->reserve( trig->getInputPointArray()->size() );
            stitch_geom->setVertexAttribArray( osg::Drawable::ATTRIBUTE_7, surfaceAttribs2.get() );


            //Initialize tex coords
            if ( d.renderLayers.size() > 0 )
            {
                for (unsigned int i = 0; i < d.renderLayers.size(); ++i)
                {
                    d.renderLayers[i]._stitchTexCoords->reserve(trig->getInputPointArray()->size());
                }
            }
            d.stitchTileCoords->reserve(trig->getInputPointArray()->size());

            // Iterate through point to convert to model coords, calculate normals, and set up tex coords
            int norm_i = -1;
            for (osg::Vec3Array::iterator it = trig->getInputPointArray()->begin(); it != trig->getInputPointArray()->end(); ++it)
            {
                // get model coords
                osg::Vec3d model;
                d.model->_tileLocator->convertLocalToModel(*it, model);
                model = model * d.world2local;

                stitch_verts->push_back(model);

                // calc normals
                osg::Vec3d local_zero(*it);
                osg::Vec3d local_one(*it);
                local_one.z() += 1.0;
                osg::Vec3d model_one;
                d.model->_tileLocator->convertLocalToModel( local_one, model_one );
                model_one = (model_one*d.world2local) - model;
                model_one.normalize();
                (*stitch_norms)[++norm_i] = model_one;


                // calculate and set vertext attributes for newly created mask geometry
                osg::Vec3d model_up;
                d.model->_tileLocator->unitToModel(local_one, model_up);
                model_up = (model_up*d.world2local) - local_zero;
                model_up.normalize();
                (*d.normals).push_back(model_up);

                // Calculate and store the "old height", i.e the height value from
                // the parent LOD.
                float heightValue = 0.0;
                d.model->_elevationData.getHeight( local_zero, d.model->_tileLocator.get(), heightValue, INTERP_TRIANGULATE );

                float     oldHeightValue = heightValue;
                osg::Vec3 oldNormal;

                // This only works if the tile size is an odd number in both directions.
                if (d.model->_tileKey.getLOD() > 0 && (d.numCols&1) && (d.numRows&1) && d.parentModel.valid())
                {
                    d.parentModel->_elevationData.getHeight( local_zero, d.model->_tileLocator.get(), oldHeightValue, INTERP_TRIANGULATE );
                    d.parentModel->_elevationData.getNormal( local_zero, d.model->_tileLocator.get(), oldNormal, INTERP_TRIANGULATE );
                }
                else
                {
                    d.model->_elevationData.getNormal(local_zero, d.model->_tileLocator.get(), oldNormal, INTERP_TRIANGULATE );
                }

                // first attribute set has the unit extrusion vector and the
                // raw height value.
                (*surfaceAttribs).push_back( osg::Vec4f(
                    model_up.x(),
                    model_up.y(),
                    model_up.z(),
                    heightValue) );

                // second attribute set has the old height value in "w"
                (*surfaceAttribs2).push_back( osg::Vec4f(
                    oldNormal.x(),
                    oldNormal.y(),
                    oldNormal.z(),
                    oldHeightValue ) );


                // set up text coords
                if (d.renderLayers.size() > 0)
                {
                    for (unsigned int i = 0; i < d.renderLayers.size(); ++i)
                    {
                        if (!d.renderLayers[i]._locator->isEquivalentTo( *d.geoLocator.get() )) //*masterTextureLocator.get()))
                        {
                            osg::Vec3d color_ndc;
                            osgTerrain::Locator::convertLocalCoordBetween(*d.geoLocator.get(), (*it), *d.renderLayers[i]._locator.get(), color_ndc);
                            d.renderLayers[i]._stitchTexCoords->push_back(osg::Vec2(color_ndc.x(), color_ndc.y()));
                        }
                        else
                        {
                            d.renderLayers[i]._stitchTexCoords->push_back(osg::Vec2((*it).x(), (*it).y()));
                        }
                    }
                }
                d.stitchTileCoords->push_back(osg::Vec2((*it).x(), (*it).y()));
            }


            // Get triangles from triangulator and add as primitive set to the geometry
            osg::ref_ptr<osg::DrawElementsUInt> tris = trig->getTriangles();
            if ( tris && tris->getNumIndices() >= 3 )
            {
                stitch_geom->addPrimitiveSet(tris.get());
            }

            // Finally, add it to the geode.
            if (stitch_geom->getVertexArray() &&
                stitch_geom->getVertexArray()->getNumElements() > 0 )
            {
                d.surfaceGeode->addDrawable(stitch_geom);
            }
        }
    }


    /**
     * Build the geometry for the tile "skirts" -- this the vertical geometry around the
     * tile edges that hides the gap effect caused when you render two adjacent tiles at
     * different LODs.
     */
    void createSkirtGeometry( Data& d, double skirtRatio )
    {
        // find the skirt height
        double skirtHeight = d.surfaceBound.radius() * skirtRatio;

        // build the verts first:
        osg::Vec3Array* skirtVerts = static_cast<osg::Vec3Array*>(d.surface->getVertexArray());
        osg::Vec3Array* skirtNormals = static_cast<osg::Vec3Array*>(d.surface->getNormalArray());
        osg::Vec4Array* skirtAttribs = static_cast<osg::Vec4Array*>(d.surface->getVertexAttribArray(osg::Drawable::ATTRIBUTE_6)); //new osg::Vec4Array();
        osg::Vec4Array* skirtAttribs2 = static_cast<osg::Vec4Array*>(d.surface->getVertexAttribArray(osg::Drawable::ATTRIBUTE_7)); //new osg::Vec4Array();

        // If we don't have any primitive sets on the surface then bail.  This might happen if a tile falls completely in a mask.
        if (d.surface->getNumPrimitiveSets() == 0)
        {
            return;
        }

        // Use the existing DrawElements on the surface so we merge the skirts and the surface together.
        osg::ref_ptr<osg::DrawElements> elements;
        if ( d.surface->getNumPrimitiveSets() > 0 )
        {
            elements = dynamic_cast< osg::DrawElements* >(d.surface->getPrimitiveSet(0));
        }
        if (!elements)
        {
            OE_WARN << LC << "Couldn't find existing DrawElements" << std::endl;
            return;
        }
       
        int skirtIndex = 0;
        // bottom:
        for( unsigned int c=0; c < d.numCols; ++c )
        {
            int orig_i = d.indices[c];
          
            if (orig_i < 0)
            {
                skirtIndex = 0;
            }
            else
            {
                const osg::Vec4f& surfaceAttribs = (*d.surfaceAttribs)[orig_i];
                skirtAttribs->push_back( surfaceAttribs - osg::Vec4f(0,0,0,skirtHeight) );

                osg::Vec3 skirtVector( surfaceAttribs.x(), surfaceAttribs.y(), surfaceAttribs.z() );

                const osg::Vec3f& surfaceVert = (*d.surfaceVerts)[orig_i];
                skirtVerts->push_back( surfaceVert - skirtVector*skirtHeight );

                const osg::Vec3f& surfaceNormal = (*d.normals)[orig_i];
                skirtNormals->push_back( surfaceNormal );

                const osg::Vec4f& surfaceAttribs2 = (*d.surfaceAttribs2)[orig_i];
                skirtAttribs2->push_back( surfaceAttribs2 - osg::Vec4f(0,0,0,skirtHeight) );

                if ( d.renderLayers.size() > 0 )
                {
                    for (unsigned int i = 0; i < d.renderLayers.size(); ++i)
                    {
                        if ( d.renderLayers[i]._ownsTexCoords )
                        {
                            const osg::Vec2& tc = (*d.renderLayers[i]._texCoords.get())[orig_i];
                            d.renderLayers[i]._texCoords->push_back( tc );
                        }                        
                    }
                }

                const osg::Vec2& tilec = (*d.renderTileCoords.get())[orig_i];
                d.renderTileCoords->push_back( tilec );

                skirtIndex++;
                if (skirtIndex > 1)
                {
                    int prev_i = d.indices[c-1];
                    elements->addElement(prev_i);
                    elements->addElement(skirtVerts->size() - 2);
                    elements->addElement(orig_i);

                    elements->addElement(skirtVerts->size() - 1);
                    elements->addElement(orig_i);
                    elements->addElement(skirtVerts->size() - 2);
                }
            }
            
        }

        // right:
        skirtIndex = 0;
        for( unsigned int r=0; r < d.numRows; ++r )
        {
            int orig_i = d.indices[r*d.numCols+(d.numCols-1)];
            if (orig_i < 0)
            {
                skirtIndex = 0;
            }
            else
            {
                const osg::Vec4f& surfaceAttribs = (*d.surfaceAttribs)[orig_i];
                skirtAttribs->push_back( surfaceAttribs - osg::Vec4f(0,0,0,skirtHeight) );

                osg::Vec3 skirtVector( surfaceAttribs.x(), surfaceAttribs.y(), surfaceAttribs.z() );

                const osg::Vec3f& surfaceVert = (*d.surfaceVerts)[orig_i];
                skirtVerts->push_back( surfaceVert - skirtVector*skirtHeight );

                const osg::Vec3f& surfaceNormal = (*d.normals)[orig_i];
                skirtNormals->push_back( surfaceNormal );

            
                const osg::Vec4f& surfaceAttribs2 = (*d.surfaceAttribs2)[orig_i];
                skirtAttribs2->push_back( surfaceAttribs2 - osg::Vec4f(0,0,0,skirtHeight) );

                if ( d.renderLayers.size() > 0 )
                {
                    for (unsigned int i = 0; i < d.renderLayers.size(); ++i)
                    {
                        if ( d.renderLayers[i]._ownsTexCoords )
                        {
                            const osg::Vec2& tc = (*d.renderLayers[i]._texCoords.get())[orig_i];
                            d.renderLayers[i]._texCoords->push_back( tc );
                        }
                    }
                }

                const osg::Vec2& tilec = (*d.renderTileCoords.get())[orig_i];
                d.renderTileCoords->push_back( tilec );

                skirtIndex++;
                if (skirtIndex > 1)
                {
                    int prev_i = d.indices[(r-1)*d.numCols+(d.numCols-1)];
                    elements->addElement(prev_i);
                    elements->addElement(skirtVerts->size() - 2);
                    elements->addElement(orig_i);

                    elements->addElement(skirtVerts->size() - 1);
                    elements->addElement(orig_i);
                    elements->addElement(skirtVerts->size() - 2);
                }
            }
        }

        // top:
        skirtIndex = 0;
        for( int c=d.numCols-1; c >= 0; --c )
        {
            int orig_i = d.indices[(d.numRows-1)*d.numCols+c];
            if (orig_i < 0)
            {
                skirtIndex = 0;
            }
            else
            {
                const osg::Vec4f& surfaceAttribs = (*d.surfaceAttribs)[orig_i];
                skirtAttribs->push_back( surfaceAttribs - osg::Vec4f(0,0,0,skirtHeight) );

                osg::Vec3 skirtVector( surfaceAttribs.x(), surfaceAttribs.y(), surfaceAttribs.z() );

                const osg::Vec3f& surfaceVert = (*d.surfaceVerts)[orig_i];
                skirtVerts->push_back( surfaceVert - skirtVector*skirtHeight );

                const osg::Vec3f& surfaceNormal = (*d.normals)[orig_i];
                skirtNormals->push_back( surfaceNormal );

                const osg::Vec4f& surfaceAttribs2 = (*d.surfaceAttribs2)[orig_i];
                skirtAttribs2->push_back( surfaceAttribs2 - osg::Vec4f(0,0,0,skirtHeight) );

                if ( d.renderLayers.size() > 0 )
                {
                    for (unsigned int i = 0; i < d.renderLayers.size(); ++i)
                    {
                        if ( d.renderLayers[i]._ownsTexCoords )
                        {
                            const osg::Vec2& tc = (*d.renderLayers[i]._texCoords.get())[orig_i];
                            d.renderLayers[i]._texCoords->push_back( tc );
                        }
                    }
                }

                const osg::Vec2& tilec = (*d.renderTileCoords.get())[orig_i];
                d.renderTileCoords->push_back( tilec );

                skirtIndex++;
                if (skirtIndex > 1)
                {
                    int prev_i = d.indices[(d.numRows - 1)*d.numCols + c + 1];
                    elements->addElement(prev_i);
                    elements->addElement(skirtVerts->size() - 2);
                    elements->addElement(orig_i);

                    elements->addElement(skirtVerts->size() - 1);
                    elements->addElement(orig_i);
                    elements->addElement(skirtVerts->size() - 2);
                }
            }
        }

        // left:
        skirtIndex = 0;
        for( int r=d.numRows-1; r>=0; --r )
        {
            int orig_i = d.indices[r*d.numCols];
            if (orig_i < 0)
            {
                skirtIndex = 0;
            }
            else
            {
                const osg::Vec4f& surfaceAttribs = (*d.surfaceAttribs)[orig_i];
                skirtAttribs->push_back( surfaceAttribs - osg::Vec4f(0,0,0,skirtHeight) );

                 osg::Vec3 skirtVector( surfaceAttribs.x(), surfaceAttribs.y(), surfaceAttribs.z() );

                const osg::Vec3f& surfaceVert = (*d.surfaceVerts)[orig_i];
                skirtVerts->push_back( surfaceVert - skirtVector*skirtHeight );

                const osg::Vec3f& surfaceNormal = (*d.normals)[orig_i];
                skirtNormals->push_back( surfaceNormal );

                const osg::Vec4f& surfaceAttribs2 = (*d.surfaceAttribs2)[orig_i];
                skirtAttribs2->push_back( surfaceAttribs2 - osg::Vec4f(0,0,0,skirtHeight) );

                if ( d.renderLayers.size() > 0 )
                {
                    for (unsigned int i = 0; i < d.renderLayers.size(); ++i)
                    {
                        if ( d.renderLayers[i]._ownsTexCoords )
                        {
                            const osg::Vec2& tc = (*d.renderLayers[i]._texCoords.get())[orig_i];
                            d.renderLayers[i]._texCoords->push_back( tc );
                        }
                    }
                }

                const osg::Vec2& tilec = (*d.renderTileCoords.get())[orig_i];
                d.renderTileCoords->push_back( tilec );

                skirtIndex++;
                if (skirtIndex > 1)
                {
                    int prev_i = d.indices[(r + 1)*d.numCols];
                    elements->addElement(prev_i);
                    elements->addElement(skirtVerts->size() - 2);
                    elements->addElement(orig_i);

                    elements->addElement(skirtVerts->size() - 1);
                    elements->addElement(orig_i);
                    elements->addElement(skirtVerts->size() - 2);
                }
            }
        }
    }



    /**
     * Builds triangles for the surface geometry, and recalculates the surface normals
     * to be optimized for slope.
     */
    void tessellateSurfaceGeometry( Data& d, bool optimizeTriangleOrientation, bool normalizeEdges )
    {    
        bool swapOrientation = !(d.model->_tileLocator->orientationOpenGL());

        bool recalcNormals   =
            d.model->hasElevation() && 
            !d.model->hasNormalMap();

        unsigned numSurfaceNormals = d.numRows * d.numCols;

        GLenum mode = d.usePatches ? GL_PATCHES : GL_TRIANGLES;
        osg::ref_ptr<osg::DrawElements> elements = d.newDrawElements(mode);
        elements->reserveElements((d.numRows-1) * (d.numCols-1) * 6);

        if ( recalcNormals )
        {
            // first clear out all the normals on the surface (but not the skirts)
            // TODO: someday go back and re-apply the skirt normals to match the
            // corresponding recalculated surface normals.
            for(unsigned n=0; n<numSurfaceNormals && n<d.normals->size(); ++n)
            {
                (*d.normals)[n].set( 0.0f, 0.0f, 0.0f );
            }
        }

        for(unsigned j=0; j<d.numRows-1; ++j)
        {
            for(unsigned i=0; i<d.numCols-1; ++i)
            {
                int i00;
                int i01;
                if (swapOrientation)
                {
                    i01 = j*d.numCols + i;
                    i00 = i01+d.numCols;
                }
                else
                {
                    i00 = j*d.numCols + i;
                    i01 = i00+d.numCols;
                }

                int i10 = i00+1;
                int i11 = i01+1;

                // remap indices to final vertex positions
                i00 = d.indices[i00];
                i01 = d.indices[i01];
                i10 = d.indices[i10];
                i11 = d.indices[i11];

                unsigned int numValid = 0;
                if (i00>=0) ++numValid;
                if (i01>=0) ++numValid;
                if (i10>=0) ++numValid;
                if (i11>=0) ++numValid;                

                if (numValid==4)
                {
                    bool VALID = true;
                    for (MaskRecordVector::iterator mr = d.maskRecords.begin(); mr != d.maskRecords.end(); ++mr)
                    {
                        float min_i = (*mr)._ndcMin.x() * (double)(d.numCols-1);
                        float min_j = (*mr)._ndcMin.y() * (double)(d.numRows-1);
                        float max_i = (*mr)._ndcMax.x() * (double)(d.numCols-1);
                        float max_j = (*mr)._ndcMax.y() * (double)(d.numRows-1);

                        // We test if mask is completely in square
                        if(i+1 >= min_i && i <= max_i && j+1 >= min_j && j <= max_j)
                        {
                            VALID = false;
                            break;
                        }
                    }

                    if (VALID) {
                        float e00 = (*d.elevations)[i00];
                        float e10 = (*d.elevations)[i10];
                        float e01 = (*d.elevations)[i01];
                        float e11 = (*d.elevations)[i11];

                        osg::Vec3f& v00 = (*d.surfaceVerts)[i00];
                        osg::Vec3f& v10 = (*d.surfaceVerts)[i10];
                        osg::Vec3f& v01 = (*d.surfaceVerts)[i01];
                        osg::Vec3f& v11 = (*d.surfaceVerts)[i11];

                        if (!optimizeTriangleOrientation || fabsf(e00-e11)<fabsf(e01-e10))
                        {
                            elements->addElement(i01);
                            elements->addElement(i00);
                            elements->addElement(i11);

                            elements->addElement(i00);
                            elements->addElement(i10);
                            elements->addElement(i11);

                            if (recalcNormals)
                            {                        
                                osg::Vec3 normal1 = (v00-v01) ^ (v11-v01);
                                (*d.normals)[i01] += normal1;
                                (*d.normals)[i00] += normal1;
                                (*d.normals)[i11] += normal1;

                                osg::Vec3 normal2 = (v10-v00) ^ (v11-v00);
                                (*d.normals)[i00] += normal2;
                                (*d.normals)[i10] += normal2;
                                (*d.normals)[i11] += normal2;
                            }
                        }
                        else
                        {
                            elements->addElement(i01);
                            elements->addElement(i00);
                            elements->addElement(i10);

                            elements->addElement(i01);
                            elements->addElement(i10);
                            elements->addElement(i11);

                            if (recalcNormals)
                            {                       
                                osg::Vec3 normal1 = (v00-v01) ^ (v10-v01);
                                (*d.normals)[i01] += normal1;
                                (*d.normals)[i00] += normal1;
                                (*d.normals)[i10] += normal1;

                                osg::Vec3 normal2 = (v10-v01) ^ (v11-v01);
                                (*d.normals)[i01] += normal2;
                                (*d.normals)[i10] += normal2;
                                (*d.normals)[i11] += normal2;
                            }
                        }
                    }
                }
            }
        }        
        
        if (recalcNormals && normalizeEdges)
        {            
            //OE_DEBUG << LC << "Normalizing edges" << std::endl;

            //Compute the edge normals if we have neighbor data
            //Get all the neighbors
            osg::HeightField* w_neighbor  = d.model->_elevationData.getNeighbor( -1, 0 );
            osg::HeightField* e_neighbor  = d.model->_elevationData.getNeighbor( 1, 0 );
            osg::HeightField* s_neighbor  = d.model->_elevationData.getNeighbor( 0, 1 );
            osg::HeightField* n_neighbor  = d.model->_elevationData.getNeighbor( 0, -1 );

            // Utility arrays:
            std::vector<osg::Vec3> boundaryVerts;
            boundaryVerts.reserve( 2 * osg::maximum(d.numRows, d.numCols) );

            std::vector< float > boundaryElevations;
            boundaryElevations.reserve( 2 * osg::maximum(d.numRows, d.numCols) );

            //Recalculate the west side
            if (w_neighbor && w_neighbor->getNumColumns() == d.originalNumCols && w_neighbor->getNumRows() == d.originalNumRows)
            {
                boundaryVerts.clear();
                boundaryElevations.clear();
                
                //Compute the verts for the west side
                for (int j = 0; j < (int)d.numRows; j++)
                {
                    for (int i = (int)d.numCols-2; i <= (int)d.numCols-1; i++)
                    {                          
                        osg::Vec3d ndc( (double)(i - static_cast<int>(d.numCols-1))/(double)(d.numCols-1), ((double)j)/(double)(d.numRows-1), 0.0);                                                                        

                        // use the sampling factor to determine the lookup index:
                        unsigned i_equiv = d.i_sampleFactor==1.0 ? i : (unsigned) (double(i)*d.i_sampleFactor);
                        unsigned j_equiv = d.j_sampleFactor==1.0 ? j : (unsigned) (double(j)*d.j_sampleFactor);

                        //TODO:  Should probably use an interpolated method here
                        float heightValue = w_neighbor->getHeight( i_equiv, j_equiv );
                        ndc.z() = heightValue;

                        osg::Vec3d model;
                        d.model->_tileLocator->unitToModel( ndc, model );
                        osg::Vec3d v = model * d.world2local; //model - d.centerModel;
                        boundaryVerts.push_back( v );
                        boundaryElevations.push_back( heightValue );
                    }
                }   

                //The boundary verts are now populated, so go through and triangulate them add add the normals to the existing normal array
                for (int j = 0; j < (int)d.numRows-1; j++)
                {                    
                    int i00;
                    int i01;
                    int i = 0;
                    if (swapOrientation)
                    {
                        i01 = j*d.numCols + i;
                        i00 = i01+d.numCols;
                    }
                    else
                    {
                        i00 = j*d.numCols + i;
                        i01 = i00+d.numCols;
                    }



                    //remap indices to final vertex position
                    i00 = d.indices[i00];
                    i01 = d.indices[i01];

                    if ( i00 >= 0 && i01 >= 0 )
                    {
                        int baseIndex = 2 * j;
                        osg::Vec3f& v00 = boundaryVerts[baseIndex    ];
                        osg::Vec3f& v10 = boundaryVerts[baseIndex + 1];
                        osg::Vec3f& v01 = boundaryVerts[baseIndex + 2];
                        osg::Vec3f& v11 = boundaryVerts[baseIndex + 3];

                        float e00 = boundaryElevations[baseIndex];
                        float e10 = boundaryElevations[baseIndex + 1];
                        float e01 = boundaryElevations[baseIndex + 2];
                        float e11 = boundaryElevations[baseIndex + 3];

                       
                        if (!optimizeTriangleOrientation || fabsf(e00-e11)<fabsf(e01-e10))
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v11-v01);
                            (*d.normals)[i01] += normal1;                        

                            osg::Vec3 normal2 = (v10-v00) ^ (v11-v00);
                            (*d.normals)[i00] += normal2;                        
                            (*d.normals)[i01] += normal2;                                                
                        }
                        else
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v10-v01);
                            (*d.normals)[i00] += normal1;                                               

                            osg::Vec3 normal2 = (v10-v01) ^ (v11-v01);
                            (*d.normals)[i00] += normal2;                                               
                            (*d.normals)[i01] += normal2;                        
                        }
                    }
                }
            }

                        
            //Recalculate the east side
            if (e_neighbor && e_neighbor->getNumColumns() == d.originalNumCols && e_neighbor->getNumRows() == d.originalNumRows)            
            {
                boundaryVerts.clear();
                boundaryElevations.clear();

                //Compute the verts for the east side
                for (int j = 0; j < (int)d.numRows; j++)
                {
                    for (int i = 0; i <= 1; i++)
                    {                           
                        osg::Vec3d ndc( ((double)(d.numCols -1 + i))/(double)(d.numCols-1), ((double)j)/(double)(d.numRows-1), 0.0);

                        unsigned i_equiv = d.i_sampleFactor==1.0 ? i : (unsigned) (double(i)*d.i_sampleFactor);
                        unsigned j_equiv = d.j_sampleFactor==1.0 ? j : (unsigned) (double(j)*d.j_sampleFactor);
                        
                        //TODO:  Should probably use an interpolated method here
                        float heightValue = e_neighbor->getHeight( i_equiv, j_equiv );
                        ndc.z() = heightValue;

                        osg::Vec3d model;
                        d.model->_tileLocator->unitToModel( ndc, model );
                        osg::Vec3d v = model * d.world2local; //model - d.centerModel;
                        boundaryVerts.push_back( v );
                        boundaryElevations.push_back( heightValue );
                    }
                }   

                //The boundary verts are now populated, so go through and triangulate them add add the normals to the existing normal array
                for (int j = 0; j < (int)d.numRows-1; j++)
                {
                    int i00;
                    int i01;
                    int i = d.numCols-1;
                    if (swapOrientation)
                    {
                        i01 = j*d.numCols + i;
                        i00 = i01+d.numCols;
                    }
                    else
                    {
                        i00 = j*d.numCols + i;
                        i01 = i00+d.numCols;
                    }

                    //remap indices to final vertex position
                    i00 = d.indices[i00];
                    i01 = d.indices[i01];

                    if ( i00 >= 0 && i01 >= 0 )
                    {
                        int baseIndex = 2 * j;
                        osg::Vec3f& v00 = boundaryVerts[baseIndex    ];
                        osg::Vec3f& v10 = boundaryVerts[baseIndex + 1];
                        osg::Vec3f& v01 = boundaryVerts[baseIndex + 2];
                        osg::Vec3f& v11 = boundaryVerts[baseIndex + 3];

                        float e00 = boundaryElevations[baseIndex];
                        float e10 = boundaryElevations[baseIndex + 1];
                        float e01 = boundaryElevations[baseIndex + 2];
                        float e11 = boundaryElevations[baseIndex + 3];

                       
                        if (!optimizeTriangleOrientation || fabsf(e00-e11)<fabsf(e01-e10))
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v11-v01);                       
                            (*d.normals)[i00] += normal1;                        
                            (*d.normals)[i01] += normal1;

                            osg::Vec3 normal2 = (v10-v00) ^ (v11-v00);                        
                            (*d.normals)[i00] += normal2;                                                
                        }
                        else
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v10-v01);
                            (*d.normals)[i00] += normal1;                        
                            (*d.normals)[i01] += normal1;                                                                        

                            osg::Vec3 normal2 = (v10-v01) ^ (v11-v01);
                            (*d.normals)[i01] += normal2;                        
                        }
                    }
                }
            }

            //Recalculate the north side
            if (n_neighbor && n_neighbor->getNumColumns() == d.originalNumCols && n_neighbor->getNumRows() == d.originalNumRows)            
            {
                boundaryVerts.clear();
                boundaryElevations.clear();

                //Compute the verts for the north side               
                for (int j = 0; j <= 1; j++)
                {
                    for (int i = 0; i < (int)d.numCols; i++)                    
                    {                           
                        osg::Vec3d ndc( (double)(i)/(double)(d.numCols-1), (double)(d.numRows -1 + j)/(double)(d.numRows-1), 0.0);

                        unsigned i_equiv = d.i_sampleFactor==1.0 ? i : (unsigned) (double(i)*d.i_sampleFactor);
                        unsigned j_equiv = d.j_sampleFactor==1.0 ? j : (unsigned) (double(j)*d.j_sampleFactor);
                        
                        //TODO:  Should probably use an interpolated method here
                        float heightValue = n_neighbor->getHeight( i_equiv, j_equiv );
                        ndc.z() = heightValue;

                        osg::Vec3d model;
                        d.model->_tileLocator->unitToModel( ndc, model );
                        osg::Vec3d v = model * d.world2local; //model - d.centerModel;
                        boundaryVerts.push_back( v );
                        boundaryElevations.push_back( heightValue );
                    }
                }   

                //The boundary verts are now populated, so go through and triangulate them add add the normals to the existing normal array                
                for (int i = 0; i < (int)d.numCols-1; i++)
                {                    
                    int i00;                    
                    int j = d.numRows-1;
                    if (swapOrientation)
                    {         
                        int i01 = j * d.numCols + i;
                        i00 = i01+d.numCols;
                    }
                    else
                    {
                        i00 = j*d.numCols + i;                        
                    }

                    int i10 = i00+1;

                    //remap indices to final vertex position
                    i00 = d.indices[i00];
                    i10 = d.indices[i10];


                    if ( i00 >= 0 && i10 >= 0 )
                    {
                        int baseIndex = i;
                        osg::Vec3f& v00 = boundaryVerts[baseIndex    ];
                        osg::Vec3f& v10 = boundaryVerts[baseIndex + 1];
                        osg::Vec3f& v01 = boundaryVerts[baseIndex + d.numCols];
                        osg::Vec3f& v11 = boundaryVerts[baseIndex + d.numCols + 1];

                        float e00 = boundaryElevations[baseIndex];
                        float e10 = boundaryElevations[baseIndex + 1];
                        float e01 = boundaryElevations[baseIndex + d.numCols];
                        float e11 = boundaryElevations[baseIndex + d.numCols + 1];

                       
                        if (!optimizeTriangleOrientation || fabsf(e00-e11)<fabsf(e01-e10))
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v11-v01);                       
                            (*d.normals)[i00] += normal1;                        
                            (*d.normals)[i10] += normal1;

                            osg::Vec3 normal2 = (v10-v00) ^ (v11-v00);                        
                            (*d.normals)[i10] += normal2;                                                
                        }
                        else
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v10-v01);
                            (*d.normals)[i00] += normal1;                                                

                            osg::Vec3 normal2 = (v10-v01) ^ (v11-v01);
                            (*d.normals)[i00] += normal2;                                                
                            (*d.normals)[i10] += normal2;                        
                        }
                    }
                }
            }

            //Recalculate the south side
            if (s_neighbor && s_neighbor->getNumColumns() == d.originalNumCols && s_neighbor->getNumRows() == d.originalNumRows)            
            {
                boundaryVerts.clear();
                boundaryElevations.clear();

                //Compute the verts for the south side               
                for (int j = (int)d.numRows-2; j <= (int)d.numRows-1; j++)
                {
                    for (int i = 0; i < (int)d.numCols; i++)                    
                    {                           
                        osg::Vec3d ndc( (double)(i)/(double)(d.numCols-1), (double)(j - static_cast<int>(d.numRows-1))/(double)(d.numRows-1), 0.0);                                                

                        unsigned i_equiv = d.i_sampleFactor==1.0 ? i : (unsigned) (double(i)*d.i_sampleFactor);
                        unsigned j_equiv = d.j_sampleFactor==1.0 ? j : (unsigned) (double(j)*d.j_sampleFactor);
                        
                        //TODO:  Should probably use an interpolated method here
                        float heightValue = s_neighbor->getHeight( i_equiv, j_equiv );                        
                        ndc.z() = heightValue;

                        osg::Vec3d model;
                        d.model->_tileLocator->unitToModel( ndc, model );
                        osg::Vec3d v = model * d.world2local; //model - d.centerModel;
                        boundaryVerts.push_back( v );
                        boundaryElevations.push_back( heightValue ); 
                    }
                }   

                //The boundary verts are now populated, so go through and triangulate them add add the normals to the existing normal array                
                for (int i = 0; i < (int)d.numCols-1; i++)
                {                    
                    int i00;                    
                    int j = 0;


                    if (swapOrientation)
                    {                   
                        int i01 = j*d.numCols + i;
                        i00 = i01+d.numCols;                    
                    }
                    else
                    {
                        i00 = j*d.numCols + i;                        
                    }                    

                    int i10 = i00+1;

                    //remap indices to final vertex position
                    i00 = d.indices[i00];
                    i10 = d.indices[i10];


                    if ( i00 >= 0 && i10 >= 0 )
                    {
                        int baseIndex = i;
                        osg::Vec3f& v00 = boundaryVerts[baseIndex    ];
                        osg::Vec3f& v10 = boundaryVerts[baseIndex + 1];
                        osg::Vec3f& v01 = boundaryVerts[baseIndex + d.numCols];
                        osg::Vec3f& v11 = boundaryVerts[baseIndex + d.numCols + 1];

                        float e00 = boundaryElevations[baseIndex];
                        float e10 = boundaryElevations[baseIndex + 1];
                        float e01 = boundaryElevations[baseIndex + d.numCols];
                        float e11 = boundaryElevations[baseIndex + d.numCols + 1];

                       
                        if (!optimizeTriangleOrientation || fabsf(e00-e11)<fabsf(e01-e10))
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v11-v01);                       
                            (*d.normals)[i00] += normal1;                                                

                            osg::Vec3 normal2 = (v10-v00) ^ (v11-v00);                        
                            (*d.normals)[i00] += normal2;                                                
                            (*d.normals)[i10] += normal2;                                                
                        }
                        else
                        {                            
                            osg::Vec3 normal1 = (v00-v01) ^ (v10-v01);
                            (*d.normals)[i00] += normal1;                                                
                            (*d.normals)[i10] += normal1;                                                

                            osg::Vec3 normal2 = (v10-v01) ^ (v11-v01);                        
                            (*d.normals)[i10] += normal2;                        
                        }
                    }
                }
            }            
        }

        if (recalcNormals)
        {
            for( osg::Vec3Array::iterator nitr = d.normals->begin(); nitr != d.normals->end(); ++nitr )
            {
                nitr->normalize();
            }       
        }

        // in the case of full-masking, this will be empty
        if ( elements->getNumIndices() > 0 )
        {
            d.surface->insertPrimitiveSet(0, elements.get()); // because we always want this first.
        }
    }


    void installRenderData( Data& d )
    {
        // pre-size all vectors:
        unsigned size = d.renderLayers.size();

        d.surface->_layers.resize( size );

        for ( MaskRecordVector::iterator mr = d.maskRecords.begin(); mr != d.maskRecords.end(); ++mr )
            mr->_geom->_layers.resize( size );
        
        if ( d.renderTileCoords.valid() )
            d.surface->_tileCoords = d.renderTileCoords;

        // install the tile coordinates in the geometry.
        if ( d.surface->_tileCoords.valid() )
        {
            int index = d.surface->getTexCoordArrayList().size();
            d.surface->setTexCoordArray( index, d.surface->_tileCoords.get() );
        }

        // install the render data for each layer:
        for( RenderLayerVector::const_iterator r = d.renderLayers.begin(); r != d.renderLayers.end(); ++r )
        {
            unsigned order = r->_layer.getOrder();

            MPGeometry::Layer layer;
            layer._layerID        = r->_layer.getUID();
            layer._imageLayer     = r->_layer.getMapLayer();
            layer._tex            = r->_layer.getTexture();
            layer._texParent      = r->_layerParent.getTexture();

            // cache stock opacity. Disable if a color filter is installed, since
            // it can modify the alpha.
            layer._opaque =
                (r->_layer.getMapLayer()->getColorFilters().size() == 0 ) &&
                (layer._tex.valid() && !r->_layer.hasAlpha()) &&
                (!layer._texParent.valid() || !r->_layerParent.hasAlpha()) &&
                (layer._imageLayer.valid() && layer._imageLayer->getMinVisibleRange() == 0.0f) &&
                (layer._imageLayer.valid() && layer._imageLayer->getMaxVisibleRange() == FLT_MAX);

            // texture matrix: scale/bias matrix of the texture. Currently we don't use
            // this for rendering because the scale/bias is already baked into the 
            // texture coordinates. BUT we still need it for sampling shared rasters etc.
            if ( r->_layer._locator.valid() )
            {
                osg::Matrixd sbmatrix;

                r->_layer._locator->createScaleBiasMatrix(
                    d.model->_tileLocator->getDataExtent(),
                    sbmatrix );

                layer._texMat = sbmatrix;

                // a shared layer needs access to a static uniform name.
                if ( layer._imageLayer->isShared() )
                {
                    layer._texMatUniformID = osg::Uniform::getNameID( layer._imageLayer->shareTexMatUniformName().get() );
                }
            }

            // parent texture matrix: it's a scale/bias matrix encoding the difference
            // between the two locators.
            if ( r->_layerParent.getLocator() )
            {
                osg::Matrixd sbmatrix;

                r->_layerParent.getLocator()->createScaleBiasMatrix(
                    r->_layer.getLocator()->getDataExtent(),
                    sbmatrix );

                layer._texMatParent = sbmatrix;
            }

            // the texture coords:
            layer._texCoords  = r->_texCoords.get();
            if ( r->_texCoords.valid() )
            {
                int index = d.surface->getTexCoordArrayList().size();
                d.surface->setTexCoordArray( index, r->_texCoords.get() );
            }

            d.surface->_layers[order] = layer;

            // the mask geometries:
            for ( MaskRecordVector::iterator mr = d.maskRecords.begin(); mr != d.maskRecords.end(); ++mr )
            {
                layer._texCoords = r->_stitchTexCoords.get();
                mr->_geom->_layers[order] = layer;
                mr->_geom->_tileCoords = d.stitchTileCoords.get();
            }
        }

        // elevation texture.
        d.surface->_elevTex = d.model->_elevationTexture.get();
    }

    osg::Geode* makeBBox(const Data& d)
    {        
        osg::Geode* geode = new osg::Geode();
        std::string sizeStr = "(empty)";
        float zpos = 0.0f;

        osg::ComputeBoundsVisitor cbv;
        d.surfaceGeode->accept( cbv );
        const osg::BoundingBox& bbox = cbv.getBoundingBox();
        if ( bbox.valid() )
        {
            osg::Geometry* geom = new osg::Geometry();
            geom->setName("bbox");
        
            osg::Vec3Array* v = new osg::Vec3Array();
            for(int i=0; i<8; ++i)
                v->push_back(bbox.corner(i));
            geom->setVertexArray(v);

            osg::DrawElementsUByte* de = new osg::DrawElementsUByte(GL_LINES);
            de->push_back(0); de->push_back(1);
            de->push_back(1); de->push_back(3);
            de->push_back(3); de->push_back(2);
            de->push_back(2); de->push_back(0);
            de->push_back(4); de->push_back(5);
            de->push_back(5); de->push_back(7);
            de->push_back(7); de->push_back(6);
            de->push_back(6); de->push_back(4);
            de->push_back(0); de->push_back(4);
            de->push_back(1); de->push_back(5);
            de->push_back(3); de->push_back(7);
            de->push_back(2); de->push_back(6);
            geom->addPrimitiveSet(de);

            osg::Vec4Array* c= new osg::Vec4Array(osg::Array::BIND_OVERALL);
            c->push_back(osg::Vec4(0,1,1,1));
            geom->setColorArray(c);

            geode->addDrawable(geom);

            sizeStr = Stringify() << bbox.xMax()-bbox.xMin();
            sizeStr = Stringify() << "min="<<bbox.zMin()<<"\nmax="<<bbox.zMax();
            zpos = bbox.zMax();
        }

        osgText::Text* t = new osgText::Text();
        t->setText( Stringify() << d.model->_tileKey.str() << "\n" << sizeStr );
        t->setFont( osgEarth::Registry::instance()->getDefaultFont() );
        t->setCharacterSizeMode(t->SCREEN_COORDS);
        t->setCharacterSize(36.0f);
        t->setAlignment(t->CENTER_CENTER);
        t->setColor(osg::Vec4(1,1,1,1));
        t->setBackdropColor(osg::Vec4(0,0,0,1));
        t->setBackdropType(t->OUTLINE);
        t->setPosition(osg::Vec3(0,0,zpos));
        geode->addDrawable(t);

        geode->getOrCreateStateSet()->setAttributeAndModes(new osg::Program(),0);
        geode->getOrCreateStateSet()->setMode(GL_LIGHTING,0); // ok .. FFP debugging code

        return geode;
    }
}

//------------------------------------------------------------------------

TileModelCompiler::TileModelCompiler(const MaskLayerVector&              maskLayers,
                                     const ModelLayerVector&             modelLayers,
                                     int                                 texImageUnit,
                                     bool                                optimizeTriOrientation,
                                     const MPTerrainEngineOptions& options) :
_maskLayers            ( maskLayers ),
_modelLayers           ( modelLayers ),
_optimizeTriOrientation( optimizeTriOrientation ),
_options               ( options ),
_textureImageUnit      ( texImageUnit )
{
    _debug =
        _options.debug() == true || 
        ::getenv("OSGEARTH_MP_DEBUG") != 0L;
}


TileNode*
TileModelCompiler::compile(TileModel*        model,
                           const MapFrame&   frame,
                           ProgressCallback* progress)
{

    // Working data for the build.
    Data d(model, frame, _maskLayers, _modelLayers);
    d.textureImageUnit = _textureImageUnit;

    GeoPoint centroid;
    model->_tileKey.getExtent().getCentroid(centroid);
    centroid.toWorld(d.centerModel);
    centroid.createLocalToWorld(d.local2world);
    d.world2local.invert(d.local2world);

    TileNode* tile = new TileNode( model->_tileKey, model, d.local2world );

    d.installParentData = model->useParentData();
    d.parentModel = model->getParentTileModel();
    d.heightScale = *_options.verticalScale();
    d.heightOffset = *_options.verticalOffset();
    d.usePatches = *_options.gpuTessellation();

    // A Geode/Geometry for the surface:
    d.surface = new MPGeometry( d.model->_tileKey, d.frame, _textureImageUnit );
    d.surface->setName( "surface" );
    d.surfaceGeode = new osg::Geode();
   
    tile->addChild( d.surfaceGeode );

    // Create the skirt if the heightfield skirt ratio is > 0.0
    d.createSkirt = (_options.heightFieldSkirtRatio().value() > 0.0);

    // adjust the tile locator for geocentric mode:
    d.geoLocator = model->_tileLocator->getCoordinateSystemType() == GeoLocator::GEOCENTRIC ? 
        model->_tileLocator->getGeographicFromGeocentric() :
        model->_tileLocator.get();

    // Set up any geometry-cutting masks:
    if ( d.maskLayers.size() > 0 || d.modelLayers.size() > 0 )
        setupMaskRecords( d );

    // allocate all the vertex, normal, and color arrays.
    setupGeometryAttributes( d, _options.tileSize().get() );

    // set up the list of layers to render and their shared arrays.
    setupTextureAttributes( d, _cache );

    // calculate the vertex and normals for the surface geometry.
    createSurfaceGeometry( d, _debug );

    // build geometry for the masked areas, if applicable
    if ( d.maskRecords.size() > 0 )
        createMaskGeometry( d );

    

    // at this point, make sure we actually built any surface geometry.
    if (d.surface->getVertexArray() &&
        d.surface->getVertexArray()->getNumElements() > 0 )
    {
        d.surfaceGeode->addDrawable( d.surface );

        // tessellate the surface verts into triangles.
        tessellateSurfaceGeometry( d, _optimizeTriOrientation, *_options.normalizeEdges() );

        // build the skirts.
        if ( d.createSkirt )
            createSkirtGeometry( d, *_options.heightFieldSkirtRatio() );

    }

    // installs the per-layer rendering data into the Geometry objects.
    installRenderData( d );

    // install a KdTree index if necessary
    if (osgDB::Registry::instance()->getBuildKdTreesHint()==osgDB::ReaderWriter::Options::BUILD_KDTREES &&
        osgDB::Registry::instance()->getKdTreeBuilder())
    {
#ifdef OSGEARTH_USE_TINYBVH
        osg::ref_ptr<Util::TinyBVHBuilder> builder = new Util::TinyBVHBuilder();
#else
        osg::ref_ptr<osg::KdTreeBuilder> builder = osgDB::Registry::instance()->getKdTreeBuilder()->clone();
#endif
        tile->accept(*builder);
    }

    // Temporary solution to the OverlayDecorator techniques' inappropriate setting of
    // uniform values during the CULL traversal, which causes corruption of the RTT 
    // camera matricies when DRAW overlaps the next frame's CULL. Please see my comments
    // in DrapingTechnique.cpp for more information.
    // NOTE: cannot set this until optimizations (above) are complete

    // Using VAOs on MPGeometry doesn't work if the data variance is set to DYNAMIC,
    // so for now, don't run with for VAO testing
//#ifndef USE_VAO
    SetDataVarianceVisitor sdv( osg::Object::DYNAMIC );
    tile->accept( sdv );
//#endif

    osg::ComputeBoundsVisitor cbv;
    d.surfaceGeode->accept(cbv);
    tile->setTerrainBoundingBox( cbv.getBoundingBox() );

    // debugging tools.
    if (_debug)
    {
        //test: run the geometry validator to make sure geometry it legal
        osgEarth::GeometryValidator validator;
        tile->accept(validator);

        //test: show the tile bounding boxes
        tile->addChild( makeBBox(d) );
    }

    return tile;
}
