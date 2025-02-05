#pragma once
#include "IR.h"
#include <FEXCore/Utils/Allocator.h>

namespace FEXCore::IR {

union PhysicalRegister {
  uint8_t Raw;
  struct {
    uint8_t Reg: 5;
    uint8_t Class: 3;
  };

  bool operator==(const PhysicalRegister &Other) const {
    return Raw == Other.Raw;
  }

  PhysicalRegister(RegisterClassType Class, uint8_t Reg) : Reg(Reg), Class(Class.Val) { }

  static const PhysicalRegister Invalid() {
    return PhysicalRegister(InvalidClass, InvalidReg);
  }

  bool IsInvalid() const {
    return *this == Invalid();
  }
};

static_assert(sizeof(PhysicalRegister) == 1);

// This class is serialized, can't have any holes in the structure
// otherwise ASAN complains about reading uninitialized memory
class FEX_PACKED RegisterAllocationData {
  public:
    uint32_t SpillSlotCount {};
    uint32_t MapCount {};
    bool IsShared {false};
    PhysicalRegister Map[0];

    PhysicalRegister GetNodeRegister(NodeID Node) const {
      return Map[Node.Value];
    }
    uint32_t SpillSlots() const { return SpillSlotCount; }

    static size_t Size(uint32_t NodeCount) {
      return sizeof(RegisterAllocationData) + NodeCount * sizeof(Map[0]);
    }

    void Serialize(std::ostream& stream) const {
      stream.write((const char*)&SpillSlotCount, sizeof(SpillSlotCount));
      stream.write((const char*)&MapCount, sizeof(MapCount));
      // RAData (inline)
      // In file, IsShared is always set
      bool _IsShared = true;
      stream.write((const char*)&_IsShared, sizeof(IsShared));
      stream.write((const char*)&Map[0], sizeof(Map[0]) * MapCount);
    }
};

struct RegisterAllocationDataDeleter {
  void operator()(RegisterAllocationData* r) {
    if (!r->IsShared) {
      FEXCore::Allocator::free(r);
    }
  }
};

}
