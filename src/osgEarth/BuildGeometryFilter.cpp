/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/BuildGeometryFilter>
#include <osgEarth/Session>
#include <osgEarth/FeatureSourceIndexNode>
#include <osgEarth/PolygonizeLines>
#include <osgEarth/WireLines>
#include <osgEarth/TextSymbol>
#include <osgEarth/PointSymbol>
#include <osgEarth/LineSymbol>
#include <osgEarth/PolygonSymbol>
#include <osgEarth/MeshSubdivider>
#include <osgEarth/ResourceCache>
#include <osgEarth/Tessellator>
#include <osgEarth/Utils>
#include <osgEarth/Clamping>
#include <osgEarth/LineDrawable>
#include <osgEarth/PointDrawable>
#include <osgEarth/Registry>
#include <osgEarth/StyleSheet>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/TriangleIndexFunctor>
#include <osgText/Text>
#include <osgUtil/Tessellator>
#include <osgUtil/Optimizer>
#include <iterator>
#include <osgEarth/Notify>
#include "weemesh.h"

#define LC "[BuildGeometryFilter] "

#define OE_TEST OE_NULL

#define USE_GNOMONIC_TESSELLATION

using namespace osgEarth;

namespace
{
    bool isCCW(double x1, double y1, double x2, double y2, double x3, double y3)
    {
        return (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1) > 0.0;
    }

    bool segmentsIntersect(double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4)
    {
        return isCCW(x1, y1, x3, y3, x4, y4) != isCCW(x2, y2, x3, y3, x4, y4) && isCCW(x1, y1, x2, y2, x3, y3) != isCCW(x1, y1, x2, y2, x4, y4);
    }

    bool holeCompare(const osg::ref_ptr<Ring>& i, const osg::ref_ptr<Ring>& j)
    {
        return i->getBounds().xMax() > j->getBounds().xMax();
    }

    bool segmentsIntersect(double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4, double &xi, double &yi)
    {
        double d = (y4-y3) * (x2-x1) - (x4-x3) * (y2-y1);

        if (d == 0) return false; // parallel

        double ua = ((x4-x3) * (y1-y3) - (y4-y3) * (x1-x3)) / d;
        double ub = ((x2-x1) * (y1-y3) - (y2-y1) * (x1-x3)) / d;

        if (ua >= 0.0 && ua <= 1.0 && ub >= 0.0 && ub <= 1.0)
        {
            xi = x1 + ua * (x2 - x1);
            yi = y1 + ua * (y2 - y1);

            return true;
        }

        return false;
    }

    void ecef_to_gnomonic(osg::Vec3d& p, const osg::Vec3d& c, const Ellipsoid& e)
    {
        osg::Vec3d lla0 = e.geocentricToGeodetic(c);
        osg::Vec3d lla = e.geocentricToGeodetic(p);

        double lon0 = osg::DegreesToRadians(lla0.x());
        double lat0 = osg::DegreesToRadians(lla0.y());
        double lon = osg::DegreesToRadians(lla.x());
        double lat = osg::DegreesToRadians(lla.y());

        double d = sin(lat0)*sin(lat) + cos(lat0)*cos(lat)*cos(lon - lon0);
        p.x() = (cos(lat)*sin(lon - lon0)) / d;
        p.y() = (cos(lat0)*sin(lat) - sin(lat0)*cos(lat)*cos(lon - lon0)) / d;
    }
}

BuildGeometryFilter::BuildGeometryFilter(const Style& style) :
    _style(style),
    _maxAngle_deg(180.0),
    _geoInterp(GEOINTERP_RHUMB_LINE),
    _maxPolyTilingAngle_deg(45.0f),
    _optimizeVertexOrdering(false),
    _maximumCreaseAngle(Angle(0.0, Units::DEGREES))
{
    //nop
}

osg::Geode*
BuildGeometryFilter::processMeshes(FeatureList& features, FilterContext& context)
{
    if (features.empty())
        return {};

    osg::Geode* geode = new osg::Geode();

    bool makeECEF = false;
    const SpatialReference* featureSRS = 0L;
    const SpatialReference* outputSRS = 0L;

    // set up the reference system info:
    if (context.isGeoreferenced())
    {
        featureSRS = features.front()->getSRS();
        outputSRS = context.outputSRS();
        makeECEF = context.outputSRS()->isGeographic();
    }

    for (FeatureList::iterator f = features.begin(); f != features.end(); ++f)
    {
        Feature* input = f->get();

        // access the polygon symbol, and bail out if there isn't one
        const PolygonSymbol* poly =
            input->style() && input->style()->has<PolygonSymbol>() ? input->style()->get<PolygonSymbol>() :
            _style.get<PolygonSymbol>();

        if (!poly) {
            OE_TEST << LC << "Discarding feature with no poly symbol\n";
            continue;
        }

        // run a symbol script if present.
        if (poly->script().isSet())
        {
            poly->script()->eval(input, context);
        }

        if (input->getGeometry() == 0L)
            continue;

        GeometryIterator parts(input->getGeometry(), false);
        while (parts.hasMore())
        {
            TriMesh* part = static_cast<TriMesh*>(parts.next());

            // resolve the color:
            auto primaryColor = poly->fill()->color();

            osg::ref_ptr<osg::Geometry> osgGeom = new osg::Geometry();
            osgGeom->setName(typeid(*this).name());
            osgGeom->setUseVertexBufferObjects(true);

            // are we embedding a feature name?
            if (_featureNameExpr.isSet())
            {
                auto name = _featureNameExpr->eval(input, context);
                osgGeom->setName(name);
            }

            // compute localizing matrices or use globals
            osg::Matrixd w2l, l2w;
            if (makeECEF)
            {
                osgEarth::GeoExtent partExtent(featureSRS, part->getBounds());
                computeLocalizers(context, partExtent, w2l, l2w);
            }
            else
            {
                w2l = _world2local;
                l2w = _local2world;
            }

            //// collect all the pre-transformation HAT (Z) values.
            //osg::ref_ptr<osg::FloatArray> hats = new osg::FloatArray();
            //hats->reserve(part->size());
            //for (Geometry::const_iterator i = part->begin(); i != part->end(); ++i)
            //    hats->push_back(i->z());

            // build the geometry:
            auto allPoints = new osg::Vec3Array();
            allPoints->reserve(part->size());
            osgGeom->setVertexArray(allPoints);
            transformAndLocalize(part->asVector(), featureSRS, allPoints, outputSRS, _world2local, makeECEF);

            // todo: compress the verts since there are lots of unused ones!

            auto indices = new osg::DrawElementsUInt(GL_TRIANGLES, part->_indices.size());
            std::copy(part->_indices.begin(), part->_indices.end(), indices->begin());
            osgGeom->addPrimitiveSet(indices);

            // assign the primary color array. PER_VERTEX required in order to support
            // vertex optimization later
            auto colors = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
            colors->assign(allPoints->size(), primaryColor);
            osgGeom->setColorArray(colors);

            geode->addDrawable(osgGeom);

            // record the geometry's primitive set(s) in the index:
            if (context.featureIndex())
                context.featureIndex()->tagDrawable(osgGeom.get(), input);

            //// install clamping attributes if necessary
            //if (_style.has<AltitudeSymbol>() &&
            //    _style.get<AltitudeSymbol>()->technique() == AltitudeSymbol::TECHNIQUE_GPU)
            //{
            //    Clamping::applyDefaultClampingAttrs(osgGeom.get(), input->getDouble("__oe_verticalOffset", 0.0));
            //}
        }
    }

    OE_TEST << LC << "Num drawables = " << geode->getNumDrawables() << "\n";
    return geode;
}

