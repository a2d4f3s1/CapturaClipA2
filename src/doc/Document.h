#pragma once

#include "capture/DibBuffer.h"
#include "doc/Annotation.h"

namespace ccl::doc {

// The captured image plus the annotations laid on top of it.
class Document {
public:
    Document() = default;
    explicit Document(ccl::capture::DibBuffer image) noexcept;

    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    bool IsValid() const noexcept { return image_.IsValid(); }
    int Width() const noexcept { return image_.Width(); }
    int Height() const noexcept { return image_.Height(); }

    const ccl::capture::DibBuffer& Image() const noexcept { return image_; }

    const AnnotationList& Annotations() const noexcept { return annotations_; }
    AnnotationList& Annotations() noexcept { return annotations_; }

private:
    ccl::capture::DibBuffer image_;
    AnnotationList annotations_;
};

}  // namespace ccl::doc
