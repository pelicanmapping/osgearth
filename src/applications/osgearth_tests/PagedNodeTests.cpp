/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/PagedNode>
#include <osgEarth/GLUtils>
#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osgUtil/IncrementalCompileOperation>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    // Waits a bounded time for asynchronous test progress without requiring a render loop.
    template<typename Predicate>
    bool waitFor(Predicate ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Coordinates a blocked loader; the timeout also releases workers after a failed assertion.
    struct LoadGate
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic_bool entered{ false };
        bool released = false;

        // Signals that the loader is running, then waits for explicit release or timeout.
        void wait()
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.wait_for(lock, std::chrono::seconds(5), [this]() { return released; });
        }

        // Waits for loader entry without placing a lambda inside a Catch assertion macro.
        bool waitUntilEntered() const
        {
            return waitFor([this]() { return entered.load(); });
        }

        // Lets the blocked loader complete from the test thread.
        void release()
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
            condition.notify_all();
        }
    };

    struct CheckedPage : PagedNode2
    {
        using PagedNode2::startLoad;
        const std::thread::id ownerThread = std::this_thread::get_id();
        mutable std::atomic_uint workerBounds{ 0u };

        // Records forbidden worker access to the live page's bound computation.
        osg::BoundingSphere computeBound() const override
        {
            if (std::this_thread::get_id() != ownerThread)
                ++workerBounds;
            return PagedNode2::computeBound();
        }
    };

    struct CheckedUserData : osg::DefaultUserDataContainer
    {
        const std::thread::id ownerThread = std::this_thread::get_id();
        mutable std::atomic_uint workerReads{ 0u };

        // Records visitor user-data lookups that escape the traversal thread.
        unsigned getUserObjectIndex(const std::string& name, unsigned start = 0) const override
        {
            if (std::this_thread::get_id() != ownerThread)
                ++workerReads;
            return osg::DefaultUserDataContainer::getUserObjectIndex(name, start);
        }
    };

    // Creates detached geometry with a valid bound and compilable drawable state, without a GL context.
    osg::ref_ptr<osg::Node> makePayload()
    {
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(), 2.0f)));
        return geode;
    }

    // Binds a page to its manager using a non-cull traversal, without starting a load or tracking usage.
    void bindManager(PagingManager* manager)
    {
        osg::ref_ptr<osg::NodeVisitor> visitor = new osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        manager->accept(*visitor);
    }

    struct PagingFixture
    {
        osg::ref_ptr<PagingManager> manager;
        osg::ref_ptr<CheckedPage> page;
        jobs::jobpool::metrics_t* metrics;

        // Gives each test an independent queue and keeps automatic page-out from obscuring assertions.
        PagingFixture()
        {
            static std::atomic_uint serial{ 0u };
            const auto poolName = std::string("paged-node-test-") + std::to_string(++serial);
            manager = new PagingManager(poolName);
            metrics = jobs::get_pool(poolName)->metrics();
            page = new CheckedPage;
            page->setAutoUnload(false);
            page->setPreCompileGLObjects(false);
            manager->addChild(page);
            bindManager(manager);
        }

        // Waits for a precise number of queued completions, including the end of their worker calls.
        bool queued(unsigned count) const
        {
            return waitFor([this, count]() {
                return metrics->postprocessing == count && metrics->running == 0u && metrics->pending == 0u;
            });
        }
    };
}

// The visitor may change or disappear while loading; the worker must only use the captured ICO observer.
TEST_CASE("PagedNode captures compile dependencies before dispatch", "[pagednode]")
{
    PagingFixture fixture;
    fixture.page->setPreCompileGLObjects(true);
    auto payload = makePayload();
    auto gate = std::make_shared<LoadGate>();
    fixture.page->setLoadFunction([gate, payload](Cancelable*) {
        gate->wait();
        return payload;
    });

    osg::ref_ptr<osg::Group> host = new osg::Group;
    osg::ref_ptr<CheckedUserData> data = new CheckedUserData;
    host->setUserDataContainer(data);
    osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico = new osgUtil::IncrementalCompileOperation;
    ObjectStorage::set(host.get(), ico.get());
    fixture.page->startLoad(host.get());
    REQUIRE(gate->waitUntilEntered());

    SECTION("visitor data changes before completion")
    {
        osg::ref_ptr<osgUtil::IncrementalCompileOperation> replacement = new osgUtil::IncrementalCompileOperation;
        ObjectStorage::set(host.get(), replacement.get());
    }
    SECTION("visitor and ICO expire before completion")
    {
        osg::observer_ptr<osg::Group> observer(host);
        host = nullptr;
        ico = nullptr;
        REQUIRE_FALSE(observer.valid());
    }

    gate->release();
    REQUIRE(fixture.queued(1u));
    CHECK(data->workerReads == 0u);
    CHECK(fixture.page->getNumChildren() == 0u);
    fixture.manager->update();
    REQUIRE(fixture.page->isLoadComplete());
    REQUIRE(fixture.page->getNumChildren() == 1u);
    CHECK(fixture.page->getChild(0)->asGroup()->getChild(0) == payload.get());
}