osg::Geode*
BuildGeometryFilter::processPolygons(FeatureList& features, FilterContext& context)
{
    if (features.empty())
        return {};

    osg::Geode* geode = new osg::Geode();

    bool makeECEF = false;
    const SpatialReference* featureSRS = 0L;
    const SpatialReference* outputSRS = 0L;

    // set up the reference system info:
    if ( context.isGeoreferenced() )
    {
        featureSRS = features.front()->getSRS();
        outputSRS  = context.outputSRS();
        makeECEF   = context.outputSRS()->isGeographic();
    }

    // if there's a skin, set that up.
    // (TODO? only works for stylesheet-level symbols, not per-feature symbols)
    auto* skin_symbol = _style.get<SkinSymbol>();
    osg::ref_ptr<SkinResource> skin_res = nullptr;
    osg::ref_ptr<osg::StateSet> skin_stateset;

    if (skin_symbol && skin_symbol->library().isSet())
    {
        auto sheet = context.session() ? context.session()->styles() : nullptr;
        if (sheet)
        {
            osg::ref_ptr<ResourceLibrary> res_lib = sheet->getResourceLibrary(skin_symbol->library().value());
            if (res_lib.valid())
            {
                // TODO: move this into the feature loop to support per-feature styling...
                skin_res = res_lib->getSkin(skin_symbol->name()->literal(), context.getDBOptions());
                if (skin_res)
                {
                    context.resourceCache()->getOrCreateStateSet(skin_res, skin_stateset, context.getDBOptions());
                }
                else
                {
                    OE_WARN << LC << "Unable to find skin '" << skin_symbol->name()->literal() << "'"
                        << "in library; geometry will have no textures." << std::endl;
                    skin_symbol = nullptr;
                }
            }
            else
            {
                OE_WARN << LC << "Unable to load resource library '" << skin_symbol->library().value() << "'"
                    << "; geometry will have no textures." << std::endl;
                skin_symbol = nullptr;
            }
        }
    }

    // ready, time to iterate the features.
    for( FeatureList::iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        // access the polygon symbol, and bail out if there isn't one
        const PolygonSymbol* poly =
            input->style() && input->style()->has<PolygonSymbol>() ? input->style()->get<PolygonSymbol>() :
            _style.get<PolygonSymbol>();

        if ( !poly ) {
            OE_TEST << LC << "Discarding feature with no poly symbol\n";
            continue;
        }

        // run a symbol script if present.
        if ( poly->script().isSet() )
        {
            poly->script()->eval(input, context);
        }

        if (input->getGeometry() == 0L)
            continue;

        GeometryIterator parts( input->getGeometry(), false );
        while( parts.hasMore() )
        {
            Geometry* part = parts.next();

            part->removeDuplicates();

            // skip geometry that is invalid for a polygon
            if ( part->size() < 3 ) {
                OE_TEST << LC << "Discarding illegal part (less than 3 verts)\n";
                continue;
            }

            // resolve the color:
            osg::Vec4f primaryColor = poly->fill()->color();

            osg::ref_ptr<osg::Geometry> osgGeom = new osg::Geometry();
            osgGeom->setName(typeid(*this).name());
            osgGeom->setUseVertexBufferObjects(true);

            // are we embedding a feature name?
            if ( _featureNameExpr.isSet() )
            {
                auto name = _featureNameExpr->eval(input, context);
                osgGeom->setName( name );
            }

            // apply the skin if there is one.
            if (skin_stateset.valid())
            {
                osgGeom->setStateSet(skin_stateset);
            }

            // compute localizing matrices or use globals
            osg::Matrixd w2l, l2w;
            if (makeECEF)
            {
                osgEarth::GeoExtent partExtent(featureSRS, part->getBounds());
                computeLocalizers(context, partExtent, w2l, l2w);
            }
            else
            {
                w2l = _world2local;
                l2w = _local2world;
            }

            // collect all the pre-transformation HAT (Z) values.
            osg::ref_ptr<osg::FloatArray> hats = new osg::FloatArray();
            hats->reserve( part->size() );
            for(Geometry::const_iterator i = part->begin(); i != part->end(); ++i )
                hats->push_back( i->z() );

            // build the geometry:
            tileAndBuildPolygon(part, featureSRS, outputSRS, makeECEF, true, osgGeom.get(), skin_res, w2l);

            osg::Vec3Array* allPoints = static_cast<osg::Vec3Array*>(osgGeom->getVertexArray());
            if (allPoints && allPoints->size() > 0)
            {
                // subdivide the mesh if necessary to conform to an ECEF globe:
                if ( makeECEF )
                {
                    //convert back to world coords
                    for( osg::Vec3Array::iterator i = allPoints->begin(); i != allPoints->end(); ++i )
                    {
                        osg::Vec3d v(*i);
                        v = v * l2w;
                        v = v * _world2local;

                        (*i)._v[0] = v[0];
                        (*i)._v[1] = v[1];
                        (*i)._v[2] = v[2];
                    }

                    double threshold = osg::DegreesToRadians( *_maxAngle_deg );
                    //OE_TEST << "Running mesh subdivider with threshold " << *_maxAngle_deg << std::endl;
                    MeshSubdivider ms( _world2local, _local2world );
                    if ( input->geoInterp().isSet() )
                        ms.run( *osgGeom, threshold, *input->geoInterp() );
                    else
                        ms.run( *osgGeom, threshold, *_geoInterp );
                }

                // assign the primary color array. PER_VERTEX required in order to support
                // vertex optimization later
                unsigned count = osgGeom->getVertexArray()->getNumElements();
                osg::Vec4Array* colors = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
                colors->assign( count, primaryColor );
                osgGeom->setColorArray( colors );

                geode->addDrawable( osgGeom );

                // record the geometry's primitive set(s) in the index:
                if ( context.featureIndex() )
                    context.featureIndex()->tagDrawable( osgGeom.get(), input );

                // install clamping attributes if necessary
                if (_style.has<AltitudeSymbol>() &&
                    _style.get<AltitudeSymbol>()->technique() == AltitudeSymbol::TECHNIQUE_GPU)
                {
                    Clamping::applyDefaultClampingAttrs( osgGeom.get(), input->getDouble("__oe_verticalOffset", 0.0) );
                }
            }
            else
            {
                OE_TEST << LC << "Oh no. tileAndBuildPolygon returned nothing.\n";
            }
        }
    }

    OE_TEST << LC << "Num drawables = " << geode->getNumDrawables() << "\n";
    return geode;
}

namespace
{
    struct CopyHeightsCallback : public PolygonizeLinesOperator::Callback
    {
        osg::FloatArray* _heights;
        osg::ref_ptr<osg::FloatArray> _newHeights;
        CopyHeightsCallback(osg::FloatArray* heights) : _heights(heights) {
            if (_heights) {
                _newHeights = new osg::FloatArray();
                _newHeights->reserve(_heights->size() * 3);
            }
        }
        void operator()(unsigned i) {
            if (_newHeights.valid() && _heights) {
                _newHeights->push_back((*_heights)[i]);
            }
        }
    };
}

