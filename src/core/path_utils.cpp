#include "path_utils.hpp"

namespace asyncdownload::core {

std::filesystem::path make_temporary_path(const std::filesystem::path& output_path) {
    return output_path.string() + ".part";
}

std::filesystem::path make_metadata_path(const std::filesystem::path& output_path) {
    return output_path.string() + ".config.json";
}

} // namespace asyncdownload::core
