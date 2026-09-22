/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include "ExampleResources"
#include "Shadowing"
#include "LogarithmicDepthBuffer"
#include "SimpleOceanLayer"
#include "Registry"
#include "MapNode"
#include "TerrainEngineNode"
#include "EarthManipulator"
#include "NodeUtils"
#include "GLUtils"
#include "CullingUtils"
#include "Sky"

#include <osgDB/ReadFile>
#include <osgGA/StateSetManipulator>
#include <osgViewer/ViewerEventHandlers>

#define VP_MIN_DURATION      2.0     // minimum fly time.
#define VP_METERS_PER_SECOND 2500.0  // fly speed
#define VP_MAX_DURATION      2.0 //8.0     // maximum fly time.

using namespace osgEarth;
using namespace osgEarth::Util;

#undef  LC
#define LC "[MapNodeHelper] "

namespace
{
    struct MultiRealizeOperation : public osg::Operation
    {
        void operator()(osg::Object* obj) override
        {
            for (auto& op : _ops)
                op->operator()(obj);
        }
        std::vector<osg::ref_ptr<osg::Operation>> _ops;
    };
}

std::string
MapNodeHelper::usage() const
{
    return Stringify()
        << "  --sky                         : add a default sky model\n"
        << "  --sky2                        : replace sky with core SkyNode2 (--sky-low / --sky-high)\n"
        << "  --ortho                       : use an orthographic camera\n"
        << "  --shadows                     : activates model layer shadows\n"
        << "  --path [file]                 : load and playback an animation path\n"
        << "  --nologdepth                  : disables the logarithmic depth buffer\n"
        << "  --nogui                       : start with GUI hidden ('y' to show)\n"
        << "  --nvgl                        : use NVGL rendering path if available\n"
        << "  --osg-options [string]        : options to pass to osgDB::read* methods\n"
        << "  --no-cache                    : disable the cache if one is configured\n"
        << "  --cahce-only                  : only read data from the disk cache\n"
        << "  --gldebug                     : activate GL debug messages\n"
        << "  --novsync                     : disable vertical sync\n"
        << "  --tess                        : enable GPU tessellation\n"
        << "  --lodscale [float]            : set the OSG LOD scale factor (default is 1.0)\n";
    
}