osg::Group*
BuildGeometryFilter::processPolygonizedLines(FeatureList&   features,
                                             bool           twosided,
                                             FilterContext& context,
                                             bool wireLines)
{
    if (features.empty())
        return {};

    osg::Group* group = new osg::Group;

    // establish some referencing
    bool                    makeECEF   = false;
    const SpatialReference* featureSRS = 0L;
    const SpatialReference* outputSRS  = 0L;

    if ( context.isGeoreferenced() )
    {
        featureSRS = features.front()->getSRS();
        outputSRS = context.outputSRS();
        makeECEF = outputSRS->isGeographic();
    }

    // We need to create a different geode for each texture that is used so they can share statesets.
    typedef std::map< std::string, osg::ref_ptr< osg::Geode > > TextureToGeodeMap;
    TextureToGeodeMap geodes;

    // Do we have a resource library?
    ResourceLibrary* resourceLib = nullptr;
    if (context.session() && context.session()->styles())
    {
        resourceLib = context.session()->styles()->getResourceLibrary(_style.get<LineSymbol>()->library().get());
    }

    // iterate over all features.
    for( FeatureList::iterator i = features.begin(); i != features.end(); ++i )
    {
        Feature* input = i->get();
        // extract the required line symbol; bail out if not found.
        const LineSymbol* line =
            input->style() && input->style()->has<LineSymbol>() ? input->style()->get<LineSymbol>() :
            _style.get<LineSymbol>();

        if ( !line )
            continue;

        // Try to find the existing geode, otherwise create one.
        osg::ref_ptr< osg::Geode > geode;
        TextureToGeodeMap::iterator itr = geodes.find(line->imageURI()->full());
        if (itr != geodes.end())
        {
            geode = itr->second;
        }
        else
        {
            geode = new osg::Geode;

            // Create the texture for the geode.
            if (line->imageURI().isSet() && !line->imageURI()->empty())
            {
                osg::ref_ptr<osg::StateSet> stateSet;

                if (resourceLib)
                {
                    auto* skin = resourceLib->getSkin(line->imageURI()->base(), context.getDBOptions());
                    if (skin)
                    {
                        if (context.resourceCache())
                        {
                            context.resourceCache()->getOrCreateStateSet(skin, stateSet, context.getDBOptions());
                        }

                        else
                        {
                            stateSet = skin->createStateSet(context.getDBOptions());
                        }
                    }
                }

                if (!stateSet.valid())
                {
                    osg::ref_ptr<osg::Texture> tex;

                    if (context.session()->getResourceCache())
                    {
                        context.session()->getResourceCache()->getOrCreateLineTexture(
                            line->imageURI().value(), tex, context.getDBOptions());
                    }

                    else
                    {

                        osg::ref_ptr<osg::Image> image = line->imageURI()->getImage(context.getDBOptions());
                        if (image.valid())
                        {
                            tex = new osg::Texture2D(image);
                            tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
                            tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
                            tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
                            tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
                            tex->setMaxAnisotropy(4.0f);
                            tex->setResizeNonPowerOfTwoHint(false);
                            tex->setUnRefImageDataAfterApply(false);
                        }
                    }

                    if (tex.valid())
                    {
                        stateSet = new osg::StateSet();
                        stateSet->setTextureAttributeAndModes(0, tex.get(), 1);
                    }
                }

                if (stateSet.valid())
                {
                    geode->setStateSet(stateSet);
                }
            }

            geodes[line->imageURI()->full()] = geode;
        }

        // run a symbol script if present.
        if ( line->script().isSet() )
        {
            line->script()->eval(input, context);
        }

        Distance lineWidth(1.0f, Units::PIXELS);
        if (line->stroke().isSet() && line->stroke()->width().isSet())
        {
            lineWidth = line->stroke()->width()->eval(input, context);
        }

        // The operator we'll use to make lines into polygons.
        osg::ref_ptr<OsgGeometryOperator> polygonizer;
        if (wireLines)
        {
            polygonizer = new WireLinesOperator(*line->stroke());
        }
        else
        {
            auto p = new PolygonizeLinesOperator(line);
            p->_copyToBackFace = (line->doubleSided() == true);
            polygonizer = p;            
        }

        // iterate over all the feature's geometry parts. We will treat
        // them as lines strings.
        GeometryIterator parts( input->getGeometry(), true );
        while( parts.hasMore() )
        {
            Geometry* part = parts.next();

            // if the underlying geometry is a ring (or a polygon), close it so the
            // polygonizer will generate a closed loop.
            Ring* ring = dynamic_cast<Ring*>(part);
            if ( ring )
                ring->close();

            // skip invalid geometry
            if ( part->size() < 2 )
                continue;

            // GPU clamping enabled?
            bool gpuClamping =
                _style.has<AltitudeSymbol>() &&
                _style.get<AltitudeSymbol>()->technique() == AltitudeSymbol::TECHNIQUE_GPU;

            // collect all the pre-transformation HAT (Z) values.
            osg::ref_ptr<osg::FloatArray> hats = 0L;
            if (gpuClamping)
            {
                hats = new osg::FloatArray();
                hats->reserve( part->size() );
                for(Geometry::const_iterator i = part->begin(); i != part->end(); ++i )
                    hats->push_back( i->z() );
            }

            // transform the geometry into the target SRS and localize it about
            // a local reference point.
            osg::ref_ptr<osg::Vec3Array> verts   = new osg::Vec3Array();
            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array();
            transformAndLocalize( part->asVector(), featureSRS, verts.get(), normals.get(), outputSRS, _world2local, makeECEF );

            // turn the lines into polygons.
            CopyHeightsCallback copyHeights(hats.get());
            osg::Geometry* geom = (*polygonizer)(verts.get(), normals.get(), lineWidth.as(Units::METERS), gpuClamping ? &copyHeights : nullptr, twosided);
            //osg::Geometry* geom = gpuLines(verts.get());
            if ( geom )
            {
                geode->addDrawable( geom );
            }

            // record the geometry's primitive set(s) in the index:
            if ( context.featureIndex() )
                context.featureIndex()->tagDrawable( geom, input );

            // install clamping attributes if necessary
            if (gpuClamping)
            {
                Clamping::applyDefaultClampingAttrs( geom, input->getDouble("__oe_verticalOffset", 0.0) );
                Clamping::setHeights( geom, copyHeights._newHeights.get() );
                //OE_WARN << "heights = " << hats->size() << ", new hats = " << copyHeights._newHeights->size() << ", verts=" << geom->getVertexArray()->getNumElements() << std::endl;
            }
        }
        polygonizer->installShaders( geode.get() );
    }

    for (TextureToGeodeMap::iterator itr = geodes.begin(); itr != geodes.end(); ++itr)
    {
        // Optimize the Geode
        osg::Geode* geode = itr->second.get();

        if (mergeGeometry() == true)
        {
            osgUtil::Optimizer::MergeGeometryVisitor mg;
            mg.setTargetMaximumNumberOfVertices(Registry::instance()->getMaxNumberOfVertsPerDrawable());
            geode->accept(mg);
        }

        if (_optimizeVertexOrdering == true)
        {
            osgUtil::Optimizer o;
            o.optimize( geode,
                osgUtil::Optimizer::INDEX_MESH
                | osgUtil::Optimizer::VERTEX_PRETRANSFORM
                | osgUtil::Optimizer::VERTEX_POSTTRANSFORM
                );
        }

        // Add it to the group
        group->addChild( geode );
    }

    return group;
}


osg::Group*
BuildGeometryFilter::processLines(FeatureList& features, FilterContext& context)
{
    if (features.empty())
        return {};

    // Group to contain all the lines we create here
    LineGroup* drawables = new LineGroup();
    
    bool makeECEF = false;
    const SpatialReference* featureSRS = 0L;
    const SpatialReference* outputSRS = 0L;

    // set up referencing information:
    if ( context.isGeoreferenced() )
    {
        featureSRS = features.front()->getSRS();
        outputSRS  = context.outputSRS();
        makeECEF = outputSRS->isGeographic();
    }

    // Need to know if we are GPU clamping so we can add more attribs
    bool doGpuClamping =
        _style.has<AltitudeSymbol>() &&
        _style.get<AltitudeSymbol>()->technique() == AltitudeSymbol::TECHNIQUE_GPU;

    // For each input feature:
    for (FeatureList::iterator f = features.begin(); f != features.end(); ++f)
    {
        Feature* input = f->get();

        // extract the required line symbol; bail out if not found.
        const LineSymbol* line =
            input->style() && input->style()->has<LineSymbol>() ? input->style()->get<LineSymbol>() :
            _style.get<LineSymbol>();

        // if there's no line symbol, bail.
        if ( !line )
            continue;

        // run a symbol script if present.
        if ( line->script().isSet() )
        {
            line->script()->eval(input, context);
        }

        Distance strokeWidth(1.0, Units::PIXELS);
        if (line->stroke()->width().isSet())
        {
            strokeWidth = line->stroke()->width()->eval(input, context);
            //strokeWidth = input->eval(line->stroke()->width().value(), &context);
        }

        GeometryIterator parts( input->getGeometry(), true );
        while( parts.hasMore() )
        {
            Geometry* part = parts.next();

            // skip invalid geometry for lines.
            if ( part->size() < 2 )
                continue;

            // if the underlying geometry is a ring (or a polygon), use a line loop; otherwise
            // use a line strip.
            bool isRing = (dynamic_cast<Ring*>(part) != 0L);

            // resolve the color:
            osg::Vec4f primaryColor = line->stroke()->color();

            // generate the geometry and localize to the local tangent plane
            osg::ref_ptr< osg::Vec3Array > allPoints = new osg::Vec3Array();
            transformAndLocalize( part->asVector(), featureSRS, allPoints.get(), outputSRS, _world2local, makeECEF );

            // construct a drawable for the lines
            LineDrawable* drawable = new LineDrawable(isRing? GL_LINE_LOOP : GL_LINE_STRIP);

            // if the user requested legacy rendering:
            if (line->useGLLines() == true)
                drawable->setUseGPU(false);

            drawable->importVertexArray(allPoints.get());

            if (line->stroke().isSet())
            {
                drawable->setLineWidth(strokeWidth.as(Units::PIXELS));

                if (line->stroke()->stipplePattern().isSet())
                    drawable->setStipplePattern(line->stroke()->stipplePattern().get());

                if (line->stroke()->stippleFactor().isSet())
                    drawable->setStippleFactor(line->stroke()->stippleFactor().get());

                if (line->stroke()->smooth().isSet())
                    drawable->setLineSmooth(line->stroke()->smooth().get());
            }

            // For GPU clamping, we need an attribute array with Heights above Terrain in it.
            if (doGpuClamping)
            {
                osg::FloatArray* hats = new osg::FloatArray();
                hats->setBinding(osg::Array::BIND_PER_VERTEX);
                hats->setNormalize(false);
                drawable->setVertexAttribArray(Clamping::HeightsAttrLocation, hats);
                for (Geometry::const_iterator i = part->begin(); i != part->end(); ++i)
                {
                    drawable->pushVertexAttrib(hats, i->z());
                }
            }

            // assign the color:
            drawable->setColor(primaryColor);

            // embed the feature name if requested. Warning: blocks geometry merge optimization!
            if ( _featureNameExpr.isSet() )
            {
                auto name = _featureNameExpr->eval(input, context);
                drawable->setName( name );
            }

            // record the geometry's primitive set(s) in the index:
            if ( context.featureIndex() )
            {
                context.featureIndex()->tagDrawable( drawable, input );
            }

            // install clamping attributes if necessary
            if (doGpuClamping)
            {
                Clamping::applyDefaultClampingAttrs( drawable, input->getDouble("__oe_verticalOffset", 0.0) );
            }

            // finalize the drawable and generate primitive sets
            drawable->dirty();

            drawables->addChild(drawable);
        }
    }

    // Finally, optimize the finished group for rendering.
    if (drawables)
    {
        drawables->optimize();
    }

    return drawables;
}


