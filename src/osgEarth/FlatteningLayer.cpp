/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "FlatteningLayer"
#include "HeightFieldUtils"
#include "FeatureCursor"
#include "rtree.h"

using namespace osgEarth;
using namespace osgEarth::Contrib;

REGISTER_OSGEARTH_LAYER(flattenedelevation, FlatteningLayer);
REGISTER_OSGEARTH_LAYER(flattened_elevation, FlatteningLayer);

#define LC "[FlatteningLayer] "

#define OE_TEST OE_DEBUG

namespace
{
    // linear interpolation between a and b
    double inline mix(double a, double b, double t)
    {
        return a + (b - a)*t;
    }

    // clamp "a" to [lo..hi].
    double inline clamp(double a, double lo, double hi)
    {
        return a < lo ? lo : a > hi ? hi : a;
    }

    // smoothstep (cos approx) interpolation between a and b
    double inline smoothstep(double a, double b, double t)
    {
        // smoothstep (approximates cosine):
        t = (-2.0 * t * t * t) + (3.0 * t * t);
        return mix(a, b, clamp(t, 0.0, 1.0));
    }

    double inline smootherstep(double a, double b, double t)
    {
        t = (6.0 * t * t * t * t * t) - (15.0 * t * t * t * t) + (10.0 * t * t * t);
        return mix(a, b, clamp(t, 0.0, 1.0));
    }

    typedef osg::Vec3d POINT;
    typedef osg::Vec3d VECTOR;

    // iterator that permits the use of looping indexes.
    template<typename T>
    struct CircleIterator {
        CircleIterator(const std::vector<T>& v) : _v(v) { }
        int size() const { return _v.size(); }
        const T& operator[](int i) const { return _v[i % _v.size()]; }
        const std::vector<T>& _v;
    };

    // is P inside the CCW triangle ABC?
    bool triangleContains(const POINT& A, const POINT& B, const POINT& C, const POINT& P)
    {
        VECTOR AB = B - A, BC = C - B, CA = A - C;
        if (((P - A) ^ AB).z() > 0.0) return false;
        if (((P - B) ^ BC).z() > 0.0) return false;
        if (((P - C) ^ CA).z() > 0.0) return false;
        return true;
    }

    // Find a point internal to the polygon.
    // This will not always work with polygons that contain holes,
    // so we need to come up with a different algorithm if this becomes a problem.
    // Maybe try a random point generator and profile it.
    osg::Vec3d inline getInternalPoint(const osgEarth::Polygon* p)
    {
        // Simple test: if the centroid is in the polygon, use it.
        osg::Vec3d centroid = p->getBounds().center();
        if (p->contains2D(centroid.x(), centroid.y()))
            return centroid;

        // Concave/holey polygon, so try the hard way.
        // Ref: http://apodeline.free.fr/FAQ/CGAFAQ/CGAFAQ-3.html

        CircleIterator<POINT> vi(p->asVector());

        for (int i = 0; i < vi.size(); ++i)
        {
            const POINT& V = vi[i];
            const POINT& A = vi[i - 1];
            const POINT& B = vi[i + 1];

            if (((V - A) ^ (B - V)).z() > 0.0) // Convex vertex? (assume CCW winding)
            {
                double minDistQV2 = DBL_MAX;   // shortest distance from test point to candidate point
                int indexBestQ;                // index of best candidate point

                // loop over all other verts (besides A, V, B):
                for (int j = i + 2; j < i + 2 + vi.size() - 3; ++j)
                {
                    const POINT& Q = vi[j];

                    if (triangleContains(A, V, B, Q))
                    {
                        double distQV2 = (Q - V).length2();
                        if (distQV2 < minDistQV2)
                        {
                            minDistQV2 = distQV2;
                            indexBestQ = j;
                        }
                    }
                }

                POINT result;

                // If no inside point was found, return the midpoint of AB.
                if (minDistQV2 == DBL_MAX)
                {
                    result = (A + B)*0.5;
                }

                // Otherwise, use the midpoint of QV.
                else
                {
                    const POINT& Q = vi[indexBestQ];
                    result = (Q + V)*0.5;
                }

                // make sure the resulting point doesn't fall within any of the
                // polygon's holes.
                if (p->contains2D(result.x(), result.y()))
                {
                    return result;
                }
            }
        }

        // Will only happen is holes prevent us from finding an internal point.
        OE_WARN << LC << "getInternalPoint failed miserably\n";
        return p->getBounds().center();
    }

