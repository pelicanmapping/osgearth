/* osgEarth
 * MIT License
 */
#pragma once

#include <basisu/transcoder/basisu_transcoder.h>
#include <osg/Referenced>
#include <OpenThreads/Mutex>
#include <mutex>

namespace
{
    //! Initialize lookup tables on first use in each plugin. The process-wide lock also
    //! serializes initialization when both plugins link to one shared copy of Basis.
    void initializeBasisTranscoder()
    {
        static std::once_flag initialized;
        std::call_once(initialized, []() {
            std::lock_guard<OpenThreads::Mutex> lock(*osg::Referenced::getGlobalReferencedMutex());
            basist::basisu_transcoder_init();
        });
    }
}
