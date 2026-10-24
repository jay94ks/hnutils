#include <hnfs/extensions.hpp>

namespace hnfs {

/* Extension interfaces do not own or expose driver state. */
ISnapshotCatalog::~ISnapshotCatalog()=default;
ISubvolumeCatalog::~ISubvolumeCatalog()=default;
IVolumeContainer::~IVolumeContainer()=default;
INamedStreams::~INamedStreams()=default;
IExtendedAttributes::~IExtendedAttributes()=default;
ICowTransaction::~ICowTransaction()=default;
ICowTree::~ICowTree()=default;
IJournalInspector::~IJournalInspector()=default;

} // namespace hnfs
