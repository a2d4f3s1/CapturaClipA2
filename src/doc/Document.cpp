#include "doc/Document.h"

#include <utility>

namespace ccl::doc {

Document::Document(ccl::capture::DibBuffer image) noexcept
    : image_(std::move(image)) {}

unsigned int NextAnnotationId() noexcept {
    // A plain counter is enough: it only has to stay unique for one run, and
    // one window is one run. Starts at one so that zero can mean "no id", the
    // way the stroke still being drawn is passed to the renderer.
    static unsigned int next = 1;
    return next++;
}

}  // namespace ccl::doc