    double getDistanceSquaredToClosestEdge(const osg::Vec3d& P, const osgEarth::Polygon* poly)
    {
        double Dmin = DBL_MAX;
        ConstSegmentIterator seg_iter(poly, true);
        while (seg_iter.hasMore())
        {
            auto& segment = seg_iter.next();
            const POINT& A = segment.first;
            const POINT& B = segment.second;
            const VECTOR AP = P - A, AB = B - A;
            double t = clamp((AP*AB) / AB.length2(), 0.0, 1.0);
            VECTOR PROJ = A + AB * t;
            double D = (P - PROJ).length2();
            if (D < Dmin) Dmin = D;
        }
        return Dmin;
    }


    struct Widths {
        Widths(const Widths& rhs) {
            bufferWidth = rhs.bufferWidth;
            lineWidth = rhs.lineWidth;
        }

        Widths(double bufferWidth, double lineWidth) {
            this->bufferWidth = bufferWidth;
            this->lineWidth = lineWidth;
        }

        double bufferWidth;
        double lineWidth;
    };

    typedef std::vector<Widths> WidthsList;

    // Creates a heightfield that flattens an area intersecting the input polygon geometry.
    // The height of the area is found by sampling a point internal to the polygon.
    // bufferWidth = width of transition from flat area to natural terrain.
    bool integratePolygons(const TileKey& key, osg::HeightField* hf, const MultiGeometry* geom, const SpatialReference* geomSRS,
        WidthsList& widths, ElevationPool* pool, ElevationPool::WorkingSet* workingSet,
        bool fillAllPixels, ProgressCallback* progress)
    {
        bool wroteChanges = false;

        const GeoExtent& ex = key.getExtent();

        double col_interval = ex.width() / (double)(hf->getNumColumns() - 1);
        double row_interval = ex.height() / (double)(hf->getNumRows() - 1);

        POINT Pex, P, internalP;
        GeoPoint EP(geomSRS, 0, 0, 0);

        bool needsTransform = ex.getSRS() != geomSRS;
        
        ConstGeometryIterator giter;

        for (unsigned col = 0; col < hf->getNumColumns(); ++col)
        {
            Pex.x() = ex.xMin() + (double)col * col_interval;

            for (unsigned row = 0; row < hf->getNumRows(); ++row)
            {
                // check for cancelation periodically
                //if (progress && progress->isCanceled())
                //    return false;

                Pex.y() = ex.yMin() + (double)row * row_interval;

                if (needsTransform)
                    ex.getSRS()->transform(Pex, geomSRS, P);
                else
                    P = Pex;

                bool done = false;
                double minD2 = DBL_MAX;//bufferWidth * bufferWidth; // minimum distance(squared) to closest polygon edge
                double bufferWidth = 0.0;

                const osgEarth::Polygon* bestPoly = 0L;

                for (unsigned int geomIndex = 0; geomIndex < geom->getNumComponents(); geomIndex++)
                {
                    Geometry* component = geom->getComponents()[geomIndex].get();
                    Widths width = widths[geomIndex];
                    giter.reset(component, false);
                    //ConstGeometryIterator giter(component, false);
                    while (giter.hasMore() && !done)
                    {
                        auto part = giter.next();
                        if (part->getType() == Geometry::TYPE_POLYGON)
                        {
                            auto polygon = static_cast<const osgEarth::Polygon*>(part);

                            // Does the point P fall within the polygon?
                            if (polygon->contains2D(P.x(), P.y()))
                            {
                                // yes, flatten it to the polygon's centroid elevation;
                                // and we're dont with this point.
                                done = true;
                                bestPoly = polygon;
                                minD2 = -1.0;
                                bufferWidth = width.bufferWidth;
                            }

                            // If not in the polygon, how far to the closest edge?
                            else
                            {
                                double D2 = getDistanceSquaredToClosestEdge(P, polygon);
                                if (D2 < minD2)
                                {
                                    minD2 = D2;
                                    bestPoly = polygon;
                                    bufferWidth = width.bufferWidth;
                                }
                            }
                        }
                    }
                }

                if (bestPoly && minD2 != 0.0)
                {
                    float h;
                    POINT internalP = getInternalPoint(bestPoly);
                    EP.x() = internalP.x(), EP.y() = internalP.y();
                    float elevInternal = pool->getSample(EP, workingSet).elevation().as(Units::METERS);

                    if (minD2 < 0.0)
                    {
                        h = elevInternal;
                    }
                    else
                    {
                        EP.x() = P.x(), EP.y() = P.y();
                        float elevNatural = pool->getSample(EP, workingSet).elevation().as(Units::METERS);
                        double blend = clamp(sqrt(minD2) / bufferWidth, 0.0, 1.0); // [0..1] 0=internal, 1=natural
                        h = smootherstep(elevInternal, elevNatural, blend);
                    }

                    hf->setHeight(col, row, h);
                    wroteChanges = true;
                }

                else if (fillAllPixels)
                {
                    EP.x() = P.x(), EP.y() = P.y();
                    float h = pool->getSample(EP, workingSet).elevation().as(Units::METERS);
                    hf->setHeight(col, row, h);
                    // do not set wroteChanges
                }
            }
        }

        return wroteChanges;
    }



