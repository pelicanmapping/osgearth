/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include "OgrUtils"
#include "Feature"

#define LC "[FeatureSource] "

using namespace osgEarth;

#ifndef GDAL_VERSION_AT_LEAST
#define GDAL_VERSION_AT_LEAST(MAJOR, MINOR, REV) ((GDAL_VERSION_MAJOR>MAJOR) || (GDAL_VERSION_MAJOR==MAJOR && (GDAL_VERSION_MINOR>MINOR || (GDAL_VERSION_MINOR==MINOR && GDAL_VERSION_REV>=REV))))
#endif

namespace
{
    int IsFieldSet(OGRFeatureH handle, int i)
    {
        // https://github.com/Toblerity/Fiona/issues/460
        // GDAL 2.2 changed the behavior of OGR_F_IsFieldSet so that null fields will still be considered set.
        // We consider unset or null fields to be the same, so we use OGR_F_IsFieldSetAndNotNull
    #if GDAL_VERSION_AT_LEAST(2,2,0)
        return OGR_F_IsFieldSetAndNotNull(handle, i);
    #else
        return OGR_F_IsFieldSet(handle, i);
    #endif
    }
}

void
OgrUtils::populate( OGRGeometryH geomHandle, Geometry* target, int numPoints )
{
    for (int v = 0; v < numPoints; ++v)
    {
        double x=0, y=0, z=0;
        OGR_G_GetPoint( geomHandle, v, &x, &y, &z );
        osg::Vec3d p( x, y, z );
        if ( target->size() == 0 || p != target->back() ) // remove dupes
            target->push_back( p );
    }
}

MultiGeometry*
OgrUtils::createTIN(OGRGeometryH geomHandle)
{
    MultiGeometry *multi = new MultiGeometry;

    int numParts = OGR_G_GetGeometryCount(geomHandle);
    if (numParts > 0)
    {
        for (int p = 0; p < numParts; p++)
        {
            OGRGeometryH partRef = OGR_G_GetGeometryRef(geomHandle, p);

            OGRwkbGeometryType partWkbType = OGR_G_GetGeometryType(partRef);

            unsigned int numSubParts = OGR_G_GetGeometryCount(partRef);
            OGRGeometryH subPartRef = OGR_G_GetGeometryRef(partRef, 0);
            unsigned int numSubPoints = OGR_G_GetPointCount(subPartRef);
            osgEarth::Polygon *output = new osgEarth::Polygon(numSubPoints);
            populate(subPartRef, output, numSubPoints);
            output->open();

            // Rewind the triangle so it's oriented correctly
            std::reverse(output->begin(), output->end());

            multi->add(output);
        }
    }

    return multi;
}

osgEarth::Polygon*
OgrUtils::createPolygon( OGRGeometryH geomHandle, bool rewindPolygons)
{
    osgEarth::Polygon* output = 0L;

    int numParts = OGR_G_GetGeometryCount( geomHandle );
    if ( numParts == 0 )
    {
        int numPoints = OGR_G_GetPointCount( geomHandle );
        output = new osgEarth::Polygon( numPoints );
        populate( geomHandle, output, numPoints );

        if (rewindPolygons)
        {
            output->open();
            output->rewind(Ring::ORIENTATION_CCW);
        }
    }
    else if ( numParts > 0 )
    {
        for( int p = 0; p < numParts; p++ )
        {
            OGRGeometryH partRef = OGR_G_GetGeometryRef( geomHandle, p );
            int numPoints = OGR_G_GetPointCount( partRef );

            if ( p == 0 )
            {
                output = new osgEarth::Polygon( numPoints );
                populate( partRef, output, numPoints );
                if (rewindPolygons)
                {
                    output->open();
                    output->rewind(Ring::ORIENTATION_CCW);
                }
            }
            else
            {
                Ring* hole = new Ring( numPoints );
                populate( partRef, hole, numPoints );
                if (rewindPolygons)
                {
                    hole->open();
                    hole->rewind(Ring::ORIENTATION_CW );
                }
                output->getHoles().push_back( hole );
            }
        }
    }
    return output;
}

