/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ExtrudeGeometryFilter>
#include <osgEarth/Session>
#include <osgEarth/FeatureSourceIndexNode>
#include <osgEarth/StyleSheet>
#include <osgEarth/Clamping>
#include <osgEarth/Utils>
#include <osgEarth/Tessellator>
#include <osgEarth/LineDrawable>

#include <osg/Geode>
#include <osg/Geometry>
#include <osgUtil/Tessellator>
#include <osg/PolygonOffset>
#include <osg/LineWidth>

#define LC "[ExtrudeGeometryFilter] "

using namespace osgEarth;

namespace
{
    // Calculates the rotation angle of a shape. This conanically applies to
    // buildings; it finds the longest edge and compares its angle to the
    // x-axis to determine a rotation value. This method is used so we can 
    // properly rotate textures for rooftop application.
    float getApparentRotation( const Geometry* geom )
    {
        Segment n;
        double  maxLen2 = 0.0;
        ConstSegmentIterator i(geom, true);
        while( i.hasMore() )
        {
            auto& s = i.next();
            double len2 = (s.second - s.first).length2();
            if ( len2 > maxLen2 ) 
            {
                maxLen2 = len2;
                n = s;
            }
        }

        const osg::Vec3d& p1 = n.first.x() < n.second.x() ? n.first : n.second;
        const osg::Vec3d& p2 = n.first.x() < n.second.x() ? n.second : n.first;

        return atan2( p2.x()-p1.x(), p2.y()-p1.y() );
    }

    double sign_of(double a)     {
        return a < 0.0 ? -1.0 : 1.0;
    }
}

#define AS_VEC4(V3, X) osg::Vec4f( (V3).x(), (V3).y(), (V3).z(), X )

//------------------------------------------------------------------------

ExtrudeGeometryFilter::ExtrudeGeometryFilter() :
    _mergeGeometry(true),
    _wallAngleThresh_deg(60.0),
    _styleDirty(true),
    _makeStencilVolume(false),
    _gpuClamping(false)
{
    _cosWallAngleThresh = cos(_wallAngleThresh_deg);
}

void
ExtrudeGeometryFilter::setStyle( const Style& style )
{
    _style      = style;
    _styleDirty = true;
}

void
ExtrudeGeometryFilter::reset( const FilterContext& context )
{
    _cosWallAngleThresh = cos( _wallAngleThresh_deg );
    _geodes.clear();
    
    if ( _styleDirty )
    {
        const StyleSheet* sheet = context.session() ? context.session()->styles() : 0L;

        _wallSkinSymbol    = 0L;
        _wallPolygonSymbol = 0L;
        _roofSkinSymbol    = 0L;
        _roofPolygonSymbol = 0L;
        _extrusionSymbol   = 0L;
        _outlineSymbol     = 0L;

        _gpuClamping = false;

        _extrusionSymbol = _style.get<ExtrusionSymbol>();
        if ( _extrusionSymbol.valid() )
        {
            // make a copy of the height expression so we can use it:
            if ( _extrusionSymbol->height().isSet() )
            {
                _heightExpr = _extrusionSymbol->height().value();
            }

            // If there is no height expression, and we have either absolute or terrain-relative
            // clamping, THAT means that we want to extrude DOWN from the geometry to the ground
            // (instead of from the geometry.)
            AltitudeSymbol* alt = _style.get<AltitudeSymbol>();
            if ( alt && !_extrusionSymbol->height().isSet() && !_extrusionSymbol->height().isSet() )
            {
                if (alt->clamping() == AltitudeSymbol::CLAMP_ABSOLUTE ||
                    alt->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN )
                {
                    _heightExpr = Expression<Distance>("0.0 - feature.properties.__min_hat"); // = "0-[__min_hat]";
                }
            }

            // cache the GPU Clamping directive:
            if ( alt && alt->technique() == AltitudeSymbol::TECHNIQUE_GPU )
            {
                _gpuClamping = true;
            }
            
            // attempt to extract the wall symbols:
            if ( _extrusionSymbol->wallStyleName().isSet() && sheet != 0L )
            {
                const Style* wallStyle = sheet->getStyle( *_extrusionSymbol->wallStyleName(), false );
                if ( wallStyle )
                {
                    _wallSkinSymbol = wallStyle->get<SkinSymbol>();
                    _wallPolygonSymbol = wallStyle->get<PolygonSymbol>();
                }
            }
            else if (_extrusionSymbol->wallSkinName().isSet())
            {
                auto s = new SkinSymbol();
                s->uriContext() = _extrusionSymbol->uriContext();
                s->name()->setLiteral(_extrusionSymbol->wallSkinName().value());
                _wallSkinSymbol = s;
            }

            // attempt to extract the rooftop symbols:
            if ( _extrusionSymbol->roofStyleName().isSet() && sheet != 0L )
            {
                const Style* roofStyle = sheet->getStyle( *_extrusionSymbol->roofStyleName(), false );
                if ( roofStyle )
                {
                    _roofSkinSymbol = roofStyle->get<SkinSymbol>();
                    _roofPolygonSymbol = roofStyle->get<PolygonSymbol>();
                }
            }
            else if (_extrusionSymbol->roofSkinName().isSet())
            {
                auto s = new SkinSymbol();
                s->uriContext() = _extrusionSymbol->uriContext();
                s->name()->setLiteral(_extrusionSymbol->roofSkinName().value());
                _roofSkinSymbol = s;
            }

            // if there's a line symbol, use it to outline the extruded data.
            _outlineSymbol = _style.get<LineSymbol>();

            // ...unless a wall poly symbol overrides it.
            if (_wallPolygonSymbol.valid() && _wallPolygonSymbol->outline() == false)
                _outlineSymbol = 0L;

            if (_roofPolygonSymbol.valid() && _roofPolygonSymbol->outline() == false)
                _outlineSymbol = 0L;
        }

        // backup plan for skin symbols:
        const SkinSymbol* skin = _style.get<SkinSymbol>();
        if ( skin )
        {
            if ( !_wallSkinSymbol.valid() )
                _wallSkinSymbol = skin;
            if ( !_roofSkinSymbol.valid() )
                _roofSkinSymbol = skin;
        }

        // backup plan for poly symbols:
        _polySymbol = _style.get<PolygonSymbol>();
        if (_polySymbol.valid())
        {
            if ( !_wallPolygonSymbol.valid() )
                _wallPolygonSymbol = _polySymbol.get();
            if ( !_roofPolygonSymbol.valid() )
                _roofPolygonSymbol = _polySymbol.get();
        }

        _styleDirty = false;
    }
}

