#include "hotrod/impl/operations/GetWithVersionOperation.h"

namespace infinispan {
namespace hotrod {
namespace operations {

using infinispan::hotrod::protocol::Codec;
using namespace infinispan::hotrod::transport;

GetWithVersionOperation::GetWithVersionOperation(
    const Codec&      codec_,
    std::shared_ptr<TransportFactory> transportFactory_,
    const hrbytes&    key_,
    const hrbytes&    cacheName_,
    IntWrapper&          topologyId_,
    uint32_t    flags_)
    : AbstractKeyOperation<VersionedValueImpl<hrbytes> >(
        codec_, transportFactory_, key_, cacheName_, topologyId_, flags_)
{}

VersionedValueImpl<hrbytes> GetWithVersionOperation::executeOperation(Transport& transport)
{
    TRACE("Execute GetWithVersion(flags=%u)", flags);
    TRACEBYTES("key = ", key);
    VersionedValueImpl<hrbytes> result;
    uint8_t status = sendKeyOperation(
        key, transport, GET_WITH_VERSION_REQUEST, GET_WITH_VERSION_RESPONSE);
    if (status == NO_ERROR_STATUS) {
        result.setVersion(transport.readLong());
        result.setValue(transport.readArray());
        TRACE("return version = %lld", result.version);
        TRACEBYTES("return value = ", result.getValue());
    } else {
        TRACE("Error status %u", status);
    }
    return result;
}

}}} /// namespace infinispan::hotrod::operations
