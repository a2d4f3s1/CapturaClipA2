#include "doc/History.h"

#include <utility>

namespace ccl::doc {

void History::Record(const AnnotationList& before) {
    undo_.push_back(before);
    // A fresh edit invalidates anything that was undone.
    redo_.clear();
}

bool History::Undo(AnnotationList& current) {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(current);
    current = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool History::Redo(AnnotationList& current) {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(current);
    current = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

}  // namespace ccl::doc