namespace
{
    inline osg::Vec2d asVec2d(const osg::Vec3d& v) { return osg::Vec2d(v.x(), v.y()); }

    inline osg::Vec4f shade(const osg::Vec4f& input, const osg::Vec3f& normal, float minimum)
    {
        float d = minimum + ((1.0f - minimum) * 0.5 * ((normal * osg::Vec3(1,0,0)) + 1.0));
        return osg::Vec4f(input.r() * d, input.g() * d, input.b() * d, input.a());
    }
}


bool
ExtrudeGeometryFilter::buildStructure(
    const Geometry* input,
    const SpatialReference* srs,
    double height,
    double heightOffset,
    bool flatten,
    float verticalOffset,
    const SkinResource* wallSkin,
    const SkinResource* roofSkin,
    Structure& structure,
    FilterContext& cx)
{
    bool makeECEF = false;
    osg::ref_ptr<const SpatialReference> mapSRS;
    unsigned numElevations = 0, numCorners = 0, numFaces = 0;

    if ( cx.isGeoreferenced() )
    {
       mapSRS   = cx.session()->getMapSRS();
       makeECEF = cx.session()->isMapGeocentric();
    }

    // whether this is a closed polygon structure.
    structure.isPolygon = (input->getComponentType() == Geometry::TYPE_POLYGON);

    // a negative height means the structure is inverted, so we'll need to flip the normals.
    structure.isInverted = height < 0.0;

    // store the vert offset for later encoding
    structure.verticalOffset = verticalOffset;

    // extrusion working variables
    double     targetLen = -DBL_MAX;
    osg::Vec3d minLoc(DBL_MAX, DBL_MAX, DBL_MAX);
    double     minLoc_len = DBL_MAX;
    osg::Vec3d maxLoc(0,0,0);
    double     maxLoc_len = 0;

    // Initial pass over the geometry does two things:
    // 1: Calculate the minimum Z across all parts.
    // 2: Establish a "target length" for extrusion
    double absHeight = fabs(height);

    ConstGeometryIterator zfinder( input );
    while( zfinder.hasMore() )
    {
        const Geometry* geom = zfinder.next();
        for( Geometry::const_iterator m = geom->begin(); m != geom->end(); ++m )
        {
            osg::Vec3d m_point = *m;
            m_point.z() += heightOffset;

            if ( m_point.z() + absHeight > targetLen )
                targetLen = m_point.z() + absHeight;

            if (m_point.z() < minLoc.z())
                minLoc = m_point;

            if (m_point.z() > maxLoc.z())
                maxLoc = m_point;
        }
    }

    osg::Vec3d c = input->getBounds().center();
    osg::Vec3d centroid(c.x(), c.y(), minLoc.z());

    if (srs && mapSRS.valid())
    {
        transformAndLocalize(centroid, srs, structure.baseCentroid, mapSRS.get(), _world2local, makeECEF );
    }
    
    float   roofRotation  = 0.0f;
    Bounds  roofBounds;
    float   sinR = 0.0f, cosR = 0.0f;
    double  roofTexSpanX = 0.0, roofTexSpanY = 0.0;
    osg::ref_ptr<const SpatialReference> roofProjSRS;

    if ( roofSkin )
    {
        roofBounds = input->getBounds();

        // if our data is lat/long, we need to reproject the geometry and the bounds into a projected
        // coordinate system in order to properly generate tex coords.
        if ( srs && srs->isGeographic() )
        {
            roofProjSRS = SpatialReference::create("spherical-mercator");
            if ( roofProjSRS.valid() )
            {
                GeoExtent roofExtent(srs, roofBounds);
                roofExtent = roofExtent.transform(roofProjSRS.get());
                osg::ref_ptr<Geometry> projectedInput = input->clone();
                srs->transform( projectedInput->asVector(), roofProjSRS.get() );
                roofBounds = projectedInput->getBounds();
                roofRotation = getApparentRotation( projectedInput.get() );
            }
        }
        else
        {
            roofRotation = getApparentRotation( input );
        }
            
        sinR = sin(roofRotation);
        cosR = cos(roofRotation);

        if ( !roofSkin->isTiled().value() )
        {
            //note: non-tiled roofs don't really work atm.
            double w = roofBounds.xMax() - roofBounds.xMin();
            double h = roofBounds.yMax() - roofBounds.yMin();
            roofTexSpanX = cosR*w - sinR*h;
            roofTexSpanY = sinR*w + cosR*h;
        }
        else
        {
            roofTexSpanX = roofSkin->imageWidth().isSet() ? *roofSkin->imageWidth() : roofSkin->imageHeight().isSet() ? *roofSkin->imageHeight() : 10.0;
            if ( roofTexSpanX <= 0.0 ) roofTexSpanX = 10.0;
            roofTexSpanY = roofSkin->imageHeight().isSet() ? *roofSkin->imageHeight() : roofSkin->imageWidth().isSet() ? *roofSkin->imageWidth() : 10.0;
            if ( roofTexSpanY <= 0.0 ) roofTexSpanY = 10.0;
        }
    }

    // prep for wall texture coordinate generation.
    const double defaultSpan = 100.0;
    double texWidthM = wallSkin ? wallSkin->imageWidth().getOrUse(defaultSpan) : defaultSpan;
    double texHeightM = wallSkin ? wallSkin->imageHeight().getOrUse(defaultSpan) : defaultSpan;

    OE_SOFT_ASSERT_AND_RETURN(texWidthM > 0.0, false);
    OE_SOFT_ASSERT_AND_RETURN(texHeightM > 0.0, false);

    ConstGeometryIterator iter( input );
    while( iter.hasMore() )
    {
        const Geometry* part = iter.next();

        // skip a part that's too small
        if (part->size() < 2)
            continue;

        // add a new wall.
        structure.elevations.push_back(Elevation());
        Elevation& elevation = structure.elevations.back();

        double maxHeight = targetLen - minLoc.z();

        // Adjust the texture height so it is a multiple of the maximum height
        double div = osg::round(maxHeight / texHeightM);
        elevation.texHeightAdjustedM = div > 0.0 ? maxHeight / div : maxHeight;

        // Step 1 - Create the real corners and transform them into our target SRS.
        Corners corners;
        for(auto& raw_point : *part)
        {
            auto point = raw_point + osg::Vec3d(0, 0, heightOffset);

            auto corner = corners.emplace(corners.end());
            
            // mark as "from source", as opposed to being inserted by the algorithm.
            corner->isFromSource = true;
            corner->base = point;

            if (height > 0.0)
            {
                if (flatten)
                    corner->roof.set(corner->base.x(), corner->base.y(), targetLen);
                else
                    corner->roof.set(corner->base.x(), corner->base.y(), corner->base.z() + height);
            }

            else
            {
                if (flatten)
                    corner->roof.set(corner->base.x(), corner->base.y(), -targetLen);
                else
                    corner->roof.set(corner->base.x(), corner->base.y(), corner->base.z() + height);
            }
            
            // figure out the rooftop texture coords before doing any transformation:
            if ( roofSkin && srs )
            {
                double xr, yr;

                if ( srs && srs->isGeographic() && roofProjSRS )
                {
                    osg::Vec3d projRoofPt;
                    srs->transform( corner->roof, roofProjSRS.get(), projRoofPt );
                    xr = (projRoofPt.x() - roofBounds.xMin());
                    yr = (projRoofPt.y() - roofBounds.yMin());
                }
                else
                {
                    xr = (corner->roof.x() - roofBounds.xMin());
                    yr = (corner->roof.y() - roofBounds.yMin());
                }

                corner->roofTexU = (cosR*xr - sinR*yr) / roofTexSpanX;
                corner->roofTexV = (sinR*xr + cosR*yr) / roofTexSpanY;
            }

            // transform into target SRS.
            if (srs && mapSRS.valid())
            {
                transformAndLocalize( corner->base, srs, corner->base, mapSRS.get(), _world2local, makeECEF );
                transformAndLocalize( corner->roof, srs, corner->roof, mapSRS.get(), _world2local, makeECEF );
            }

            // cache the length for later use.
            corner->height = (corner->roof - corner->base).length();
        }

        if (corners.size() > 5000)
        {
            OE_WARN << "numCorners (actual) = " << corners.size() << std::endl;
        }

        if (structure.isPolygon)
        {
            // close the polygon
            corners.push_back(corners.front());
            corners.back().isFromSource = false;
        }

        // Step 2 - Insert intermediate Corners as needed to satify texturing
        // requirements (if necessary) and record each corner offset (horizontal distance
        // from the beginning of the part geometry to the corner.)
        if (wallSkin)
        {
            double corner_offset = 0.0;
            double next_tex_boundary = texWidthM;

            for (Corners::iterator c = corners.begin(); c != corners.end(); ++c)
            {
                auto this_corner = c;
                this_corner->offsetX = corner_offset;

                auto next_corner = c;
                ++next_corner;
                if (next_corner == corners.end())
                {
                    break;
                }

                auto base_vec_3d = next_corner->base - this_corner->base;
                double span_2d = asVec2d(base_vec_3d).length();

                // if this span crosses the next texture boundary, we need to insert a corner.
                auto dist_to_next_tex_boundary = next_tex_boundary - corner_offset;
                if (equivalent(dist_to_next_tex_boundary, 0.0))
                {
                    // this is a corner that's exactly on a texture boundary. Just skip it.
                    corner_offset = next_tex_boundary;
                    next_tex_boundary += texWidthM;
                }

                else if (span_2d > dist_to_next_tex_boundary)
                {
                    osg::Vec3d roof_vec_3d = next_corner->roof - this_corner->roof;
                    double t = dist_to_next_tex_boundary / span_2d;

                    auto new_corner = corners.emplace(next_corner, *this_corner); // copy and append
                    new_corner->isFromSource = false;
                    new_corner->base = this_corner->base + base_vec_3d * t;
                    new_corner->roof = this_corner->roof + roof_vec_3d * t;
                    new_corner->height = (new_corner->roof - new_corner->base).length();
                    corner_offset = next_tex_boundary;
                    next_tex_boundary += texWidthM;
                }
                else
                {
                    corner_offset += span_2d;
                }
            }

            if (corners.size() > 3500)
            {
                //OE_WARN << "numCorners (interp) = " << corners.size() << std::endl;
            }
        }

        // Step 3 - Calculate the angle of each corner.
        // TODO: is this never used???
        osg::Vec2d prev_vec_2d;
        for(Corners::iterator c = corners.begin(); c != corners.end(); ++c)
        {
            auto this_corner = c;
            auto next_corner = c;

            if ( ++next_corner == corners.end() )
                next_corner = corners.begin();

            if ( this_corner == corners.begin() )
            {
                auto prev_corner = corners.end();
                --prev_corner;
                prev_vec_2d = asVec2d(this_corner->roof) - asVec2d(prev_corner->roof);
                prev_vec_2d.normalize();
            }

            auto this_vec_2d = asVec2d(next_corner->roof) - asVec2d(this_corner->roof);
            this_vec_2d.normalize();
            if ( c != corners.begin() )
            {
                c->cosAngle = prev_vec_2d * this_vec_2d;
            }

            prev_vec_2d = this_vec_2d; // was missing!
        }

        numCorners += corners.size();

        // Step 4 - Create faces connecting each pair of Posts.
        Faces& faces = elevation.faces;
        for(Corners::const_iterator c = corners.begin(); c != corners.end(); ++c)
        {
            auto this_corner = c;
            auto next_corner = c;
            if (++next_corner == corners.end())
                break;
            
            // only close the shape for polygons.
            //if (next_corner != corners.begin() || structure.isPolygon)
            {
                faces.emplace_back();
                Face& face = faces.back();
                face.left = *this_corner;
                face.right = *next_corner;

                //// recalculate the final offset on the last face
                //if ( next_corner == corners.begin() )
                //{
                //    osg::Vec3d vec = next_corner->roof - this_corner->roof;
                //    face.right.offsetX = face.left.offsetX + vec.length();
                //}

                face.widthM = next_corner->offsetX - this_corner->offsetX;

                ++numFaces;
            }
        }
    }

    return true;
}

