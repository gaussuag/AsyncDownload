#include "persistence_phase.hpp"

#include "asyncdownload/error.hpp"
#include "flow/packet_flow.hpp"
#include "persistence/persistence_thread.hpp"

namespace asyncdownload::download {

PersistencePhase::PersistencePhase(
    flow::PacketProducer& producer,
    persistence::PersistenceThread& persistence) noexcept
    : producer_(producer),
      persistence_(persistence) {}

PersistencePhase::~PersistencePhase() noexcept {
    if (armed_) {
        static_cast<void>(finish(make_error_code(
            DownloadErrc::internal_error)));
    }
}

void PersistencePhase::start() {
    persistence_.start();
    armed_ = true;
}

std::error_code PersistencePhase::finish(
    std::error_code first_error) noexcept {
    if (!armed_) {
        return first_error;
    }
    armed_ = false;
    const auto close_error = producer_.close();
    if (!first_error && close_error) {
        first_error = close_error;
    }
    persistence_.join();
    if (!first_error) {
        first_error = persistence_.error();
    }
    return first_error;
}

}
