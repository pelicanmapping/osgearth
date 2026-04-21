/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/AnnotationRegistry>
#include <osgEarth/Registry>
#include <osgEarth/ObjectIndex>
#include <mutex>

#define LC "[AnnotationRegistry] "

using namespace osgEarth;

//-------------------------------------------------------------------

namespace
{
    struct CollectAnnotationNodes : public osg::NodeVisitor
    {
        CollectAnnotationNodes() : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            _group.key() = "annotations";
        }

        void apply(osg::Node& node)
        {
            AnnotationNode* anno = dynamic_cast<AnnotationNode*>( &node );
            if ( anno )
            {
                Config conf = anno->getConfig();
                _group.add( conf );
            }

            traverse(node);
        }

        Config _group;
    };
}

//-------------------------------------------------------------------

AnnotationRegistry*
AnnotationRegistry::instance()
{
    // OK to be in the local scope since this gets called at static init time
    // by the OSGEARTH_REGISTER_ANNOTATION macro
    static std::once_flag s_once;
    static osg::ref_ptr<AnnotationRegistry> s_singleton;
    static std::mutex s_singletonMutex;

    std::call_once(s_once, []() {
        s_singleton = new AnnotationRegistry();
        Registry::instance()->registerSingleton(s_singleton.get());
    });

    return s_singleton;
}


void
AnnotationRegistry::add( const std::string& type, AnnotationFactory* factory )
{
    if ( factory )
        _factories[type] = factory;
}


bool
AnnotationRegistry::create(MapNode*              mapNode, 
                           const Config&         conf, 
                           const osgDB::Options* options,
                           osg::Group*&          results ) const
{
    bool createdAtLeastOne = false;

    // first try to parse the top-level config as an annotation:
    AnnotationNode* top = createOne(mapNode, conf, options);
    if ( top )
    {
        if ( results == 0L )
            results = new osg::Group();
        results->addChild( top );
        createdAtLeastOne = true;
    }

    // failing that, treat it like a group of annotations:
    else
    {
        for( ConfigSet::const_iterator i = conf.children().begin(); i != conf.children().end(); ++i )
        {
            AnnotationNode* anno = createOne( mapNode, *i, options );
            if ( anno )
            {
                if ( results == 0L )
                    results = new osg::Group();
                results->addChild( anno );
                createdAtLeastOne = true;
            }
        }
    }

    return createdAtLeastOne;
}


AnnotationNode*
AnnotationRegistry::createOne(MapNode*              mapNode, 
                              const Config&         conf, 
                              const osgDB::Options* options) const
{
    FactoryMap::const_iterator f = _factories.find( conf.key() );
    if ( f != _factories.end() && f->second != 0L )
    {
        AnnotationNode* anno = f->second->create(conf, options);
        if ( anno )
        {
            if (mapNode)
                anno->setMapNode(mapNode);

            Registry::objectIndex()->tagNode( anno, anno );
            return anno;
        }
    }
    return 0L;
}


Config
AnnotationRegistry::getConfig( osg::Node* graph ) const
{
    if ( graph )
    {
        CollectAnnotationNodes visitor;
        graph->accept( visitor );
        return visitor._group;
    }
    return Config();
}