bool
ExtrudeGeometryFilter::buildWallGeometry(
    const Structure& structure,
    Feature* feature,
    osg::Geometry* walls,
    const osg::Vec4& wallColor,
    const osg::Vec4& wallBaseColor,
    const SkinResource* wallSkin,
    float shadeMin,
    FeatureIndexBuilder* index)
{
    bool madeGeom = true;

    // 6 verts per face total (3 triangles)
    unsigned numWallVerts = 6 * structure.getNumPoints();

    const double defaultSpan = 100.0;
    double texWidthM = wallSkin ? wallSkin->imageWidth().getOrUse(defaultSpan) : defaultSpan;
    double texHeightM = wallSkin ? wallSkin->imageHeight().getOrUse(defaultSpan) : defaultSpan;
    bool   useColor    = (!wallSkin || wallSkin->texEnvMode() != osg::TexEnv::DECAL) && !_makeStencilVolume;

    OE_SOFT_ASSERT_AND_RETURN(texWidthM > 0.0, false);
    OE_SOFT_ASSERT_AND_RETURN(texHeightM > 0.0, false);

    // Scale and bias:
    osg::Vec2f scale(1, 1), bias(0, 0);
    float layer;
    if ( wallSkin )
    {
        bias.set (wallSkin->imageBiasS().getOrUse(0.0),  wallSkin->imageBiasT().getOrUse(0.0));
        scale.set(wallSkin->imageScaleS().getOrUse(1.0), wallSkin->imageScaleT().getOrUse(1.0));
        layer = (float)wallSkin->imageLayer().get();
    }

    // create all the OSG geometry components
    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(walls->getVertexArray());
    if (!verts)
    {
        verts = new osg::Vec3Array();
        walls->setVertexArray( verts );
    }
    // Store the current size of the geometry
    unsigned vertptr = verts->size();
    unsigned startVertPtr = vertptr;

    osg::Vec3Array* tex = 0L;
    if ( wallSkin )
    { 
        tex = static_cast<osg::Vec3Array*>(walls->getTexCoordArray(0));
        if (!tex)
        {
            tex = new osg::Vec3Array();
            walls->setTexCoordArray( 0, tex );
        }
    }

    osg::Vec4Array* colors = 0L;
    if ( useColor )
    {
        colors = static_cast<osg::Vec4Array*>(walls->getColorArray());
        if (!colors)
        {
            colors = new osg::Vec4Array( osg::Array::BIND_PER_VERTEX);
            walls->setColorArray( colors );
        }
    }

    osg::Vec3Array* normals = static_cast<osg::Vec3Array*>(walls->getNormalArray());
    if (!normals)
    {
        normals = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX);
        walls->setNormalArray(normals);
    }
    
    ObjectIDArray* ids = nullptr;
    if (index)
    {
        ids = static_cast<ObjectIDArray*>(walls->getVertexAttribArray(osg::Drawable::SECONDARY_COLORS));
        if (!ids)
        {
            ids = new ObjectIDArray();
            ids->setBinding(osg::Array::BIND_PER_VERTEX);
            ids->setNormalize(false);
            walls->setVertexAttribArray(osg::Drawable::SECONDARY_COLORS, ids);
            ids->setPreserveDataType(true);
        }
    }

    osg::Vec4Array* anchors = 0L;
    
    // If GPU clamping is in effect, create clamping attributes.
    if ( _gpuClamping )
    {     
        anchors = static_cast<osg::Vec4Array*>(walls->getVertexAttribArray(Clamping::AnchorAttrLocation));
        if (!anchors)
        {
            anchors = new osg::Vec4Array( osg::Array::BIND_PER_VERTEX);
            anchors->setNormalize(false);
            walls->setVertexAttribArray( Clamping::AnchorAttrLocation, anchors );
        } 
        //anchors->resize(anchors->size() + numWallVerts);
    }

    bool tex_repeats_y = (wallSkin && wallSkin->isTiled() == true);

    bool flatten =
        _style.has<ExtrusionSymbol>() &&
        _style.get<ExtrusionSymbol>()->flatten() == true;

    osg::DrawElements* de = nullptr;
    if (walls->getNumPrimitiveSets() == 0)
    {
        de = new osg::DrawElementsUInt(GL_TRIANGLES);
        walls->addPrimitiveSet(de);
    }
    else
    {
        de = static_cast<osg::DrawElements*>(walls->getPrimitiveSet(0));
    }

    for(Elevations::const_iterator elev = structure.elevations.begin(); elev != structure.elevations.end(); ++elev)
    {
        for(Faces::const_iterator f = elev->faces.begin(); f != elev->faces.end(); ++f, vertptr+=6)
        {
            // set the 6 wall verts.
            if (structure.isInverted)
            {
                verts->push_back(f->left.roof);
                verts->push_back(f->right.roof);
                verts->push_back(f->right.base);
                verts->push_back(f->right.base);
                verts->push_back(f->left.base);
                verts->push_back(f->left.roof);
            }
            else
            {
                verts->push_back(f->left.roof);
                verts->push_back(f->left.base);
                verts->push_back(f->right.base);
                verts->push_back(f->right.base);
                verts->push_back(f->right.roof);
                verts->push_back(f->left.roof);
            }

            //TODO: use the cosAngle to decide whether to smooth the corner!

            osg::Vec3 normal_cache[6];

            const osg::Vec3& v1 = (*verts)[vertptr]; // f->left.roof;
            const osg::Vec3& v2 = (*verts)[vertptr + 1]; // f->left.base;
            const osg::Vec3& v3 = (*verts)[vertptr + 2]; // ->right.base;
            osg::Vec3 normal((v2 - v1) ^ (v3 - v1));

            for (int i = 0; i < 6; ++i)
            {
                normal.normalize();
                normal_cache[i] = normal;
                normals->push_back(normal);
            }
            
            if ( anchors )
            {
                float x = structure.baseCentroid.x(), y = structure.baseCentroid.y(), vo = structure.verticalOffset;

                (*anchors)[vertptr+1].set( x, y, vo, Clamping::ClampToGround );
                (*anchors)[vertptr+2].set( x, y, vo, Clamping::ClampToGround );
                (*anchors)[vertptr+3].set( x, y, vo, Clamping::ClampToGround );

                if ( flatten )
                {
                    (*anchors)[vertptr+0].set( x, y, vo, Clamping::ClampToAnchor );
                    (*anchors)[vertptr+4].set( x, y, vo, Clamping::ClampToAnchor );
                    (*anchors)[vertptr+5].set( x, y, vo, Clamping::ClampToAnchor );
                }
                else
                {                    
                    (*anchors)[vertptr+0].set( x, y, vo + f->left.height,  Clamping::ClampToGround );
                    (*anchors)[vertptr+4].set( x, y, vo + f->right.height, Clamping::ClampToGround );
                    (*anchors)[vertptr+5].set( x, y, vo + f->left.height,  Clamping::ClampToGround );
                }
            }

            // Assign wall polygon colors.
            if (useColor)
            {
                colors->push_back(shade(wallColor, normal_cache[0], shadeMin));
                colors->push_back(shade(wallBaseColor, normal_cache[1], shadeMin));
                colors->push_back(shade(wallBaseColor, normal_cache[2], shadeMin));
                colors->push_back(shade(wallBaseColor, normal_cache[3], shadeMin));
                colors->push_back(shade(wallColor, normal_cache[4], shadeMin));
                colors->push_back(shade(wallColor, normal_cache[5], shadeMin));
            }

            // Calculate texture coordinates:
            if (wallSkin)
            {
                // Calculate left and right corner V coordinates:
                double hL = tex_repeats_y ? (f->left.roof - f->left.base).length()   : elev->texHeightAdjustedM;
                double hR = tex_repeats_y ? (f->right.roof - f->right.base).length() : elev->texHeightAdjustedM;
                
                // Calculate the texture coordinates at each corner. The structure builder
                // will have spaced the verts correctly for this to work.
                float uL = fmod( f->left.offsetX, texWidthM ) / texWidthM;
                float uR = fmod( f->right.offsetX, texWidthM ) / texWidthM;

                // Correct for the case in which the rightmost corner is exactly on a
                // texture boundary.
                if ( uR < uL || (uL == 0.0 && uR == 0.0))
                    uR = 1.0f;

                osg::Vec2f texBaseL( uL, 0.0f );
                osg::Vec2f texBaseR( uR, 0.0f );
                osg::Vec2f texRoofL( uL, hL/elev->texHeightAdjustedM );
                osg::Vec2f texRoofR( uR, hR/elev->texHeightAdjustedM );

                texRoofL = bias + osg::componentMultiply(texRoofL, scale);
                texRoofR = bias + osg::componentMultiply(texRoofR, scale);
                texBaseL = bias + osg::componentMultiply(texBaseL, scale);
                texBaseR = bias + osg::componentMultiply(texBaseR, scale);

                tex->push_back({ texRoofL.x(), texRoofL.y(), layer });
                tex->push_back({ texBaseL.x(), texBaseL.y(), layer });
                tex->push_back({ texBaseR.x(), texBaseR.y(), layer });
                tex->push_back({ texBaseR.x(), texBaseR.y(), layer });
                tex->push_back({ texRoofR.x(), texRoofR.y(), layer });
                tex->push_back({ texRoofL.x(), texRoofL.y(), layer });
            }

            for(int i=0; i<6; ++i)
            {
                de->addElement( vertptr+i );
            }
        }
    }

    if (index)
    {
        index->tagRange(walls, feature, startVertPtr, vertptr - startVertPtr);
    }

    return madeGeom;
}

