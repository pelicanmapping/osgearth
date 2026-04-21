/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/SubstituteModelFilter>
#include <osgEarth/FeatureSourceIndexNode>
#include <osgEarth/FilterContext>

#include <osgEarth/MeshFlattener>
#include <osgEarth/StyleSheet>

#include <osgEarth/ECEF>
#include <osgEarth/DrawInstanced>
#include <osgEarth/ScreenSpaceLayout>
#include <osgEarth/CullingUtils>
#include <osgEarth/NodeUtils>

#include <osg/AutoTransform>
#include <osg/Drawable>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Billboard>

#include <vector>

#define LC "[SubstituteModelFilter] "

#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif

using namespace osgEarth;
using namespace osgEarth::Util;

//------------------------------------------------------------------------

namespace
{
    static osg::Node* s_defaultModel = 0L;

    struct SetSmallFeatureCulling : public osg::NodeCallback
    {
        bool _value;
        SetSmallFeatureCulling(bool value) : _value(value) { }
        void operator()(osg::Node* node, osg::NodeVisitor* nv) {
            Culling::asCullVisitor(nv)->setSmallFeatureCullingPixelSize(-1.0f);
            traverse(node, nv);
        }
    };

    void transpose3x3(const osg::Matrixd& in, osg::Matrixd& out)
    {
#if OSG_VERSION_LESS_THAN(3,6,0)
        if (&in == &out)
        {
            osg::Matrixd temp(out);
            transpose3x3(in, temp);
            out = temp;
        }
        else
        {
            out(0, 0) = in(0, 0);
            out(0, 1) = in(1, 0);
            out(0, 2) = in(2, 0);
            out(1, 0) = in(0, 1);
            out(1, 1) = in(1, 1);
            out(1, 2) = in(2, 1);
            out(2, 0) = in(0, 2);
            out(2, 1) = in(1, 2);
            out(2, 2) = in(2, 2);
        }
#else
        out.transpose3x3(in);
#endif
    }
}

//------------------------------------------------------------------------

SubstituteModelFilter::SubstituteModelFilter(const Style& style) :
    _style(style),
    _cluster(false),
    _useDrawInstanced(true),
    _merge(true),
    _normalScalingRequired(false),
    _instanceCache(128u),
    _filterUsage(FILTER_USAGE_NORMAL)
{
    //NOP
}

bool
SubstituteModelFilter::findResource(const URI&            uri,
    const InstanceSymbol* symbol,
    FilterContext&        context,
    std::set<URI>&        missing,
    osg::ref_ptr<InstanceResource>& output)
{

    // be careful about refptrs here since _instanceCache is an LRU.
    auto cached = _instanceCache.get(uri);
    
    if (cached.has_value())
    {
        // found it in the cache:
        output = cached.value().get();
    }
    else if (_resourceLib.valid())
    {
        // look it up in the resource library:
        output = _resourceLib->getInstance(uri.base(), context.getDBOptions());
    }
    else
    {
        // create it on the fly:
        output = symbol->createResource();

        if (!uri.empty())
        {
            output->uri() = uri;
            _instanceCache.insert(uri, output.get());
        }
        else if (symbol->asModel())
        {
            output->node() = symbol->asModel()->getModel();
        }
    }

    // failed to find the instance.
    if (!output.valid())
    {
        if (missing.find(uri) == missing.end())
        {
            missing.insert(uri);
            OE_WARN << LC << "Failed to locate resource: " << uri.full() << std::endl;
        }
    }

    return output.valid();
}