// A completed background load must not invalidate ancestors or compute bounds on the live page.
TEST_CASE("PagedNode updates live bounds only during merge", "[pagednode]")
{
    PagingFixture fixture;
    auto payload = makePayload();
    fixture.page->setLoadFunction([payload](Cancelable*) { return payload; });
    const auto before = fixture.manager->getBound();
    fixture.page->load();
    REQUIRE(fixture.queued(1u));
    CHECK(fixture.page->workerBounds == 0u);
    CHECK(fixture.manager->getBound().radius() == before.radius());
    fixture.manager->update();
    CHECK(fixture.page->isLoadComplete());
    CHECK(fixture.manager->getBound().valid());
    CHECK(fixture.page->workerBounds == 0u);
}

// A stale queued entry must not resolve a replacement load, even when that load is ready to merge.
TEST_CASE("PagedNode queued completions retain their load generation", "[pagednode]")
{
    PagingFixture fixture;
    fixture.manager->setMaxMergesPerFrame(1u);
    auto oldPayload = makePayload();
    fixture.page->setLoadFunction([oldPayload](Cancelable*) { return oldPayload; });
    fixture.page->load();
    REQUIRE(fixture.queued(1u));
    fixture.page->unload();

    // Put another valid merge between the stale entry and the replacement entry so that
    // one update must stop before the replacement can merge.
    osg::ref_ptr<CheckedPage> other = new CheckedPage;
    other->setAutoUnload(false);
    other->setPreCompileGLObjects(false);
    auto otherPayload = makePayload();
    other->setLoadFunction([otherPayload](Cancelable*) { return otherPayload; });
    fixture.manager->addChild(other);
    bindManager(fixture.manager);
    other->load();
    REQUIRE(fixture.queued(2u));

    auto newPayload = makePayload();
    fixture.page->setLoadFunction([newPayload](Cancelable*) { return newPayload; });
    fixture.page->load();
    REQUIRE(fixture.queued(3u));
    fixture.manager->update();
    CHECK(other->isLoadComplete());
    CHECK_FALSE(fixture.page->isLoadComplete());
    CHECK(fixture.page->getNumChildren() == 0u);
    CHECK(oldPayload->getNumParents() == 0u);

    fixture.manager->update();
    REQUIRE(fixture.page->isLoadComplete());
    REQUIRE(fixture.page->getNumChildren() == 1u);
    CHECK(fixture.page->getChild(0)->asGroup()->getChild(0) == newPayload.get());
}

// A loader already running during unload must not enqueue its result under a newer revision.
TEST_CASE("PagedNode rejects completion after unload and reload", "[pagednode]")
{
    PagingFixture fixture;
    auto gate = std::make_shared<LoadGate>();
    auto oldPayload = makePayload();
    fixture.page->setLoadFunction([gate, oldPayload](Cancelable*) {
        gate->wait();
        return oldPayload;
    });
    fixture.page->load();
    REQUIRE(gate->waitUntilEntered());
    fixture.page->unload();

    auto newPayload = makePayload();
    fixture.page->setLoadFunction([newPayload](Cancelable*) { return newPayload; });
    fixture.page->load();
    gate->release();
    REQUIRE(fixture.queued(1u));
    CHECK_FALSE(fixture.page->isLoadComplete());
    fixture.manager->update();
    REQUIRE(fixture.page->getNumChildren() == 1u);
    CHECK(fixture.page->getChild(0)->asGroup()->getChild(0) == newPayload.get());
    CHECK(oldPayload->getNumParents() == 0u);
}

