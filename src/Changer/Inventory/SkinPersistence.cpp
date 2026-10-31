#include "SkinPersistence.h"

#include "Changer/Inventory/FakeItems.h"
#include "Changer/Gc/ItemStyles.h"
#include "Changer/Items/ItemSockets.h"
#include "Changer/Unusual/UnusualStore.h"
#include "SDK/Files/Paths.h"
#include "SDK/Logger/Logger.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr const char* FileName = "anchousechanger.sets";
    constexpr char Magic[4] = { 'A', 'C', 'H', 'S' };
    constexpr std::uint32_t FormatVersion = 1;
    constexpr std::uint32_t MaxRecords = 8192;
    constexpr std::uint32_t MaxRecordSize = 1024 * 1024;
    constexpr auto SaveDelay = std::chrono::milliseconds(400);

    using Clock = std::chrono::steady_clock;

    std::recursive_mutex StateMutex;
    std::unordered_set<std::uint64_t> Dirty;
    std::unordered_set<std::uint64_t> Loaded;
    std::vector<CSOEconItem> Saved;
    bool Initialized = false;
    bool Applied = false;
    std::atomic<bool> SaveRequested{ false };
    Clock::time_point LastChange = Clock::now();

    std::string FilePath()
    {
        static const std::string Path = Paths::Resolve(FileName);
        return Path;
    }

    template <typename T>
    bool ReadValue(std::ifstream& File, T& Value)
    {
        return static_cast<bool>(File.read(reinterpret_cast<char*>(&Value), sizeof(Value)));
    }

    template <typename T>
    bool WriteValue(std::ofstream& File, const T& Value)
    {
        return static_cast<bool>(File.write(reinterpret_cast<const char*>(&Value), sizeof(Value)));
    }

    void RestoreEffect(const CSOEconItem& Item)
    {
        for (std::uint32_t Socket = ItemSockets::FirstSocket; Socket <= ItemSockets::LastSocket; ++Socket)
        {
            auto Value = ItemSockets::Bytes(Item, Socket);
            if (!Value)
                continue;

            // Unusual sockets are serialized as CProtoItemSocket_Effect, which
            // wraps CProtoItemSocket plus the effect id. ItemSockets::Read()
            // intentionally parses only the common wrapper, so read this one
            // directly to recover the effect.
            CProtoItemSocket_Effect Parsed;
            if (Parsed.ParseFromString(*Value) &&
                Parsed.socket().gem_def_index() == ItemSockets::UnusualGemDef &&
                Parsed.effect() > 0)
            {
                UnusualStore::Set(Item.id(), static_cast<int>(Parsed.effect()));
                return;
            }
        }
    }

    bool IsSavedItem(const CSOEconItem& Item)
    {
        if (!Item.id() || !Item.def_index() || !FakeItems::IsFake(Item.id()))
            return false;
        if (Item.equipped_state_size() > 0)
            return true;

        std::lock_guard<std::recursive_mutex> Lock(StateMutex);
        return Dirty.count(Item.id()) != 0 || Loaded.count(Item.id()) != 0 ||
               UnusualStore::Get(Item.id()) != 0;
    }

    bool SaveNow()
    {
        // Clear the request before touching FakeItems/disk. A failure puts it
        // back so the next RunFrame can retry.
        SaveRequested.store(false);

        std::vector<CSOEconItem> Records;
        for (const CSOEconItem& Item : FakeItems::All())
            if (IsSavedItem(Item))
                Records.push_back(Item);

        std::sort(Records.begin(), Records.end(), [](const CSOEconItem& Left, const CSOEconItem& Right) {
            return Left.id() < Right.id();
        });

        if (Records.size() > MaxRecords)
        {
            LogW("Persistence: %zu records exceed limit %u, keeping equipped/first %u",
                 Records.size(), MaxRecords, MaxRecords);
            Records.resize(MaxRecords);
        }

        const std::string Path = FilePath();
        if (Records.empty())
        {
            if (std::remove(Path.c_str()) == 0)
                Log("Persistence: no selected items left, removed %s", Path.c_str());
            return true;
        }

        const std::string Temporary = Path + ".tmp";
        std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
        if (!File)
        {
            LogE("Persistence: failed to open %s", Temporary.c_str());
            SaveRequested.store(true);
            return false;
        }

        const std::uint32_t Count = static_cast<std::uint32_t>(Records.size());
        bool Ok = WriteValue(File, Magic) && WriteValue(File, FormatVersion) && WriteValue(File, Count);
        for (const CSOEconItem& Item : Records)
        {
            const std::string Bytes = Item.SerializeAsString();
            const std::uint32_t Size = static_cast<std::uint32_t>(Bytes.size());
            if (!WriteValue(File, Size) ||
                !File.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size())))
            {
                Ok = false;
                break;
            }
        }
        File.close();

        if (!Ok)
        {
            LogE("Persistence: failed to write %s", Temporary.c_str());
            std::remove(Temporary.c_str());
            SaveRequested.store(true);
            return false;
        }

        if (!MoveFileExA(Temporary.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            LogE("Persistence: failed to replace %s (error %lu)", Path.c_str(), GetLastError());
            std::remove(Temporary.c_str());
            SaveRequested.store(true);
            return false;
        }

        {
            std::lock_guard<std::recursive_mutex> Lock(StateMutex);
            Dirty.clear();
        }

        Log("Persistence: saved %u selected item(s) to %s", Count, Path.c_str());
        return true;
    }
}

