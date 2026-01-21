#pragma once
/// @file parser.hpp
/// @brief libclang wrapper for parsing translation units

#include <clang-c/Index.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <optional>

namespace clangsql {

/// RAII wrapper for CXIndex
class Index {
public:
    Index(bool excludeDeclarationsFromPCH = false, bool displayDiagnostics = true)
        : index_(clang_createIndex(excludeDeclarationsFromPCH ? 1 : 0,
                                    displayDiagnostics ? 1 : 0)) {}

    ~Index() {
        if (index_) clang_disposeIndex(index_);
    }

    // Non-copyable
    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;

    // Movable
    Index(Index&& other) noexcept : index_(other.index_) { other.index_ = nullptr; }
    Index& operator=(Index&& other) noexcept {
        if (this != &other) {
            if (index_) clang_disposeIndex(index_);
            index_ = other.index_;
            other.index_ = nullptr;
        }
        return *this;
    }

    CXIndex get() const { return index_; }
    explicit operator bool() const { return index_ != nullptr; }

private:
    CXIndex index_ = nullptr;
};

/// RAII wrapper for CXTranslationUnit
class TranslationUnit {
public:
    TranslationUnit() = default;

    TranslationUnit(CXIndex index, const std::string& filename,
                    const std::vector<std::string>& args = {}) {
        parse(index, filename, args);
    }

    ~TranslationUnit() {
        if (tu_) clang_disposeTranslationUnit(tu_);
    }

    // Non-copyable
    TranslationUnit(const TranslationUnit&) = delete;
    TranslationUnit& operator=(const TranslationUnit&) = delete;

    // Movable
    TranslationUnit(TranslationUnit&& other) noexcept
        : tu_(other.tu_), path_(std::move(other.path_)) {
        other.tu_ = nullptr;
    }

    TranslationUnit& operator=(TranslationUnit&& other) noexcept {
        if (this != &other) {
            if (tu_) clang_disposeTranslationUnit(tu_);
            tu_ = other.tu_;
            path_ = std::move(other.path_);
            other.tu_ = nullptr;
        }
        return *this;
    }

    /// Parse a source file
    bool parse(CXIndex index, const std::string& filename,
               const std::vector<std::string>& args = {}) {
        if (tu_) {
            clang_disposeTranslationUnit(tu_);
            tu_ = nullptr;
        }

        // Convert args to C-style array
        std::vector<const char*> c_args;
        c_args.reserve(args.size());
        for (const auto& arg : args) {
            c_args.push_back(arg.c_str());
        }

        CXErrorCode err = clang_parseTranslationUnit2(
            index,
            filename.c_str(),
            c_args.data(),
            static_cast<int>(c_args.size()),
            nullptr, 0,  // unsaved files
            CXTranslationUnit_DetailedPreprocessingRecord,  // Need bodies for extent
            &tu_
        );

        if (err == CXError_Success && tu_) {
            path_ = filename;
            return true;
        }
        return false;
    }

    /// Reparse the translation unit (for incremental updates)
    bool reparse() {
        if (!tu_) return false;
        return clang_reparseTranslationUnit(tu_, 0, nullptr,
            CXTranslationUnit_DetailedPreprocessingRecord) == 0;
    }

    CXTranslationUnit get() const { return tu_; }
    explicit operator bool() const { return tu_ != nullptr; }

    const std::string& path() const { return path_; }

    /// Get the root cursor
    CXCursor cursor() const {
        return tu_ ? clang_getTranslationUnitCursor(tu_) : clang_getNullCursor();
    }

    /// Get diagnostic count
    unsigned diagnosticCount() const {
        return tu_ ? clang_getNumDiagnostics(tu_) : 0;
    }

private:
    CXTranslationUnit tu_ = nullptr;
    std::string path_;
};

/// Helper to convert CXString to std::string and dispose
inline std::string to_string(CXString cx_str) {
    const char* c_str = clang_getCString(cx_str);
    std::string result = c_str ? c_str : "";
    clang_disposeString(cx_str);
    return result;
}

/// Get cursor spelling (name)
inline std::string cursor_spelling(CXCursor cursor) {
    return to_string(clang_getCursorSpelling(cursor));
}

/// Get cursor USR (Unified Symbol Resolution - stable cross-TU identifier)
inline std::string cursor_usr(CXCursor cursor) {
    return to_string(clang_getCursorUSR(cursor));
}

/// Get cursor type spelling
inline std::string cursor_type_spelling(CXCursor cursor) {
    CXType type = clang_getCursorType(cursor);
    return to_string(clang_getTypeSpelling(type));
}

/// Get cursor location
struct SourceLocation {
    std::string filename;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
};

inline SourceLocation cursor_location(CXCursor cursor) {
    SourceLocation loc;
    CXSourceLocation cx_loc = clang_getCursorLocation(cursor);
    CXFile file;
    clang_getSpellingLocation(cx_loc, &file, &loc.line, &loc.column, &loc.offset);
    if (file) {
        loc.filename = to_string(clang_getFileName(file));
    }
    return loc;
}

/// Get cursor extent (start and end)
struct SourceRange {
    SourceLocation start;
    SourceLocation end;
};

inline SourceRange cursor_extent(CXCursor cursor) {
    SourceRange range;
    CXSourceRange cx_range = clang_getCursorExtent(cursor);

    CXSourceLocation start_loc = clang_getRangeStart(cx_range);
    CXSourceLocation end_loc = clang_getRangeEnd(cx_range);

    CXFile file;
    clang_getSpellingLocation(start_loc, &file, &range.start.line,
                               &range.start.column, &range.start.offset);
    if (file) range.start.filename = to_string(clang_getFileName(file));

    clang_getSpellingLocation(end_loc, &file, &range.end.line,
                               &range.end.column, &range.end.offset);
    if (file) range.end.filename = to_string(clang_getFileName(file));

    return range;
}

/// Visitor callback type
using CursorVisitor = std::function<CXChildVisitResult(CXCursor cursor, CXCursor parent)>;

/// Visit children of a cursor with a C++ callback
inline void visit_children(CXCursor cursor, CursorVisitor visitor) {
    clang_visitChildren(cursor,
        [](CXCursor c, CXCursor parent, CXClientData data) -> CXChildVisitResult {
            auto* visitor = static_cast<CursorVisitor*>(data);
            return (*visitor)(c, parent);
        },
        &visitor);
}

} // namespace clangsql