Geometry*
OgrUtils::createGeometry( OGRGeometryH geomHandle, bool rewindPolygons)
{
    Geometry* output = 0L;

    OGRwkbGeometryType wkbType = OGR_G_GetGeometryType( geomHandle );

    int numPoints, numGeoms;

    switch (wkbType)
    {
    case wkbPolygon:
    case wkbPolygon25D:
    case wkbPolygonM:
    case wkbPolygonZM:
        output = createPolygon(geomHandle, rewindPolygons);
        break;

    case wkbLineString:
    case wkbLineString25D:
    case wkbLineStringM:
    case wkbLineStringZM:
        numPoints = OGR_G_GetPointCount( geomHandle );
        output = new LineString( numPoints );
        populate( geomHandle, output, numPoints );
        break;

    case wkbLinearRing:
        numPoints = OGR_G_GetPointCount( geomHandle );
        output = new Ring( numPoints );
        populate( geomHandle, output, numPoints );
        break;

    case wkbPoint:
    case wkbPoint25D:
    case wkbPointM:
    case wkbPointZM:
        numPoints = OGR_G_GetPointCount( geomHandle );
        output = new Point( numPoints );
        populate( geomHandle, output, numPoints );
        break;

    case wkbMultiPoint:
    case wkbMultiPoint25D:
    case wkbMultiPointM:
    case wkbMultiPointZM:
        numGeoms = OGR_G_GetGeometryCount(geomHandle);
        output = new PointSet();
        for (int n = 0; n < numGeoms; n++)
        {
            OGRGeometryH subGeomRef = OGR_G_GetGeometryRef(geomHandle, n);
            if (subGeomRef)
            {
                numPoints = OGR_G_GetPointCount(subGeomRef);
                populate(subGeomRef, output, numPoints);
            }
        }
        break;

    case wkbTINZ:
    case wkbTIN:
    case wkbTINM:
    case wkbTINZM:
        output = createTIN(geomHandle);
        break;

    case wkbGeometryCollection:
    case wkbGeometryCollection25D:
    case wkbMultiLineString:
    case wkbMultiLineString25D:
    case wkbMultiPolygon:
    case wkbMultiPolygon25D:
    case wkbGeometryCollectionM:
    case wkbGeometryCollectionZM:
    case wkbMultiLineStringM:
    case wkbMultiLineStringZM:
    case wkbMultiPolygonM:
    case wkbMultiPolygonZM:
        MultiGeometry* multi = new MultiGeometry();
        numGeoms = OGR_G_GetGeometryCount( geomHandle );
        for( int n=0; n<numGeoms; n++ )
        {
            OGRGeometryH subGeomRef = OGR_G_GetGeometryRef( geomHandle, n );
            if ( subGeomRef )
            {
                Geometry* geom = createGeometry( subGeomRef, rewindPolygons);
                if ( geom ) multi->getComponents().push_back( geom );
            }
        }
        output = multi;
        break;
    }

    return output;
}

OGRwkbGeometryType
OgrUtils::getOGRGeometryType(const osgEarth::Geometry::Type& geomType)
{
    OGRwkbGeometryType requestedType = wkbUnknown;

    switch (geomType)
    {
    case osgEarth::Geometry::TYPE_POLYGON:
        requestedType = wkbPolygon;
        break;
    case osgEarth::Geometry::TYPE_POINT:
        requestedType = wkbPoint;
        break;
    case osgEarth::Geometry::TYPE_POINTSET:
        requestedType = wkbMultiPoint;
        break;
    case osgEarth::Geometry::TYPE_LINESTRING:
        requestedType = wkbLineString;
        break;
    case osgEarth::Geometry::TYPE_RING:
        requestedType = wkbLinearRing;
        break;
    default:
    case Geometry::TYPE_UNKNOWN:
        break;
    }

    return requestedType;
}

