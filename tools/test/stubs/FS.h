#pragma once
// Host stub. The real FS.h declares the filesystem base classes; on the host
// everything the client touches (File, SD_MMC) already lives in the SD_MMC
// stub, so this only has to exist for the include to resolve.
#include "SD_MMC.h"
