/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "AltitudeFilter"
#include "GeoData"

#define LC "[AltitudeFilter] "

using namespace osgEarth;
using namespace osgEarth::Util;

//---------------------------------------------------------------------------

void
AltitudeFilter::setPropertiesFromStyle(const Style& style)
{
    _altitude = (AltitudeSymbol*)style.get<const AltitudeSymbol>();
    if ( _altitude )
    {
        setMaxResolution( *_altitude->clampingResolution() );
    }
}

AltitudeSymbol*
AltitudeFilter::getOrCreateSymbol()
{
    if (!_altitude)
    {
        _altitude = new AltitudeSymbol();
    }
    return _altitude.get();
}

FilterContext
AltitudeFilter::push( FeatureList& features, FilterContext& cx )
{
    bool clampToMap = 
        _altitude.valid()                                          && 
        _altitude->clamping()  != AltitudeSymbol::CLAMP_NONE       &&
        _altitude->technique() == AltitudeSymbol::TECHNIQUE_MAP    &&
        cx.session()        != 0L                               &&
        cx.featureProfile()    != 0L;

    if ( clampToMap )
        pushAndClamp( features, cx );
    else
        pushAndDontClamp( features, cx );

    return cx;
}

void
AltitudeFilter::pushAndDontClamp( FeatureList& features, FilterContext& cx )
{
    bool gpuClamping =
        _altitude.valid() &&
        _altitude->technique() == _altitude->TECHNIQUE_GPU;

    bool ignoreZ =
        gpuClamping && 
        _altitude->clamping() == _altitude->CLAMP_TO_TERRAIN;

    for(auto& feature : features)
    {
        if (!feature)
            continue;
        
        // run a symbol script if present.
        if ( _altitude.valid() && _altitude->script().isSet() )
        {
            _altitude->script()->eval(feature, cx);
        }
        if (feature->getGeometry() == 0L)
            continue;

        double minHAT       =  DBL_MAX;
        double maxHAT       = -DBL_MAX;

        double scaleZ = 1.0;
        if (_altitude.valid() && _altitude->verticalScale().isSet())
            scaleZ = _altitude->verticalScale()->eval(feature, cx);

        double offsetZ = 0.0;
        if (_altitude.valid() && _altitude->verticalOffset().isSet())
            offsetZ = _altitude->verticalOffset()->eval(feature, cx).as(Units::METERS);
        
        GeometryIterator gi( feature->getGeometry() );
        while( gi.hasMore() )
        {
            Geometry* geom = gi.next();
            for( Geometry::iterator g = geom->begin(); g != geom->end(); ++g )
            {
                if ( ignoreZ )
                {
                    g->z() = 0.0;
                }

                if ( !gpuClamping )
                {
                    g->z() *= scaleZ;
                    g->z() += offsetZ;
                }

                if ( g->z() < minHAT )
                    minHAT = g->z();
                if ( g->z() > maxHAT )
                    maxHAT = g->z();
            }
        }

        if ( minHAT != DBL_MAX )
        {
            feature->set( "__min_hat", minHAT );
            feature->set( "__max_hat", maxHAT );
        }

        // encode the Z offset if
        if ( gpuClamping )
        {
            feature->set("__oe_verticalScale",  scaleZ);
            feature->set("__oe_verticalOffset", offsetZ);
        }
    }
}

