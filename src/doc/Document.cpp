#include "doc/Document.h"

#include <utility>

namespace ccl::doc {

Document::Document(ccl::capture::DibBuffer image) noexcept
    : image_(std::move(image)) {}

}  // namespace ccl::doc
