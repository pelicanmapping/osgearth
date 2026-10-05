// Read a vendor OSG scene with its matching SDK and export a portable static OSG text scene.
// This offline bridge is compiled separately against the source reader, never linked into osgEarth.
#ifdef _WIN32
#include <windows.h>
#endif
#include <osg/NodeVisitor>
#include <osg/Texture>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <iostream>

namespace
{
    //! Make external image references independent of the exported model directory without decoding images.
    struct ResolveImages : osg::NodeVisitor
    {
        osg::ref_ptr<osgDB::Options> options;
        bool valid = true;
        //! Use the input model directory to resolve relative DDS references.
        explicit ResolveImages(osgDB::Options* value) : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), options(value) { }
        //! Preserve compressed images and their full mip chains; fail if any referenced image is missing.
        void apply(osg::Node& node) override
        {
            auto* state = node.getStateSet();
            if (state)
            {
                for (unsigned unit = 0; unit < state->getTextureAttributeList().size(); ++unit)
                {
                    auto* texture = dynamic_cast<osg::Texture*>(
                        state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                    if (!texture) continue;
                    for (unsigned i = 0; i < texture->getNumImages(); ++i)
                    {
                        auto* image = texture->getImage(i);
                        if (!image || !image->data()) { valid = false; continue; }
                        auto file = osgDB::findDataFile(image->getFileName(), options);
                        if (file.empty()) valid = false;
                        else image->setFileName(osgDB::convertFileNameToUnixStyle(file));
                    }
                }
            }
            traverse(node);
        }
    };
}

//! Keep vendor readers in an isolated process; return failure rather than silently dropping missing resources.
int main(int argc, char** argv)
{
    if (argc != 3) { std::cerr << "export_source_scene input.ive output.osg\n"; return 2; }
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options;
    options->getDatabasePathList().push_back(osgDB::getFilePath(argv[1]));
    auto node = osgDB::readRefNodeFile(argv[1], options);
    if (!node) return 1;
    ResolveImages resolve(options);
    node->accept(resolve);
    return resolve.valid && osgDB::writeNodeFile(*node, argv[2]) ? 0 : 1;
}