OGRwkbGeometryType
OgrUtils::getOGRGeometryType(const osgEarth::Geometry* geometry)
{
    OGRwkbGeometryType requestedType = wkbUnknown;

    switch (geometry->getType())
    {
    case osgEarth::Geometry::TYPE_POLYGON:
        requestedType = wkbPolygon;
        break;
    case osgEarth::Geometry::TYPE_POINT:
        requestedType = wkbPoint;
        break;
    case osgEarth::Geometry::TYPE_POINTSET:
        requestedType = wkbMultiPoint;
        break;
    case osgEarth::Geometry::TYPE_LINESTRING:
        requestedType = wkbLineString;
        break;
    case osgEarth::Geometry::TYPE_RING:
        requestedType = wkbLinearRing;
        break;
    case Geometry::TYPE_UNKNOWN:
        break;
    case Geometry::TYPE_MULTI:
    {
        const osgEarth::MultiGeometry* multi = dynamic_cast<const MultiGeometry*>(geometry);
        if (multi)
        {
            osgEarth::Geometry::Type componentType = multi->getComponentType();
            requestedType = componentType == Geometry::TYPE_POLYGON ? wkbMultiPolygon :
                componentType == Geometry::TYPE_POINT? wkbMultiPoint :
                componentType == Geometry::TYPE_POINTSET ? wkbMultiPoint :
                componentType == Geometry::TYPE_LINESTRING ? wkbMultiLineString :
                wkbNone;
        }
    }
    break;
    }

    return requestedType;
}

OGRGeometryH
OgrUtils::createOgrGeometry(const osgEarth::Geometry* geometry, OGRwkbGeometryType requestedType)
{
    if (!geometry) return NULL;

    if (requestedType == wkbUnknown)
    {
        requestedType = getOGRGeometryType(geometry);
    }

    OGRwkbGeometryType geom_type, mono_type, sub_type;

    switch(requestedType)
    {
    case wkbPolygon:
    case wkbPolygon25D:
    case wkbPolygonM:
    case wkbPolygonZM:
        geom_type = wkbPolygon, mono_type = wkbPolygon, sub_type = wkbLinearRing; break;
    case wkbLineString:
    case wkbLineString25D:
    case wkbLineStringM:
    case wkbLineStringZM:
        geom_type = wkbLineString, mono_type = wkbLineString, sub_type = wkbNone; break;
    case wkbPoint:
    case wkbPoint25D:
    case wkbPointM:
    case wkbPointZM:
        geom_type = wkbPoint, mono_type = wkbPoint, sub_type = wkbNone; break;
    case wkbMultiPolygon:
    case wkbMultiPolygon25D:
    case wkbMultiPolygonM:
    case wkbMultiPolygonZM:
        geom_type = wkbMultiPolygon, mono_type = wkbPolygon, sub_type = wkbLinearRing; break;
    case wkbMultiLineString:
    case wkbMultiLineString25D:
    case wkbMultiLineStringM:
    case wkbMultiLineStringZM:
        geom_type = wkbMultiLineString, mono_type = wkbLineString; sub_type = wkbNone; break; // wkbLineString; break;
    case wkbMultiPoint:
    case wkbMultiPoint25D:
    case wkbMultiPointM:
    case wkbMultiPointZM:
    default:
        geom_type = wkbMultiPoint, mono_type = wkbPoint, sub_type = wkbPoint; break;
    }

    auto encodePart = [](const Geometry* input, OGRGeometryH parent) -> OGRGeometryH
        {
            for (int v = input->size() - 1; v >= 0; v--)
            {
                osg::Vec3d p = (*input)[v];
                OGR_G_AddPoint(parent, p.x(), p.y(), p.z());
            }
            return parent;
        };

    auto encodeMonoGeometry = [&encodePart](const Geometry* input, OGRGeometryH parent, OGRwkbGeometryType type, OGRwkbGeometryType subType)
        {
            if (subType == wkbNone)
            {
                // no subtype? add points directly to the parent
                for (int v = 0; v < input->size(); v++)
                {
                    osg::Vec3d p = (*input)[v];
                    OGR_G_AddPoint(parent, p.x(), p.y(), p.z());
                }
            }
            else if (subType == wkbPoint)
            {
                // point subtype? handle special case in which we add points to a multipoint monotype
                for (int v = 0; v < input->size(); v++)
                {
                    osg::Vec3d p = (*input)[v];
                    auto part = OGR_G_CreateGeometry(subType);
                    OGR_G_AddPoint(part, p.x(), p.y(), p.z());
                    OGR_G_AddGeometryDirectly(parent, part);
                }
            }
            else
            {
                // everything else (linestrings, polygons, multi-same)
                ConstGeometryIterator itr(input, true); // able to dive into polygon holes
                while (itr.hasMore())
                {
                    const Geometry* geom = itr.next();
                    auto part = OGR_G_CreateGeometry(subType);
                    if (part)
                    {
                        encodePart(geom, part);
                        OGR_G_AddGeometryDirectly(parent, part);
                    }
                }
            }
        };

    auto encodeMultiGeometry = [&encodeMonoGeometry](const Geometry* input, OGRGeometryH parent, OGRwkbGeometryType type, OGRwkbGeometryType subType)
        {
            ConstGeometryIterator itr(input, false); // geoms only (no holes)
            while (itr.hasMore())
            {
                const Geometry* geom = itr.next();
                if (geom)
                {
                    auto component = OGR_G_CreateGeometry(type);
                    if (component)
                    {
                        encodeMonoGeometry(geom, component, type, subType);
                        OGR_G_AddGeometryDirectly(parent, component);
                    }
                }
            }
        };


    auto root = OGR_G_CreateGeometry(geom_type);
    if (root)
    {
        if (geom_type == mono_type)
        {
            encodeMonoGeometry(geometry, root, mono_type, sub_type);
        }
        else
        {            
            encodeMultiGeometry(geometry, root, mono_type, sub_type);
        }
    }

    return root;
}

