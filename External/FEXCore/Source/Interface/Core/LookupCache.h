#pragma once
#include <FEXCore/Utils/LogManager.h>

#include <cstdint>
#include <functional>
#include <map>
#include <stddef.h>
#include <utility>
#include <vector>

namespace FEXCore {
namespace Context {
  struct Context;
}

class LookupCache {
public:

  struct LookupCacheEntry { 
    uintptr_t HostCode;
    uintptr_t GuestCode;
  };

  LookupCache(FEXCore::Context::Context *CTX);
  ~LookupCache();

  using LookupCacheIter = uintptr_t;
  uintptr_t End() { return 0; }

  uintptr_t FindBlock(uint64_t Address) {
    auto HostCode = FindCodePointerForAddress(Address);
    if (HostCode) {
      return HostCode;
    } else {
      auto HostCode = BlockList.find(Address);

      if (HostCode != BlockList.end()) {
        CacheBlockMapping(Address, HostCode->second);
        return HostCode->second;
      } else {
        return 0;
      }
    }
  }

  std::map<uint64_t, std::vector<uint64_t>> CodePages;

  void AddBlockMapping(uint64_t Address, void *HostCode, uint64_t Start, uint64_t Length) { 
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    auto InsertPoint =
#endif
    BlockList.emplace(Address, (uintptr_t)HostCode);
    LOGMAN_THROW_A_FMT(InsertPoint.second == true, "Dupplicate block mapping added");

    for (auto CurrentPage = Start >> 12, EndPage = (Start + Length) >> 12; CurrentPage <= EndPage; CurrentPage++) {
      CodePages[CurrentPage].push_back(Address);
    }

    // There is no need to update L1 or L2, they will get updated on first lookup
    // However, adding to L1 here increases performance
    auto &L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Address & L1_ENTRIES_MASK];
    L1Entry.GuestCode = Address;
    L1Entry.HostCode = (uintptr_t)HostCode;
  }

  void Erase(uint64_t Address) {

    // Sever any links to this block
    auto lower = BlockLinks.lower_bound({Address, 0});
    auto upper = BlockLinks.upper_bound({Address, UINTPTR_MAX});
    for (auto it = lower; it != upper; it = BlockLinks.erase(it)) {
      it->second();
    }

    // Remove from BlockList
    BlockList.erase(Address);

    // Do L1
    auto &L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Address & L1_ENTRIES_MASK];
    if (L1Entry.GuestCode == Address) {
      L1Entry.GuestCode = L1Entry.HostCode = 0;
    }

    // Do full map
    Address = Address & (VirtualMemSize -1);
    uint64_t PageOffset = Address & (0x0FFF);
    Address >>= 12;

    uintptr_t *Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
    uint64_t LocalPagePointer = Pointers[Address];
    if (!LocalPagePointer) {
      // Page for this code didn't even exist, nothing to do
      return;
    }

    // Page exists, just set the offset to zero
    auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);
    BlockPointers[PageOffset].GuestCode = 0;
    BlockPointers[PageOffset].HostCode = 0;
  }


  void AddBlockLink(uint64_t GuestDestination, uintptr_t HostLink, const std::function<void()> &delinker) {
    BlockLinks.insert({{GuestDestination, HostLink}, delinker});
  }

  void ClearCache();
  void ClearL2Cache();

  void HintUsedRange(uint64_t Address, uint64_t Size);

  uintptr_t GetL1Pointer() const { return L1Pointer; }
  uintptr_t GetPagePointer() const { return PagePointer; }
  uintptr_t GetVirtualMemorySize() const { return VirtualMemSize; }

  constexpr static size_t L1_ENTRIES = 1 * 1024 * 1024; // Must be a power of 2
  constexpr static size_t L1_ENTRIES_MASK = L1_ENTRIES - 1;

private:
  void CacheBlockMapping(uint64_t Address, uintptr_t HostCode) { 
    // Do L1
    auto &L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Address & L1_ENTRIES_MASK];
    L1Entry.GuestCode = Address;
    L1Entry.HostCode = HostCode;

    // Do ful map
    auto FullAddress = Address;
    Address = Address & (VirtualMemSize -1);

    uint64_t PageOffset = Address & (0x0FFF);
    Address >>= 12;
    uintptr_t *Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
    uint64_t LocalPagePointer = Pointers[Address];
    if (!LocalPagePointer) {
      // We don't have a page pointer for this address
      // Allocate one now if we can
      uintptr_t NewPageBacking = AllocateBackingForPage();
      if (!NewPageBacking) {
        // Couldn't allocate, clear L2 and retry
        ClearL2Cache();
        CacheBlockMapping(Address, HostCode);
        return;
      }
      Pointers[Address] = NewPageBacking;
      LocalPagePointer = NewPageBacking;
    }

    // Add the new pointer to the page block
    auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);

    // This silently replaces existing mappings
    BlockPointers[PageOffset].GuestCode = FullAddress;
    BlockPointers[PageOffset].HostCode = HostCode;
  }

  uintptr_t AllocateBackingForPage() {
    uintptr_t NewBase = AllocateOffset;
    uintptr_t NewEnd = AllocateOffset + SIZE_PER_PAGE;

    if (NewEnd >= CODE_SIZE) {
      // We ran out of block backing space. Need to clear the block cache and tell the JIT cores to clear their caches as well
      // Tell whatever is calling this that it needs to do it.
      return 0;
    }

    AllocateOffset = NewEnd;
    return PageMemory + NewBase;
  }

  uintptr_t FindCodePointerForAddress(uint64_t Address) {
    
    // Do L1
    auto &L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Address & L1_ENTRIES_MASK];
    if (L1Entry.GuestCode == Address) {
      return L1Entry.HostCode;
    }

    auto FullAddress = Address;
    Address = Address & (VirtualMemSize -1);

    uint64_t PageOffset = Address & (0x0FFF);
    Address >>= 12;
    uintptr_t *Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
    uint64_t LocalPagePointer = Pointers[Address];
    if (!LocalPagePointer) {
      // We don't have a page pointer for this address
      return 0;
    }

    // Find there pointer for the address in the blocks
    auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);

    if (BlockPointers[PageOffset].GuestCode == FullAddress)
    {
      L1Entry.GuestCode = FullAddress;
      return L1Entry.HostCode = BlockPointers[PageOffset].HostCode;
    }
    else
      return 0;
  }

  uintptr_t PagePointer;
  uintptr_t PageMemory;
  uintptr_t L1Pointer;

  struct BlockLinkTag {
    uint64_t GuestDestination;
    uintptr_t HostLink;

    bool operator <(const BlockLinkTag& other) const {
      if (GuestDestination < other.GuestDestination)
        return true;
      else if (GuestDestination == other.GuestDestination)
        return HostLink < other.HostLink;
      else
        return false;
    }
  };


  std::map<BlockLinkTag, std::function<void()>> BlockLinks;
  std::map<uint64_t, uint64_t> BlockList;

  constexpr static size_t CODE_SIZE = 128 * 1024 * 1024;
  constexpr static size_t SIZE_PER_PAGE = 4096 * sizeof(LookupCacheEntry);
  constexpr static size_t L1_SIZE = L1_ENTRIES * sizeof(LookupCacheEntry);

  size_t AllocateOffset {};

  FEXCore::Context::Context *ctx;
  uint64_t VirtualMemSize{};
};
}