osg::ref_ptr<osg::Node>
MapNodeHelper::load(osg::ArgumentParser& args, osgViewer::ViewerBase* viewer) const
{
    // Pause do the user can attach a debugger
    if (args.read("--pause"))
    {
        std::cout << "Press <ENTER> to continue" << std::endl;
        ::getchar();
    }

    // OSG options
    osg::ref_ptr<osgDB::Options> myReadOptions;
    std::string str;
    if (args.read("--osg-options", str) || args.read("-O", str))
    {
        myReadOptions = new osgDB::Options();
        myReadOptions->setOptionString(str);
    }

    // NVGL4 rendering?
    if (args.read("--nvgl") || args.read("--gl4") || args.read("--use-gl4"))
    {
        GLUtils::useNVGL(true);

        if (!myReadOptions.valid())
            myReadOptions = new osgDB::Options();

        myReadOptions->setOptionString(myReadOptions->getOptionString() + " OSGEARTH_USE_NVGL");
    }

    // terrain engine?
    std::string engine;
    if (args.read("--engine", engine))
    {
        Registry::instance()->overrideTerrainEngineDriverName() = engine;
    }

    // caching?
    if (args.read("--cache-only"))
    {
        auto cp = Registry::instance()->overrideCachePolicy().get();
        cp.usage() = CachePolicy::USAGE_CACHE_ONLY;
        Registry::instance()->setOverrideCachePolicy(cp);
    }

    if (args.read("--no-cache"))
    {
        Registry::instance()->setOverrideCachePolicy(CachePolicy::NO_CACHE);
    }

    // GL debugging stuff
    if (args.read("--gldebug") || args.read("--gl-debug"))
    {
        GLUtils::enableGLDebugging();
    }

    // collect the views
    osgViewer::Viewer::Views views;
    if (viewer)
    {
        viewer->getViews(views);
    }

    // configures each view with some stock goodies
    for (auto view : views)
    {
        configureView(view);
    }

    if (args.read("--headless"))
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits();
        traits->x = 0;
        traits->y = 0;
        traits->width = 16;
        traits->height = 16;
        traits->windowDecoration = false;
        traits->pbuffer = true;
        traits->doubleBuffer = true;
        traits->sharedContext = nullptr;

        osg::ref_ptr<osg::GraphicsContext> gc = osg::GraphicsContext::createGraphicsContext(traits.get());
        for (auto view : views)
        {
            view->getCamera()->setGraphicsContext(gc.get());
            view->getCamera()->setDrawBuffer(GL_BACK);
            view->getCamera()->setReadBuffer(GL_BACK);
            view->getCamera()->setViewport(new osg::Viewport(0, 0, traits->width, traits->height));
        }
    }

    // vsync on/off?
    optional<bool> vsync;
    if (args.read("--vsync"))
        vsync = true;
    else if (args.read("--novsync"))
        vsync = false;

    // VP debugging
    if (args.read("--vpdebug") || args.read("--vp-debug"))
    {
        GLUtils::enableGLDebugging();
        VirtualProgram::enableGLDebugging();
    }

    if (viewer)
    {
        MultiRealizeOperation* op = new MultiRealizeOperation();

        if (viewer->getRealizeOperation())
            op->_ops.push_back(viewer->getRealizeOperation());

        GL3RealizeOperation* rop = new GL3RealizeOperation();
        if (vsync.isSet())
            rop->setSyncToVBlank(vsync.get());

        op->_ops.push_back(rop);

        viewer->setRealizeOperation(op);
    }

    // read in the Earth file:
    osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFiles(args, myReadOptions.get());

    // fallback in case none is specified:
    if (!node.valid())
    {
        OE_WARN << LC << "No earth file loaded" << std::endl;
        return node;
    }

    osg::ref_ptr<MapNode> mapNode = MapNode::get(node.get());
    if (!mapNode.valid())
    {
        OE_WARN << LC << "Loaded scene graph does not contain a MapNode" << std::endl;
        return node;
    }

    // move any mapnode siblings under the mapnode.
    auto siblings = findSiblings(mapNode.get());
    for (auto& sibling : siblings)
    {
        mapNode->addChild(sibling);
        sibling->getParent(0)->removeChild(sibling);
    }

    // GPU tessellation?
    if (args.read("--tessellation") || args.read("--tess"))
    {
        mapNode->getTerrainOptions().setGPUTessellation(true);
    }

    // a root node to hold everything:
    osg::ref_ptr<osg::Group> root = new osg::Group();
    root->addChild(node);

    // open the map node:
    if (!mapNode->open())
    {
        OE_WARN << LC << "Failed to open MapNode" << std::endl;
        return nullptr;
    }

    // parses common cmdline arguments and apply to the first view:
    if (!views.empty())
    {
        parse(mapNode.get(), args, views.front(), root.get());

        float lodscale;
        if (args.read("--lodscale", lodscale))
        {
            LODScaleGroup* g = new LODScaleGroup();
            g->setLODScaleFactor(osg::maximum(lodscale, 0.0001f));
            osgEarth::insertGroup(g, mapNode->getParent(0));
            OE_DEBUG << "LOD Scale set to: " << lodscale << std::endl;
        }
    }

    return root;
}

