/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/PowerlineLayer>
#include <osgEarth/AltitudeFilter>
#include <osgEarth/ElevationQuery>
#include <osgEarth/ECEF>
#include <osgEarth/GeometryUtils>
#include <osgEarth/Math>
#include <osgEarth/FeatureModelGraph>

#include <iterator>

using namespace osgEarth;

#define LC "[PowerlineLayer]"

#define OE_TEST OE_NULL

REGISTER_OSGEARTH_LAYER(PowerlineModel, PowerlineLayer);

namespace
{
    namespace Array
    {
        // A matrix-like accessor into an area of memory. Given stride
        // and offset arguments, you can treat a region of memory as a
        // rectangular matrix and access subregions of it.
        // An array of osg::Vec3 as a matrix:
        // osg::Vec3Array vecs(16);
        // View<osg::Vec3> array(&vecs[0], 4, 4);
        // for (int j = 0; j < array.rowDim; j++)
        //    for (int i = 0; j < array.colDim; i ++)
        //        array(j, i) = ...;

        // This is quite general; a View with an appropriate offset can
        // support access to a smaller area of backing storage than
        // would be used by the full range of indices to the View, as
        // long as those indices are constrained.
        template<class ElementType>
        struct View
        {
            ElementType* const data;
            const int offset;
            const int stride;
            const unsigned rowDim;       // Number of rows
            const unsigned colDim;       // Number of columns
            View(ElementType* data_, unsigned offset_, unsigned stride_,
                unsigned rowDim_, unsigned colDim_)
                : data(data_), offset(offset_), stride(stride_), rowDim(rowDim_), colDim(colDim_)
            {
            }

            View(ElementType* data_, unsigned rowDim_, unsigned colDim_)
                : data(data_), offset(0), stride(colDim_), rowDim(rowDim_), colDim(colDim_)
            {
            }
            // Create a view from another view
            View(const View& rhs, int xo, int yo, unsigned rowDim_, unsigned colDim_)
                : data(rhs.data), offset(rhs.offset + rhs.stride * xo + yo), stride(rhs.stride),
                rowDim(rowDim_), colDim(colDim_)
            {
            }

            ElementType operator()(unsigned row, unsigned col) const
            {
                return *(data + (offset + static_cast<int>(stride * row + col)));
            }
            ElementType& operator()(unsigned row, unsigned col)
            {
                return *(data + (offset + static_cast<int>(stride * row + col)));
            }
        };

        // The Matrix is a View that allocates its own storage.
        template <class ElementType>
        struct Matrix : public View<ElementType>
        {
            template <class ViewType>
            void copyFromView(const ViewType& view)
            {
                for (int j = 0; j < this->rowDim; ++j)
                {
                    for (int i = 0; i < this->colDim; ++i)
                    {
                        (*this)(j, i) = view(j, i);
                    }
                }
            }

            Matrix(unsigned rowDim_, unsigned colDim_, const ElementType& init = ElementType())
                : View<ElementType>(new ElementType[rowDim_ * colDim_], rowDim_, colDim_)
            {
                std::fill(this->data, this->data + rowDim_ * colDim_, init);
            }

            template <class ViewType>
            Matrix(unsigned rowDim_, unsigned colDim_, const ViewType& view)
                : View<ElementType>(new ElementType[rowDim_ * colDim_], rowDim_, colDim_)
            {
                copyFromView(view);
            }

            ~Matrix()
            {
                delete[] this->data;
            }
        };
    }
}

// Render power lines with their towers as models.
// 
// This code was originally written to use OSM data served in a tiled
// data source. In that model, features are provided for the power
// towers and power lines, then the attributes of both (tower height,
// line voltage, numbers of lines) are useful, but there's not
// explicit link between the two kinds of features. On the other hand,
// power line data could come from an shp file or other GDAL layer in
// which only the line features are available. To handle this, we
// generate features for the towers if they aren't available.

void PowerlineLayer::ModelOptions::fromConfig(const Config& conf)
{
    if (conf.hasChild("name"))
    {
        name() = conf.child("name").value();
    }
    if (conf.hasChild("attachment_points"))
    {
        osg::ref_ptr<Geometry> attachGeom = GeometryUtils::geometryFromWKT(conf.child("attachment_points").value());
        std::copy(attachGeom->asVector().begin(), attachGeom->asVector().end(),
                  std::back_inserter(attachment_points()));
    }

    if (conf.hasChild("uri"))
    {
        uri() = conf.child("uri").value();
    }
    else if (conf.hasChild("model"))
    {
        uri() = conf.child("model").value();
    }
    conf.get("max_sag", maxSag());
}

Config PowerlineLayer::ModelOptions::getConfig() const
{
    Config conf;
    if (!attachment_points().empty())
    {
        osg::ref_ptr<Geometry> attachGeom = new LineString(&attachment_points());
        conf.set("attachment_points", GeometryUtils::geometryToWKT(attachGeom.get()));
    }
    conf.set("uri", uri());
    conf.set("max_sag", maxSag());
    return conf;
}

PowerlineLayer::Options::Options()
    : FeatureModelLayer::Options()
{
    fromConfig(_conf);
}

PowerlineLayer::Options::Options(const ConfigOptions& options)
    : FeatureModelLayer::Options(options)
{
    fromConfig(_conf);
}