osg::Geode*
BuildGeometryFilter::processPoints(FeatureList& features, FilterContext& context)
{
    if (features.empty())
        return {};

    PointGroup* drawables = new PointGroup();

    bool makeECEF = false;
    const SpatialReference* featureSRS = 0L;
    const SpatialReference* outputSRS = 0L;

    // set up referencing information:
    if ( context.isGeoreferenced() )
    {
        featureSRS = features.front()->getSRS();
        outputSRS  = context.outputSRS();
        makeECEF = outputSRS->isGeographic();
    }

    // Need to know if we are GPU clamping so we can add more attribs
    bool doGpuClamping =
        _style.has<AltitudeSymbol>() &&
        _style.get<AltitudeSymbol>()->technique() == AltitudeSymbol::TECHNIQUE_GPU;

    for( FeatureList::iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        GeometryIterator parts( input->getGeometry(), true );
        while( parts.hasMore() )
        {
            Geometry* part = parts.next();

            // extract the required point symbol; bail out if not found.
            const PointSymbol* point =
                input->style() && input->style()->has<PointSymbol>() ? input->style()->get<PointSymbol>() :
                _style.get<PointSymbol>();

            if ( !point )
                continue;

            // collect all the pre-transformation HAT (Z) values.
            osg::ref_ptr<osg::FloatArray> hats = new osg::FloatArray();
            hats->reserve( part->size() );
            for(Geometry::const_iterator i = part->begin(); i != part->end(); ++i )
                hats->push_back( i->z() );

            // resolve the color:
            osg::Vec4f primaryColor = point->fill()->color();

            // build the geometry:
            osg::ref_ptr<osg::Vec3Array> allPoints = new osg::Vec3Array();
            transformAndLocalize( part->asVector(), featureSRS, allPoints.get(), outputSRS, _world2local, makeECEF );

            PointDrawable* drawable = new PointDrawable();

            drawable->importVertexArray(allPoints.get());

            if (point->size().isSet())
                drawable->setPointSize(point->size().get());

            if (point->smooth().isSet())
                drawable->setPointSmooth(point->smooth().get());

            // For GPU clamping, we need an attribute array with Heights above Terrain in it.
            if (doGpuClamping)
            {
                osg::FloatArray* hats = new osg::FloatArray();
                hats->setBinding(osg::Array::BIND_PER_VERTEX);
                hats->setNormalize(false);
                drawable->setVertexAttribArray(Clamping::HeightsAttrLocation, hats);
                for (Geometry::const_iterator i = part->begin(); i != part->end(); ++i)
                {
                    drawable->pushVertexAttrib(hats, i->z());
                }
            }

            // assign the color:
            drawable->setColor(primaryColor);

            // embed the feature name if requested. Warning: blocks geometry merge optimization!
            if ( _featureNameExpr.isSet() )
            {
                auto name = _featureNameExpr->eval(input, context);
                drawable->setName( name );
            }

            // record the geometry's primitive set(s) in the index:
            if ( context.featureIndex() )
            {
                context.featureIndex()->tagDrawable( drawable, input );
            }

            // install clamping attributes if necessary
            if (doGpuClamping)
            {
                Clamping::applyDefaultClampingAttrs( drawable, input->getDouble("__oe_verticalOffset", 0.0) );
            }

            drawable->dirty();

            drawables->addChild(drawable);
        }
    }

    // Finally, optimize the finished group for rendering.
    if (drawables)
    {
        drawables->optimize();
    }

    return drawables;
}

// Borrowed from MeshConsolidator.cpp
namespace
{
    template<typename FROM, typename TO>
    osg::PrimitiveSet* copy( FROM* src, unsigned offset )
    {
        TO* newDE = new TO( src->getMode() );
        newDE->reserve( src->size() );
        for( typename FROM::const_iterator i = src->begin(); i != src->end(); ++i )
            newDE->push_back( (*i) + offset );
        return newDE;
    }


    /**
     * Converts an osg::Geometry to use osg::DrawElementsUInt if it doesn't already.
     * This only works on Geometries that are already using DrawElementsUInt, DrawElementsUByte, or DrawElementsUShort
     * We do this to normalize the primitive set types so that we can merge multiple geometries
     * into one later down the road.
     */
    void convertToDrawElementsUInt(osg::Geometry* geometry)
    {
        for (unsigned int i = 0; i < geometry->getNumPrimitiveSets(); i++)
        {
            osg::PrimitiveSet* ps = geometry->getPrimitiveSet(i);
            // See if it's already a DrawElementsUInt and do nothing if it is.
            osg::DrawElementsUInt* deUint = dynamic_cast<osg::DrawElementsUInt*>(ps);
            if (!deUint)
            {
                // Copy values from the existing primitive set to a new DrawElementsUInt
                osg::PrimitiveSet* newPS = 0;
                if (dynamic_cast<osg::DrawElementsUByte*>(ps))
                {
                    newPS = copy<osg::DrawElementsUByte, osg::DrawElementsUInt>(static_cast<osg::DrawElementsUByte*>(ps), 0);
                }
                else if (dynamic_cast<osg::DrawElementsUShort*>(ps))
                {
                    newPS = copy<osg::DrawElementsUShort, osg::DrawElementsUInt>(static_cast<osg::DrawElementsUShort*>(ps), 0);
                }

                // Set the new primitive set
                if (newPS)
                {
                    geometry->setPrimitiveSet(i, newPS);
                }
            }
        }
    }

    /**
     * Tesselates an osg::Geometry using the osgEarth tesselator.
     * If it fails, fall back to the osgUtil tesselator.
     */
    bool tesselateGeometry(osg::Geometry* geometry, bool useOSGTessellator)
    {
        if (useOSGTessellator) {
            osgUtil::Tessellator tess;
            tess.setTessellationType(osgUtil::Tessellator::TESS_TYPE_GEOMETRY);
            tess.setWindingType(osgUtil::Tessellator::TESS_WINDING_ODD);
            tess.retessellatePolygons(*geometry);
        }
        else {
            osgEarth::Tessellator oeTess;
            if (!oeTess.tessellateGeometry(*geometry))
            {
                osgUtil::Tessellator tess;
                tess.setTessellationType(osgUtil::Tessellator::TESS_TYPE_GEOMETRY);
                tess.setWindingType(osgUtil::Tessellator::TESS_WINDING_POSITIVE);
                tess.retessellatePolygons(*geometry);
            }
        }

        // Make sure all of the primitive sets are osg::DrawElementsUInt
        // The osgEarth tesselator will occassionally fail, and we fall back to the osgUtil::Tesselator which can produce a mix
        // of DrawElementsUInt, DrawElementsUByte and DrawElementsUShort depending on the number of vertices.
        convertToDrawElementsUInt(geometry);
        return true;
    }

    /**
     * Tiles the Geometry into the given number of columns and rows
     */
    void tileGeometry(Geometry* geometry, const SpatialReference* featureSRS, unsigned int numCols, unsigned int numRows, GeometryCollection& out)
    {
        // Clear the output list.
        out.clear();

        Bounds b = geometry->getBounds();
        double tw = width(b) / (double)numCols;
        double th = height(b) / (double)numRows;

        // Get the average Z, since GEOS will set teh Z of new verts to that of the cropping polygon,
        // which is stupid but that's how it is.
        double z = 0.0;
        for(unsigned i=0; i<geometry->size(); ++i)
            z += geometry->at(i).z();
        z /= geometry->size();

        osgEarth::Polygon boundary;
        boundary.resize(4);

        for(int x=0; x<(int)numCols; ++x)
        {
            for(int y=0; y<(int)numRows; ++y)
            {
                boundary[0].set( b.xMin() + tw*(double)x,     b.yMin() + th*(double)y,     z );
                boundary[1].set( b.xMin() + tw*(double)(x+1), b.yMin() + th*(double)y,     z );
                boundary[2].set( b.xMin() + tw*(double)(x+1), b.yMin() + th*(double)(y+1), z );
                boundary[3].set( b.xMin() + tw*(double)x,     b.yMin() + th*(double)(y+1), z );

                if (auto cropped = geometry->crop(&boundary))
                {
                    // Use an iterator since crop could return a multi-polygon
                    GeometryIterator gi(cropped, false);
                    while (gi.hasMore())
                    {
                        Geometry* geom = gi.next();
                        out.push_back(geom);
                    }
                }
            }
        }
    }