    struct Sample {
        double D2;      // distance to segment squared
        osg::Vec3d A;   // endpoint of segment
        osg::Vec3d B;   // other endpoint of segment;
        double AElev;   // The elevation at start of segment
        double BElev;   // The elevation at end of segment
        double T;       // segment parameter of closest point

        // used later:
        float elevPROJ; // elevation at point on segment
        float elev;     // flattened elevation
        double D;       // distance

        double innerRadius;
        double outerRadius;

        bool operator < (struct Sample& rhs) const { return D2 < rhs.D2; }
    };

    typedef std::vector<Sample> Samples;

    bool EQ2(const osg::Vec3d& a, const osg::Vec3d& b) {
        return osg::equivalent(a.x(), b.x()) && osg::equivalent(a.y(), b.y());
    }

    bool isSampleValid(const Sample* b1, Samples& samples)
    {
        for (unsigned i = 0; i < samples.size(); ++i)
        {
            Sample* b2 = &samples[i];
            if (b1 == b2) continue;
            if (equivalent(b1->T, 0.0) && (EQ2(b1->A, b2->A) || EQ2(b1->A, b2->B))) return false;
            if (equivalent(b1->T, 1.0) && (EQ2(b1->B, b2->A) || EQ2(b1->B, b2->B))) return false;
        }
        return true;
    }

    // Inverse Direct Weighting (IDW) interpolation
    float interpolateSamplesIDW(Samples& samples)
    {
        // higher powerParam corresponds to increased proximity favoring
        const double powerParam = 2.5;

        if (samples.size() == 0) return 0.0f;
        if (samples.size() == 1) return samples[0].elev;

        double numer = 0.0;
        double denom = 0.0;
        for (unsigned i = 0; i < samples.size(); ++i)
        {
            if (equivalent(samples[i].D, 0.0)) {
                numer = samples[i].elev, denom = 1.0;
                break;
            }
            else {
                double w = pow(1.0 / samples[i].D, powerParam);
                numer += w * samples[i].elev;
                denom += w;
            }
        }

        return numer / denom;
    }

    // Linear interpolation (simple mean)
    float interpolateSamplesLinear(Samples& samples)
    {
        double numer = 0.0;
        for (unsigned i = 0; i < samples.size(); ++i)
            numer += samples[i].elev;
        return samples.size() > 0 ? (numer / (double)(samples.size())) : FLT_MAX;
    }

    struct LineSegment
    {
        LineSegment(const osg::Vec3d& _a, const osg::Vec3d& _b, unsigned int _geomIndex) :
            A(_a),
            B(_b),
            geomIndex(_geomIndex)
        {
            AB = B - A;
            length2 = AB.length2();
        }

        osg::Vec3d A;
        osg::Vec3d B;
        osg::Vec3d AB;
        double length2;
        unsigned int geomIndex;
        double AElev = NO_DATA_VALUE;
        double BElev = NO_DATA_VALUE;
    };

