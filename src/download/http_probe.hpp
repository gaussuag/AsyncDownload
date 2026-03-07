#pragma once

#include "core/models.hpp"

#include <string>

namespace asyncdownload::download {

class HttpProbe {
public:
    [[nodiscard]] core::RemoteProbeResult probe(const std::string& url) const noexcept;
};

} // namespace asyncdownload::download