// XXX cropFeatures is required to be true in order to include line
// features using their extent instead of their centroid. What we
// really want is to include line features by extent without cropping
// them.
void PowerlineLayer::Options::fromConfig(const Config& conf)
{
    conf.get("line_expr", towerExpr()); // back compat
    conf.get("tower_expr", towerExpr());
    conf.get("cable_expr", cableStyleExpr());
    conf.get("num_cables", numCablesExpr());
    conf.get("infer_tower_locations", inferTowerLocations());
    conf.get("combine_lines", combineLines());

    for (auto& modelConf : conf.children("tower_model"))
        towerModels().push_back(ModelOptions(modelConf));

    referrer = conf.referrer();
}

Config
PowerlineLayer::Options::getConfig() const
{
    Config conf = super::getConfig();

    conf.set("tower_expr", towerExpr());
    conf.set("cable_expr", cableStyleExpr());
    conf.set("num_cables", numCablesExpr());
    conf.set("infer_tower_locations", inferTowerLocations());
    conf.set("combine_lines", combineLines());

    for(auto& towerModel : towerModels())
        conf.add("tower_model", towerModel.getConfig());

    return conf;
}

void
PowerlineLayer::Options::mergeConfig(const Config& conf)
{
    super::mergeConfig(conf);
    fromConfig(conf);
}

namespace
{
    // Custom FeatureNodeFactory that creates powerline cables and towers
    class PowerlineFeatureNodeFactory : public GeomFeatureNodeFactory
    {
    public:
        PowerlineFeatureNodeFactory(const PowerlineLayer::Options& options, StyleSheet* styles);

        bool createOrUpdateNode(
            FeatureCursor* cursor,
            const Style& style,
            const FilterContext& context,
            osg::ref_ptr<osg::Node>& node,
            const Query& query) override;

    private:
        FeatureList makeCableFeatures(FeatureList& powerFeatures, FeatureList& towerFeatures,
            FilterContext& cx, const Query& query, const Style& style);

        PowerlineLayer::ModelOptions evalTowerModel(Feature* f, const FilterContext& context);

        optional<Expression<String>> _towerExpr;
        optional<Expression<String>> _cableStyleExpr;
        optional<Expression<int>> _numCablesExpr;
        std::vector<PowerlineLayer::ModelOptions> _renderData;
        PowerlineLayer::Options _powerlineOptions;
    };
}

void
PowerlineLayer::init()
{
    super::init();

    // do NOT crop features to the working extent! We need features from adjoining tiles.
    options().autoCropFeatures().setDefault(false);

    // TODO:
    // This class surely creates some duplicate geometry in neighboring tiles.
    // We create geometry from neighboring tiles but do nothing to prevent dupes.
    // Will solve later (if it becomes a problem)
}

void
PowerlineLayer::create()
{
    super::create();

    auto* fmg = findTopMostNodeOfType<FeatureModelGraph>(getNode());
    if (fmg)
    {
        fmg->setFeatureQueryBufferWidthAsPercentage(0.75);
    }
}

FeatureNodeFactory*
PowerlineLayer::createFeatureNodeFactoryImplementation() const
{
    return new PowerlineFeatureNodeFactory(options(), getStyleSheet());
}

// Make a catenary curve between two points
//
// A catenary is of the form y = a * cosh((x + k) / a) + c. We want to
// find the curve that passes between two points of different
// heights. This curve is determined by several parameters which may
// be hard to know, such as the maximum tension in the cable and its
// weight. Instead, we assume that the cable is a certain factor
// longer (slack )than the straight-line distance between the two
// attachment points.
//
// Arguments are in ECEF, orientation is at p1. We use that frame for
// the whole catenary.

namespace
{
    void toRow(osg::Matrixd& mat, osg::Vec3d& vec, int row)
    {
        for (int i = 0; i < 3; ++i)
        {
            mat(row, i) = vec[i];
        }
    }

    // Function for a in terms of arc length L, height difference h,
    // and horizontal distance d. This will be solved by a
    // Newton-Raphson solver, so we need both the function and its
    // derivative.

    struct GFunc
    {
        GFunc(double d_, double L, double h)
            : d(d_), rootLh(sqrt(L * L - h * h))
        {}

        double operator()(double a) const
        {
            return 2.0 * a * sinh(d / (2.0 * a)) - rootLh;
        }
        double d;
        double rootLh;
    };

    struct GFuncDeriv
    {
        GFuncDeriv(double d_)
            : d(d_)
        {
        }
        double operator()(double a) const
        {
            double da = d / a;
            return 2.0 * sinh(da/2.0) - da * cosh(da/2.0);
        }
        double d;
    };

    // Support around evaluating the catenary formula x = a * cosh((x + x1) / a) + C
    // and solving for the parameters a,x1, and C given the arc length of the curve L and the
    // horizontal and vertical displacement of the endpoints.
    struct CatenaryFunc
    {
        double a;
        double x1;
        double C;
        double L;
        double d;
        double h;
        CatenaryFunc(double a_, double x1_, double C_, double L_ = 0.0, double d_ = 0.0, double h_ = 0.0)
            : a(a_), x1(x1_), C(C_), L(L_), d(d_), h(h_)
        {
        }
        double operator()(double x) const
        {
            return a * cosh((x + x1) /a) + C;
        }
        static CatenaryFunc solveIt(double L, double d, double h)
        {
            GFunc gfunc(d, L, h);
            GFuncDeriv gfuncDeriv(d);
            bool validSoln = false;
            const double a = solve(gfunc, gfuncDeriv, d / 2, 1.0e-6, validSoln);
            // Parameters of the curve
            const double x1 = (a * log((L + h) / (L - h)) - d) / 2.0;
            const double C = -a * cosh(x1 / a);
            return CatenaryFunc(a, x1, C, L, d, h);
        }
    };
    // Find the difference between a catenary's minimum and refHeight. minHeight should be negative
    // i.e., below both towers in this usage.  d, h, and L are as in CatenaryFunc, but now L is a
    // free variable.
    struct MinCatHeight
    {
        double d;
        double h;
        double refHeight;
        MinCatHeight(double d_, double h_, double refHeight_ = 0.0)
            : d(d_), h(h_), refHeight(refHeight_)
        {
        }
        double operator()(double L) const
        {
            if (L * L < d * d + h * h)
            {
                // not happening
                return 0.0;
            }
            CatenaryFunc func = CatenaryFunc::solveIt(L, d, h);
            double minHeight = 0.0;
            if (func.x1 > 0.0)
            {
                // minimum is out of the range of the curve (x < 0)
                return 0.0;
            }
            // Could use func.a + func.C
            return (func(-func.x1) -refHeight);
        }
    };

