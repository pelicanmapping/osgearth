/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>
#include <osgEarth/ImageUtils>

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    osg::ref_ptr<osg::Image> createTestImage()
    {
        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);

        for (int t = 0; t < image->t(); ++t)
        {
            for (int s = 0; s < image->s(); ++s)
            {
                unsigned char* pixel = image->data(s, t);
                pixel[0] = static_cast<unsigned char>(s * 40);
                pixel[1] = static_cast<unsigned char>(t * 50);
                pixel[2] = static_cast<unsigned char>(s * 20 + t * 10);
                pixel[3] = static_cast<unsigned char>(255 - s * 5 - t * 3);
            }
        }

        return image;
    }
}

TEST_CASE("resizeImage bilinear RGBA8 output is stable")
{
    osg::ref_ptr<osg::Image> input = createTestImage();
    osg::ref_ptr<osg::Image> output;

    REQUIRE(ImageUtils::resizeImage(input.get(), 3, 3, output, 0, true));
    REQUIRE(output.valid());
    REQUIRE(output->s() == 3);
    REQUIRE(output->t() == 3);

    const unsigned char* center = output->data(1, 1);
    REQUIRE(center[0] == 53);
    REQUIRE(center[1] == 66);
    REQUIRE(center[2] == 40);
    REQUIRE(center[3] == 244);

    const unsigned char* lowerRight = output->data(2, 2);
    REQUIRE(lowerRight[0] == 106);
    REQUIRE(lowerRight[1] == 133);
    REQUIRE(lowerRight[2] == 80);
    REQUIRE(lowerRight[3] == 233);
}

TEST_CASE("resizeImage nearest RGBA8 output is stable")
{
    osg::ref_ptr<osg::Image> input = createTestImage();
    osg::ref_ptr<osg::Image> output;

    REQUIRE(ImageUtils::resizeImage(input.get(), 3, 3, output, 0, false));
    REQUIRE(output.valid());
    REQUIRE(output->s() == 3);
    REQUIRE(output->t() == 3);

    const unsigned char* center = output->data(1, 1);
    REQUIRE(center[0] == 40);
    REQUIRE(center[1] == 50);
    REQUIRE(center[2] == 30);
    REQUIRE(center[3] == 247);

    const unsigned char* lowerRight = output->data(2, 2);
    REQUIRE(lowerRight[0] == 120);
    REQUIRE(lowerRight[1] == 150);
    REQUIRE(lowerRight[2] == 90);
    REQUIRE(lowerRight[3] == 231);
}
