#pragma once

// ADR-017: see process.h. GPU facts the engine needs (ADR-019).

#include <string>

namespace ustudio::platform {

// The hardware video decode API MLT's avformat producer should ask for
// here, or "" when there's no device to try. Linux: "vaapi" when the
// device MLT will open exists (MLT_AVFORMAT_HWACCEL_DEVICE, else
// /dev/dri/renderD128). A device that exists but can't decode is fine:
// avformat falls back to software. Windows (with the port): "d3d11va".
std::string hardwareDecodeApi();

} // namespace ustudio::platform