void
MapNodeHelper::parse(MapNode* mapNode, osg::ArgumentParser& args, osgViewer::View* view, osg::Group* root) const
{
    if ( !root )
        root = mapNode;

    // parse out custom example arguments first:
    bool useLogDepth2  = args.read("--logdepth2");
    bool useLogDepth   = !args.read("--nologdepth") && !useLogDepth2;

    // animation path:
    std::string animpath;
    if ( args.read("--path", animpath) )
    {
        view->setCameraManipulator( new osgGA::AnimationPathManipulator(animpath) );
    }

    // vertical field of view:
    float vfov = -1.0f;
    if (args.read("--vfov", vfov) && vfov > 0.0f)
    {
        double fov, ar, n, f;
        view->getCamera()->getProjectionMatrixAsPerspective(fov, ar, n, f);
        view->getCamera()->setProjectionMatrixAsPerspective(vfov, ar, n, f);
    }

    // Configure for an ortho camera:
    if ( args.read("--ortho") )
    {
        EarthManipulator* em = dynamic_cast<EarthManipulator*>(view->getCameraManipulator());
        if (em)
        {
            double V, A, N, F;
            view->getCamera()->getProjectionMatrixAsPerspective(V, A, N, F);
            em->setInitialVFOV( V );
        }

        view->getCamera()->setProjectionMatrixAsOrtho(-1, 1, -1, 1, 0, 1);
    }

    // Install logarithmic depth buffer on main camera
    if ( useLogDepth )
    {
        OE_DEBUG << LC << "Activating logarithmic depth buffer (vertex-only) on main camera" << std::endl;
        osgEarth::Util::LogarithmicDepthBuffer logDepth;
        logDepth.setUseFragDepth( false );
        logDepth.install( view->getCamera() );
    }

    else if ( useLogDepth2 )
    {
        OE_DEBUG << LC << "Activating logarithmic depth buffer (precise) on main camera" << std::endl;
        osgEarth::Util::LogarithmicDepthBuffer logDepth;
        logDepth.setUseFragDepth( true );
        logDepth.install( view->getCamera() );
    }

    // Explicit core or legacy sky selection:
    if (mapNode)
    {
        bool sky2 = args.read("--sky2");
        SkyOptions::Quality sky_quality = SkyOptions::parseQuality(args);
        if (sky2 && sky_quality == SkyOptions::QUALITY_UNSET) sky_quality = SkyOptions::QUALITY_DEFAULT;

        if (sky_quality != SkyOptions::QUALITY_UNSET && mapNode->open())
        {
            if (sky2)
            {
                // A command-line replacement also supersedes sky extensions from the earth file.
                auto extensions = mapNode->getExtensions();
                for (auto& extension : extensions)
                    if (extension->as<SkyNodeFactory>()) mapNode->removeExtension(extension.get());
            }
            std::string ext = sky2 ? "sky2" : mapNode->getMapSRS()->isGeographic() ? "sky_simple" : "sky_gl";
            SkyOptions options;
            options.quality() = sky_quality;
            mapNode->addExtension(Extension::create(ext, options));
        }
    }

    // Simple ocean model:
    if (args.read("--ocean") && mapNode)
    {
        SimpleOceanLayer* layer = new SimpleOceanLayer();
        mapNode->getMap()->addLayer(layer);
    }

    // Arbitrary extension:
    std::string extname;
    if (args.read("--extension", extname) && mapNode)
    {
        Extension* ext = Extension::create(extname, ConfigOptions());
        if (ext)
            mapNode->addExtension(ext);
    }

    // Hook up the extensions!
    if (mapNode)
    {
        for(auto& extension : mapNode->getExtensions())
        {
            // Check for a View interface:
            ExtensionInterface<osg::View>* viewIF = ExtensionInterface<osg::View>::get(extension.get());
            if (viewIF)
                viewIF->connect(view);
        }
    }

    // Shadowing. This is last because it needs access to a light which may be provided
    // by one of the Sky extensions.
    if (args.read("--shadows") && mapNode)
    {
        int unit;
        if ( mapNode->getTerrainEngine()->getResources()->reserveTextureImageUnit(unit, "ShadowCaster") )
        {
            ShadowCaster* caster = new ShadowCaster();
            caster->setTextureImageUnit(unit);
            caster->setLight( view->getLight() );
            caster->getShadowCastingGroup()->addChild(mapNode->getLayerNodeGroup());
            caster->getShadowCastingGroup()->addChild(mapNode->getTerrainEngine()->getNode());
            if ( mapNode->getNumParents() > 0 )
            {
                osgEarth::insertGroup(caster, mapNode->getParent(0));
            }
            else
            {
                caster->addChild(mapNode);
                root = caster;
            }
        }
    }
}