    bool eq2d(const osg::Vec3d& lhs, const osg::Vec3d& rhs, double EPS = 1e-3)
    {
        return
            equivalent(lhs.x(), rhs.x(), EPS) &&
            equivalent(lhs.y(), rhs.y(), EPS);
    }
    void transpose3x3(osg::Matrix& m)
    {
        std::swap(m(0, 1), m(1, 0));
        std::swap(m(0, 2), m(2, 0));
        std::swap(m(1, 2), m(2, 1));
    }

    double calculateGeometryHeading(const osg::Vec3d& point, const osg::Vec3d& previous, const osg::Vec3d& next)
    {
        osg::Vec3d dir;
        if (previous.x() == DBL_MAX && next.x() == DBL_MAX)
        {
            return 0.0;
        }
        else if (previous.x() != DBL_MAX && next.x() != DBL_MAX)
        {
            auto a = point - previous;
            auto b = next - point;
            a.normalize();
            b.normalize();
            dir = a + b;
        }
        else if (previous.x() != DBL_MAX)
        {
            dir = point - previous;
        }
        else if (next.x() != DBL_MAX)
        {
            dir = next - point;
        }
        else
        {
            return 0.0;
        }

        dir.normalize();
        double heading = std::atan2(-dir.x(), dir.y());
        while (heading < -osg::PI_2) heading += osg::PI;
        while (heading >= osg::PI_2) heading -= osg::PI;
        return osg::RadiansToDegrees(heading);
    }

    struct PointEntry
    {
        PointEntry(Feature* feature)
            : pointFeature(feature),
            previous(DBL_MAX, DBL_MAX, DBL_MAX),
            next(DBL_MAX, DBL_MAX, DBL_MAX)
        {}
        PointEntry() : PointEntry(0L) {}
        osg::ref_ptr<Feature> pointFeature;
        FeatureList lineFeatures;
        osg::Vec3d previous;
        osg::Vec3d next;
    };

    // Map that ignores elevation component of points
    struct CompPoints
    {
        bool operator()(const osg::Vec3d& lhs, const osg::Vec3d& rhs) const
        {
            const double E = 0.1;
            double dx = rhs.x() - lhs.x(), dy = rhs.y() - lhs.y();
            if (dx < -E) return true;
            if (dx > +E) return false;
            if (dy < -E) return true;
            return false;
        }
    };

    using PointMap = std::map<osg::Vec3d, PointEntry, CompPoints>;

    inline PointEntry& getPoint(PointMap& map, const osg::Vec3d& coord)
    {
        return map[coord];
    }

    inline PointMap::iterator findPoint(PointMap& map, const osg::Vec3d& coord)
    {
        return map.find(coord);
    }