    /**
     * Tiles the geometry up until all the cells have less than given number of points.
     */
    void downsizeGeometry(Geometry* geometry, const SpatialReference* featureSRS, unsigned int maxPoints, GeometryCollection& out)
    {
        // If the geometyr is greater than the maximum number of points, we need to tile it up further.
        if (geometry->size() > maxPoints)
        {
            OE_NOTICE << "Downsizing geometry of size " << geometry->size() << std::endl;
            // Tile the geometry.
            GeometryCollection tmp;
            tileGeometry(geometry, featureSRS, 2, 2, tmp );

            for (unsigned int i = 0; i < tmp.size(); i++)
            {
                Geometry* g = tmp[i].get();

                // If the generated geometry still has too many points, continue to downsample it recursively.
                if (g->size() > maxPoints)
                {
                    // We pass "out" as the destination here since downsizeGeometry will only append tiles that are less than the max size.
                    downsizeGeometry( g, featureSRS, maxPoints, out );
                }
                else
                {
                    // Append the geometry to the output list.
                    out.push_back( g );
                }
            }
        }
        else
        {
            // The geometry is valid, so add it to the output list.
            out.push_back( geometry );
        }
    }

    /**
     * Prepares a geometry into a grid if it is too big geospatially to have a sensible local tangent plane
     * We will also tile the geometry if it just has too many points to speed up the tesselator.
     */
    void prepareForTesselation(Geometry* geometry, const SpatialReference* featureSRS, double targetTileSizeDeg, unsigned int maxPointsPerTile, GeometryCollection& out)
    {
        // Clear the output list.
        GeometryCollection tiles;

        unsigned int count = geometry->size();

        unsigned int tx = 1;
        unsigned int ty = 1;

        // Tile the geometry if it's geospatial size is too large to have a sensible local tangent plane.
        GeoExtent featureExtentDeg = GeoExtent(featureSRS, geometry->getBounds()).transform(SpatialReference::create("wgs84"));

        // Tile based on the extent
        if ( featureExtentDeg.width() > targetTileSizeDeg  || featureExtentDeg.height() > targetTileSizeDeg)
        {
            // Determine the tile size based on the extent.
            tx = ceil( featureExtentDeg.width() / targetTileSizeDeg );
            ty = ceil (featureExtentDeg.height() / targetTileSizeDeg );
        }
        else if (count > maxPointsPerTile)
        {
            // Determine the size based on the number of points.
            unsigned numTiles = ((double)count / (double)maxPointsPerTile) + 1u;
            tx = ceil(sqrt((double)numTiles));
            ty = tx;
        }

        if (tx == 1 && ty == 1)
        {
            // The geometry doesn't need modified so just add it to the list.
            tiles.push_back( geometry );
        }
        else
        {
            tileGeometry( geometry, featureSRS, tx, ty, tiles );
        }

        out.clear();

    #if 1
        // Just copy the output tiles to the output.
        std::copy(tiles.begin(), tiles.end(), std::back_inserter(out));
    #else
        // Calling this code will recursively subdivide the cells based on the number of points they have.
        // This works but it will produces a non-regular grid which doesn't render well in geocentric
        // due to the curvature of the earth so we disable it for now.
        //
        // Reduce the size of the tiles if needed.
        for (unsigned int i = 0; i < tiles.size(); i++)
        {
            if (tiles[i]->size() > maxPointsPerTile)
            {
                GeometryCollection tmp;
                downsizeGeometry(tiles[i].get(), featureSRS, maxPointsPerTile, tmp);
                std::copy(tmp.begin(), tmp.end(), std::back_inserter(out));
            }
            else
            {
                out.push_back( tiles[i].get() );
            }
        }
    #endif
    }
}

#ifdef USE_GNOMONIC_TESSELLATION

namespace
{   
    // Transforms a range of points from geographic (long lat) to gnomonic coordinates
    // around a centroid with an optional scale.
    template<class T>
    void geo_to_gnomonic(T& p, const T& centroid, double scale)
    {
        double lon0 = deg2rad(centroid[0]);
        double lat0 = deg2rad(centroid[1]);

        double lon = deg2rad(p[0]);
        double lat = deg2rad(p[1]);
        double d = sin(lat0) * sin(lat) + cos(lat0) * cos(lat) * cos(lon - lon0);
        p[0] = scale * (cos(lat) * sin(lon - lon0)) / d;
        p[1] = scale * (cos(lat0) * sin(lat) - sin(lat0) * cos(lat) * cos(lon - lon0)) / d;
    }

    // Transforms a range of points from gnomonic coordinates around a centroid with a
    // given scale to geographic (long lat) coordinates.
    template<class T1, class T2>
    void gnomonic_to_geo(T1& p, const T2& centroid, double scale)
    {
        double lon0 = deg2rad(centroid[0]);
        double lat0 = deg2rad(centroid[1]);

        double x = p[0] / scale, y = p[1] / scale;
        double rho = sqrt(x * x + y * y);
        double c = atan(rho);

        double lat = asin(cos(c) * sin(lat0) + (y * sin(c) * cos(lat0) / rho));
        double lon = lon0 + atan((x * sin(c)) / (rho * cos(lat0) * cos(c) - y * sin(lat0) * sin(c)));

        p[0] = rad2deg(lon);
        p[1] = rad2deg(lat);
    }
}

