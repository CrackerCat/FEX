/*
$info$
tags: glue|x86-guest-code
$end_info$
*/

#pragma once
#include <FEXCore/Config/Config.h>

#include <stdint.h>

namespace FEXCore {
class X86GeneratedCode final {
public:
  X86GeneratedCode();
  ~X86GeneratedCode();

  uint64_t SignalReturn{};
  uint64_t CallbackReturn{};

private:
  void *CodePtr{};
  void* AllocateGuestCodeSpace(size_t Size);
};
}
