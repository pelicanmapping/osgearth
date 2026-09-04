/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/

#include "CesiumCreditsNode"
#include "Context"

#include <osgEarth/Registry>
#include <osgEarth/Notify>
#include <osgEarth/XmlUtils>
#include <osgText/Text>

#include <algorithm>
#include <regex>

using namespace osgEarth::Cesium;

namespace
{
    namespace
    {
        // Taken from vsgCS.  Cleans up self closing <img src="..."> elements to be valid XML.
        void cleanHtml(std::string& input)
        {
            std::locale locale = std::locale();
            const std::string img("<img");
            auto searchStartItr = input.begin();
            decltype(searchStartItr) imgItr;
            while ((imgItr = std::search(searchStartItr, input.end(), img.begin(), img.end(),
                [&](char c1, char c2)
                {
                    return std::toupper(c1, locale) == std::toupper(c2, locale);
                })) != input.end())
            {
                auto closeItr = std::find(imgItr, input.end(), '>');
                if (closeItr == input.end())
                {
                    OE_WARN << "No closing tag in HTML " << input << std::endl;
                }
                auto ltPos = closeItr - input.begin();
                if (*(closeItr - 1) != '/')
                {
                    input.insert(closeItr, '/');
                    searchStartItr = input.begin() + ltPos + 1;
                }
                else
                {
                    searchStartItr = closeItr;
                }
            }
        }
    }

    struct ParsedCredit
    {
        std::string text;
        std::string link;
        std::string image;
    };

    void parseCredit(const Config& config, std::vector< ParsedCredit >& credits)
    {
        if (config.hasChild("a"))
        {
            const Config& a = config.child("a");
            std::string link = a.value("link");
            if (!a.value().empty())
            {
                ParsedCredit c;
                c.link = link;
                c.text = a.value();
                credits.emplace_back(std::move(c));
            }
            else
            {
                parseCredit(a, credits);
            }            
        }
        if (config.hasChild("span"))
        {
            const Config& span = config.child("span");
            if (!span.value().empty())
            {
                ParsedCredit c;
                c.text = span.value();
                credits.emplace_back(std::move(c));
            }
            else
            {
                parseCredit(span, credits);
            }
        }
        if (config.hasChild("div"))
        {
            const Config& div = config.child("div");
            if (!div.value().empty())
            {
                ParsedCredit c;
                c.text = div.value();
                credits.emplace_back(std::move(c));
            }
            else
            {
                parseCredit(div, credits);
            }
        }
        if (config.hasChild("img"))
        {
            const Config& img = config.child("img");
            ParsedCredit c;
            c.image = img.value("src");
            credits.emplace_back(std::move(c));
        }
    }

    void parseCredit(const std::string& html, std::vector< ParsedCredit >& credits)
    {
        std::string cleanedHTML = html;
        cleanHtml(cleanedHTML);
        if (!cleanedHTML.empty())
        {
            std::stringstream buf(cleanedHTML);
            osg::ref_ptr<XmlDocument> doc = XmlDocument::load(buf);
            parseCredit(doc->getConfig(), credits);
        }
    }
}

CesiumCreditsNode::CesiumCreditsNode(osg::View* view, CesiumUtility::CreditSystem* creditSystem)
    : _lastCreditsCount(0)
{
    setNumChildrenRequiringUpdateTraversal(1);

    _view = view;
    _creditSystem = std::shared_ptr< CesiumUtility::CreditSystem >(creditSystem);

    _camera = new osg::Camera;    
    _camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    _camera->setViewMatrix(osg::Matrix::identity());
    _camera->setClearMask(GL_DEPTH_BUFFER_BIT);
    _camera->setRenderOrder(osg::Camera::POST_RENDER);
    _camera->setAllowEventFocus(false);

    _camera->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    _camera->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);

    addChild(_camera);
}

