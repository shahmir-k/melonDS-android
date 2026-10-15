#ifndef EMULATORARGSBUILDER_H
#define EMULATORARGSBUILDER_H

#include "Args.h"
#include "Configuration.h"

namespace MelonDSAndroid
{

std::optional<std::unique_ptr<melonDS::NDSArgs>> BuildArgsFromConfiguration(const EmulatorConfiguration& configuration, int instanceId);

// The DSi BIOS images; the boot-screen-off reset-vector patch is applied unless `dsiWareTitle`
// (the args are built before the cart is known, so a DSiWare title reloads them unpatched)
std::unique_ptr<melonDS::DSiBIOSImage> loadDSiARM9BIOS(const EmulatorConfiguration& configuration, bool dsiWareTitle) noexcept;
std::unique_ptr<melonDS::DSiBIOSImage> loadDSiARM7BIOS(const EmulatorConfiguration& configuration, bool dsiWareTitle) noexcept;

}

#endif