namespace
{
// Extract values from an array (e.g. a row of a matrix) into a Vec3d

osg::Vec3d getVec3d(double* val)
{
    return osg::Vec3d(val[0], val[1], val[2]);
}

void calculateGeometryHeading(Feature* input, FilterContext& context)
{
    if (input->hasAttr("node-headings"))
        return;
    std::vector<double> headings;
    const SpatialReference* targetSRS = 0L;
    if (context.session()->isMapGeocentric())
    {
        targetSRS = context.session()->getMapSRS();
    }
    else
    {
        targetSRS = context.featureProfile()->getSRS()->getGeocentricSRS();
    }
    GeometryIterator gi( input->getGeometry(), false );
    while( gi.hasMore() )
    {
        Geometry* geom = gi.next();
        if (geom->getType() != Geometry::TYPE_LINESTRING)
            break;
        std::vector<osg::Vec3d> worldPts(geom->size());
        std::vector<osg::Matrixd> orientations(geom->size());
        for(unsigned i = 0; i < geom->size(); ++i)
        {
            ECEF::transformAndGetRotationMatrix((*geom)[i], context.featureProfile()->getSRS(), worldPts[i],
                                                targetSRS, orientations[i]);
        }

        for(unsigned i = 0; i < geom->size(); ++i)
        {
            // XXX
            osg::Matrixd toLocal(orientations[i]);
            transpose3x3(toLocal, toLocal);
            osg::Vec3d backWorld, forwardWorld, back, forward;
            // Project vectors into and out of point into the local
            // tangent plane
            if (i > 0)
            {
                backWorld = worldPts[i - 1] - worldPts[i];
                back = backWorld * toLocal;
                back.z() = 0.0;
                back.normalize();
            }
            if (i < geom->size())
            {
                forwardWorld = worldPts[i + 1] - worldPts[i];
                forward = forwardWorld * toLocal;
                forward.z() = 0.0;
                forward.normalize();
            }
            // The half vector points "half way" between the in and
            // out vectors. If they are parallel, then it will have 0
            // length.
            double heading;
            if (i > 0 && i < geom->size())
            {
                osg::Vec3d half = back + forward;
                double halfLen = half.normalize();

                if (osg::equivalent(halfLen, 0.0))
                {
                    heading = std::atan2(-forward[0], forward[1]);
                }
                else
                {
                    // Heading we want is rotated 90 degrees from half vector
                    heading = std::atan2(-half[1], -half[0]);
                }
            }
            else if (i == 0)
            {
                heading = std::atan2(-forward[0], forward[1]);
            }
            else
            {
                heading = std::atan2(forward[0], -forward[1]);
            }
            headings.push_back(osg::RadiansToDegrees(heading));
        }
    }
    if (!headings.empty())
    {
        std::stringstream buf;
        for(auto& heading : headings)
            buf << std::to_string(heading);
        input->set("node-headings", buf.str());
    }
}
}