    using LineSegmentList = std::vector<LineSegment>;

    using LineSegmentIndex = RTree<unsigned, double, 2>;

    void buildSegmentList(const MultiGeometry* geom, LineSegmentList& segments, LineSegmentIndex& index)
    {
        ConstGeometryIterator giter;

        for (unsigned int geomIndex = 0; geomIndex < geom->getNumComponents(); geomIndex++)
        {
            Geometry* component = geom->getComponents()[geomIndex].get();

            giter.reset(component);
            while (giter.hasMore())
            {
                const Geometry* part = giter.next();
                if (part->getType() == Geometry::TYPE_LINESTRING || part->getType() == Geometry::TYPE_RING)
                {
                    for (int i = 0; i < part->size() - 1; ++i)
                    {
                        // AB is a candidate line segment:
                        const osg::Vec3d& A = (*part)[i];
                        const osg::Vec3d& B = (*part)[i + 1];
                        segments.emplace_back(A, B, geomIndex);

                        double min[2] = { osg::minimum(A.x(), B.x()), osg::minimum(A.y(), B.y()) };
                        double max[2] = { osg::maximum(A.x(), B.x()), osg::maximum(A.y(), B.y()) };

                        unsigned int segmentIndex = segments.size() - 1;

                        index.Insert(min, max, segmentIndex);
                    }
                }
            }
        }
    }