void
AltitudeFilter::pushAndClamp(FeatureList& features, FilterContext& cx)
{
    unsigned total = 0;

    const Session* session = cx.session();

    if (features.empty())
        return;

    // the map against which we'll be doing elevation clamping
    osg::ref_ptr<const Map> map = session->getMap();
    if (!map.valid())
        return;

    const SpatialReference* mapSRS = map->getSRS();
    osg::ref_ptr<const SpatialReference> featureSRS = cx.featureProfile()->getSRS();

    // whether to record the min/max height-above-terrain values.
    bool collectHATs =
        _altitude->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN ||
        _altitude->clamping() == AltitudeSymbol::CLAMP_ABSOLUTE;

    // how to clamp:
    bool perVertex =
        _altitude->binding() == AltitudeSymbol::BINDING_VERTEX;

    bool perCentroid = 
        _altitude->binding() == AltitudeSymbol::BINDING_CENTROID;

    bool perEndpoint = 
        _altitude->binding() == AltitudeSymbol::BINDING_ENDPOINT;

    // whether the SRS's have a compatible vertical datum.
    bool vertEquiv =
        featureSRS->isVertEquivalentTo( mapSRS );

    ElevationPool::Envelope envelope;

    map->getElevationPool()->prepareEnvelope(
        envelope,
        features.begin()->get()->getExtent().getCentroid(),
        _maxResolution);

    for(auto& feature : features)
    {        
        // run a symbol script if present.
        if ( _altitude.valid() && _altitude->script().isSet() )
        {
            _altitude->script()->eval(feature, cx);
        }
        if (feature->getGeometry() == 0L)
            continue;

        double maxTerrainZ  = -DBL_MAX;
        double minTerrainZ  =  DBL_MAX;
        double minHAT       =  DBL_MAX;
        double maxHAT       = -DBL_MAX;

        double scaleZ = 1.0;
        if (_altitude.valid() && _altitude->verticalScale().isSet())
            scaleZ = _altitude->verticalScale()->eval(feature, cx);

        double offsetZ = 0.0;
        if ( _altitude.valid() && _altitude->verticalOffset().isSet() )
            offsetZ = _altitude->verticalOffset()->eval(feature, cx).as(Units::METERS);

        osgEarth::Bounds bounds = feature->getGeometry()->getBounds();
        auto center = bounds.center();
        GeoPoint centroid(featureSRS.get(), center.x(), center.y());
        double   centroidElevation = 0.0;

        // If we aren't doing per vertex clamping go ahead and get the centroid.
        // We do this now instead of within the geometry iterator to ensure that multipolygons
        // are clamped to the whole multipolygon and not per polygon.
        if (perCentroid)
        {
            std::vector<osg::Vec3d> temp(1);
            temp[0].set(centroid.x(), centroid.y(), 0);
            featureSRS->transform(temp, mapSRS);
            envelope.sampleMapCoords(temp.begin(), temp.end(), nullptr);
            centroid.z() = temp[0].z();
            centroidElevation = centroid.z();

            // Check for NO_DATA_VALUE and use zero instead.
            if (centroidElevation == NO_DATA_VALUE)
            {
                centroidElevation = 0.0;
            }
        }

        std::vector<osg::Vec3d> workspace;
        
        GeometryIterator gi( feature->getGeometry() );
        while( gi.hasMore() )
        {
            Geometry* geom = gi.next();

            total += geom->size();

            // Absolute heights in Z. Only need to collect the HATs; the geometry
            // remains unchanged.
            if ( _altitude->clamping() == AltitudeSymbol::CLAMP_ABSOLUTE )
            {
                if ( perVertex )
                {
                    // prep our workspace and transform the features to the map's SRS.
                    workspace.resize(geom->size());
                    std::copy(geom->asVector().begin(), geom->asVector().end(), workspace.begin());
                    featureSRS->transform(workspace, mapSRS);

                    // sample the Z values.
                    envelope.sampleMapCoords(workspace.begin(), workspace.end(), nullptr);

                    for (unsigned i = 0; i < geom->size(); ++i)
                    {
                        if (workspace[i].z() != NO_DATA_VALUE)
                        {
                            osg::Vec3d& p = (*geom)[i];

                            p.z() *= scaleZ;
                            p.z() += offsetZ;

                            double z = p.z();

                            if (!vertEquiv)
                            {
                                osg::Vec3d tempgeo;
                                if (!featureSRS->transform(p, mapSRS->getGeographicSRS(), tempgeo))
                                    z = tempgeo.z();
                            }

                            double hat = z - workspace[i].z();

                            if (hat > maxHAT)
                                maxHAT = hat;
                            if (hat < minHAT)
                                minHAT = hat;

                            if (workspace[i].z() > maxTerrainZ)
                                maxTerrainZ = workspace[i].z();
                            if (workspace[i].z() < minTerrainZ)
                                minTerrainZ = workspace[i].z();
                        }
                    }
                }
                else // per centroid
                {
                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];
                        p.z() *= scaleZ;
                        p.z() += offsetZ;

                        double z = p.z();
                        if ( !vertEquiv )
                        {
                            osg::Vec3d tempgeo;
                            if ( !featureSRS->transform(p, mapSRS->getGeographicSRS(), tempgeo) )
                                z = tempgeo.z();
                        }

                        double hat = z - centroidElevation;

                        if ( hat > maxHAT )
                            maxHAT = hat;
                        if ( hat < minHAT )
                            minHAT = hat;
                    }

                    if ( centroidElevation > maxTerrainZ )
                        maxTerrainZ = centroidElevation;
                    if ( centroidElevation < minTerrainZ )
                        minTerrainZ = centroidElevation;
                }
            }

            // Heights-above-ground in Z. Need to resolve this to an absolute number
            // and record HATs along the way.
            else if ( _altitude->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN )
            {
                osg::ref_ptr<const SpatialReference> featureSRSwithMapVertDatum = !vertEquiv ?
                    SpatialReference::create(featureSRS->getHorizInitString(), mapSRS->getVertInitString()) : 0L;

                if ( perVertex )
                {
                    std::vector<osg::Vec3d> workspace;
                    workspace.resize(geom->size());
                    std::copy(geom->asVector().begin(), geom->asVector().end(), workspace.begin());
                    featureSRS->transform(workspace, mapSRS);

                    envelope.sampleMapCoords(workspace.begin(), workspace.end(), nullptr);
                    
                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];

                        if (workspace[i].z() != NO_DATA_VALUE)
                        {
                            p.z() *= scaleZ;
                            p.z() += offsetZ;

                            double hat = p.z();
                            p.z() = workspace[i].z() + p.z();

                            // if necessary, convert the Z value (which is now in the map's SRS) back to
                            // the feature's SRS.
                            if ( !vertEquiv )
                            {
                                featureSRSwithMapVertDatum->transform(p, featureSRS.get(), p);
                            }

                            if ( hat > maxHAT )
                                maxHAT = hat;
                            if ( hat < minHAT )
                                minHAT = hat;

                            if (workspace[i].z() > maxTerrainZ )
                                maxTerrainZ = workspace[i].z();
                            if (workspace[i].z() < minTerrainZ )
                                minTerrainZ = workspace[i].z();
                        }
                    }
                }
                else // per-centroid
                {
                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];
                        p.z() *= scaleZ;
                        p.z() += offsetZ;

                        double hat = p.z();
                        p.z() = centroidElevation + p.z();

                        // if necessary, convert the Z value (which is now in the map's SRS) back to
                        // the feature's SRS.
                        if ( !vertEquiv )
                        {
                            featureSRSwithMapVertDatum->transform(p, featureSRS.get(), p);
                        }

                        if ( hat > maxHAT )
                            maxHAT = hat;
                        if ( hat < minHAT )
                            minHAT = hat;
                    }

                    if ( centroidElevation > maxTerrainZ )
                        maxTerrainZ = centroidElevation;
                    if ( centroidElevation < minTerrainZ )
                        minTerrainZ = centroidElevation;
                }
            }

            // Clamp - replace the geometry's Z with the terrain height.
            else // CLAMP_TO_TERRAIN
            {
                if ( perVertex )
                {
                    bool xform = !featureSRS->isHorizEquivalentTo(map->getSRS());

                    std::vector<osg::Vec3d>* points = &geom->asVector();
                    std::vector<osg::Vec3d> transformed;

                    if (xform)
                    {
                        transformed = *points;
                        featureSRS->transform(transformed, map->getSRS());
                        points = &transformed;
                    }

                    envelope.sampleMapCoords(points->begin(), points->end(), nullptr);

                    // replace no-data with zero?
                    for (auto& point : *points)
                        if (point.z() == NO_DATA_VALUE)
                            point.z() = 0.0f;

                    if (xform)
                    {
                        for (int i = 0; i < points->size(); ++i)
                            geom->asVector()[i].z() = (*points)[i].z();
                    }
                    
                    // if necessary, transform the Z values (which are now in the map SRS) back
                    // into the feature's SRS.
                    if ( !vertEquiv )
                    {
                        auto* featureSRSwithMapVertDatum =
                            SpatialReference::create(featureSRS->getHorizInitString(), mapSRS->getVertInitString());

                        osg::Vec3d tempgeo;
                        for( unsigned i=0; i<geom->size(); ++i )
                        {
                            osg::Vec3d& p = (*geom)[i];
                            featureSRSwithMapVertDatum->transform(p, featureSRS.get(), p);
                        }
                    }
                }

                else if (perEndpoint)
                {
                    auto* featureSRSwithMapVertDatum = !vertEquiv ?
                        SpatialReference::create(featureSRS->getHorizInitString(), mapSRS->getVertInitString()) : nullptr;

                    // clamp the front and back points:
                    std::vector<osg::Vec3d> endpoints { geom->front(), geom->back() };

                    bool xform = !featureSRS->isHorizEquivalentTo(map->getSRS());
                    if (xform)
                    {
                        featureSRS->transform(endpoints, map->getSRS());
                    }
                    envelope.sampleMapCoords(endpoints.begin(), endpoints.end(), nullptr);

                    geom->front().z() = endpoints[0].z();
                    geom->back().z() = endpoints[1].z();

                    // interpolate all the Z values in between
                    for( unsigned i=1; i<geom->size()-1; ++i )
                    {
                        auto& p = (*geom)[i];
                        double t = (double)i / (double)(geom->size() - 1);
                        p.z() = mix(endpoints[0], endpoints[1], t).z();

                        // if necessary, convert the Z value (which is now in the map's SRS) back to
                        // the feature's SRS.
                        if (!vertEquiv)
                        {
                            featureSRSwithMapVertDatum->transform(p, featureSRS.get(), p);
                        }
                    }
                }

                else if (perCentroid)
                {
                    osg::ref_ptr<const SpatialReference> featureSRSWithMapVertDatum;
                    if ( !vertEquiv )
                        featureSRSWithMapVertDatum = SpatialReference::create(featureSRS->getHorizInitString(), mapSRS->getVertInitString());

                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];
                        p.z() = centroidElevation;
                        if ( !vertEquiv )
                        {
                            featureSRSWithMapVertDatum->transform(p, featureSRS.get(), p);
                        }
                    }
                }
            }

            if ( !collectHATs )
            {
                for( Geometry::iterator i = geom->begin(); i != geom->end(); ++i )
                {
                    i->z() *= scaleZ;
                    i->z() += offsetZ;
                }
            }
        }

        if ( minHAT != DBL_MAX )
        {
            feature->set( "__min_hat", minHAT );
            feature->set( "__max_hat", maxHAT );
        }

        if ( minTerrainZ != DBL_MAX )
        {
            feature->set( "__min_terrain_z", minTerrainZ );
            feature->set( "__max_terrain_z", maxTerrainZ );
        }
    }
}