bool
SubstituteModelFilter::process(const FeatureList&           features,
    const InstanceSymbol*        symbol,
    Session*                     session,
    osg::Group*                  attachPoint,
    FilterContext&               context)
{
    // Establish SRS information:
    bool makeECEF = context.session()->isMapGeocentric();
    const SpatialReference* targetSRS = context.session()->getMapSRS();

    // first, go through the features and build the model cache. Apply the model matrix' scale
    // factor to any AutoTransforms directly (cloning them as necessary)
    std::map< std::pair<std::string, float>, osg::ref_ptr<osg::Node> > uniqueModels;

    SubstituteModelFilterNode* substituteModelFilterNode = 0;

    if (_filterUsage == FILTER_USAGE_ZERO_WORK_CALLBACK_BASED)
    {
        substituteModelFilterNode = osgEarth::findTopMostNodeOfType<SubstituteModelFilterNode>(attachPoint);
        if (substituteModelFilterNode == nullptr)
        //if (attachPoint->getUserData() == 0)
        {
            substituteModelFilterNode = new SubstituteModelFilterNode();
            substituteModelFilterNode->setDataVariance(osg::Object::DYNAMIC);
            attachPoint->addChild(substituteModelFilterNode);
            //attachPoint->setUserData(substituteModelFilterNode);

            bool instancing = getUseDrawInstanced() == true && getClustering() == false;
            bool clustering = getClustering() == true && getUseDrawInstanced() == false;
            if (!(instancing || clustering))
            {
                // prefer instancing over clustering
                instancing = true;
                clustering = false;
            }

            substituteModelFilterNode->setInstanced(instancing);
            substituteModelFilterNode->setClustered(clustering);
        }
    }

    // URI cache speeds up URI creation since it can be slow.
    std::unordered_map<std::string, URI> uriCache;

    // keep track of failed URIs so we don't waste time or warning messages on them
    std::set< URI > missing;

    const ModelSymbol* modelSymbol = dynamic_cast<const ModelSymbol*>(symbol);
    const IconSymbol*  iconSymbol = dynamic_cast<const IconSymbol*> (symbol);

    for(auto& input : features)
    {
        // Run a feature pre-processing script.
        if (symbol->script().isSet())
        {
            symbol->script()->eval(input, context);
        }

        // calculate the orientation of each element of the feature
		// geometry, if needed.
        if ( modelSymbol && modelSymbol->orientationFromFeature().get() )
        {
            calculateGeometryHeading(input, context);
        }

        // evaluate the instance URI expression:
        std::string resourceKey;
        if (symbol->url().isSet())
        {
            resourceKey = symbol->url()->eval(input, context).full();
        }
        else if (modelSymbol && modelSymbol->getModel())
        {
            resourceKey = Stringify() << modelSymbol->getModel();
        }
        else if (iconSymbol && iconSymbol->getImage())
        {
            resourceKey = Stringify() << iconSymbol->getImage();
        }

        URI instanceURI;
        if (symbol->url().isSet())
        {
            instanceURI = uriCache[resourceKey];
            if (instanceURI.empty()) // Create a map, to reuse URI's, since they take a long time to create
            {
                instanceURI = URI(resourceKey, symbol->url()->referrer());
            }
        }

        // find the corresponding marker in the cache
        osg::ref_ptr<InstanceResource> instance;
        if (!findResource(instanceURI, symbol, context, missing, instance))
            continue;

        // evalute the scale expression (if there is one)
        float scale = 1.0f;
        osg::Vec3d scaleVec(1.0, 1.0, 1.0);
        osg::Matrixd scaleMatrix;
        if (symbol->scale().isSet())
        {
            scale = symbol->scale()->eval(input, context);
            scaleVec.set(scale, scale, scale);
        }
        if (modelSymbol)
        {
            if (modelSymbol->scaleX().isSet())
            {
                scaleVec.x() *= modelSymbol->scaleX()->eval(input, context);
            }
            if (modelSymbol->scaleY().isSet())
            {
                scaleVec.y() *= modelSymbol->scaleY()->eval(input, context);
            }
            if (modelSymbol->scaleZ().isSet())
            {
                scaleVec.z() *= modelSymbol->scaleZ()->eval(input, context);
            }
        }

        if (scaleVec.x() == 0.0) scaleVec.x() = 1.0;
        if (scaleVec.y() == 0.0) scaleVec.y() = 1.0;
        if (scaleVec.z() == 0.0) scaleVec.z() = 1.0;

        scaleMatrix = osg::Matrix::scale( scaleVec );
        
        osg::Matrixd headingRotation;
        std::vector<double> headingArray;

        if ( modelSymbol )
        {
            if ( modelSymbol->orientationFromFeature().get() )
            {
                if (input->hasAttr("node-headings"))
                {
                    std::string value = input->getString("node-headings");
                    auto values = StringTokenizer().delim(",").tokenize(value);
                    for (auto& value : values)
                        headingArray.emplace_back(std::atof(value.c_str()));
                }
                else if (input->hasAttr("heading"))
                {
                    float heading = input->getDouble("heading");
                    headingRotation.makeRotate( osg::Quat(osg::DegreesToRadians(heading), osg::Vec3(0,0,1)) );
                }
            }
            else if ( modelSymbol->heading().isSet() )
            {
                float heading = modelSymbol->heading()->eval(input, context);
                headingRotation.makeRotate( osg::Quat(osg::DegreesToRadians(heading), osg::Vec3(0,0,1)) );
            }
        }

        // now that we have a marker source, create a node for it
        std::pair<std::string,float> key( resourceKey, iconSymbol? scale : 1.0f ); //use 1.0 for models, since we don't want unique models based on scaling

        // cache nodes per instance.
        osg::ref_ptr<osg::Node> model;

        if (_filterUsage == FILTER_USAGE_NORMAL)
        {
            //This is a not so obvious way of writing to the map.
            // Notice the & in the definition of modeRefOfRefPtr
            osg::ref_ptr<osg::Node>& unique_model = uniqueModels[key];

            if (!unique_model.valid())
            {
                // Always clone the cached instance so we're not processing data that's
                // already in the scene graph. -gw
                context.resourceCache()->cloneOrCreateInstanceNode(instance.get(), unique_model, context.getDBOptions());

                // if icon decluttering is off, install an AutoTransform.
                if (iconSymbol)
                {
                    if (iconSymbol->declutter() == true)
                    {
                        ScreenSpaceLayout::activate(unique_model->getOrCreateStateSet());
                    }
                    else if (dynamic_cast<osg::AutoTransform*>(unique_model.get()) == 0L)
                    {
                        osg::AutoTransform* at = new osg::AutoTransform();
                        at->setAutoRotateMode(osg::AutoTransform::ROTATE_TO_SCREEN);
                        at->setAutoScaleToScreen(true);
                        at->addChild(unique_model);
                        unique_model = at;
                    }
                }
            }
            model = unique_model;
        }

        if ((_filterUsage == FILTER_USAGE_NORMAL && model.valid()) || (_filterUsage == FILTER_USAGE_ZERO_WORK_CALLBACK_BASED))
        {
            GeometryIterator gi( input->getGeometry(), false );
            int pointIdx = 0;
            while( gi.hasMore() )
            {
                Geometry* geom = gi.next();

                // if necessary, transform the points to the target SRS:
                if ( !makeECEF && !targetSRS->isEquivalentTo(context.featureProfile()->getSRS()) )
                {
                    context.featureProfile()->getSRS()->transform( geom->asVector(), targetSRS );
                }

                for (unsigned i = 0; i < geom->size(); ++i)
                {
                    osg::Matrixd mat;

                    // need to recalculate expression-based data per-point, not just per-feature!
                    float scale = 1.0f;
                    osg::Vec3d scaleVec(1.0, 1.0, 1.0);
                    osg::Matrixd scaleMatrix;
                    if (symbol->scale().isSet())
                    {
                        scale = symbol->scale()->eval(input, context);
                        scaleVec.set(scale, scale, scale);
                    }
                    if (modelSymbol)
                    {
                        if (modelSymbol->scaleX().isSet())
                        {
                            scaleVec.x() *= modelSymbol->scaleX()->eval(input, context);
                        }
                        if (modelSymbol->scaleY().isSet())
                        {
                            scaleVec.y() *= modelSymbol->scaleY()->eval(input, context);
                        }
                        if (modelSymbol->scaleZ().isSet())
                        {
                            scaleVec.z() *= modelSymbol->scaleZ()->eval(input, context);
                        }
                    }

                    if (scaleVec.x() == 0.0) scaleVec.x() = 1.0;
                    if (scaleVec.y() == 0.0) scaleVec.y() = 1.0;
                    if (scaleVec.z() == 0.0) scaleVec.z() = 1.0;

                    scaleMatrix = osg::Matrix::scale(scaleVec);

                    if ( modelSymbol && !headingArray.empty() && geom->getType() == Geometry::TYPE_LINESTRING)
                    {
                        headingRotation.makeRotate(osg::Quat(osg::DegreesToRadians(headingArray[pointIdx++]),
                                                             osg::Vec3(0,0,1)));
                    }

                    osg::Vec3d point = (*geom)[i];

                    if (makeECEF)
                    {
                        // the "rotation" element lets us re-orient the instance to ensure it's pointing up. We
                        // could take a shortcut and just use the current extent's local2world matrix for this,
                        // but if the tile is big enough the up vectors won't be quite right.
                        osg::Matrixd upRotation;
                        ECEF::transformAndGetRotationMatrix( point, context.featureProfile()->getSRS(), point, targetSRS, upRotation );
                        mat = scaleMatrix * headingRotation * upRotation * osg::Matrixd::translate(point); // *_world2local;
                    }
                    else
                    {
                        mat = scaleMatrix * headingRotation * osg::Matrixd::translate(point); //* _world2local;
                    }

                    if (_filterUsage == FILTER_USAGE_NORMAL)
                    {
                        mat = mat*_world2local;

                        osg::MatrixTransform* xform = new osg::MatrixTransform();
                        xform->setMatrix(mat);
                        xform->setDataVariance(osg::Object::STATIC);
                        xform->addChild(model.get());
                        attachPoint->addChild(xform);

                        // Only tag nodes if we aren't using clustering.
                        if (context.featureIndex() && !_cluster)
                        {
                            context.featureIndex()->tagNode(xform, input);
                        }

                        // name the feature if necessary
                        if (_featureNameExpr)
                        {
                            auto name = _featureNameExpr.eval(input, context);
                            if (!name.empty())
                                xform->setName(name);
                        }
                    }
                    else if (_filterUsage == FILTER_USAGE_ZERO_WORK_CALLBACK_BASED)
                    {
                        //osg::Vec3d modelPoint = point;

                        substituteModelFilterNode->modelSymbolList().push_back(SubstituteModelFilterNode::ModelSymbol());
                        SubstituteModelFilterNode::ModelSymbol& symbol = substituteModelFilterNode->modelSymbolList().back();
                        symbol.instanceURI = instanceURI;
                        symbol.xform = mat;

                    }
                }
            }
        }
    }

    if (iconSymbol)
    {
        // activate decluttering for icons if requested
        if (iconSymbol->declutter() == true)
        {
            ScreenSpaceLayout::activate(attachPoint->getOrCreateStateSet());
        }

        // activate horizon culling if we are in geocentric space
        if ( context.session() && context.session()->isMapGeocentric() )
        {
            // should this use clipping, or a horizon cull callback?

            //HorizonCullingProgram::install( attachPoint->getOrCreateStateSet() );

            attachPoint->getOrCreateStateSet()->setMode(GL_CLIP_DISTANCE0, 1);
        }
    }

    // active DrawInstanced if required:
    if (_useDrawInstanced && _filterUsage == FILTER_USAGE_NORMAL)
    {
        DrawInstanced::convertGraphToUseDrawInstanced(attachPoint);

        // install a shader program to render draw-instanced.
        DrawInstanced::install(attachPoint->getOrCreateStateSet());
    }

    return true;
}