void
MapNodeHelper::configureView( osgViewer::View* view ) const
{
    // default uniform values:
    GLUtils::setGlobalDefaults(view->getCamera()->getOrCreateStateSet());

    // disable small feature culling (otherwise Text annotations won't render)
    view->getCamera()->setSmallFeatureCullingPixelSize(-1.0f);

    // thread-safe initialization of the OSG wrapper manager. Calling this here
    // prevents the "unsupported wrapper" messages from OSG
    //osgDB::Registry::instance()->getObjectWrapperManager()->findWrapper("osg::Image");

    // add some stock OSG handlers:
    view->addEventHandler(new osgViewer::StatsHandler());
    view->addEventHandler(new osgViewer::WindowSizeHandler());
    view->addEventHandler(new osgViewer::ThreadingHandler());
    view->addEventHandler(new osgViewer::LODScaleHandler());
    view->addEventHandler(new osgGA::StateSetManipulator(view->getCamera()->getOrCreateStateSet()));
    view->addEventHandler(new osgViewer::RecordCameraPathHandler());

    // Taking this out because it does not properly check getHandled()
    //view->addEventHandler(new osgViewer::ScreenCaptureHandler());
}




//........................................................................

#if 0
namespace
{
    struct SkyHoursSlider : public ui::ControlEventHandler
    {
        SkyHoursSlider(SkyNode* sky) : _sky(sky)  { }
        SkyNode* _sky;
        void onValueChanged(ui::Control* control, float value )
        {
            DateTime d = _sky->getDateTime();
            _sky->setDateTime(DateTime(d.year(), d.month(), d.day(), value));

            d = _sky->getDateTime();
        }
    };

    struct SkyDaysSlider : public ui::ControlEventHandler
    {
        SkyDaysSlider(SkyNode* sky) : _sky(sky)  { }
        SkyNode* _sky;
        void onValueChanged(ui::Control* control, float value )
        {
            DateTime d = _sky->getDateTime();
            _sky->setDateTime(DateTime(d.year(), d.month(), floor(value), d.hours()));
        }
    };