bool
ExtrudeGeometryFilter::buildRoofGeometry(const Structure&     structure,
                                         Feature* feature,
                                         osg::Geometry*       roof,                                         
                                         const osg::Vec4&     roofColor,
                                         const SkinResource*  roofSkin,
                                         FeatureIndexBuilder* index)
{    
    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(roof->getVertexArray());
    if (!verts)
    {
        verts = new osg::Vec3Array();
        roof->setVertexArray(verts);
    }
    osg::Vec4Array* color = static_cast<osg::Vec4Array*>(roof->getColorArray());
    if (!color)
    {
        color = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
        roof->setColorArray(color);
    }

    osg::Vec3Array* tex = 0L;
    if ( roofSkin )
    {
        tex = static_cast<osg::Vec3Array*>(roof->getTexCoordArray(0));
        if (!tex)
        {
            tex = new osg::Vec3Array();
            roof->setTexCoordArray(0, tex);
        }
    }

    ObjectIDArray* ids = nullptr;
    if (index)
    {
        ids = static_cast<ObjectIDArray*>(roof->getVertexAttribArray(osg::Drawable::SECONDARY_COLORS));
        if (!ids)
        {
            ids = new ObjectIDArray();
            ids->setBinding(osg::Array::BIND_PER_VERTEX);
            ids->setNormalize(false);
            roof->setVertexAttribArray(osg::Drawable::SECONDARY_COLORS, ids);
            ids->setPreserveDataType(true);
        }
    }

    osg::Vec4Array* anchors = 0L;    
    if ( _gpuClamping )
    {
        anchors = static_cast<osg::Vec4Array*>(roof->getVertexAttribArray(Clamping::AnchorAttrLocation));
        if (!anchors)
        {
            anchors = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
            anchors->setNormalize(false);
            roof->setVertexAttribArray(Clamping::AnchorAttrLocation, anchors);
        }
    }

    osg::Vec3Array* normal = static_cast<osg::Vec3Array*>(roof->getNormalArray());
    if (!normal)
    {
        normal = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX);
        roof->setNormalArray(normal);
    }

    bool flatten =
        _style.has<ExtrusionSymbol>() &&
        _style.get<ExtrusionSymbol>()->flatten() == true;


    osg::ref_ptr< osg::Geometry > tempGeom = new osg::Geometry;
    osg::Vec3Array* tempVerts = new osg::Vec3Array;
    tempGeom->setVertexArray(tempVerts);

    // Create a series of line loops that the tessellator can reorganize
    // into polygons.
    unsigned int vertptr = 0;// verts->size();
    unsigned int startVertPtr = verts->size();

    for(auto& elev : structure.elevations)
    {
        unsigned elevptr = vertptr;

        for(auto& face : elev.faces)
        {
            // Only use source verts; we skip interim verts inserted by the 
            // structure building since they are co-linear anyway and thus we don't
            // need them for the roof line.
            if ( face.left.isFromSource )
            {
                verts->push_back(face.left.roof);
                tempVerts->push_back(face.left.roof);
                color->push_back( roofColor );
                normal->push_back(osg::Vec3(0, 0, 1));

                if ( tex )
                {
                    tex->push_back( osg::Vec3f(face.left.roofTexU, face.left.roofTexV, (float)0.0f) );
                }

                if ( anchors )
                {
                    float 
                        x = structure.baseCentroid.x(),
                        y = structure.baseCentroid.y(), 
                        vo = structure.verticalOffset;

                    if ( flatten )
                    {
                        anchors->push_back( osg::Vec4f(x, y, vo, Clamping::ClampToAnchor) );
                    }
                    else
                    {
                        anchors->push_back( osg::Vec4f(x, y, vo + face.left.height, Clamping::ClampToGround) );
                    }
                }
                ++vertptr;
            }
        }
        tempGeom->addPrimitiveSet( new osg::DrawArrays(GL_LINE_LOOP, elevptr, vertptr-elevptr) );
    } 

    // Tessellate the roof lines into polygons.
    osgEarth::Tessellator oeTess;
    if (!oeTess.tessellateGeometry(*tempGeom))
    {
        // fallback to osg tessellator
        osgUtil::Tessellator tess;
        tess.setTessellationType( osgUtil::Tessellator::TESS_TYPE_GEOMETRY );
        tess.setWindingType( osgUtil::Tessellator::TESS_WINDING_ODD );
        tess.retessellatePolygons( *tempGeom);
    }

    // Get or create the primitive set
    osg::DrawElementsUInt* de = nullptr;
    if (roof->getNumPrimitiveSets() == 0)
    {
        de = new osg::DrawElementsUInt(GL_TRIANGLES);
        roof->addPrimitiveSet(de);
    }
    else
    {
        de = static_cast<osg::DrawElementsUInt*>(roof->getPrimitiveSet(0));
    }

    auto deptr = de->size();

    // Add the tesselated polygon to the main DrawElements, offseting the indices since the tesselation is going to 
    // return values based a zero index.  This might be something we need to address later.
    for (unsigned int i = 0; i < tempGeom->getNumPrimitiveSets(); ++i)
    {
        osg::DrawElementsUInt* p = static_cast<osg::DrawElementsUInt*>(tempGeom->getPrimitiveSet(i));
        if (p)
        {
            for (unsigned int j = 0; j < p->size(); ++j)
            {
                de->addElement(p->at(j) + startVertPtr);
            }
        }        
    }

    // inverted? flip the triangles and the normals.
    if (structure.isInverted)
    {
        for(unsigned i= deptr; i<de->size(); i+=3)
        {
            std::swap((*de)[i], (*de)[i+2]);
        }

        for(unsigned i= startVertPtr; i<normal->size(); ++i)
        {
            (*normal)[i] = -(*normal)[i];
        }
    }

    if (index)
    {
        unsigned count = vertptr;
        ids->resize(ids->size() + count);
        index->tagRange(roof, feature, startVertPtr, count);
    }

    return true;
}