namespace
{
    /**
    * Extracts unclusterable things like lightpoints and billboards from the given scene graph and copies them into a cloned scene graph
    * This actually just removes all geodes from the scene graph, so this could be applied to any other type of node that you want to keep
    * The geodes will be clustered together in the flattened graph.
    */
    osg::Node* extractUnclusterables(osg::Node* node)
    {
        // Clone the scene graph
        osg::ref_ptr< osg::Node > clone = (osg::Node*)node->clone(osg::CopyOp::DEEP_COPY_NODES);

        // Now remove any geodes
        osgEarth::FindNodesVisitor<osg::Geode> findGeodes;
        clone->accept(findGeodes);
        for (unsigned int i = 0; i < findGeodes._results.size(); i++)
        {
            osg::ref_ptr< osg::Geode > geode = findGeodes._results[i];


            // Special check for billboards.  Me want to keep them in this special graph of 
            // unclusterable stuff.
            osg::Billboard* billboard = dynamic_cast< osg::Billboard* >(geode.get());


            if (geode->getNumParents() > 0 && !billboard)
            {
                // Get all the parents for the geode and remove it from them.
                std::vector< osg::ref_ptr< osg::Group > > parents;
                for (unsigned int j = 0; j < geode->getNumParents(); j++)
                {
                    parents.push_back(geode->getParent(j));
                }

                for (unsigned int j = 0; j < parents.size(); j++)
                {
                    parents[j]->removeChild(geode);
                }
            }

        }

        return clone.release();
    };
}