    /**
     * Create a heightfield that flattens the terrain around linear geometry.
     * lineWidth = width of completely flat area
     * bufferWidth = width of transition from flat area to natural terrain
     *
     * Note: this algorithm only samples elevation data from the source (elevation pool).
     * As it progresses, however, it is creating new modified elevation data -- but later
     * points will continue to derive their source data from the original data. This means
     * there will be some discontinuities in the final data, especially along the edges of
     * the flattening buffer.
     *
     * There is not perfect solution for this, but one improvement would be to copy the
     * source elevation into the heightfield as a starting point, and then sample that
     * modifiable heightfield as we go along.
     */
    bool integrateLines(
        const TileKey& key,
        osg::HeightField* hf,
        LineSegmentList& segments,
        LineSegmentIndex& index,
        const SpatialReference* geomSRS,
        WidthsList& widths,
        ElevationPool* pool,
        ElevationPool::WorkingSet* workingSet,
        bool fillAllPixels,
        ProgressCallback* progress)
    {
        double maxBufferDistance = 0.0;
        for (auto& w : widths)
        {
            double d = w.bufferWidth + w.lineWidth;
            if (d > maxBufferDistance) maxBufferDistance = d;
        }

        // Sample heights for the line segments
        std::vector< osg::Vec3d > segmentPoints;        
        segments.reserve(segments.size() * 2);
        for (auto itr = segments.begin(); itr != segments.end(); ++itr)
        {
            segmentPoints.push_back(itr->A);
            segmentPoints.push_back(itr->B);
        }
        geomSRS->transform(segmentPoints, pool->getMapSRS());
        Distance samplingResolution = Distance(0.0, Units::METERS);
        pool->sampleMapCoords(segmentPoints.begin(), segmentPoints.end(), samplingResolution, workingSet, nullptr);
        // Assign the samples back to the segments
        for (unsigned int i = 0; i < segments.size(); ++i)
        {
            segments[i].AElev = segmentPoints[i * 2].z();
            segments[i].BElev = segmentPoints[i * 2 + 1].z();
        }

        GeoExtent keyExtent = key.getExtent();
        GeoExtent ex = key.getExtent();
        if (ex.getSRS() != geomSRS)
        {
            ex = ex.transform(geomSRS);
        }

        double col_interval = ex.width() / (double)(hf->getNumColumns() - 1);
        double row_interval = ex.height() / (double)(hf->getNumRows() - 1);

        double col_key_interval = keyExtent.width() / (double)(hf->getNumColumns() - 1);
        double row_key_interval = keyExtent.height() / (double)(hf->getNumRows() - 1);

        std::vector< osg::Vec3d > pixelPoints;
        pixelPoints.reserve(hf->getNumRows() * hf->getNumColumns());
        for (unsigned row = 0; row < hf->getNumRows(); ++row)
        {
            double y = keyExtent.yMin() + (double)row * row_key_interval;

            for (unsigned col = 0; col < hf->getNumColumns(); ++col)
            {         
                double x = keyExtent.xMin() + (double)col * col_key_interval;
                pixelPoints.emplace_back(x, y, 0.0);
            }
        }
        keyExtent.getSRS()->transform(pixelPoints, pool->getMapSRS());
        pool->sampleMapCoords(pixelPoints.begin(), pixelPoints.end(), samplingResolution, workingSet, nullptr);


        bool wroteChanges = false;
       
        osg::Vec3d P, PROJ;

        for (unsigned row = 0; row < hf->getNumRows(); ++row)
        {
            P.y() = ex.yMin() + (double)row * row_interval;

            std::vector<unsigned> hits;

            double searchMin[2] = { ex.xMin() - maxBufferDistance, P.y() - maxBufferDistance };
            double searchMax[2] = { ex.xMax() + maxBufferDistance, P.y() + maxBufferDistance };

            index.Search(searchMin, searchMax, [&hits](const unsigned& hit)
                {
                    hits.push_back(hit);
                    return RTREE_KEEP_SEARCHING;
                });

            // If there are no hits just skip the whole row.
            if (hits.size() == 0)
            {
                continue;
            }

            static const unsigned Maxsamples = 4;
            Samples samples;

            for (unsigned col = 0; col < hf->getNumColumns(); ++col)
            {
                P.x() = ex.xMin() + (double)col * col_interval;

                // For each point, we need to find the closest line segments to that point
                // because the elevation values on these line segments will be the flattening
                // value. There may be more than one line segment that falls within the search
                // radius; we will collect up to MaxSamples of these for each heightfield point.
                samples.clear();

                unsigned int hitIndex = 0;
                // What is the maximum number of hits we see in this area?
                for (auto hit : hits)
                {
                    LineSegment& segment = segments[hit];

                    const Widths& w = widths[segment.geomIndex];

                    double innerRadius = w.lineWidth * 0.5;
                    double outerRadius = innerRadius + w.bufferWidth;
                    double outerRadius2 = outerRadius * outerRadius;

                    // AB is a candidate line segment:
                    const osg::Vec3d& A = segment.A;
                    const osg::Vec3d& B = segment.B;

                    //osg::Vec3d AB = B - A;    // current segment AB
                    const osg::Vec3d& AB = segment.AB;

                    double t;                 // parameter [0..1] on segment AB
                    double D2;                // shortest distance from point P to segment AB, squared
                    double L2 = segment.length2; // length (squared) of segment AB

                    osg::Vec3d AP = P - A;    // vector from endpoint A to point P

                    if (equivalent(L2, 0.0))
                    {
                        // trivial case: zero-length segment
                        t = 0.0;
                        D2 = AP.length2();
                    }
                    else
                    {
                        // Calculate parameter "t" [0..1] which will yield the closest point on AB to P.
                        // Clamping it means the closest point won't be beyond the endpoints of the segment.
                        t = clamp((AP * AB) / L2, 0.0, 1.0);

                        // project our point P onto segment AB:
                        PROJ.set(A + AB * t);

                        // measure the distance (squared) from P to the projected point on AB:
                        D2 = (P - PROJ).length2();
                    }

                    // If the distance from our point to the line segment falls within
                    // the maximum flattening distance, store it.
                    if (D2 <= outerRadius2)
                    {
                        // see if P is a new sample.
                        Sample* b;
                        if (samples.size() < Maxsamples)
                        {
                            // If we haven't collected the maximum number of samples yet,
                            // just add this to the list:
                            samples.emplace_back(std::move(Sample()));
                            b = &samples.back();
                        }
                        else
                        {
                            // If we are maxed out on samples, find the farthest one we have so far
                            // and replace it if the new point is closer:
                            unsigned max_i = 0;
                            for (unsigned i = 1; i < samples.size(); ++i)
                                if (samples[i].D2 > samples[max_i].D2)
                                    max_i = i;

                            b = &samples[max_i];

                            if (b->D2 < D2)
                                b = 0L;
                        }

                        if (b)
                        {
                            b->D2 = D2;
                            b->A = A;
                            b->B = B;
                            b->T = t;                            

                            b->AElev = segment.AElev;
                            b->BElev = segment.BElev;

                            b->innerRadius = innerRadius;
                            b->outerRadius = outerRadius;
                        }
                    }
                }

                // Remove unnecessary sample points that lie on the endpoint of a segment
                // that abuts another segment in our list.
                for (unsigned i = 0; i < samples.size();)
                {
                    if (!isSampleValid(&samples[i], samples))
                    {
                        samples[i] = samples[samples.size() - 1];
                        samples.resize(samples.size() - 1);
                    }
                    else ++i;
                }

                // Now that we are done searching for line segments close to our point,
                // we will collect the elevations at our sample points and use them to
                // create a new elevation value for our point.
                if (samples.size() > 0)
                {
                    float elevP = pixelPoints[row * hf->getNumColumns() + col].z();                    

                    for (unsigned i = 0; i < samples.size(); ++i)
                    {
                        Sample& sample = samples[i];

                        sample.D = sqrt(sample.D2);

                        // Blend factor. 0 = distance is less than or equal to the inner radius;
                        //               1 = distance is greater than or equal to the outer radius.
                        double blend = clamp(
                            (sample.D - sample.innerRadius) / (sample.outerRadius - sample.innerRadius),
                            0.0, 1.0);

                        float a = sample.AElev;
                        float b = sample.BElev;
                        if (a == NO_DATA_VALUE && b != NO_DATA_VALUE)
                            sample.elevPROJ = b;
                        else if (a != NO_DATA_VALUE && b == NO_DATA_VALUE)
                            sample.elevPROJ = a;
                        else if (a == NO_DATA_VALUE && b == NO_DATA_VALUE)
                            sample.elevPROJ = elevP;
                        else
                            sample.elevPROJ = mix(a, b, sample.T);

                        // smoothstep interpolation of along the buffer (perpendicular to the segment)
                        // will gently integrate the new value into the existing terrain.
                        sample.elev = smootherstep(sample.elevPROJ, elevP, blend);
                    }

                    // Finally, combine our new elevation values and set the new value in the output.
                    float finalElev = interpolateSamplesIDW(samples);
                    if (finalElev < FLT_MAX)
                        hf->setHeight(col, row, finalElev);
                    else
                        hf->setHeight(col, row, elevP);

                    wroteChanges = true;
                }

                else if (fillAllPixels)
                {
                    // No close segments were found, so just copy over the source data.
                    float h = pixelPoints[row * hf->getNumColumns() + col].z();
                    hf->setHeight(col, row, h);

                    // Note: do not set wroteChanges to true.
                }
            }
        }

        return wroteChanges;
    }

