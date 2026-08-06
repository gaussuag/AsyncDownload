#pragma once

#include <system_error>

namespace asyncdownload::flow {
class PacketProducer;
}

namespace asyncdownload::persistence {
class PersistenceThread;
}

namespace asyncdownload::download {

class PersistencePhase {
public:
    PersistencePhase(
        flow::PacketProducer& producer,
        persistence::PersistenceThread& persistence) noexcept;
    ~PersistencePhase() noexcept;

    PersistencePhase(const PersistencePhase&) = delete;
    PersistencePhase& operator=(const PersistencePhase&) = delete;

    void start();
    [[nodiscard]] std::error_code finish(
        std::error_code first_error) noexcept;

private:
    flow::PacketProducer& producer_;
    persistence::PersistenceThread& persistence_;
    bool armed_ = false;
};

}