osg::Drawable*
ExtrudeGeometryFilter::buildOutlineGeometry(const Structure& structure)
{
    // minimum angle between adjacent faces for which to draw a post.
    const float cosMinAngle = cos(osg::DegreesToRadians(_outlineSymbol->creaseAngle().get()));

    osg::ref_ptr<LineDrawable> lines = new LineDrawable(GL_LINES);
    
    const optional<Stroke>& stroke = _outlineSymbol->stroke();
    if (stroke.isSet())
    {
        lines->setColor(stroke->color());

        Distance lineWidth = stroke->width()->literal();
        lines->setLineWidth(lineWidth.as(Units::PIXELS));

        if (stroke->stipplePattern().isSet())
            lines->setStipplePattern(stroke->stipplePattern().get());

        if (stroke->stippleFactor().isSet())
            lines->setStippleFactor(stroke->stippleFactor().get());
    }

    osg::Vec4Array* anchors = 0L;
    if ( _gpuClamping )
    {
        anchors = new osg::Vec4Array();
        anchors->setBinding(osg::Array::BIND_PER_VERTEX);
        lines->setVertexAttribArray( Clamping::AnchorAttrLocation, anchors );
    }

    bool flatten =
        _style.has<ExtrusionSymbol>() &&
        _style.get<ExtrusionSymbol>()->flatten() == true;
    
    float
        x  = structure.baseCentroid.x(),
        y  = structure.baseCentroid.y(),
        vo = structure.verticalOffset;

    for(Elevations::const_iterator e = structure.elevations.begin(); e != structure.elevations.end(); ++e)
    {
        osg::Vec3d prev_vec;

        for(Faces::const_iterator f = e->faces.begin(); f != e->faces.end(); ++f)
        {
            // Only use source verts for posts.
            bool drawPost     = f->left.isFromSource;
            bool drawCrossbar = true;

            osg::Vec3d this_vec = f->right.roof - f->left.roof;
            this_vec.normalize();

            if (f->left.isFromSource && f != e->faces.begin())
            {
                drawPost = (this_vec * prev_vec) < cosMinAngle;
            }

            if (drawPost)
            {
                lines->pushVertex(f->left.roof);
                lines->pushVertex(f->left.base);
                
                if (anchors)
                {
                    if (flatten) 
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                    }
                    else
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + f->left.height, Clamping::ClampToGround));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToGround));
                    }
                }
            }

            if (drawCrossbar)
            {
                lines->pushVertex(f->left.roof);
                lines->pushVertex(f->right.roof);
                
                if (anchors)
                {
                    if (flatten) 
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                    }
                    else
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + f->left.height, Clamping::ClampToGround));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + f->right.height, Clamping::ClampToGround));
                    }
                }
            }

            prev_vec = this_vec;
        }

        // Draw an end-post if this isn't a closed polygon.
        if ( !structure.isPolygon )
        {
            Faces::const_iterator last = e->faces.end()-1;

            lines->pushVertex(last->right.roof);

            if (anchors)
            {
                if (flatten)
                    lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                else
                    lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + last->right.height, Clamping::ClampToGround));
            }

            lines->pushVertex(last->right.base);

            if (anchors)
                lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToGround));
        }
    }

    // finalize the line set
    lines->dirty();

    return lines->empty() ? 0L : lines.release();
}

