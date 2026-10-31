#pragma once

#include <cstdint>
#include <vector>

namespace SkinPersistence
{
    // Restores the selected fake loadout before the catalog is appended to the
    // next SOCache subscription. Only equipped/modified fake items are stored;
    // the catalog itself is never written to disk.
    void Load();
    void Apply();

    // Mark a fake item as changed. The actual write is debounced and also
    // flushed from DllMain(DLL_PROCESS_DETACH).
    void MarkItem(std::uint64_t ItemId);
    void MarkItems(const std::vector<std::uint64_t>& ItemIds);

    void Update();
    void Flush();
}
