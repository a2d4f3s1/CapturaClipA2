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

    // Changing anything goes through one of these two, which move the revision
    // on. They are named apart from the reading ones rather than being const
    // overloads of them on purpose: overloads would be picked by anything
    // holding a non-const document, reads included, and there would be no way
    // left to tell a read from a write.
    //
    // The revision moves on when one of these is called, whether or not
    // anything is actually written. Erring towards one redraw too many is the
    // safe side of that trade.
    ccl::capture::DibBuffer& MutableImage() noexcept {
        ++revision_;
        return image_;
    }
    AnnotationList& MutableAnnotations() noexcept {
        ++revision_;
        return annotations_;
    }

    // Counts changes to the picture and the annotations together. Drawing keeps
    // a copy of what it last drew from, so a frame can tell whether anything it
    // would have to draw again has moved on.
    unsigned int Revision() const noexcept { return revision_; }

private:
    ccl::capture::DibBuffer image_;
    AnnotationList annotations_;
    unsigned int revision_ = 0;
};

}  // namespace ccl::doc