// Manual loading visitors can request a load before traversing the page and binding its manager.
TEST_CASE("PagedNode can discover its manager after a manual load starts", "[pagednode]")
{
    PagingFixture fixture;
    osg::ref_ptr<CheckedPage> page = new CheckedPage;
    page->setPreCompileGLObjects(false);
    page->setAutoUnload(false);
    fixture.manager->addChild(page);
    auto gate = std::make_shared<LoadGate>();
    auto payload = makePayload();
    page->setLoadFunction([gate, payload](Cancelable*) {
        gate->wait();
        return payload;
    });
    page->load();
    REQUIRE(gate->waitUntilEntered());
    bindManager(fixture.manager);
    gate->release();
    REQUIRE(fixture.queued(1u));
    fixture.manager->update();
    CHECK(page->isLoadComplete());
    CHECK(page->getNumChildren() == 1u);
}

// Independent cull visitors must safely share initial manager binding, future publication, and usage tracking.
TEST_CASE("PagedNode supports concurrent first cull traversals", "[pagednode]")
{
    PagingFixture fixture;
    fixture.page->getBound();
    osg::ref_ptr<CheckedPage> page = new CheckedPage;
    page->setAutoUnload(false);
    page->setPreCompileGLObjects(false);
    page->setCenter(osg::Vec3());
    page->setRadius(1.0f);
    page->getBound(); // Warm the OSG cache before concurrent traversal.
    fixture.manager->addChild(page);
    auto calls = std::make_shared<std::atomic_uint>(0u);
    page->setLoadFunction([calls](Cancelable*) {
        ++(*calls);
        return makePayload();
    });

    std::atomic_bool go{ false };
    std::vector<std::thread> threads;
    for (unsigned i = 0u; i < 8u; ++i)
    {
        threads.emplace_back([&]() {
            osg::ref_ptr<osg::NodeVisitor> visitor = new osg::NodeVisitor(
                osg::NodeVisitor::CULL_VISITOR, osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
            while (!go.load())
                std::this_thread::yield();
            for (unsigned pass = 0u; pass < 500u; ++pass)
            {
                fixture.manager->accept(*visitor);
                (void)page->isLoadComplete();
            }
        });
    }
    go = true;
    for (auto& thread : threads)
        thread.join();

    REQUIRE(fixture.queued(1u));
    CHECK(calls->load() == 1u);
    CHECK(fixture.manager->getNumTrackedNodes() == 2u);
    fixture.manager->update();
    CHECK(page->isLoadComplete());
    CHECK(page->getNumChildren() == 1u);
}

// Reusing a surviving page after its manager dies must allocate a token in the new tracker.
TEST_CASE("PagedNode drops tokens from an expired paging manager", "[pagednode]")
{
    PagingFixture fixture;
    fixture.page->touch();
    REQUIRE(fixture.manager->getNumTrackedNodes() == 1u);
    osg::observer_ptr<PagingManager> oldManager(fixture.manager);
    fixture.manager = nullptr;
    REQUIRE_FALSE(oldManager.valid());

    fixture.manager = new PagingManager("paged-node-replacement-test");
    fixture.manager->addChild(fixture.page);
    bindManager(fixture.manager);
    fixture.page->touch();
    fixture.page->touch();
    CHECK(fixture.manager->getNumTrackedNodes() == 1u);
}

// Empty loads and unavailable compilers must still resolve, and rejected shared children must not hang paging.
TEST_CASE("PagedNode resolves unsuccessful and noncompiled loads", "[pagednode]")
{
    PagingFixture fixture;
    SECTION("empty loader result")
    {
        fixture.page->setLoadFunction([](Cancelable*) { return osg::ref_ptr<osg::Node>(); });
        fixture.page->load();
        const bool complete = waitFor([&]() { return fixture.page->isLoadComplete(); });
        REQUIRE(complete);
        CHECK(fixture.page->getNumChildren() == 0u);
    }
    SECTION("loaded node already belongs to another parent")
    {
        auto payload = makePayload();
        osg::ref_ptr<osg::Group> owner = new osg::Group;
        owner->addChild(payload);
        fixture.page->setLoadFunction([payload](Cancelable*) { return payload; });
        fixture.page->load();
        REQUIRE(fixture.queued(1u));
        fixture.manager->update();
        CHECK(fixture.page->isLoadComplete());
        CHECK(fixture.page->getNumChildren() == 0u);
        CHECK(payload->getNumParents() == 1u);
    }
}