osg::Node*
SubstituteModelFilter::push(FeatureList& features, FilterContext& context)
{
    if (!isSupported())
    {
        OE_WARN << "SubstituteModelFilter support not enabled" << std::endl;
        return 0L;
    }

    if (_style.empty())
    {
        OE_WARN << LC << "Empty style; cannot process features" << std::endl;
        return 0L;
    }

    osg::ref_ptr<const InstanceSymbol> symbol = _style.get<InstanceSymbol>();

    if (!symbol.valid())
    {
        OE_WARN << LC << "No appropriate symbol found in stylesheet; aborting." << std::endl;
        return 0L;
    }

    // establish the resource library, if there is one:
    _resourceLib = 0L;

    const StyleSheet* sheet = context.session() ? context.session()->styles() : 0L;

    if (sheet && symbol->library().isSet())
    {
        _resourceLib = sheet->getResourceLibrary( symbol->library().value() );

        if (!_resourceLib.valid())
        {
            OE_WARN << LC << "Unable to load resource library '" << symbol->library().value() << "'"
                << "; may not find instance models." << std::endl;
        }
    }

    // reset this marker:
    _normalScalingRequired = false;

    // Compute localization info:
    FilterContext newContext(context);

    computeLocalizers(context);

    osg::Group* group = createDelocalizeGroup();

    osg::ref_ptr< osg::Group > attachPoint = new osg::Group;
    group->addChild(attachPoint.get());

    // Process the feature set, using clustering if requested
    bool ok = true;

    process( features, symbol.get(), context.session(), attachPoint.get(), newContext );
    if (_filterUsage == FILTER_USAGE_NORMAL && _cluster)
    {
        // Extract the unclusterable things
        osg::ref_ptr< osg::Node > unclusterables = extractUnclusterables(attachPoint.get());

        // We run on the attachPoint instead of the main group so that we don't lose the double precision delocalizer transform.
        MeshFlattener::run(attachPoint.get());

        // Add the unclusterables back to the attach point after the rest of the graph was flattened.
        if (unclusterables.valid())
        {
            attachPoint->addChild(unclusterables);
        }
    }

    // return proper context
    context = newContext;

    return group;
}