void
BuildGeometryFilter::tileAndBuildPolygon(
    Geometry*               input,
    const SpatialReference* inputSRS,
    const SpatialReference* outputSRS,
    bool                    makeECEF,
    bool                    tessellate,
    osg::Geometry*          osgGeom,
    const SkinResource*     skin_res,
    const osg::Matrixd&     world2local)
{
    OE_SOFT_ASSERT_AND_RETURN(input != nullptr, void());
    OE_SOFT_ASSERT_AND_RETURN(input->getType() != Geometry::TYPE_MULTI, void());

    double R = outputSRS ? outputSRS->getEllipsoid().getSemiMajorAxis() : 6378137.0;
    double circum = osg::PI * 2.0 * R;
    double ref_x = 0.0, ref_y = 0.0;

    osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
    verts->reserve(input->getTotalPointCount());

    osg::ref_ptr<osg::Vec2Array> uvs;
    if (skin_res)
    {
        uvs = new osg::Vec2Array();
        uvs->reserve(verts->size());
        osgGeom->setTexCoordArray(0, uvs.get());
    }

    // hard copy so we can project the values
    osg::ref_ptr<Geometry> proj = input->clone();

    auto render = _style.get<RenderSymbol>();

    // weemesh path ONLY happens if maxTessAngle is set for now.
    // We will keep it this way until testing is complete -gw
    if (outputSRS && render && render->maxTessAngle().isSet())
    {
        // weemesh triangulation approach (from Rocky)
        double xspan, yspan;
        double z = -DBL_MAX;
        Bounds local_ex;
        osg::Vec3d centroid;
        auto local_geom = proj; // working copy

        // scales our local gnomonic coordinates so they are the same order of magnitude as
        // weemesh's default epsilon values (for geographic only)
        // TODO: figure out the relationship between the gnomonic scale and the maxTessAngle
        const double gnomonic_scale = 1e6; // 1000.0; // 1e7;

        if (outputSRS->isGeographic())
        {
            // Meshed triangles will be at a maximum this many degrees across in size,
            // to help follow the curvature of the earth.
            const double resolution_degrees = render ? render->maxTessAngle()->as(Units::DEGREES) : 1.0;

            // some conversions we will need:
            auto feature_geo = inputSRS;

            // centroid for use with the gnomonic projection:
            centroid = input->getBounds().center();
            inputSRS->transform(centroid, outputSRS, centroid); // to geographic

            // transform to gnomonic. We are not using SRS/PROJ for the gnomonic projection
            // because it would require creating a new SRS for each and every feature (because
            // of the centroid) and that is way too slow.
            Bounds geo_ex;
            GeometryIterator iter(local_geom.get());
            while (iter.hasMore())
            {
                auto part = iter.next();
                inputSRS->transform(part->asVector(), outputSRS); // to geographic
                for (auto& p : *part)
                {
                    geo_ex.expandBy(p);
                    geo_to_gnomonic(p, centroid, gnomonic_scale);
                    local_ex.expandBy(p);
                    z = std::max(z, p.z());
                }
            }

            // calculate a UV reference point close to the centroid of the geometry.
            if (skin_res)
            {
                auto geo_ex_anchor = geo_ex.center();
                auto x = circum * (geo_ex_anchor.x() + 180.0) / 360.0;
                auto y = (0.5 * circum) * (geo_ex_anchor.y() + 90.0) / 180.0;
                ref_x = x - fmod(x, skin_res->imageWidth().value());
                ref_y = y - fmod(y, skin_res->imageHeight().value());
            }

            xspan = gnomonic_scale * resolution_degrees * 3.14159 / 180.0;
            yspan = gnomonic_scale * resolution_degrees * 3.14159 / 180.0;
        }

        else // projected SRS
        {
            double max_span_m = DBL_MAX; // render->maxTessAngle()->as(Units::METERS);
            xspan = max_span_m;
            yspan = max_span_m;

            ConstGeometryIterator iter(local_geom.get());
            while (iter.hasMore())
            {
                auto* part = iter.next();
                for (auto& p : *part)
                {
                    local_ex.expandBy(p);
                    z = std::max(z, p.z());
                }
            }
        }

        // start with a weemesh covering the feature extent.
        weemesh::mesh_t m;
        const int marker = 0;
        double width = (local_ex.xMax() - local_ex.xMin());
        double height = (local_ex.yMax() - local_ex.yMin());
        int cols = std::max(2, (int)(width / xspan));
        int rows = std::max(2, (int)(height / yspan));
        for (int row = 0; row < rows; ++row)
        {
            double v = (double)row / (double)(rows - 1);
            double y = local_ex.yMin() + v * height;

            for (int col = 0; col < cols; ++col)
            {
                double u = (double)col / (double)(cols - 1);
                double x = local_ex.xMin() + u * width;
                m.get_or_create_vertex_from_vec3(weemesh::vert_t{ x, y, z }, marker);
            }
        }

        for (int row = 0; row < rows - 1; ++row)
        {
            for (int col = 0; col < cols - 1; ++col)
            {
                int k = row * cols + col;
                m.add_triangle(k, k + 1, k + cols);
                m.add_triangle(k + 1, k + cols + 1, k + cols);
            }
        }

        // next, apply the segments of the polygon to slice the mesh into triangles.
        ConstGeometryIterator segment_iter(local_geom.get());
        while (segment_iter.hasMore())
        {
            auto part = segment_iter.next();
            for (unsigned i = 0; i < part->size(); ++i)
            {
                unsigned j = (i == part->size() - 1) ? 0 : i + 1;
                weemesh::vert_t a((*part)[i].x(), (*part)[i].y(), (*part)[i].z());
                weemesh::vert_t b((*part)[j].x(), (*part)[j].y(), (*part)[j].z());
                m.insert(weemesh::segment_t{ a, b }, marker | m._has_elevation_marker);
            }
        }

        // next we need to remove all the exterior triangles.
        std::unordered_set<weemesh::triangle_t*> insiders;
        std::unordered_set<weemesh::triangle_t*> outsiders;
        ConstGeometryIterator remove_iter(local_geom.get(), false);
        while (remove_iter.hasMore())
        {
            auto part = remove_iter.next();

            for (auto& tri_iter : m.triangles)
            {
                weemesh::triangle_t& tri = tri_iter.second;
                auto c = (tri.p0 + tri.p1 + tri.p2) * (1.0 / 3.0); // centroid
                bool inside = part->contains2D(c.x, c.y);
                if (inside)
                    insiders.insert(&tri);
                else
                    outsiders.insert(&tri);
            }
        }
        for (auto tri : outsiders)
        {
            if (insiders.count(tri) == 0)
            {
                m.remove_triangle(*tri);
            }
        }

#if 0
        // *** Copied from TileMesher - TODO consolidate ***
        // This algorithm looks for verts with no elevation data, and assigns each
        // one an elevation based on the closest edge that HAS elevation data.
        // Typical use case: remove exterior polygons, leaving you with an interior
        // mesh in which the edge points have set elevations. This will then interpolate
        // the elevations of any new interior points giving you a "flat" surface for
        // a road or river (for example).
        // TODO: do some kind of smoothing for exterior points to make nice transitions...?
        weemesh::vert_t closest;

        // collect every edge that has valid elevation data in Z.
        // usually this means the caller assigned Z values to the constraint geometry
        weemesh::edgeset_t elevated_edges(m, m._has_elevation_marker);

        // find every vertex without elevation, and set its elevation to the same value
        // as that of the closest point on the nearest constrained edge
        for (int i = 0; i < m.verts.size(); ++i)
        {
            if ((m.markers[i] & m._has_elevation_marker) == 0)
            {
                if (elevated_edges.point_on_any_edge_closest_to(m.verts[i], m, closest))
                {
                    m.verts[i].z = closest.z;
                    m.markers[i] |= (m._constraint_marker | m._has_elevation_marker);
                }
            }
        }
#endif

        // Finally we convert from gnomonic back to localized tile coordinates.
        osg::ref_ptr<osg::Vec3Array> new_verts = new osg::Vec3Array();
        new_verts->reserve(m.verts.size());

        if (outputSRS->isGeographic())
        {
            osg::Vec3d ecef;
            for (auto& vert : m.verts)
            {
                gnomonic_to_geo(vert, centroid, gnomonic_scale); // to geographic
                outputSRS->transformToWorld(osg::Vec3d(vert.x, vert.y, vert.z), ecef); // to ECEF
                new_verts->push_back(ecef * world2local); // localized to tile

                if (uvs)
                {
                    auto x = circum * (vert.x + 180.0) / 360.0;
                    auto y = (0.5 * circum) * (vert.y + 90.0) / 180.0;
                    auto u = (x - ref_x) / skin_res->imageWidth().value();
                    auto v = (y - ref_y) / (skin_res->imageHeight().value());
                    uvs->push_back(osg::Vec2f(u, v));
                }
            }
        }
        else // projected SRS
        {
            for (auto& vert : m.verts)
            {
                new_verts->push_back(osg::Vec3d(vert.x, vert.y, vert.z) * world2local);
                if (uvs)
                {
                    uvs->push_back(osg::Vec2f());
                }
            }
        }


        // Assemble the final geometry.
        auto de = new osg::DrawElementsUInt(GL_TRIANGLES);
        de->reserve(m.triangles.size() * 3);
        for (auto& tri : m.triangles)
        {
            de->addElement(tri.second.i0);
            de->addElement(tri.second.i1);
            de->addElement(tri.second.i2);
        }

        osgGeom->setVertexArray(new_verts.get());
        osgGeom->removePrimitiveSet(0, osgGeom->getNumPrimitiveSets());
        osgGeom->addPrimitiveSet(de);
    }

    else
    {
        // original tesselation approach
        Tessellator::Plane plane = Tessellator::PLANE_XY;
        Bounds geo_ex;

        if (outputSRS)
        {
            // for geographic data we need to project into 2D before tessellating:
            if (outputSRS->isGeographic())
            {
                bool useGnomonic = true;
                Polygon* polygon = dynamic_cast<Polygon*>(proj.get());
                if (polygon)
                {
                    double area = fabs(polygon->getSignedArea2D());
                    // Only use gnomonic projection if the polygon is geographically large enough to warrant it.
                    if (area <= 0.05)
                    {
                        useGnomonic = false;
                        plane = Tessellator::PLANE_AUTO;
                    }
                }
                osg::Vec3d geo;
                osg::BoundingBoxd ecef_bb;

                bool allOnEquator = true;
                GeometryIterator xform_iter(proj.get(), true);
                while (xform_iter.hasMore())
                {
                    Geometry* part = xform_iter.next();
                    part->open();
                    for (osg::Vec3d& p : *part)
                    {
                        inputSRS->transform(p, outputSRS, geo);
                        if (geo.y() != 0.0)
                        {
                            allOnEquator = false;
                        }
                        geo_ex.expandBy(geo);
                        outputSRS->transformToWorld(geo, p);
                        ecef_bb.expandBy(p);
                    }
                }

                // calculate a UV reference point close to the centroid of the geometry.
                if (skin_res)
                {
                    auto geo_ex_anchor = geo_ex.center();
                    auto x = circum * (geo_ex_anchor.x() + 180.0) / 360.0;
                    auto y = (0.5*circum) * (geo_ex_anchor.y() + 90.0) / 180.0;
                    ref_x = x - fmod(x, skin_res->imageWidth().value());
                    ref_y = y - fmod(y, skin_res->imageHeight().value());
                }

                if (useGnomonic)
                {
                    const osg::Vec3d& center = ecef_bb.center();

                    GeometryIterator proj_iter(proj.get(), true);
                    while (proj_iter.hasMore())
                    {
                        Geometry* part = proj_iter.next();
                        for (osg::Vec3d& p : *part)
                        {
                            // The gnomonic equation won't provide any variation in y values if all of the coordinates are on the equator, so
                            // adjust the point slightly up from the equator if all points lie on the equator.
                            if (allOnEquator)
                            {
                                p.z() += 0.0000001;
                            }
                            ecef_to_gnomonic(p, center, outputSRS->getEllipsoid());
                        }
                    }
                }
            }

            else
            {
                GeometryIterator xform_iter(proj.get(), true);
                while (xform_iter.hasMore())
                {
                    Geometry* part = xform_iter.next();
                    part->open();
                    inputSRS->transform(part->asVector(), outputSRS);
                }
            }
        }
        else
        {
            // with no SRS, we need to automatically figure out what 
            // is the closest plane for tessellation
            plane = Tessellator::PLANE_AUTO;
        }

        // tessellate
        Tessellator tess;

        std::vector<uint32_t> indices;
        if (tess.tessellate2D(proj.get(), indices, plane) == false)
            return;

        if (indices.empty())
            return;

        int offset = verts->size();

        osg::Vec3d temp, vert;

        if (outputSRS && outputSRS->isGeographic())
        {
            osg::Vec3d geo;
            ConstGeometryIterator verts_iter(input, true);
            while (verts_iter.hasMore())
            {
                const Geometry* part = verts_iter.next();
                for (const auto& p : *part)
                {
                    inputSRS->transform(p, outputSRS, geo);
                    outputSRS->transformToWorld(geo, vert);
                    vert = vert * world2local;
                    verts->push_back(vert);

                    if (uvs)
                    {
                        auto x = circum * (geo.x() + 180.0) / 360.0;
                        auto y = (0.5*circum) * (geo.y() + 90.0) / 180.0;
                        auto u = (x - ref_x) / skin_res->imageWidth().value();
                        auto v = (y - ref_y) / (skin_res->imageHeight().value());
                        uvs->push_back(osg::Vec2f(u, v));
                    }
                }
            }
        }
        else
        {
            ConstGeometryIterator verts_iter(proj.get(), true);
            while (verts_iter.hasMore())
            {
                const Geometry* part = verts_iter.next();
                for (const auto& p : *part)
                {
                    verts->push_back(p * world2local);
                    if (uvs)
                    {
                        uvs->push_back(osg::Vec2f(0, 0));
                    }
                }
            }
        }

        osg::DrawElements* de = new osg::DrawElementsUInt(
            GL_TRIANGLES,
            indices.size(),
            &indices[0]);

        osgGeom->setVertexArray(verts.get());
        osgGeom->addPrimitiveSet(de);
    }
}