    void preparePowerFeatures(FeatureList& input, FilterContext& context, bool combineLines)
    {
        // collect all point features (towers and poles).
        PointMap pointMap;
        FeatureList points;
        for (auto& feature : input)
        {
            if (feature->getGeometry()->isPointSet())
            {
                GeometryIterator iter(feature->getGeometry(), false);
                iter.forEach([&](auto* geom)
                    {
                        for (auto& pt : *geom)
                        {
                            pointMap[pt] = PointEntry(feature);
                        }
                    });

                points.emplace_back(feature);
            }
        }

        // collect all linear features as single linestrings.
        FeatureList lines;
        for (auto& feature : input) {
            auto* g = feature->getGeometry();
            if (g && g->isLinear()) {
                GeometryIterator iter(g, false);
                iter.forEach([&](auto* geom)
                    {
                        if (geom->size() >= 2) {
                            auto* new_feature = new Feature(*feature);
                            new_feature->setGeometry(geom->cloneAs(Geometry::TYPE_LINESTRING));
                            lines.emplace_back(new_feature);
                        }
                    });
            }
        }

        if (combineLines)
        {
            // combine linestrings with common endpoints:
            for (int changes = 1; changes > 0; )
            {
                changes = 0;
                for (auto& feature : lines)
                {
                    if (!feature.valid())
                        continue;

                    auto* geom = feature->getGeometry();

                    for (auto& other : lines)
                    {
                        if (other.valid() && other != feature)
                        {
                            auto* other_geom = other->getGeometry();

                            if (eq2d(geom->back(), other_geom->front()))
                            {
                                geom->resize(geom->size() - 1);
                                geom->insert(geom->end(), other_geom->begin(), other_geom->end());
                                changes++;
                                other = nullptr;
                            }

                            else if (eq2d(geom->back(), other_geom->back()))
                            {
                                geom->resize(geom->size() - 1);
                                geom->insert(geom->end(), other_geom->rbegin(), other_geom->rend());
                                changes++;
                                other = nullptr;
                            }

                            else if (eq2d(other_geom->back(), geom->front()))
                            {
                                other_geom->resize(other_geom->size() - 1);
                                other_geom->insert(other_geom->end(), geom->begin(), geom->end());
                                changes++;
                                feature = nullptr;
                                break;
                            }

                            else if (eq2d(other_geom->back(), geom->back()))
                            {
                                other_geom->resize(other_geom->size() - 1);
                                other_geom->insert(other_geom->end(), geom->rbegin(), geom->rend());
                                changes++;
                                feature = nullptr;
                                break;
                            }
                        }
                    }
                }
            }

            // remove the ones that were null'd out during connection:
            FeatureList temp;
            for (auto& feature : lines)
                if (feature.valid())
                    temp.push_back(feature);
            lines.swap(temp);
        }

        // associate all linears to their component points
        for (auto& feature : lines)
        {
            if (feature.valid() && feature->getGeometry()->isLinear())
            {
                auto* output = new LineString();

                GeometryIterator iter(feature->getGeometry(), false);
                iter.forEach([&](Geometry* geom)
                    {
                        for (int i = 0; i < geom->size(); ++i)
                        {
                            osg::Vec3d point = (*geom)[i];

                            // skip duplicates.
                            if (i == 0 || !eq2d(point, (*geom)[i - 1], 0.1)) // local data (mercator)
                            {
                                auto ptItr = pointMap.find(point);
                                if (ptItr != pointMap.end())
                                {
                                    PointEntry& entry = ptItr->second;
                                    entry.lineFeatures.emplace_back(feature);

                                    if (!output->empty())
                                    {
                                        entry.previous = output->back();
                                        auto prev_point = pointMap.find(entry.previous);
                                        prev_point->second.next = point;
                                    }

                                    output->push_back(point);
                                }
                            }
                        }
                    });

                feature->setGeometry(output);
            }
        }

        const SpatialReference* targetSRS = nullptr;
        if (context.session()->isMapGeocentric())
            targetSRS = context.session()->getMapSRS();
        else
            targetSRS = context.featureProfile()->getSRS()->getGeocentricSRS();

        // calculate the heading of each tower/pole and record it to a feature attribute.
        for (auto& i : pointMap)
        {
            PointEntry& entry = i.second;
            entry.pointFeature->set("heading", calculateGeometryHeading(i.first, entry.previous, entry.next));
        }

        // finally, remove any points that are not associated with a line.
        FeatureList pointsWithLines;
        pointsWithLines.reserve(points.size());
        for(auto iter : pointMap)
        {
            if (!iter.second.lineFeatures.empty())
            {            
                pointsWithLines.push_back(iter.second.pointFeature.get());
            }
        }

        // combine all new features for output.
        input.clear();
        input.reserve(pointsWithLines.size() + lines.size());
        input.insert(input.end(), pointsWithLines.begin(), pointsWithLines.end());
        input.insert(input.end(), lines.begin(), lines.end());
    }


    void makeCatenary(osg::Vec3d p1, osg::Vec3d p2, const osg::Matrixd& orientation, double slack,
        double maxSag,
        std::vector<osg::Vec3d>& result, float tessellationSize)
    {
        // Create a frame centered at p1 with orientation normal to
        // earth's surface
        osg::Matrixd FrameP1(orientation);
        toRow(FrameP1, p1, 3);
        // p1 in this frame is at origin. Get p2 into the local frame
        osg::Matrixd FrameP1Inv;
        FrameP1Inv.invert(FrameP1);
        osg::Vec3d p2local = p2 * FrameP1Inv;
        // The math for solving the catenary assumes that the higher point
        // is to the right on the X axis. Build a frame that reflects
        // that.
        //
        // If it turns out that p1 is higher, then at the end we will need
        // to generate points in reverse so that the resulting feature
        // makes sense.
        bool swapped = false;
        osg::Vec3d Xaxis;
        if (p2local.z() < 0.0)
        {
            swapped = true;
            Xaxis = osg::Vec3d(-p2local.x(), -p2local.y(), 0.0);
        }
        else
        {
            Xaxis = osg::Vec3d(p2local.x(), p2local.y(), 0.0);
        }
        // Horizontal distance between points
        const double d = Xaxis.normalize();
        osg::Vec3d Yaxis = osg::Vec3d(0.0, 0.0, 1.0) ^ Xaxis;
        osg::Matrixd FrameCat;
        toRow(FrameCat, Xaxis, 0);
        toRow(FrameCat, Yaxis, 1);
        // Height difference between points; should be positive
        double h = 0.0;
        if (swapped)
        {
            h = -p2local.z();
            toRow(FrameCat, p2local, 3);
        }
        else
        {
            h = p2local.z();
        }
        // straight distance between attachment points; obviously a lower limit on the cable length
        double straightDist = p2local.length();
        CatenaryFunc func = CatenaryFunc::solveIt(straightDist * slack, d, h);

        double xMin = -func.x1;
        double yMin = func(xMin);
        if (0.0 < xMin && yMin < -maxSag)
        {
            // Cable droops too much, so try a more expensive strategy
            MinCatHeight minimum(d, h, -maxSag);
            // No downside in choosing a very low lower bound
            double newGuess = ((slack - 1.0) * .01 + 1.0) * straightDist;
            // centimeter tolerence is fine
            double Lmin = solveBisect(minimum, newGuess, straightDist * slack, 0.01, 8);
            func = CatenaryFunc::solveIt(Lmin, d, h);
        }
        const osg::Vec3d P1(0.0, 0.0, 0.0), P2(d, 0.0, h);
        double begin, inc;
        int numSteps = ceil(p2local.length() / tessellationSize);
        std::vector<osg::Vec3d> cablePts;
        if (swapped)
        {
            inc = -d / numSteps;
            begin = d + inc;
            cablePts.push_back(P2);
        }
        else
        {
            inc = d / numSteps;
            begin = inc;
            cablePts.push_back(P1);
        }
        double x = begin;
        for (int i = 1; i <= numSteps; ++i, x += inc)
        {
            double z = func(x);
            cablePts.push_back(osg::Vec3d(x, 0.0, z));
        }
        osg::Matrixd cat2world = FrameCat * FrameP1;
        for (std::vector<osg::Vec3d>::iterator itr = cablePts.begin(), end = cablePts.end();
            itr != end;
            ++itr)
        {
            *itr = *itr * cat2world;
        }
        result.insert(result.end(), cablePts.begin(), cablePts.end());
    }

