/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include "AssetAccessor"

#include <osgEarth/Notify>
#include <osgEarth/URI>
#include <osgEarth/Registry>

using namespace osgEarth;
using namespace osgEarth::Cesium;

/**********************************************/
AssetRequest::AssetRequest(const std::string& method, const std::string& url, const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers) :
    _method(method),
    _url(url)
{
    _headers.insert(headers.begin(), headers.end());
}

const std::string& AssetRequest::method() const
{
    return _method;
}

const std::string& AssetRequest::url() const
{
    return _url;
}

const CesiumAsync::HttpHeaders& AssetRequest::headers() const
{
    return _headers;
}

const CesiumAsync::IAssetResponse* AssetRequest::response() const
{
    return _response.get();
}

void AssetRequest::setResponse(std::unique_ptr< AssetResponse > response)
{
    _response = std::move(response);
}

/**********************************************/

uint16_t AssetResponse::statusCode() const
{
    return _statusCode;
}

std::string AssetResponse::contentType() const
{
    return _contentType;
}


const CesiumAsync::HttpHeaders& AssetResponse::headers() const
{
    return _headers;
}

std::span<const std::byte> AssetResponse::data() const
{
    return std::span<const std::byte>(_result.data(), _result.size());
}

/**********************************************/

AssetAccessor::AssetAccessor()
{
    _options = new osgDB::Options;
    _cacheSettings = new CacheSettings;
    _cacheSettings->setCache(Registry::instance()->getDefaultCache());
    _cacheSettings->store(_options.get());
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
AssetAccessor::get(const CesiumAsync::AsyncSystem& asyncSystem,
    const std::string& url,
    const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers)
{
    osg::ref_ptr<osgDB::Options> options = _options.get();
    auto request = std::make_shared<AssetRequest>("GET", url, headers);
    return asyncSystem.createFuture<std::shared_ptr<CesiumAsync::IAssetRequest>>(
        [&](const auto& promise)
        {
            asyncSystem.runInWorkerThread([promise, request, url, headers, options]() {
                // This should run in another thread.
                URIContext uriContext;
                for (auto header : headers)
                {
                    uriContext.addHeader(header.first, header.second);
                }
                
                URI uri(url, uriContext);

                auto httpResponse = uri.readString(options.get());
                std::unique_ptr< AssetResponse > response = std::make_unique< AssetResponse >();

                if (httpResponse.code() == ReadResult::RESULT_OK)
                {
                    response->_statusCode = 200;
                }
                response->_contentType = httpResponse.metadata().value(IOMetadata::CONTENT_TYPE);
                for (auto& i : httpResponse.metadata().children())
                {
                    response->_headers[i.key()] = i.value();
                }
                std::string content = httpResponse.getString();

                std::vector<std::byte> result(content.size());
                for (unsigned int i = 0; i < content.size(); ++i)
                {
                    result[i] = (std::byte)content[i];
                }

                response->_result = result;
                request->setResponse(std::move(response));
                promise.resolve(request);
                });
        }
    );
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
AssetAccessor::request(
    const CesiumAsync::AsyncSystem& asyncSystem,
    const std::string& verb,
    const std::string& url,
    const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers,
    const std::span<const std::byte>& contentPayload)
{
    auto request = std::make_shared<AssetRequest>(verb, url, headers);
    return asyncSystem.createFuture<std::shared_ptr<CesiumAsync::IAssetRequest>>(
        [&](const auto& promise)
        {
        }
    );
}

void AssetAccessor::tick() noexcept
{
}