    static std::string s_month[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    struct SkyMonthSlider : public ui::ControlEventHandler
    {
        SkyMonthSlider(SkyNode* sky, ui::LabelControl* label) : _sky(sky), _label(label) { }
        SkyNode* _sky;
        ui::LabelControl* _label;
        void onValueChanged(ui::Control* control, float value )
        {
            int m = 1 + osg::minimum((int)value, 11);
            DateTime d = _sky->getDateTime();
            _sky->setDateTime(DateTime(d.year(), m, d.day(), d.hours()));
            _label->setText(s_month[m-1]);
        }
    };

    struct SkyYearSlider : public ui::ControlEventHandler
    {
        SkyYearSlider(SkyNode* sky, ui::LabelControl* label) : _sky(sky), _label(label) { }
        SkyNode* _sky;
        ui::LabelControl* _label;
        void onValueChanged(ui::Control* control, float value )
        {
            DateTime d = _sky->getDateTime();
            _sky->setDateTime(DateTime((int)value, d.month(), d.day(), d.hours()));
            _label->setText(Stringify() << (int)value);
        }
    };

    struct AmbientBrightnessHandler : public ui::ControlEventHandler
    {
        AmbientBrightnessHandler(SkyNode* sky) : _sky(sky) { }

        SkyNode* _sky;

        void onValueChanged(ui::Control* control, float value )
        {
            if (_sky->getSunLight())
                _sky->getSunLight()->setAmbient(osg::Vec4(value,value,value,1));
        }
    };
}

ui::Control* SkyControlFactory::create(SkyNode* sky)
{
    ui::Grid* grid = new ui::Grid();
    grid->setBackColor(0,0,0,.1);
    grid->setChildVertAlign( ui::Control::ALIGN_CENTER );
    grid->setChildSpacing( 10 );

    if (sky)
    {
        DateTime dt = sky->getDateTime();

        int r=0;
        grid->setControl( 0, r, new ui::LabelControl("Hours UTC: ", 16) );
        ui::HSliderControl* skyHoursSlider = grid->setControl(1, r, new ui::HSliderControl( 0.0f, 24.0f, dt.hours() ));
        skyHoursSlider->setHorizFill( true, 250 );
        skyHoursSlider->addEventHandler( new SkyHoursSlider(sky) );
        grid->setControl(2, r, new ui::LabelControl(skyHoursSlider) );

        r++;
        grid->setControl( 0, r, new ui::LabelControl("Day: ", 16) );
        ui::HSliderControl* skyDaySlider = grid->setControl(1, r, new ui::HSliderControl( 1, 31, dt.day() ));
        skyDaySlider->setHorizFill( true, 250 );
        skyDaySlider->addEventHandler( new SkyDaysSlider(sky) );
        grid->setControl(2, r, new ui::LabelControl(skyDaySlider) );

        ++r;
        grid->setControl( 0, r, new ui::LabelControl("Month: ", 16) );
        ui::HSliderControl* skyMonthSlider = grid->setControl(1, r, new ui::HSliderControl( 0.0f, 12.0f, dt.month() ));
        skyMonthSlider->setHorizFill( true, 250 );
        ui::LabelControl* monthLabel = grid->setControl(2, r, new ui::LabelControl(s_month[dt.month()-1]));
        skyMonthSlider->addEventHandler( new SkyMonthSlider(sky, monthLabel) );

        ++r;
        grid->setControl( 0, r, new ui::LabelControl("Year: ", 16) );
        ui::HSliderControl* skyYearSlider = grid->setControl(1, r, new ui::HSliderControl( 1970.0f, 2061.0f, dt.year() ));
        skyYearSlider->setHorizFill( true, 250 );
        ui::LabelControl* yearLabel = grid->setControl(2, r, new ui::LabelControl(Stringify()<<dt.year()));
        skyYearSlider->addEventHandler( new SkyYearSlider(sky, yearLabel) );

        ++r;
        grid->setControl(0, r, new ui::LabelControl("Ambient Light: ", 16) );
        ui::HSliderControl* ambient = grid->setControl(1, r, new ui::HSliderControl(0.0f, 1.0f, sky->getSunLight()->getAmbient().r()));
        ambient->addEventHandler( new AmbientBrightnessHandler(sky) );
        grid->setControl(2, r, new ui::LabelControl(ambient) );
    }

    return grid;
}

//........................................................................


namespace
{
    struct OceanSeaLevel : public ui::ControlEventHandler
    {
        OceanSeaLevel(SimpleOceanLayer* ocean) : _ocean(ocean) { }
        SimpleOceanLayer* _ocean;
        void onValueChanged(ui::Control* control, float value )
        {
            _ocean->setSeaLevel(value);
        }
    };
}

ui::Control*
OceanControlFactory::create(SimpleOceanLayer* ocean)
{
    ui::Grid* grid = new ui::Grid();
    grid->setBackColor(0,0,0,.1);
    grid->setChildVertAlign( ui::Control::ALIGN_CENTER );
    grid->setChildSpacing( 10 );

    if (ocean)
    {
        int r=0;

        grid->setControl( 0, r, new ui::LabelControl("Sea Level: ", 16) );
        ui::HSliderControl* seaLevel = grid->setControl(1, r, new ui::HSliderControl(-250.0f, 250.0f, 0.0f, new OceanSeaLevel(ocean)));
        seaLevel->setHorizFill( true, 250 );
        grid->setControl(2, r, new ui::LabelControl(seaLevel) );
    }

    return grid;
}
#endif