void
ExtrudeGeometryFilter::addDrawable(osg::Drawable*       drawable,
                                   osg::StateSet*       stateSet,
                                   const std::string&   name,
                                   Feature*             feature,
                                   FeatureIndexBuilder* index )
{
    // find the geode for the active stateset, creating a new one if necessary. NULL is a 
    // valid key as well.
    osg::Group* geode;
    
    if (dynamic_cast<LineDrawable*>(drawable))
    {
        geode = _lineGroups[stateSet].get();
        if (!geode)
        {
            geode = new LineGroup();
            if (stateSet)
            {
                geode->getOrCreateStateSet()->merge(*stateSet);
            }
            _lineGroups[stateSet] = geode;
        }
    }
    else
    {
        geode = _geodes[stateSet].get();
        if (!geode)
        {
            geode = new osg::Geode();
            geode->setStateSet(stateSet);
            _geodes[stateSet] = geode;
        }
    }

    geode->addChild( drawable );
}

bool
ExtrudeGeometryFilter::process( FeatureList& features, FilterContext& context )
{
    for( FeatureList::iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        // run a symbol script if present.
        if (_polySymbol.valid() && _polySymbol->script().isSet())
        {
            _polySymbol->script()->eval(input, context);
        }

        if (input->getGeometry() == 0L)
            continue;

        // run a symbol script if present.
        if ( _extrusionSymbol->script().isSet() )
        {
            _extrusionSymbol->script()->eval(input, context);
        }

        if (input->getGeometry() == 0L)
            continue;

        // iterator over the parts.
        GeometryIterator iter( input->getGeometry(), false );
        while( iter.hasMore() )
        {
            Geometry* part = iter.next();

            part->removeDuplicates();

            // calculate the extrusion height:
            float height = 10.0f;

            if (_heightCallback.valid())
            {
                height = _heightCallback->operator()(input, context);
            }
            else if (_heightExpr.isSet())
            {
                height = _heightExpr->eval(input, context).as(Units::METERS);
            }
            else if (_extrusionSymbol->height().isSet())
            {
                height = _extrusionSymbol->height()->eval(input, context).as(Units::METERS);
            }

            // Set up for feature naming and feature indexing:
            std::string name;
            if (_featureNameExpr)
                name = _featureNameExpr.eval(input, context);

            osg::ref_ptr<osg::StateSet> wallStateSet;
            osg::ref_ptr<osg::StateSet> roofStateSet;

            // calculate the wall texturing:
            osg::ref_ptr<SkinResource> wallSkin;

            if (_wallSkinSymbol.valid())
            {
                unsigned int wallRand = f->get()->getFID() + (_wallSkinSymbol.valid() ? *_wallSkinSymbol->randomSeed() : 0);

                SkinSymbol querySymbol(*_wallSkinSymbol.get());
                querySymbol.objectHeight() = fabs(height);
                wallSkin = querySymbol.getResource(_wallResLib.get(), wallRand, context.getDBOptions());
            }
            else if (_extrusionSymbol->wallSkinName().isSet())
            {
                SkinSymbol temp;
                temp.name() = _extrusionSymbol->wallSkinName().value();
                wallSkin = temp.getResource(_wallResLib.get(), 0, context.getDBOptions());
            }

            if (wallSkin)
            {
                context.resourceCache()->getOrCreateStateSet(wallSkin, wallStateSet, context.getDBOptions());
            }

            // calculate the rooftop texture:
            osg::ref_ptr<SkinResource> roofSkin;

            if (_roofSkinSymbol.valid())
            {
                unsigned int roofRand = f->get()->getFID() + (_roofSkinSymbol.valid() ? *_roofSkinSymbol->randomSeed() : 0);
                roofSkin = _roofSkinSymbol->getResource(_roofResLib.get(), roofRand, context.getDBOptions());
            }
            else if (_extrusionSymbol->roofSkinName().isSet())
            {
                SkinSymbol temp;
                temp.name() = _extrusionSymbol->roofSkinName().value();
                roofSkin = temp.getResource(_roofResLib.get(), 0, context.getDBOptions());
            }

            if (roofSkin)
            {
                context.resourceCache()->getOrCreateStateSet(roofSkin, roofStateSet, context.getDBOptions());
            }

            osg::ref_ptr<osg::Geometry> walls = _wallGeometries[wallStateSet.get()];
            if (!walls.valid())
            {
                walls = new osg::Geometry();
                walls->setName("Walls");
                walls->setUseVertexBufferObjects(true);
                _wallGeometries[wallStateSet.get()] = walls.get();
                addDrawable(walls.get(), wallStateSet.get(), name, input, context.featureIndex());                
            }

            osg::ref_ptr<osg::Geometry> rooflines = 0L;
            osg::ref_ptr<osg::Geometry> baselines = 0L;
            osg::ref_ptr<osg::Drawable> outlines  = 0L;

            if (part->getType() == Geometry::TYPE_POLYGON)
            {
                part->rewind(osgEarth::Geometry::ORIENTATION_CCW);

                rooflines = _roofGeometries[roofStateSet.get()];
                if (!rooflines.valid())
                {
                    rooflines = new osg::Geometry();
                    rooflines->setName("Roofs");
                    rooflines->setUseVertexBufferObjects(true);
                    _roofGeometries[roofStateSet.get()] = rooflines.get();
                    addDrawable(rooflines.get(), roofStateSet.get(), name, input, context.featureIndex());
                }
                    
                // prep the shapes by making sure all polys are open:
                static_cast<osgEarth::Polygon*>(part)->open();
            }

            // make a base cap if we're doing stencil volumes.
            if ( _makeStencilVolume )
            {
                baselines = _baselineGeometries[nullptr];
                if (!baselines.valid())
                {
                    baselines = new osg::Geometry();
                    baselines->setName(typeid(*this).name());
                    baselines->setUseVertexBufferObjects(true);
                    _baselineGeometries[nullptr] = baselines.get();
                    addDrawable(baselines.get(), 0L, name, input, context.featureIndex());
                }
            }

            float verticalOffset = (float)input->getDouble("__oe_verticalOffset", 0.0);

            // modify our values based on the directionality.
            double heightOffset = 0.0;
            if (height < 0.0 && _extrusionSymbol->direction() == ExtrusionSymbol::DIRECTION_UP)
            {
                heightOffset = height;
                height = -height;
            }
            else if (height > 0.0 && _extrusionSymbol->direction() == ExtrusionSymbol::DIRECTION_DOWN)
            {
                height = -height;
            }
            else if (height < 0.0 && _extrusionSymbol->direction() == ExtrusionSymbol::DIRECTION_DOWN)
            {
                heightOffset = -height;
            }

            // Build the data model for the structure.
            Structure structure;

            buildStructure(
                part,
                input->getSRS(),
                height,
                heightOffset,
                _extrusionSymbol->flatten().get(),
                verticalOffset,
                wallSkin,
                roofSkin,
                structure,
                context);

            // Create the walls.
            if ( walls.valid() )
            {
                osg::Vec4f wallColor(1,1,1,1), wallBaseColor(1,1,1,1);

                if ( _wallPolygonSymbol.valid() )
                {
                    wallColor = _wallPolygonSymbol->fill()->color();
                }

                if ( _extrusionSymbol->wallGradientPercentage().isSet() )
                {
                    wallBaseColor = Color(wallColor).brightness( 1.0 - *_extrusionSymbol->wallGradientPercentage() );
                }
                else
                {
                    wallBaseColor = wallColor;
                }

                float shadeMin = 1.0f;
                if (_extrusionSymbol->wallShadePercentage().isSet())
                {
                    shadeMin = 1.0f - _extrusionSymbol->wallShadePercentage().value();
                }

                buildWallGeometry(structure, input, walls.get(), wallColor, wallBaseColor, wallSkin, shadeMin, context.featureIndex());
            }

            // tessellate and add the roofs if necessary:
            if ( rooflines.valid() )
            {
                osg::Vec4f roofColor(1,1,1,1);
                if ( _roofPolygonSymbol.valid() )
                {
                    roofColor = _roofPolygonSymbol->fill()->color();
                }
                buildRoofGeometry(structure, input, rooflines.get(), roofColor, roofSkin, context.featureIndex());                
            }

            if (_outlineSymbol.valid())
            {
                outlines = buildOutlineGeometry(structure);
                addDrawable(outlines.get(), 0L, name, input, context.featureIndex());
            }

            if ( baselines.valid() )
            {
                osgUtil::Tessellator tess;
                tess.setTessellationType( osgUtil::Tessellator::TESS_TYPE_GEOMETRY );
                tess.setWindingType( osgUtil::Tessellator::TESS_WINDING_ODD );
                tess.retessellatePolygons( *(baselines.get()) );
            }        
        }
    }

    return true;
}