void SkinPersistence::Load()
{
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    if (Initialized)
        return;
    Initialized = true;

    const std::string Path = FilePath();
    std::ifstream File(Path, std::ios::binary);
    if (!File)
    {
        Log("Persistence: no saved sets at %s", Path.c_str());
        return;
    }

    char FileMagic[4] = {};
    std::uint32_t Version = 0;
    std::uint32_t Count = 0;
    if (!File.read(FileMagic, sizeof(FileMagic)) || std::memcmp(FileMagic, Magic, sizeof(Magic)) != 0 ||
        !ReadValue(File, Version) || Version != FormatVersion || !ReadValue(File, Count) || Count > MaxRecords)
    {
        LogE("Persistence: unsupported or corrupt file %s", Path.c_str());
        return;
    }

    std::vector<CSOEconItem> Records;
    Records.reserve(Count);
    for (std::uint32_t Index = 0; Index < Count; ++Index)
    {
        std::uint32_t Size = 0;
        if (!ReadValue(File, Size) || Size == 0 || Size > MaxRecordSize)
        {
            LogE("Persistence: invalid record %u in %s", Index, Path.c_str());
            Records.clear();
            break;
        }

        std::string Bytes(Size, '\0');
        if (!File.read(Bytes.data(), static_cast<std::streamsize>(Bytes.size())))
        {
            LogE("Persistence: truncated record %u in %s", Index, Path.c_str());
            Records.clear();
            break;
        }

        CSOEconItem Item;
        if (!Item.ParseFromString(Bytes) || !Item.id() || !Item.def_index() ||
            !FakeItems::IsFake(Item.id()) || Item.def_index() > 0xFFFF)
            continue;

        Records.push_back(std::move(Item));
    }

    Saved = std::move(Records);
    for (const CSOEconItem& Item : Saved)
    {
        Loaded.insert(Item.id());
        RestoreEffect(Item);
    }

    Log("Persistence: loaded %zu selected item(s) from %s", Saved.size(), Path.c_str());
}

void SkinPersistence::Apply()
{
    if (Applied)
        return;
    Applied = true;

    std::size_t AppliedCount = 0;
    for (const CSOEconItem& Record : Saved)
    {
        auto Existing = FakeItems::Find(Record.id());
        if (!Existing)
            continue;

        CSOEconItem Item = *Existing;
        Item.set_style(Record.style());
        Item.set_quality(Record.quality());
        Item.set_level(Record.level());
        *Item.mutable_attribute() = Record.attribute();
        // Keep styles usable even if the saved record predates the current
        // UnlockAll attribute (or was written by an older format).
        ItemStyles::UnlockAll(Item);
        *Item.mutable_equipped_state() = Record.equipped_state();
        FakeItems::Store(Item);
        ++AppliedCount;
    }

    Log("Persistence: restored %zu of %zu selected item(s)", AppliedCount, Saved.size());
}

void SkinPersistence::MarkItem(std::uint64_t ItemId)
{
    if (!ItemId)
        return;

    {
        std::lock_guard<std::recursive_mutex> Lock(StateMutex);
        Dirty.insert(ItemId);
        LastChange = Clock::now();
    }
    SaveRequested.store(true);
}

void SkinPersistence::MarkItems(const std::vector<std::uint64_t>& ItemIds)
{
    if (ItemIds.empty())
        return;

    {
        std::lock_guard<std::recursive_mutex> Lock(StateMutex);
        for (std::uint64_t Id : ItemIds)
            if (Id)
                Dirty.insert(Id);
        LastChange = Clock::now();
    }
    SaveRequested.store(true);
}

void SkinPersistence::Update()
{
    if (!SaveRequested.load())
        return;

    {
        std::lock_guard<std::recursive_mutex> Lock(StateMutex);
        if (Clock::now() - LastChange < SaveDelay)
            return;
    }

    SaveNow();
}

void SkinPersistence::Flush()
{
    // Called from DLL_PROCESS_DETACH. Do not wait for the debounce; this is
    // the last chance to persist a change made just before the player quits.
    if (SaveRequested.load())
        SaveNow();
}

