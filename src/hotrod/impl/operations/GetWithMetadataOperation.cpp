#include "hotrod/impl/operations/GetWithMetadataOperation.h"

namespace infinispan {
namespace hotrod {
namespace operations {

using infinispan::hotrod::protocol::Codec;
using namespace infinispan::hotrod::transport;

GetWithMetadataOperation::GetWithMetadataOperation(
    const Codec&      codec_,
    std::shared_ptr<TransportFactory> transportFactory_,
    const hrbytes&    key_,
    const hrbytes&    cacheName_,
    IntWrapper&  topologyId_,
    uint32_t    flags_)
    : AbstractKeyOperation<MetadataValueImpl<hrbytes> >(
        codec_, transportFactory_, key_, cacheName_, topologyId_, flags_)
{}

MetadataValueImpl<hrbytes> GetWithMetadataOperation::executeOperation(Transport& transport)
{
    MetadataValueImpl<hrbytes> result;
    TRACE("Execute GetWithMetadata(flags=%u)", flags);
    TRACEBYTES("key = ", key);
    uint8_t status = sendKeyOperation(
        key, transport, GET_WITH_METADATA_REQUEST, GET_WITH_METADATA_RESPONSE);
    if (status == NO_ERROR_STATUS) {
        uint8_t flag = transport.readByte();
        if ((flag & INFINITE_LIFESPAN) != INFINITE_LIFESPAN) {
        	result.setCreated(transport.readLong());
        	result.setLifespan(transport.readVInt());
        }
        if ((flag & INFINITE_MAXIDLE) != INFINITE_MAXIDLE) {
            result.setLastUsed(transport.readLong());
            result.setMaxIdle(transport.readVInt());
        }
        result.setVersion(transport.readLong());
        result.setValue(transport.readArray());
        TRACE("return created=%lld, lifespan=%d, lastUsed=%lld, maxIdle=%d, version=%lld", result.created, result.lifespan, result.lastUsed, result.maxIdle, result.version);
        TRACEBYTES("return value = ", result.getValue());
    } else {
        TRACE("Error status %u", status);
    }
    return result;
}

}}} /// namespace infinispan::hotrod::operations