    int chooseAttachment(
        const osg::Matrixd& towerFrame0, const osg::Matrixd& towerFrame1,
        const osg::Vec3d& attachStart, const osg::Vec3d& attachEnd0, const osg::Vec3d& attachEnd1)
    {
        // Get everything into tower0's local frame
        osg::Matrixd world2Tower0 = osg::Matrixd::inverse(towerFrame0);
        osg::Vec3d tower0(0, 0, 0);
        osg::Vec3d tower1 = osg::Vec3d(towerFrame1(3, 0), towerFrame1(3, 1), towerFrame1(3, 2)) * world2Tower0;
        osg::Vec3d end0 = attachEnd0 * towerFrame1 * world2Tower0;
        osg::Vec3d end1 = attachEnd1 * towerFrame1 * world2Tower0;

        // Project the line segments into the XY plane.
        // Segment 2D does just that.
        Segment2d tower_to_tower(tower0, tower1);
        Segment2d line0(attachStart, end0);
        Segment2d line1(attachStart, end1);

        osg::Vec3d dummy;
        bool line0_crosses = tower_to_tower.intersect(line0, dummy);
        bool line1_crosses = tower_to_tower.intersect(line1, dummy);

        if (line0_crosses) return 1;
        if (line1_crosses) return 0;

        if ((tower1-tower0).length() < 1.0e-6)
        {
            // towers are on top of each other; shouldn't happen since we remove duplicates
            // along the way :(
            //OE_WARN << LC << "coincident towers..." << std::endl;
            //return 0;
        }

        return 0;
    }

    osg::Matrixd getLocalToWorld(const osg::Vec3d & geodeticPt,
        const SpatialReference * inputSRS,
        const SpatialReference * outputSRS)
    {
        osg::Matrixd result;
        osg::Vec3d worldPt;
        ECEF::transformAndGetRotationMatrix(geodeticPt, inputSRS, worldPt, outputSRS, result);
        result.setTrans(worldPt);
        return result;
    }

    bool evalStyle(Feature * f, FilterContext & context, const Expression<String>& styleExpr, Style & combinedStyle)
    {
        StyleSheet* sheet = context.session()->styles();
        // See if multiple selectors become necessary
        const StyleSelector& sel = sheet->getSelectors().begin()->second;
        const std::string& styleString = styleExpr.eval(f, context);
        if (!styleString.empty() && styleString != "null")
        {
            // resolve the style:

            // if the style string begins with an open bracket, it's an inline style definition.
            if (styleString.length() > 0 && styleString[0] == '{')
            {
                Config conf("style", styleString);
                conf.setReferrer(sel.styleExpression()->referrer());
                conf.set("type", "text/css");
                combinedStyle = Style(conf);
            }

            // otherwise, look up the style in the stylesheet. Do NOT fall back on a default
            // style in this case: for style expressions, the user must be explicity about
            // default styling; this is because there is no other way to exclude unwanted
            // features.
            else
            {
                const Style* selectedStyle = context.session()->styles()->getStyle(styleString, false);
                if (selectedStyle)
                    combinedStyle = *selectedStyle;
            }
            return true;
        }
        return false;
    }

    void setCableStyleDefaults(Style& cableStyle)
    {
        osg::ref_ptr<LineSymbol> lineSymbol = cableStyle.getOrCreateSymbol<LineSymbol>();
        if (!lineSymbol->stroke()->width().isSet())
        {
            lineSymbol->stroke().mutable_value().width() = Distance(.05, Units::METERS);
        }
        if (!lineSymbol->tessellationSize().isSet())
        {
            lineSymbol->tessellationSize() = Distance(20, Units::METERS);
        }
        if (!lineSymbol->useWireLines().isSet())
        {
            lineSymbol->useWireLines() = true;
        }
    }

    void setModelStyleDefaults(Style& modelStyle, bool force = true)
    {
        osg::ref_ptr<ModelSymbol> modelSymbol = modelStyle.getOrCreate<ModelSymbol>();
        osg::ref_ptr<AltitudeSymbol> altitudeSymbol = modelStyle.getOrCreate<AltitudeSymbol>();
        if (!modelSymbol->orientationFromFeature().isSet() || force)
        {
            modelSymbol->orientationFromFeature() = true;
        }
    }

    void setModelStyleDefaults(Style& modelStyle, const std::string& modelName, const std::string& referrer, bool force = true)
    {
        setModelStyleDefaults(modelStyle, force);
        osg::ref_ptr<ModelSymbol> modelSymbol = modelStyle.getOrCreate<ModelSymbol>();
        if (!modelSymbol->url().isSet() || force)
        {
            modelSymbol->url()->setLiteral(modelName, referrer);
        }
    }
}