    bool integrate(const TileKey& key, osg::HeightField* hf, const MultiGeometry* geom, const SpatialReference* geomSRS,
        WidthsList& widths, ElevationPool* pool, ElevationPool::WorkingSet* workingSet,
        bool fillAllPixels, ProgressCallback* progress)
    {
        if (geom->getComponentType() == Geometry::TYPE_POLYGON)
        {
            return integratePolygons(key, hf, geom, geomSRS, widths, pool, workingSet, fillAllPixels, progress);
        }
        else
        {
            LineSegmentList segments;
            LineSegmentIndex index;
            buildSegmentList(geom, segments, index);
            return integrateLines(key, hf, segments, index, geomSRS, widths, pool, workingSet, fillAllPixels, progress);
        }
    }
}

//........................................................................

Config
FlatteningLayer::Options::getConfig() const
{
    Config conf = ElevationLayer::Options::getConfig();

    featureSource().set(conf, "features");
    styleSheet().set(conf, "styles");

    if (filters().empty() == false)
    {
        Config temp;
        for (unsigned i = 0; i < filters().size(); ++i)
            temp.add(filters()[i].getConfig());
        conf.set("filters", temp);
    }

    conf.set("line_width", _lineWidth);
    conf.set("buffer_width", _bufferWidth);
    conf.set("fill", _fill);

    return conf;
}