void CesiumCreditsNode::updateCredits()
{
    std::vector< ParsedCredit > parsedCredits;

    auto creditSystem = _creditSystem;
    auto credits = _creditSystem->getSnapshot();

    auto creditsToShowThisFrame = credits.currentCredits;

    auto updated = creditsToShowThisFrame.size() != _lastCreditsCount || credits.removedCredits.size() > 0;

    if (updated) {
        _lastCreditsCount = creditsToShowThisFrame.size();

        for (auto& credit : creditsToShowThisFrame) {
            if (creditSystem->shouldBeShownOnScreen(credit))
            {
                auto html = creditSystem->getHtml(credit);
                parseCredit(html, parsedCredits);
            }
        }
        _camera->removeChildren(0, _camera->getNumChildren());

        osg::Geode* geode = new osg::Geode();
        _camera->addChild(geode);

        float margin = 4.0f;
        osg::Vec3 position(5, 5, 0);
        for (unsigned int i = 0; i < parsedCredits.size(); ++i)
        {
            auto& c = parsedCredits[i];
            if (!c.text.empty())
            {
                osgText::Text* text = new  osgText::Text;
                text->setFont(osgEarth::Registry::instance()->getDefaultFont());
                text->setPosition(position);
                text->setText(c.text);
                geode->addDrawable(text);
                position.x() += (text->getBoundingBox().xMax() - text->getBoundingBox().xMin()) + margin;
            }

            if (!c.image.empty())
            {
                osg::Texture2D* texture = getOrCreateTexture(c.image);
                if (texture)
                {
                    osg::Geometry* geometry = new osg::Geometry;
                    osg::Vec3Array* verts = new osg::Vec3Array;
                    geometry->setVertexArray(verts);
                    verts->push_back(osg::Vec3(0, 0, 0));
                    verts->push_back(osg::Vec3(texture->getImage()->s(), 0, 0));
                    verts->push_back(osg::Vec3(texture->getImage()->s(), texture->getImage()->t(), 0));
                    verts->push_back(osg::Vec3(0, texture->getImage()->t(), 0));

                    osg::Vec2Array* texCoords = new osg::Vec2Array;
                    geometry->setTexCoordArray(0, texCoords);
                    texCoords->push_back(osg::Vec2(0, 0));
                    texCoords->push_back(osg::Vec2(1, 0));
                    texCoords->push_back(osg::Vec2(1, 1));
                    texCoords->push_back(osg::Vec2(0, 1));

                    osg::Vec4Array* colors = new osg::Vec4Array;
                    colors->push_back(osg::Vec4(1, 1, 1, 1));
                    geometry->setColorArray(colors, osg::Array::BIND_OVERALL);

                    geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
                    osg::DrawElementsUByte* de = new osg::DrawElementsUByte(GL_TRIANGLES);
                    de->push_back(0); de->push_back(1); de->push_back(2);
                    de->push_back(0); de->push_back(2); de->push_back(3);

                    geometry->addPrimitiveSet(de);

                    osgEarth::Registry::shaderGenerator().run(geometry);
                    geode->addDrawable(geometry);

                    position.x() += texture->getImage()->s() + margin;
                }
            }
        }
    }

}

osg::Texture2D* CesiumCreditsNode::getOrCreateTexture(const std::string& url)
{
    osg::Texture2D* result = nullptr;
    auto itr = _textureCache.find(url);
    if (itr != _textureCache.end())
    {
        return itr->second.get();
    }

    URI uri(url);
    osg::Image* image = uri.getImage();
    if (image)
    {
        result = new osg::Texture2D(image);
        result->setResizeNonPowerOfTwoHint(false);
        _textureCache[url] = result;
    }

    return result;
}

void CesiumCreditsNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
    {
        // Get the credits from the previous frame.
        updateCredits();
        auto vp = _view->getCamera()->getViewport();
        if (vp)
        {
            _camera->setProjectionMatrix(osg::Matrix::ortho2D(0, vp->width(), 0, vp->height()));
        }
    }
    osg::Group::traverse(nv);
}