PowerlineFeatureNodeFactory::PowerlineFeatureNodeFactory(const PowerlineLayer::Options& options, StyleSheet* styles)
    : GeomFeatureNodeFactory(options),
    _powerlineOptions(options)
{
    if (options.towerModels().empty())
    {
        OE_WARN << LC << "No tower models defined!" << std::endl;
        return;
    }

    _renderData = options.towerModels();
    _towerExpr = options.towerExpr();
    _cableStyleExpr = options.cableStyleExpr();
    _numCablesExpr = options.numCablesExpr();
}

PowerlineLayer::ModelOptions
PowerlineFeatureNodeFactory::evalTowerModel(Feature *f, const FilterContext& cx)
{
    if (_powerlineOptions.towerExpr().isSet())
    {
        std::string renderDataString = _powerlineOptions.towerExpr()->eval(f, cx);
        if (renderDataString[0] == '<')
        {
            PowerlineLayer::ModelOptions featureRenderData;
            std::stringstream renderDataStream(renderDataString);
            Config renderDataConfig;
            renderDataConfig.fromXML(renderDataStream);
            ConfigSet models = renderDataConfig.children("tower_model");
            featureRenderData.fromConfig(models.front());
            return featureRenderData;
        }
        else
        {
            // It's the name of a model
            for (auto& towerModel : _powerlineOptions.towerModels())
            {
                if (towerModel.name().isSet() && towerModel.name() == renderDataString)
                {
                    return towerModel;
                }
            }
            // No model? Shouldn't happen

            OE_WARN << LC << "Named tower model \"" << renderDataString << "\" not found!" << std::endl;
            return {};
        }
    }
    else
    {
        return _powerlineOptions.towerModels().front();
    }
}

FeatureList
PowerlineFeatureNodeFactory::makeCableFeatures(
    FeatureList& allPowerFeatures,
    FeatureList& towerFeatures, FilterContext& cx,
    const Query& query,
    const Style& cableStyle)
{
    FeatureList result;
    const Session* session = cx.session();

    // the map against which we'll be doing elevation clamping
    osg::ref_ptr<const Map> map = session->getMap();
    if (!map.valid() || (_renderData[0].attachment_points().empty() && !_towerExpr.isSet()))
        return result;

    const SpatialReference* mapSRS = map->getSRS();
    osg::ref_ptr<const SpatialReference> featureSRS = cx.featureProfile()->getSRS();

    // establish an elevation query interface based on the features'
    // SRS. XXX This should be based on the style sheet option

    ElevationQuery eq(map.get());
    PointMap pointMap;

    for(auto& feature : towerFeatures)
    {
        Geometry* geom = feature->getGeometry();
        for(auto& pt : *geom)
        {
            getPoint(pointMap, pt) = PointEntry(feature);
        }
    }

    const SpatialReference* targetSRS = nullptr;
    if (cx.session()->isMapGeocentric())
        targetSRS = cx.session()->getMapSRS();
    else
        targetSRS = featureSRS->getGeocentricSRS();

    for(auto& feature : allPowerFeatures)
    {
        Geometry* geom = feature->getGeometry();

        if (geom->isLinear() && geom->size() >= 2)
        {
            const int size = geom->size();
            std::vector<osg::Matrixd> towerMats;
           
            towerMats.reserve(size);

            for (int i = 0; i < size; ++i)
            {
                double heading = 0.0;
                PointMap::iterator itr = findPoint(pointMap, (*geom)[i]);
                if (itr != pointMap.end())
                {
                    heading = itr->second.pointFeature->getDouble("heading", 0.0);
                    const osg::Vec3d Z(0.0, 0.0, 1.0);
                    osg::Matrixd headingMat = osg::Matrixd::rotate(osg::DegreesToRadians(heading), Z);
                    osg::Matrixd geodMat = getLocalToWorld(itr->first, featureSRS.get(), targetSRS);
                    towerMats.push_back(headingMat * geodMat);
                }
                else
                {
                    OE_NOTICE << LC << "tower not found!" << std::endl;
                    //break;
                }
            }

            PowerlineLayer::ModelOptions featureRenderData = evalTowerModel(feature, cx);

            Style localStyle;
            if (_cableStyleExpr.isSet())
            {
                evalStyle(feature, cx, _cableStyleExpr.get(), localStyle);
                setCableStyleDefaults(localStyle);
            }
            const Style& styleRef = _cableStyleExpr.isSet() ? localStyle : cableStyle;

            // calculate the number of cables to render:
            unsigned numCables = 0;
            if (_numCablesExpr.isSet())
                numCables = _numCablesExpr->eval(feature, cx);
            else
                numCables = feature->getInt("cables", featureRenderData.attachment_points().size());

            // For various reasons the headings of successive towers can be inconsistant, causing
            // the cables between attachment points to cross each other. Ideally, the points are
            // specified in pairs. If the attachment point being used causes a cable to cross over
            // the center line between towers, then switch to the other attachment point. If the
            // attachment points are not in pairs... awkward...

            unsigned count = 0;

            Array::View<osg::Vec3d> attachments(featureRenderData.attachment_points().data(),
                                                featureRenderData.attachment_points().size() / 2, 2);

            for (int attachRow = 0; attachRow < featureRenderData.attachment_points().size() / 2 && count < numCables; ++attachRow)
            {
                for (int startingAttachment = 0; startingAttachment < 2 && count < numCables; ++startingAttachment, ++count)
                {
                    // New feature for each cable
                    Feature* newFeature = new Feature(*feature);
                    LineString* newGeom = new LineString(towerMats.size());
                    int currAttachment = startingAttachment;
                    std::vector<osg::Vec3d> cablePoints;
                    cablePoints.push_back(attachments(attachRow, currAttachment) * towerMats[0]);
                    for (int i = 1; i < towerMats.size(); ++i)
                    {
                        // This function attempts to resolve the (rare) issue where lines cross between towers.
                        // While is does prevent crossing, it also has the side effect of putting all the 
                        // cables on the same side of the towers often.
                        int next = chooseAttachment(towerMats[i - 1], towerMats[i],
                                                    attachments(attachRow, currAttachment),
                                                    attachments(attachRow, 0), attachments(attachRow, 1));

                        osg::Vec3d worldAttach = attachments(attachRow, next) * towerMats[i];
                        cablePoints.push_back(worldAttach);
                        currAttachment = next;
                    }
                    const bool catenary = true;
                    std::vector<osg::Vec3d> *cableSource = 0L;
                    std::vector<osg::Vec3d> catenaryPoints;
                    if (catenary)
                    {
                        for (int i = 0; i < cablePoints.size() -1; ++i)
                        {
                            makeCatenary(cablePoints[i], cablePoints[i + 1], towerMats[i], 1.002, featureRenderData.maxSag().get(),
                                         catenaryPoints,
                                         styleRef.get<LineSymbol>()->tessellationSize()->as(Units::METERS));
                        }
                        cableSource = &catenaryPoints;
                    }
                    else
                    {
                        cableSource = &cablePoints;
                    }

                    for(auto& point : *cableSource)
                    {
                        osg::Vec3d wgs84, mapAttach;
                        featureSRS->getGeographicSRS()->transformFromWorld(point, wgs84);
                        featureSRS->getGeographicSRS()->transform(wgs84, featureSRS.get(), mapAttach);
                        newGeom->push_back(mapAttach);
                    }

                    newFeature->setGeometry(newGeom);
                    result.push_back(newFeature);
                }
            }
        }
    }
    return result;
}

