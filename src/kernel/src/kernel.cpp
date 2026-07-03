// Kernel facade implementation (R-KERNEL-001).

#include "context/kernel/kernel.h"

namespace context::kernel
{

Kernel::Kernel()
    : owned_platform_(std::make_unique<DefaultPlatform>()), platform_(owned_platform_->view())
{
}

Kernel::Kernel(Platform platform) : owned_platform_(nullptr), platform_(platform) {}

Kernel::~Kernel() = default;

} // namespace context::kernel