#else

void
BuildGeometryFilter::tileAndBuildPolygon(Geometry*               ring,
                                         const SpatialReference* featureSRS,
                                         const SpatialReference* outputSRS,
                                         bool                    makeECEF,
                                         bool                    tessellate,
                                         osg::Geometry*          osgGeom,
                                         const osg::Matrixd      &world2local)
{
    if (ring==NULL)
        return;

#define MAX_POINTS_PER_CROP_TILE 1024
//#define TARGET_TILE_SIZE_EXTENT_DEGREES 5

    if ( ring == 0L )
    {
        OE_WARN << LC << "Ring is NULL.\n";
        return;
    }

    // Tile the incoming polygon if necessary
    // NB: this breaks down at higher latitudes; see https://github.com/pelicanmapping/osgearth/issues/746

    GeometryCollection tiles;
    if (_maxPolyTilingAngle_deg.isSet())
        prepareForTesselation( ring, featureSRS, _maxPolyTilingAngle_deg.get(), MAX_POINTS_PER_CROP_TILE, tiles);
    else
        tiles.push_back( ring );

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;

    //OE_NOTICE << LC << "TABP: tiles = " << tiles.size() << "\n";

    // Process each ring independently
    for (int ringIndex = 0; ringIndex < tiles.size(); ringIndex++)
    {
        Geometry* geom = tiles[ringIndex].get();
        if (geom)
        {
            // temporary target geometry for this cell:
            osg::ref_ptr<osg::Geometry> temp = new osg::Geometry();
            temp->setVertexArray( new osg::Vec3Array() );

            // establish a local plane for this cell based on its centroid:
            GeoPoint cellCenter(featureSRS, geom->getBounds().center());
            cellCenter.transform(outputSRS, cellCenter);
            osg::Matrix world2cell;
            cellCenter.createWorldToLocal( world2cell );

            // build the localized polygon:
            buildPolygon(geom, featureSRS, outputSRS, makeECEF, temp.get(), world2cell);

            // if successful, transform the verts back into our master LTP:
            if ( temp->getNumPrimitiveSets() > 0 )
            {
                // Tesselate the polygon while the coordinates are still in the LTP
                if (tesselateGeometry( temp.get(), useOSGTessellator().value() ))
                {
                    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(temp->getVertexArray());
                    if ( verts->getNumElements() > 0 )
                    {
                        // Convert the coordinates back to the master LTP.
                        // This is ok, but you will probably run into precision errors if the tile size is very large.
                        osg::Matrix cell2world;
                        cell2world.invert( world2cell );
                        osg::Matrix cell2local = cell2world * world2local; // pre-multiply to avoid precision loss

                        for(int i=0; i<verts->size(); ++i)
                        {
                            (*verts)[i] = (*verts)[i] * cell2local;
                        }
                        geode->addDrawable(temp.get());
                    }
                }
            }
        }
        else
        {
            OE_TEST << LC << "TABP: Uh oh. found a non-Ring geometry: " << geom->toString(geom->getComponentType()) << "\n";
        }
    }

    // The geode is going to contain all of our polygons now, so merge them into one.
    osgUtil::Optimizer optimizer;
    osgUtil::Optimizer::MergeGeometryVisitor mgv;
    mgv.setTargetMaximumNumberOfVertices(Registry::instance()->getMaxNumberOfVertsPerDrawable());
    mgv.apply( *geode.get() );

    // and copy them into the output geometry.
    if ( geode->getNumDrawables() > 0 )
    {
        // If we have more than one drawable after the MergeGeometryVisitor ran, we have a problem so
        // dump out some info to help debug.
        if (geode->getNumDrawables() != 1)
        {
            OE_WARN << LC << "MergeGeometryVisitor failed to merge geometries into a single one.  Num drawables " << geode->getNumDrawables() << std::endl;
            for (unsigned int i = 0; i < geode->getNumDrawables(); i++)
            {
                osg::Geometry* g = geode->getDrawable(i)->asGeometry();
                if (g)
                {
                    osg::Vec3Array* verts = dynamic_cast<osg::Vec3Array*>(g->getVertexArray());
                    if (verts)
                    {
                        OE_WARN << "Geometry " << i << " has " << verts->size() << " verts" << std::endl;
                        OE_WARN << "Geometry " << i << " has " << g->getNumPrimitiveSets() << " primitive sets" << std::endl;
                        for (unsigned int j = 0; j < g->getNumPrimitiveSets(); j++)
                        {
                            osg::PrimitiveSet* ps = g->getPrimitiveSet(j);
                            OE_WARN << "PrimitiveSet " << j << ps->className() << std::endl;
                        }
                    }
                }
            }
        }
        osgGeom->setVertexArray( geode->getDrawable(0)->asGeometry()->getVertexArray() );
        osgGeom->setPrimitiveSetList( geode->getDrawable(0)->asGeometry()->getPrimitiveSetList() );
    }
}
#endif


namespace
{
    struct GenerateNormalFunctor
    {
        osg::Vec3Array* _verts;
        osg::Vec3Array* _normals;

        GenerateNormalFunctor() : _verts(0L), _normals(0L) { }

        void set(osg::Vec3Array *cb, osg::Vec3Array *nb)
        {
            _verts = cb;
            _normals = nb;
        }

        inline void operator()(unsigned i1, unsigned i2, unsigned i3)
        {
            const osg::Vec3& v1 = (*_verts)[i1];
            const osg::Vec3& v2 = (*_verts)[i2];
            const osg::Vec3& v3 = (*_verts)[i3];

            // calc orientation of triangle.
            osg::Vec3 normal = (v2 - v1) ^ (v3 - v1);
            normal.normalize();

            (*_normals)[i1] += normal;
            (*_normals)[i2] += normal;
            (*_normals)[i3] += normal;
        }

        void finish()
        {
            for (unsigned i = 0; i < _normals->size(); ++i)
            {
                (*_normals)[i].normalize();
            }
        }
    };

    struct GenerateNormals : public osg::NodeVisitor
    {
        GenerateNormals() : osg::NodeVisitor()
        {
            setTraversalMode(TRAVERSE_ALL_CHILDREN);
            setNodeMaskOverride(~0);
        }

        inline void apply(osg::Drawable& drawable)
        {
            osg::Geometry* geom = drawable.asGeometry();
            if (geom)
            {
                osg::Vec3Array* verts = dynamic_cast<osg::Vec3Array*>(geom->getVertexArray());

                osg::Vec3Array* normals = new osg::Vec3Array(verts->size());
                normals->setBinding(normals->BIND_PER_VERTEX);

                osg::TriangleIndexFunctor<GenerateNormalFunctor> f;
                f.set(verts, normals);
                geom->accept(f);
                f.finish();

                geom->setNormalArray(normals);
            }
            traverse(drawable);
        }
    };
}