void
FlatteningLayer::Options::fromConfig(const Config& conf)
{
    URIContext uriContext = URIContext(conf.referrer());

    featureSource().get(conf, "features");
    styleSheet().get(conf, "styles");

    const Config& filtersConf = conf.child("filters");
    for (ConfigSet::const_iterator i = filtersConf.children().begin(); i != filtersConf.children().end(); ++i)
        filters().push_back(ConfigOptions(*i));

    conf.get("line_width", _lineWidth);
    conf.get("buffer_width", _bufferWidth);
    conf.get("fill", _fill);
}

//........................................................................

OE_LAYER_PROPERTY_IMPL(FlatteningLayer, Expression<double>, LineWidth, lineWidth);
OE_LAYER_PROPERTY_IMPL(FlatteningLayer, Expression<double>, BufferWidth, bufferWidth);
OE_LAYER_PROPERTY_IMPL(FlatteningLayer, bool, Fill, fill);

void
FlatteningLayer::init()
{
    ElevationLayer::init();
}

Config
FlatteningLayer::getConfig() const
{
    Config c = ElevationLayer::getConfig();
    return c;
}

Status
FlatteningLayer::openImplementation()
{
    Status parent = ElevationLayer::openImplementation();
    if (parent.isError())
        return parent;

    // we either need a feature source:
    Status fsStatus = options().featureSource().open(getReadOptions());
    if (fsStatus.isError())
        return fsStatus;

    Status ssStatus = options().styleSheet().open(getReadOptions());
    if (ssStatus.isError())
        return ssStatus;

    _filterChain = FeatureFilterChain::create(options().filters(), getReadOptions());
    
    const Profile* profile = getProfile();
    if (!profile)
    {
        profile = Profile::create(Profile::GLOBAL_GEODETIC);
        setProfile(profile);
    }

    addDataExtent(DataExtent(profile->getExtent(), getMinLevel(),  getMaxDataLevel()));

    return Status::NoError;
}

FlatteningLayer::~FlatteningLayer()
{
    //nop
}

void
FlatteningLayer::setFeatureSource(FeatureSource* layer)
{
    options().featureSource().setLayer(layer);
}

void
FlatteningLayer::addedToMap(const Map* map)
{
    ElevationLayer::addedToMap(map);

    // Feature source:
    options().featureSource().addedToMap(map);
    auto fs = getFeatureSource();
    if (fs && fs->getStatus().isError()) {
        setStatus(fs->getStatus());
        return;
    }

    // Stylesheet
    options().styleSheet().addedToMap(map);
    auto ss = getStyleSheet();
    if (ss && ss->getStatus().isError()) {
        setStatus(ss->getStatus());
        return;
    }

    // Collect all elevation layers preceding this one and use them for flattening.
    ElevationLayerVector layers;
    map->getLayers(layers);
    
    ElevationLayerVector layersToUse;
    for (auto& layer : layers)
    {
        if (layer.get() == this)
            break;

        layersToUse.push_back(layer);
    }

    if (!layersToUse.empty())
    {
        _elevWorkingSet.setElevationLayers(layersToUse);
    }

    // Initialize the elevation pool with our map:
    _pool = map->getElevationPool();

    // Make a feature session
    _session = new Session(map);
    _session->setStyles(getStyleSheet());
}

void
FlatteningLayer::removedFromMap(const Map* map)
{
    options().featureSource().removedFromMap(map);
    options().styleSheet().removedFromMap(map);

    ElevationLayer::removedFromMap(map);
}

GeoHeightField
FlatteningLayer::createHeightFieldImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (getStatus().isOK() && getFeatureSource())
    {
        return createFromFeatures(key, progress);
    }

    return GeoHeightField::INVALID;
}