namespace
{
    // todo: move this into FeatureCursor header
    class PredicateCursor : public FeatureCursor
    {
    public:
        using Predicate = std::function<bool(Feature*)>;

        PredicateCursor(FeatureCursor* cursor, Predicate predicate)
            : FeatureCursor(), _cursor(cursor), _accept(predicate)
        {
            fetch();
        }

        bool hasMore() const override {
            return _next.valid();
        }

        Feature* nextFeature() override {
            osg::ref_ptr<Feature> result = _next.get();
            fetch();
            return result.release();
        }

        void fetch() {
            while (_cursor->hasMore()) {
                _next = _cursor->nextFeature();
                if (_accept(_next.get()))
                    return;
            }
            _next = nullptr;
        }
    private:
        Predicate _accept;
        osg::ref_ptr<FeatureCursor> _cursor;
        osg::ref_ptr<Feature> _next;
    };
}

bool PowerlineFeatureNodeFactory::createOrUpdateNode(
    FeatureCursor* cursor,
    const Style& style,
    const FilterContext& context,
    osg::ref_ptr<osg::Node>& node,
    const Query& query)
{
    FilterContext sharedCX = context;
    FeatureList workingSet; 

    if (_powerlineOptions.inferTowerLocations() == true)
    {
        // collect just the lines, toss the points
        cursor->fill(workingSet, [&](const Feature* feature) {
            return feature->getGeometry()->isLinear();
        });

        // add back in a point-set version of each line
        int count = 0;
        for (auto& feature : workingSet)
        {
            count += feature->getGeometry()->size();
        }
        FeatureList towerFeatures;
        towerFeatures.reserve(count);

        for (auto& feature : workingSet)
        {
            GeometryIterator iter(feature->getGeometry(), false);
            iter.forEach([&](Geometry* geom)
                {
                    for (auto& p : *geom)
                    {
                        auto point = new Point();
                        point->push_back(p);
                        towerFeatures.push_back(new Feature(point, feature->getSRS()));
                    }
                });
        }
        workingSet.reserve(workingSet.size() + towerFeatures.size());
        for (auto& f : towerFeatures)
            workingSet.push_back(f);
    }
    else
    {
        // assume the feature input includes tower locations as points
        cursor->fill(workingSet);
    }

    Style cableStyle;

    if (!_cableStyleExpr.isSet())
    {        
        if (const Style* sessionCableStyle = context.session()->styles()->getStyle("cables"))
        {
            cableStyle = *sessionCableStyle;
        }
        else
        {
            // defaults
            auto* lineSymbol = cableStyle.getOrCreateSymbol<LineSymbol>();
            lineSymbol->stroke().mutable_value().color() = Color("#6f6f6f");
            lineSymbol->stroke().mutable_value().width() = Distance(1.5f, Units::PIXELS);
            lineSymbol->useGLLines() = true;
        }

        auto* lineSymbol = cableStyle.getOrCreateSymbol<LineSymbol>();

        if (!lineSymbol->stroke()->width().isSet())
        {
            lineSymbol->stroke().mutable_value().width() = Distance(.05, Units::METERS);
        }

        if (!lineSymbol->tessellationSize().isSet())
        {
            lineSymbol->tessellationSize() = Distance(20, Units::METERS);
        }

        if (!lineSymbol->useWireLines().isSet())
        {
            lineSymbol->useWireLines() = true;
        }
    }

    // Render towers and lines (cables) seperately
    // Features for the tower models. This normally comes from feature
    // data in a layer, but it can be synthesized using only the line
    // features.
    FeatureList pointSet;
    FilterContext localCX = sharedCX;

    // Get all the data ready.
    preparePowerFeatures(workingSet, localCX, _powerlineOptions.combineLines().value());

    // collect just the point features.
    for(auto& feature : workingSet)
    {
        if (feature->getGeometry()->isPointSet())
        {
            pointSet.push_back(feature);
        }
    }

    // clamp them to the terrain manually.
    Style temp_style;
    auto* alt = temp_style.getOrCreate<AltitudeSymbol>();
    alt->clamping() = alt->CLAMP_TO_TERRAIN;
    AltitudeFilter clamper;
    clamper.setPropertiesFromStyle(temp_style);
    clamper.push(pointSet, localCX);   


    osg::ref_ptr<osg::Node> pointsNode;
    osg::ref_ptr<FeatureListCursor> listCursor = new FeatureListCursor(pointSet);
    StyleSheet* sheet = context.session()->styles();

    // The tower model should come from the "model" attribute of
    // tower_model if it is not specified in the tower model style
    // (usually called "towers"). If the tower_model is chosen
    // dynamically, the model set there still needs to be the default
    // in the model style.
    auto& selectors = sheet->getSelectors();
    bool useSelectorExp = !selectors.empty() && selectors.begin()->second.styleExpression().isSet();
    Style combinedStyle;

    // Create the graph for the tower models.
    if (useSelectorExp || _powerlineOptions.towerExpr().isSet())
    {
        // The style is different for each feature, either explicitly
        // due to the style selector, or implicitly from a tower_model
        // expression that selects different models.
        osg::Group* towersNode = new osg::Group;
        pointsNode = towersNode;
        while (listCursor->hasMore())
        {
            osg::ref_ptr<osg::Node> pointsNode;
            Feature* f = listCursor->nextFeature();

            if (useSelectorExp)
            {
                const StyleSelector& sel = selectors.begin()->second;
                auto styleString = sel.styleExpression()->eval(f, context);
                if (!styleString.empty() && styleString != "null")
                {
                    // resolve the style:
                    // if the style string begins with an open bracket, it's an inline style definition.
                    if (styleString.length() > 0 && styleString[0] == '{')
                    {
                        Config conf("style", styleString);
                        conf.setReferrer(sel.styleExpression()->referrer());
                        conf.set("type", "text/css");
                        combinedStyle = Style(conf);
                    }

                    // otherwise, look up the style in the stylesheet. Do NOT fall back on a default
                    // style in this case: for style expressions, the user must be explicity about
                    // default styling; this is because there is no other way to exclude unwanted
                    // features.
                    else
                    {
                        const Style* selectedStyle = context.session()->styles()->getStyle(styleString, false);
                        if (selectedStyle)
                            combinedStyle = *selectedStyle;
                    }
                }
                setModelStyleDefaults(combinedStyle, false);
            }
            else
            {
                PowerlineLayer::ModelOptions modelOptions = evalTowerModel(f, context);
                if (modelOptions.uri().isSet())
                {
                    setModelStyleDefaults(combinedStyle, modelOptions.uri().get(), _powerlineOptions.referrer, true);
                }
            }

            if (!combinedStyle.empty())
            {
                osg::ref_ptr<osg::Node> towerNode;
                osg::ref_ptr<Feature> featureRef(f);
                FeatureList flist = {featureRef};
                osg::ref_ptr<FeatureListCursor> towerCursor = new FeatureListCursor(flist);

                GeomFeatureNodeFactory::createOrUpdateNode(towerCursor.get(), combinedStyle, localCX, towerNode, query);
                towersNode->addChild(towerNode.get());
            }
        }
    }
    else
    {
        PowerlineLayer::ModelOptions modelOptions = _powerlineOptions.towerModels().front();
        const Style* sessionTowerStyle = context.session()->styles()->getStyle("towers", false);

        if (sessionTowerStyle)
        {
            combinedStyle = *sessionTowerStyle;
        }
        if (modelOptions.uri().isSet())
        {
            setModelStyleDefaults(combinedStyle, modelOptions.uri().get(), _powerlineOptions.referrer, true);
        }
        else
        {
            setModelStyleDefaults(combinedStyle);
        }

        osg::ref_ptr<FeatureCursor> towersInExtentCursor = new PredicateCursor(
            listCursor.get(),
            [&](Feature* f) { 
                return localCX.workingExtent()->intersects(f->getExtent());
            });

        // This has the side effect of updating the elevations of the point features according to the
        // model style sheet. We rely on this in makeCableFeatures().
        // FOR THIS REASON we cannot just remove the towers that are outside the working extent
        // yet. Gotta find a workaround.
        GeomFeatureNodeFactory::createOrUpdateNode(towersInExtentCursor.get(), combinedStyle, localCX, pointsNode, query);
    }

    osg::ref_ptr<osg::Group> results(new osg::Group);
    results->addChild(pointsNode.get());

    FeatureList cableFeatures = makeCableFeatures(workingSet, pointSet, localCX, query, cableStyle);

    GeometryCompiler compiler;
    if (_cableStyleExpr.isSet())
    {
        for(auto& feature : cableFeatures)
        {
            Style localStyle;
            evalStyle(feature.get(), localCX, _cableStyleExpr.get(), localStyle);
            setCableStyleDefaults(localStyle);
            osg::Node* cable = compiler.compile(feature.get(), localStyle, localCX);
            results->addChild(cable);
        }
    }
    else
    {
        osg::Node* cables = compiler.compile(cableFeatures, cableStyle, localCX);
        results->addChild(cables);
    }

    node = results;

    return true;
}