namespace
{
    static bool check_modelSymbolList(const SubstituteModelFilterNode& node)
    {
        return node.modelSymbolList().size() > 0;
    }

    static bool read_modelSymbolList(osgDB::InputStream& is, SubstituteModelFilterNode& node)
    {
        bool clustered;
        bool instanced;
        is >> clustered;
        is >> instanced;
        node.setClustered(clustered);
        node.setInstanced(instanced);

        unsigned int size = 0; is >> size >> is.BEGIN_BRACKET;
        for (unsigned int i = 0; i < size; ++i)
        {
            SubstituteModelFilterNode::ModelSymbol model;
            std::string base;
            is.readWrappedString(base);
            std::string context;
            is.readWrappedString(context);
            model.instanceURI = URI(base, context);

            is >> model.xform;
            node.modelSymbolList().push_back(model);
        }
        is >> is.END_BRACKET;
        return true;
    }

    static bool write_modelSymbolList(osgDB::OutputStream& os, const SubstituteModelFilterNode& node)
    {
        os << node.getClustered();
        os << node.getInstanced();

        unsigned int size = node.modelSymbolList().size();
        os << size << os.BEGIN_BRACKET << std::endl;

        for (SubstituteModelFilterNode::ModelSymbolList::const_iterator iter = node.modelSymbolList().begin(); iter != node.modelSymbolList().end(); ++iter)
        {

            const SubstituteModelFilterNode::ModelSymbol& model = (*iter);
            os.writeWrappedString(model.instanceURI.base());
            os.writeWrappedString(model.instanceURI.context().referrer());

            os << model.xform;
        }
        os << os.END_BRACKET << std::endl;
        return true;
    }
}

REGISTER_OBJECT_WRAPPER(SubstituteModelFilterNode,
    new SubstituteModelFilterNode,
    osgEarth::SubstituteModelFilterNode,
    "osg::Node osgEarth::SubstituteModelFilterNode")
{
    //ADD_BOOL_SERIALIZER(Instanced, false);
    //ADD_BOOL_SERIALIZER(Clustered, false);
    ADD_USER_SERIALIZER(_modelSymbolList);
}