Feature*
OGRFeatureFactory::createFeature(OGRFeatureH handle) const
{
    FeatureID fid = OGR_F_GetFID( handle );

    OGRGeometryH geomRef = OGR_F_GetGeometryRef( handle );

    Geometry* geom = nullptr;

    if ( geomRef )
    {
        geom = OgrUtils::createGeometry( geomRef, rewindPolygons);
    }

    Feature* feature = new Feature(geom, srs, Style(), fid);

    if (srs && interp.isSet())
        feature->geoInterp() = interp.value();

    int numAttrs = OGR_F_GetFieldCount(handle);
    for (int i = 0; i < numAttrs; ++i)
    {
        OGRFieldDefnH field_handle_ref = OGR_F_GetFieldDefnRef( handle, i );

        // get the field name and convert to lower case:
        std::string name;
        if (i < fieldNames.size())
        {
            name = fieldNames[i];
        }
        else
        {
            name = osgEarth::toLower(std::string(OGR_Fld_GetNameRef(field_handle_ref)));
        }

        // get the field type and set the value appropriately
        OGRFieldType field_type = OGR_Fld_GetType( field_handle_ref );
        switch( field_type )
        {
        case OFTInteger:
            {
                if (IsFieldSet( handle, i ))
                {
                    std::int64_t value = OGR_F_GetFieldAsInteger(handle, i);
                    feature->set( name, value );
                }
                else if (keepNullValues)
                {
                    feature->setNull(name);
                }
            }
            break;

        case OFTInteger64:
            {
                if (IsFieldSet(handle, i))
                {
                    std::int64_t value = OGR_F_GetFieldAsInteger64(handle, i);
                    feature->set(name, value);
                }
                else if (keepNullValues)
                {
                    feature->setNull(name);
                }
            }
            break;

        case OFTReal:
            {
                if (IsFieldSet( handle, i ))
                {
                    double value = OGR_F_GetFieldAsDouble( handle, i );
                    feature->set( name, value );
                }
                else if (keepNullValues)
                {
                    feature->setNull(name);
                }
            }
            break;
        default:
            {
                if (IsFieldSet( handle, i ))
                {
                    const char* value = OGR_F_GetFieldAsString(handle, i);
                    feature->set( name, std::string(value) );
                }
                else if (keepNullValues)
                {
                    feature->setNull(name);
                }
            }
        }
    }

    return feature;
}

AttributeType
OgrUtils::getAttributeType(OGRFieldType type)
{
    switch (type)
    {
    case OFTString: return ATTRTYPE_STRING;
    case OFTReal: return ATTRTYPE_DOUBLE;
    case OFTInteger: return ATTRTYPE_INT;
    default: return ATTRTYPE_UNSPECIFIED;
    };
}