GeoHeightField
FlatteningLayer::createFromFeatures(const TileKey& key, ProgressCallback* progress) const
{
    if (!getFeatureSource())
    {
        setStatus(Status(Status::ServiceUnavailable, "No feature source"));
        return GeoHeightField::INVALID;
    }

    if (getFeatureSource()->getStatus().isError())
    {
        setStatus(getFeatureSource()->getStatus());
        return GeoHeightField::INVALID;
    }

    const FeatureProfile* featureProfile = getFeatureSource()->getFeatureProfile();
    if (!featureProfile)
    {
        setStatus(Status(Status::ConfigurationError, "Feature profile is missing"));
        return GeoHeightField::INVALID;
    }

    const SpatialReference* featureSRS = featureProfile->getSRS();
    if (!featureSRS)
    {
        setStatus(Status(Status::ConfigurationError, "Feature profile has no SRS"));
        return GeoHeightField::INVALID;
    }

    // Lat/Long extent:
    GeoExtent geoExtent = key.getExtent().transform(featureSRS->getGeographicSRS());
    if (!geoExtent.isValid())
    {
        return GeoHeightField::INVALID;
    }

    // TODO:  JBFIX.  Add a "max" setting somewhere.
    double linewidth = 10.0;
    double bufferwidth = 10.0;
    double queryBuffer = 0.5*linewidth + bufferwidth;

    // We need to work in projected space since we are buffering in meters.
    auto workingSRS = key.getExtent().getSRS()->isGeographic() ? 
        SpatialReference::get("spherical-mercator") : key.getExtent().getSRS();

    bool needsTransform = !featureSRS->isHorizEquivalentTo(workingSRS);

    FilterContext context(_session.get());
    Query query(key);
    query.buffer() = Distance(queryBuffer, Units::METERS);

    // Fetch our features:
    auto cursor = getFeatureSource()->createFeatureCursor(query, _filterChain, &context, progress);
    if (!cursor.valid())
        return GeoHeightField::INVALID;

    FeatureList features;
    auto featureCount = cursor->fill(features);
        
    // preallocate some memory for speed
    MultiGeometry geoms;
    geoms.getComponents().reserve(featureCount);

    WidthsList widths;
    widths.reserve(featureCount);

    // Figure out what the buffered extent is going to be so we can
    // remove any features that don't fall within the extent
    GeoExtent bufferedExtent = key.getExtent();
    bufferedExtent = bufferedExtent.transform(workingSRS);
    double d = bufferedExtent.getSRS()->transformDistance(
        query.buffer().get(),
        bufferedExtent.getSRS()->getUnits(),
        0.5 * (bufferedExtent.yMin() + bufferedExtent.yMax()));    
    bufferedExtent.expand(d, d);

    // Now collect all the features we need for this tile.
    for (auto& feature : features)
    {
        // Skip features that don't fall within the extent
        if (!bufferedExtent.intersects(feature->getExtent()))
        {            
            continue;
        }    

        double lineWidth = 0.0;
        double bufferWidth = 0.0;
        if (options().lineWidth().isSet())
        {
            lineWidth = options().lineWidth()->eval(feature, context);
        }

        if (lineWidth > 0.0)
        {
            if (options().bufferWidth().isSet())
            {
                bufferWidth = options().bufferWidth()->eval(feature, context);
            }

            // Transform the feature geometry to our working (projected) SRS.
            if (needsTransform)
            {
                feature->transform(workingSRS);
            }

            lineWidth = SpatialReference::transformUnits(
                Distance(lineWidth, Units::METERS),
                featureSRS,
                geoExtent.getCentroid().y());

            bufferWidth = SpatialReference::transformUnits(
                Distance(bufferWidth, Units::METERS),
                featureSRS,
                geoExtent.getCentroid().y());

            //TODO: optimization: test the geometry bounds against the expanded tilekey bounds
            //      in order to discard geometries we don't care about
            geoms.getComponents().push_back(feature->getGeometry());
            widths.emplace_back(bufferWidth, lineWidth);
        }        
    }    

    if (!geoms.getComponents().empty())
    {
        // Make an empty heightfield to populate:
        auto hf = HeightFieldUtils::createReferenceHeightField(
            key.getExtent(),
            osgEarth::ELEVATION_TILE_SIZE,
            osgEarth::ELEVATION_TILE_SIZE,
            0u,                 // no border
            false,              // don't process HAEs
            NO_DATA_VALUE);     // initialize to no data

        bool wrote_to_hf = integrate(
            key,
            hf.get(),
            &geoms,
            workingSRS,
            widths,
            _pool.get(),
            &_elevWorkingSet,
            options().fill().value(),
            progress);

        if (wrote_to_hf)
        {
            return GeoHeightField(hf.get(), key.getExtent());
        }
    }

    return GeoHeightField::INVALID;
}