osg::Node*
BuildGeometryFilter::push( FeatureList& input, FilterContext& context )
{
    osg::ref_ptr<osg::Group> result = new osg::Group();

    computeLocalizers( context );

    const LineSymbol*    line  = _style.get<LineSymbol>();
    const PolygonSymbol* poly  = _style.get<PolygonSymbol>();
    const PointSymbol*   point = _style.get<PointSymbol>();

    // bin the feautres into polygons, lines, polygonized lines, and points.
    FeatureList polygons;
    FeatureList lines;
    FeatureList polygonizedLines;
    FeatureList points;
    FeatureList wireLines;
    FeatureList meshes;

    FeatureList splitFeatures;

    // Split features across the dateline if necessary
    if (context.outputSRS() && !context.outputSRS()->isGeographic())
    {
        for(auto& feature : input)
        {
            auto* clone = new Feature(*feature.get());
            clone->splitAcrossAntimeridian();
            splitFeatures.emplace_back(clone);
        }
    }
    else
    {
        // Just copy the input list to the split list
        std::copy(input.begin(), input.end(), std::back_inserter(splitFeatures));
    }

    osg::ref_ptr<osg::Group> polysGroup, linesGroup, pointsGroup, meshesGroup;

    for(FeatureList::iterator i = splitFeatures.begin(); i != splitFeatures.end(); ++i)
    {
        Feature* f = i->get();

        // first consider the overall style:
        bool has_polysymbol = poly != 0L;
        bool has_pointsymbol = point != 0L;

        bool has_linesymbol = false;
        bool has_polylinesymbol = false;
        bool has_wirelinessymbol = false;

        if (line)
        {
            bool widthInPixels = line->stroke()->width()->eval(f, context).getUnits() == Units::PIXELS;
            has_linesymbol = widthInPixels;
            has_polylinesymbol = !widthInPixels;
            has_wirelinessymbol = line->useWireLines() == true;
        }

        // if the featue has a style set, that overrides:
        if ( f->style() )
        {
            has_polysymbol = has_polysymbol     || (f->style()->has<PolygonSymbol>());
            has_pointsymbol = has_pointsymbol || (f->style()->has<PointSymbol>());

            auto* f_line = f->style()->get<LineSymbol>();
            if (f_line)
            {
                bool widthInPixels = f_line->stroke()->width()->eval(f, context).getUnits() == Units::PIXELS;
                has_linesymbol = has_linesymbol || (widthInPixels == true);
                has_polylinesymbol = has_polylinesymbol || (widthInPixels == false);
                has_wirelinessymbol = has_wirelinessymbol || (f_line->useWireLines() == true);
            }
        }

        // if there's a polygon with outlining disabled, nix the line symbol.
        if (has_polysymbol)
        {
            if (poly && poly->outline() == false)
                has_linesymbol = false;
            else if (f->style() && f->style()->has<PolygonSymbol>() && f->style()->get<PolygonSymbol>()->outline() == false)
                has_linesymbol = false;
        }

        // if no style is set, use the geometry type:
        if ( !has_polysymbol && !has_linesymbol && !has_polylinesymbol && !has_pointsymbol && f->getGeometry() )
        {
            Style new_style;
            if (f->style()) new_style = *f->style();

            switch( f->getGeometry()->getComponentType() )
            {
            default:
            case Geometry::TYPE_LINESTRING:
            case Geometry::TYPE_RING:
                new_style.add( new LineSymbol() );
                has_linesymbol = true;
                break;

            case Geometry::TYPE_POINT:
            case Geometry::TYPE_POINTSET:
                new_style.add( new PointSymbol() );
                has_pointsymbol = true;
                break;

            case Geometry::TYPE_POLYGON:
                new_style.add( new PolygonSymbol() );
                has_polysymbol = true;
                break;
            }

            f->setStyle(new_style);
        }

        if ( has_polysymbol )
        {
            //if (f->getGeometry()->getType() == Geometry::TYPE_TRIMESH)
            //    meshes.push_back(f);
            //else
            polygons.push_back( f );
        }

        if (has_linesymbol)
        {
            lines.push_back(f);
        }

        if ( has_wirelinessymbol)
        {
            wireLines.push_back( f);
        }

        else if (has_polylinesymbol)
        {
            polygonizedLines.push_back(f);
        }

        if (has_pointsymbol)
        {
            points.push_back(f);
        }
    }

    // process them separately.

    if ( polygons.size() > 0 )
    {
        OE_TEST << LC << "Building " << polygons.size() << " polygons." << std::endl;
        osg::ref_ptr<osg::Geode> geode = processPolygons(polygons, context);
        if ( geode.valid() && geode->getNumDrawables() > 0 )
        {
            osgUtil::Optimizer::MergeGeometryVisitor mg;
            mg.setTargetMaximumNumberOfVertices(Registry::instance()->getMaxNumberOfVertsPerDrawable());
            geode->accept(mg);

            if (_optimizeVertexOrdering == true)
            {
                osg::Timer_t t = osg::Timer::instance()->tick();
                osgUtil::Optimizer o;
                o.optimize( geode.get(),
                    osgUtil::Optimizer::INDEX_MESH |
                    osgUtil::Optimizer::VERTEX_PRETRANSFORM |
                    osgUtil::Optimizer::VERTEX_POSTTRANSFORM );
                OE_INFO << "Vertex ordering optimization took " << osg::Timer::instance()->delta_s(t, osg::Timer::instance()->tick()) << std::endl;
            }

            // Generate normals. CANNOT use OSG's SmoothingVisitor because it adds verts
            // but ignores other vertex attribute arrays.
            GenerateNormals gen;
            geode->accept(gen);

            if (!polysGroup.valid())
                polysGroup = new osg::Group();

            polysGroup->addChild( geode.get() );
        }
    }

    if ( polygonizedLines.size() > 0 )
    {
        OE_TEST << LC << "Building " << polygonizedLines.size() << " polygonized lines." << std::endl;
        bool twosided = polygons.size() > 0 ? false : true;
        osg::ref_ptr< osg::Group > lines = processPolygonizedLines(polygonizedLines, twosided, context, false);
        if (lines.valid() && lines->getNumChildren() > 0)
        {
            if (!linesGroup.valid())
                linesGroup = new osg::Group();

            linesGroup->addChild( lines.get() );
        }
    }

    if ( wireLines.size() > 0 )
    {
        OE_TEST << LC << "Building " << wireLines.size() << " wire lines." << std::endl;
        osg::ref_ptr< osg::Group > lines = processPolygonizedLines(wireLines, true, context, true);

        if (lines.valid() && lines->getNumChildren() > 0)
        {
            if (!meshesGroup.valid())
                meshesGroup = new osg::Group();
            meshesGroup->addChild( lines.get() );
        }

    }

    if ( lines.size() > 0 )
    {
        OE_TEST << LC << "Building " << lines.size() << " lines." << std::endl;

        osg::ref_ptr<osg::Group> group = processLines(lines, context);

        if ( group.valid() && group->getNumChildren() > 0 )
        {
            if (!linesGroup.valid())
                linesGroup = new osg::Group();
            linesGroup->addChild(group.get());
        }
    }

    if ( points.size() > 0 )
    {
        OE_TEST << LC << "Building " << points.size() << " points." << std::endl;

        osg::ref_ptr<osg::Group> group = processPoints(points, context);

        if ( group.valid() && group->getNumChildren() > 0 )
        {
            if (!pointsGroup.valid())
                pointsGroup = new osg::Group();
            pointsGroup->addChild(group.get());
        }
    }

    if (meshes.size() > 0)
    {
        osg::ref_ptr<osg::Group> group = processMeshes(meshes, context);

        if (group.valid() && group->getNumChildren() > 0)
        {
            if (!meshesGroup.valid())
                meshesGroup = new osg::Group();
            meshesGroup->addChild(group.get());
        }
    }

    if (polysGroup.valid())
        result->addChild(polysGroup.get());
    if (linesGroup.valid())
        result->addChild(linesGroup.get());
    if (pointsGroup.valid())
        result->addChild(pointsGroup.get());
    if (meshesGroup.valid())
        result->addChild(meshesGroup.get());


    //// indicate that geometry contains clamping attributes
    if (_style.has<AltitudeSymbol>() &&
        _style.get<AltitudeSymbol>()->technique() == AltitudeSymbol::TECHNIQUE_GPU)
    {
        Clamping::installHasAttrsUniform( result->getOrCreateStateSet() );
    }

    // Prepare buffer objects.
    AllocateAndMergeBufferObjectsVisitor allocAndMerge;
    result->accept( allocAndMerge );


    if ( result->getNumChildren() > 0 )
    {
        // apply the delocalization matrix for no-jitter
        return delocalize( result.release() );
    }
    else
    {
        return 0L;
    }
}
