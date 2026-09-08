/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once

#include <osg/Object>
#include <osg/ref_ptr>
#include <string>

namespace osgDB
{
    class InputStream;
    class OutputStream;
}

namespace osgEarth
{
    class ExternalNode;
    class InstancedExternalNode;

    // Internal, common wire format for ordinary and instanced references.
    // Uses OSG's stream-local object IDs: first use defines a string, later
    // uses reference it. No output-stream pointers or caller options are cached.
    class ExternalNodeSerializer
    {
    public:
        static bool readStrings(osgDB::InputStream& input,
            std::string& filename, std::string& readOptions);
        static bool writeStrings(osgDB::OutputStream& output, const ExternalNode& node);
        static bool writeStrings(osgDB::OutputStream& output, const InstancedExternalNode& node);

    private:
        static bool writeStrings(osgDB::OutputStream& output,
            const std::string& filename, const std::string& readOptions,
            osg::ref_ptr<osg::Object>& filenameObject,
            osg::ref_ptr<osg::Object>& optionsObject);
    };
}