namespace
{
    struct Counter : public osg::NodeVisitor
    {
        unsigned _numVerts = 0u;
        unsigned _numElements = 0u;

        Counter()
        {
            // Use the "active chidren" mode to only bring in default switch
            // and osgSim::MultiSwitch children for now. -gw
            setTraversalMode(TRAVERSE_ACTIVE_CHILDREN);
            setNodeMaskOverride(~0);
        }

        void apply(osg::Geometry& node) override
        {
            auto verts = dynamic_cast<osg::Vec3Array*>(node.getVertexArray());
            if (verts)
            {
                _numVerts += verts->size();
            }

            for (unsigned i = 0; i < node.getNumPrimitiveSets(); ++i)
            {
                auto p = node.getPrimitiveSet(i);
                if (p)
                {
                    if (p->getMode() == p->TRIANGLES ||
                        p->getMode() == p->TRIANGLES_ADJACENCY ||
                        p->getMode() == p->TRIANGLE_FAN ||
                        p->getMode() == p->TRIANGLE_STRIP ||
                        p->getMode() == p->QUADS ||
                        p->getMode() == p->QUAD_STRIP)
                    {
                        _numElements += p->getNumIndices();
                    }
                }
            }
        }
    };
}

osg::Node*
ExtrudeGeometryFilter::push( FeatureList& input, FilterContext& context )
{
    reset( context );

    // minimally, we require an extrusion symbol.
    if ( !_extrusionSymbol.valid() )
    {
        OE_WARN << LC << "Missing required extrusion symbolology; geometry will be empty" << std::endl;
        return new osg::Group();
    }

    // establish the active resource library, if applicable.
    _wallResLib = 0L;
    _roofResLib = 0L;

    const StyleSheet* sheet = context.session() ? context.session()->styles() : 0L;

    if ( sheet != nullptr )
    {
        if ( _wallSkinSymbol.valid() && _wallSkinSymbol->library().isSet() )
        {
            _wallResLib = sheet->getResourceLibrary( *_wallSkinSymbol->library() );

            if ( !_wallResLib.valid() )
            {
                OE_WARN << LC << "Unable to load resource library '" << *_wallSkinSymbol->library() << "'"
                    << "; wall geometry will not be textured." << std::endl;
                _wallSkinSymbol = nullptr;
            }
        }
        else if (_extrusionSymbol->library().isSet())
        {
            _wallResLib = sheet->getResourceLibrary(*_extrusionSymbol->library());

            if (!_wallResLib.valid())
            {
                OE_WARN << LC << "Unable to load resource library '" << *_extrusionSymbol->library() << "'"
                    << "; wall geometry will not be textured." << std::endl;
                _wallSkinSymbol = nullptr;
            }
        }

        if ( _roofSkinSymbol.valid() && _roofSkinSymbol->library().isSet() )
        {
            _roofResLib = sheet->getResourceLibrary( *_roofSkinSymbol->library() );
            if ( !_roofResLib.valid() )
            {
                OE_WARN << LC << "Unable to load resource library '" << *_roofSkinSymbol->library() << "'"
                    << "; roof geometry will not be textured." << std::endl;
                _roofSkinSymbol = nullptr;
            }
        }
        else if (_extrusionSymbol->library().isSet())
        {
            _roofResLib = sheet->getResourceLibrary(*_extrusionSymbol->library());
            if (!_roofResLib.valid())
            {
                OE_WARN << LC << "Unable to load resource library '" << *_roofSkinSymbol->library() << "'"
                    << "; roof geometry will not be textured." << std::endl;
                _roofSkinSymbol = nullptr;
            }
        }
    }

    // calculate the localization matrices (_local2world and _world2local)
    computeLocalizers( context );

    // push all the features through the extruder.
    bool ok = process( input, context );

    // parent geometry with a delocalizer (if necessary)
    osg::Group* group = createDelocalizeGroup();
    group->setName("ExtrudeGeometryFilter::Delocalizer");

    unsigned int numDrawables = 0;

#if 0
    osg::Group* oqn  = 0;
    if (osgEarth::OcclusionQueryNodeFactory::_occlusionFactory) {
       oqn = osgEarth::OcclusionQueryNodeFactory::_occlusionFactory->createQueryNode();
       group->addChild(oqn);
    }
    if (!oqn) {
       oqn = group;
    }
#endif
    
    for( SortedGeodeMap::iterator i = _geodes.begin(); i != _geodes.end(); ++i )
    {
        group->addChild( i->second.get() );
        numDrawables += i->second->getNumChildren();
    }
    _geodes.clear();

    for (SortedGeodeMap::iterator i = _lineGroups.begin(); i != _lineGroups.end(); ++i)
    {
        group->addChild(i->second.get());
    }
    _lineGroups.clear();

    // Prepare buffer objects.
    AllocateAndMergeBufferObjectsVisitor allocAndMerge;
    group->accept( allocAndMerge );

    // set a uniform indicating that clamping attributes are available.
    Clamping::installHasAttrsUniform( group->getOrCreateStateSet() );

    // if we drew outlines, apply a poly offset too.
    if ( _outlineSymbol.valid() )
    {
        osg::StateSet* groupStateSet = group->getOrCreateStateSet();
        groupStateSet->setAttributeAndModes( new osg::PolygonOffset(1,1), 1 );
#ifdef OSG_GL_FIXED_FUNCTION_AVAILABLE
        if (_outlineSymbol->stroke()->width().isSet())
            groupStateSet->setAttributeAndModes(new osg::LineWidth(_outlineSymbol->stroke()->width()->literal().getValue()), 1);
#endif
    }

    return group;
}



REGISTER_OBJECT_WRAPPER(ExtrudeGeometryFilterNode,
    new ExtrudeGeometryFilterNode,
    osgEarth::ExtrudeGeometryFilterNode,
    "osg::Node osgEarth::ExtrudeGeometryFilterNode")
{
    ADD_OBJECT_SERIALIZER(_extrusionGroup, osg::Group, nullptr);
    ADD_MATRIX_SERIALIZER(_xform, osg::Matrixd::identity());
}