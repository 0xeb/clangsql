// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

/// @file tables.cpp
/// @brief Virtual table implementations for clangsql

#include <clangsql/tables.hpp>
#include <clangsql/parser.hpp>
#include <xsql/vtable.hpp>
#include <xsql/database.hpp>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>

namespace clangsql {

// ============================================================================
// Shared ID Maps (used by all table builders for consistent foreign keys)
// ============================================================================

/// Shared file path to ID mapping
/// Built once from the TU, used by all table builders for consistent file_id
class FileMap {
public:
    std::unordered_map<std::string, int64_t> path_to_id;
    std::string main_file;

    /// Get file ID for a path (returns 0 if not found)
    int64_t get_id(const std::string& path) const {
        if (path.empty()) return 0;
        auto it = path_to_id.find(path);
        return (it != path_to_id.end()) ? it->second : 0;
    }

    /// Check if path is the main file
    bool is_main(const std::string& path) const {
        return !path.empty() && path == main_file;
    }

    /// Check if path is a system header (heuristic)
    bool is_system_header(const std::string& path) const {
        if (path.empty()) return false;
        // Common system paths on different platforms
        if (path.find("/usr/include") != std::string::npos) return true;
        if (path.find("/usr/lib") != std::string::npos) return true;
        if (path.find("\\Program Files\\") != std::string::npos) return true;
        if (path.find("\\Windows Kits\\") != std::string::npos) return true;
        if (path.find("\\Microsoft Visual Studio\\") != std::string::npos) return true;
        if (path.find("\\LLVM\\") != std::string::npos) return true;
        return false;
    }
};

/// Shared USR to ID mapping for classes, free functions, methods, enums
/// Built once and shared across all table builders for consistent foreign keys.
/// Free functions and methods live in disjoint id spaces so that child rows
/// (parameters, variables, string literals) can unambiguously refer to either
/// via `function_id` (free functions) or `method_id` (methods).
class EntityMap {
public:
    std::unordered_map<std::string, int64_t> class_usr_to_id;
    std::unordered_map<std::string, int64_t> free_func_usr_to_id;
    std::unordered_map<std::string, int64_t> method_usr_to_id;
    std::unordered_map<std::string, int64_t> enum_usr_to_id;

    struct CallableRef {
        int64_t id = 0;
        bool is_method = false;
    };

    int64_t get_class_id(const std::string& usr) const {
        auto it = class_usr_to_id.find(usr);
        return (it != class_usr_to_id.end()) ? it->second : 0;
    }

    int64_t get_free_func_id(const std::string& usr) const {
        auto it = free_func_usr_to_id.find(usr);
        return (it != free_func_usr_to_id.end()) ? it->second : 0;
    }

    int64_t get_method_id(const std::string& usr) const {
        auto it = method_usr_to_id.find(usr);
        return (it != method_usr_to_id.end()) ? it->second : 0;
    }

    /// Resolve an enclosing callable USR without caring which kind it is.
    /// Returns {id, is_method}. {0, false} when not found.
    CallableRef resolve_callable(const std::string& usr) const {
        auto fit = free_func_usr_to_id.find(usr);
        if (fit != free_func_usr_to_id.end()) return {fit->second, false};
        auto mit = method_usr_to_id.find(usr);
        if (mit != method_usr_to_id.end()) return {mit->second, true};
        return {};
    }

    int64_t get_enum_id(const std::string& usr) const {
        auto it = enum_usr_to_id.find(usr);
        return (it != enum_usr_to_id.end()) ? it->second : 0;
    }
};

// Forward declarations for _with_map variants
static FileMap build_file_map(const TranslationUnit& tu);
static EntityMap build_entity_map(const TranslationUnit& tu);
std::vector<FileRow> build_files_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<FunctionRow> build_functions_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<ClassRow> build_classes_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<MethodRow> build_methods_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<FieldRow> build_fields_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<VariableRow> build_variables_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<ParameterRow> build_parameters_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<EnumRow> build_enums_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<EnumValueRow> build_enum_values_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);
std::vector<CallRow> build_calls_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<InheritanceRow> build_inheritance_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<StringLiteralRow> build_string_literals_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities);

// ============================================================================
// AST Traversal Helpers
// ============================================================================

namespace {

/// Convert access specifier to string
const char* access_string(CX_CXXAccessSpecifier access) {
    switch (access) {
        case CX_CXXPublic: return "public";
        case CX_CXXProtected: return "protected";
        case CX_CXXPrivate: return "private";
        default: return "unknown";
    }
}

/// Check if cursor is a definition (not just declaration)
bool is_definition(CXCursor cursor) {
    return clang_isCursorDefinition(cursor) != 0;
}

/// Get qualified name (namespace::class::name)
std::string qualified_name(CXCursor cursor) {
    std::string result;

    // Build path from cursor to root
    std::vector<std::string> parts;
    CXCursor current = cursor;

    while (!clang_Cursor_isNull(current) &&
           clang_getCursorKind(current) != CXCursor_TranslationUnit) {
        CXCursorKind kind = clang_getCursorKind(current);

        // Only include named scopes
        if (kind == CXCursor_Namespace ||
            kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl ||
            kind == CXCursor_UnionDecl ||
            kind == CXCursor_EnumDecl ||
            kind == CXCursor_FunctionDecl ||
            kind == CXCursor_CXXMethod ||
            kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor) {
            std::string name = cursor_spelling(current);
            if (!name.empty()) {
                parts.push_back(name);
            }
        }

        current = clang_getCursorSemanticParent(current);
    }

    // Build qualified name in reverse
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!result.empty()) result += "::";
        result += *it;
    }

    return result;
}

/// Check if cursor is in a system header
bool is_in_system_header(CXCursor cursor) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    return clang_Location_isInSystemHeader(loc) != 0;
}

/// Get the full definition extent (including body) for functions/methods
/// Returns the extent including the function body if this is a definition,
/// otherwise returns the cursor's own extent
SourceRange get_definition_extent(CXCursor cursor) {
    SourceRange result = cursor_extent(cursor);

    // If not a definition, just return the declaration extent
    if (!is_definition(cursor)) {
        return result;
    }

    // Find the child with the largest end line (usually the body)
    // This works for both functions and methods
    struct ExtentFinder {
        unsigned max_end_line = 0;
        unsigned max_end_column = 0;
    } finder;
    finder.max_end_line = result.end.line;
    finder.max_end_column = result.end.column;

    clang_visitChildren(cursor,
        [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
            auto* f = static_cast<ExtentFinder*>(data);
            SourceRange child_extent = cursor_extent(child);

            // Track the furthest end position
            if (child_extent.end.line > f->max_end_line ||
                (child_extent.end.line == f->max_end_line &&
                 child_extent.end.column > f->max_end_column)) {
                f->max_end_line = child_extent.end.line;
                f->max_end_column = child_extent.end.column;
            }
            return CXChildVisit_Recurse;  // Check nested children too
        },
        &finder);

    // Update end position to the furthest we found
    result.end.line = finder.max_end_line;
    result.end.column = finder.max_end_column;

    return result;
}

} // anonymous namespace

// ============================================================================
// Build FileMap and EntityMap (must be called first)
// ============================================================================

static FileMap build_file_map(const TranslationUnit& tu) {
    FileMap map;
    std::unordered_set<std::string> seen;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();
    map.main_file = tu.path();

    // Add main file first (ID 1)
    if (!map.main_file.empty()) {
        map.path_to_id[map.main_file] = next_id++;
        seen.insert(map.main_file);
    }

    // Traverse AST to find all referenced files
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        auto loc = cursor_location(cursor);
        if (!loc.filename.empty() && seen.find(loc.filename) == seen.end()) {
            map.path_to_id[loc.filename] = next_id++;
            seen.insert(loc.filename);
        }
        return CXChildVisit_Recurse;
    });

    return map;
}

/// Build shared entity map for classes, functions, and enums
/// This ensures consistent IDs across all tables that reference these entities
static EntityMap build_entity_map(const TranslationUnit& tu) {
    EntityMap map;
    int64_t next_class_id = 1;
    int64_t next_free_func_id = 1;
    int64_t next_method_id = 1;
    int64_t next_enum_id = 1;

    CXCursor root = tu.cursor();

    // Pass 1: collect all classes, free functions, and enums. These are the
    // emittable parent-table entities. We do classes first so that pass 2
    // (methods) can reliably test whether a method's parent class is in
    // `class_usr_to_id` — AST visitation order can produce template classes
    // interleaved with concrete classes, and we need a stable "is this class
    // emittable?" check that doesn't depend on traversal order.
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);
        std::string usr = cursor_usr(cursor);

        if (usr.empty()) {
            return CXChildVisit_Recurse;
        }

        if (kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl ||
            kind == CXCursor_UnionDecl) {
            if (map.class_usr_to_id.find(usr) == map.class_usr_to_id.end()) {
                map.class_usr_to_id[usr] = next_class_id++;
            }
        }
        else if (kind == CXCursor_FunctionDecl) {
            if (map.free_func_usr_to_id.find(usr) == map.free_func_usr_to_id.end()) {
                map.free_func_usr_to_id[usr] = next_free_func_id++;
            }
        }
        else if (kind == CXCursor_EnumDecl) {
            if (map.enum_usr_to_id.find(usr) == map.enum_usr_to_id.end()) {
                map.enum_usr_to_id[usr] = next_enum_id++;
            }
        }

        return CXChildVisit_Recurse;
    });

    // Pass 2: collect methods whose parent class is emittable. Methods on
    // class templates / template specializations are skipped here because
    // `build_methods_table_with_map` wouldn't emit a row for them (it gates
    // on `get_class_id(parent_usr) != 0`). Registering them in
    // `method_usr_to_id` would cause child tables (parameters, variables,
    // string_literals) to route FKs into `method_id` slots that have no
    // matching row in `methods`, producing dangling joins.
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind != CXCursor_CXXMethod &&
            kind != CXCursor_Constructor &&
            kind != CXCursor_Destructor) {
            return CXChildVisit_Recurse;
        }

        std::string usr = cursor_usr(cursor);
        if (usr.empty()) {
            return CXChildVisit_Recurse;
        }

        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        if (parent_usr.empty() ||
            map.class_usr_to_id.find(parent_usr) == map.class_usr_to_id.end()) {
            return CXChildVisit_Recurse;
        }

        if (map.method_usr_to_id.find(usr) == map.method_usr_to_id.end()) {
            map.method_usr_to_id[usr] = next_method_id++;
        }
        return CXChildVisit_Recurse;
    });

    return map;
}

// ============================================================================
// Table Builders (using shared FileMap)
// ============================================================================

std::vector<FileRow> build_files_table(const TranslationUnit& tu) {
    // Build fresh file map for standalone use
    FileMap map = build_file_map(tu);
    return build_files_table_with_map(tu, map);
}

std::vector<FileRow> build_files_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<FileRow> files;
    files.reserve(map.path_to_id.size());

    for (const auto& pair : map.path_to_id) {
        FileRow row;
        row.id = pair.second;
        row.path = pair.first;
        row.is_main_file = map.is_main(pair.first);
        row.mtime = 0;  // TODO: Get actual mtime
        row.is_header = pair.first.find(".h") != std::string::npos ||
                        pair.first.find(".hpp") != std::string::npos;
        row.is_system = map.is_system_header(pair.first);
        files.push_back(row);
    }

    // Sort by ID for consistent ordering
    std::sort(files.begin(), files.end(), [](const FileRow& a, const FileRow& b) {
        return a.id < b.id;
    });

    return files;
}

std::vector<FunctionRow> build_functions_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_functions_table_with_map(tu, map, entities);
}

std::vector<FunctionRow> build_functions_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<FunctionRow> functions;

    CXCursor root = tu.cursor();

    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        // Only process function declarations (not methods - those go in methods table)
        if (kind != CXCursor_FunctionDecl) {
            return CXChildVisit_Recurse;
        }

        std::string usr = cursor_usr(cursor);
        int64_t id = entities.get_free_func_id(usr);
        if (id == 0) {
            return CXChildVisit_Recurse;  // Not in entity map
        }

        auto loc = cursor_location(cursor);
        auto extent = get_definition_extent(cursor);  // Includes body for definitions

        FunctionRow row;
        row.id = id;  // Use shared ID from EntityMap
        row.usr = usr;
        row.name = cursor_spelling(cursor);
        row.qualified_name = qualified_name(cursor);

        // Return type
        CXType func_type = clang_getCursorType(cursor);
        CXType ret_type = clang_getResultType(func_type);
        row.return_type = to_string(clang_getTypeSpelling(ret_type));

        row.file_id = map.get_id(loc.filename);
        row.line = loc.line;
        row.column = loc.column;
        row.end_line = extent.end.line;
        row.end_column = extent.end.column;
        row.is_definition = is_definition(cursor);
        row.is_inline = clang_Cursor_isFunctionInlined(cursor) != 0;
        row.is_variadic = clang_Cursor_isVariadic(cursor) != 0;

        // Check storage class for static
        CX_StorageClass storage = clang_Cursor_getStorageClass(cursor);
        row.is_static = (storage == CX_SC_Static);

        // Track if in system header
        row.is_system = is_in_system_header(cursor);

        functions.push_back(row);

        return CXChildVisit_Recurse;
    });

    return functions;
}

std::vector<ClassRow> build_classes_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_classes_table_with_map(tu, map, entities);
}

std::vector<ClassRow> build_classes_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<ClassRow> classes;

    CXCursor root = tu.cursor();

    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        // Only process class/struct/union declarations
        if (kind != CXCursor_ClassDecl &&
            kind != CXCursor_StructDecl &&
            kind != CXCursor_UnionDecl) {
            return CXChildVisit_Recurse;
        }

        std::string usr = cursor_usr(cursor);
        int64_t id = entities.get_class_id(usr);
        if (id == 0) {
            return CXChildVisit_Recurse;  // Not in entity map
        }

        auto loc = cursor_location(cursor);

        ClassRow row;
        row.id = id;  // Use shared ID from EntityMap
        row.usr = usr;
        row.name = cursor_spelling(cursor);
        row.qualified_name = qualified_name(cursor);

        switch (kind) {
            case CXCursor_ClassDecl: row.kind = "class"; break;
            case CXCursor_StructDecl: row.kind = "struct"; break;
            case CXCursor_UnionDecl: row.kind = "union"; break;
            default: row.kind = "unknown"; break;
        }

        row.file_id = map.get_id(loc.filename);
        row.line = loc.line;
        row.is_definition = is_definition(cursor);
        row.is_abstract = clang_CXXRecord_isAbstract(cursor) != 0;
        row.namespace_id = 0;  // TODO: Track namespace hierarchy
        row.is_system = is_in_system_header(cursor);

        classes.push_back(row);

        return CXChildVisit_Recurse;
    });

    return classes;
}

std::vector<MethodRow> build_methods_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_methods_table_with_map(tu, map, entities);
}

std::vector<MethodRow> build_methods_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<MethodRow> methods;
    (void)map;  // map accepted for builder-signature uniformity; methods have no file_id column

    CXCursor root = tu.cursor();

    // Collect methods using shared EntityMap for class_id and method id
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_CXXMethod &&
            kind != CXCursor_Constructor &&
            kind != CXCursor_Destructor) {
            return CXChildVisit_Recurse;
        }

        std::string usr = cursor_usr(cursor);
        int64_t id = entities.get_method_id(usr);
        if (id == 0) {
            return CXChildVisit_Recurse;  // Not in entity map (empty USR, etc.)
        }

        // Get parent class
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        int64_t class_id = entities.get_class_id(parent_usr);
        if (class_id == 0) {
            return CXChildVisit_Recurse;  // Parent class not in entity map
        }

        auto loc = cursor_location(cursor);
        auto extent = get_definition_extent(cursor);  // Includes body for definitions

        MethodRow row;
        row.id = id;               // Stable, USR-keyed id from EntityMap
        row.usr = usr;
        row.class_id = class_id;   // Use shared ID from EntityMap
        row.name = cursor_spelling(cursor);
        row.qualified_name = qualified_name(cursor);

        // Return type (not meaningful for constructors/destructors)
        if (kind == CXCursor_CXXMethod) {
            CXType func_type = clang_getCursorType(cursor);
            CXType ret_type = clang_getResultType(func_type);
            row.return_type = to_string(clang_getTypeSpelling(ret_type));
        }

        row.access = access_string(clang_getCXXAccessSpecifier(cursor));
        row.is_virtual = clang_CXXMethod_isVirtual(cursor) != 0;
        row.is_pure_virtual = clang_CXXMethod_isPureVirtual(cursor) != 0;
        row.is_static = clang_CXXMethod_isStatic(cursor) != 0;
        row.is_const = clang_CXXMethod_isConst(cursor) != 0;

        row.is_override = false;  // TODO
        row.is_final = false;     // TODO

        row.line = loc.line;
        row.column = loc.column;
        row.end_line = extent.end.line;
        row.end_column = extent.end.column;
        row.is_system = is_in_system_header(cursor);

        methods.push_back(row);

        return CXChildVisit_Recurse;
    });

    return methods;
}

std::vector<FieldRow> build_fields_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_fields_table_with_map(tu, map, entities);
}

std::vector<FieldRow> build_fields_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<FieldRow> fields;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // Collect fields using shared EntityMap for class_id
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_FieldDecl) {
            return CXChildVisit_Recurse;
        }

        // Get parent class
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        int64_t class_id = entities.get_class_id(parent_usr);
        if (class_id == 0) {
            return CXChildVisit_Recurse;  // Parent class not in entity map
        }

        FieldRow row;
        row.id = next_id++;
        row.class_id = class_id;  // Use shared ID from EntityMap
        row.name = cursor_spelling(cursor);
        row.type = cursor_type_spelling(cursor);
        row.access = access_string(clang_getCXXAccessSpecifier(cursor));

        row.is_static = false;
        row.is_mutable = clang_CXXField_isMutable(cursor) != 0;
        row.bit_width = clang_getFieldDeclBitWidth(cursor);

        // Offset (in bits)
        CXType parent_type = clang_getCursorType(parent);
        row.offset_bits = clang_Type_getOffsetOf(parent_type, row.name.c_str());
        row.is_system = is_in_system_header(cursor);

        fields.push_back(row);

        return CXChildVisit_Recurse;
    });

    return fields;
}

std::vector<VariableRow> build_variables_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_variables_table_with_map(tu, map, entities);
}

std::vector<VariableRow> build_variables_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<VariableRow> variables;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // Collect variables using shared EntityMap for function_id
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_VarDecl) {
            return CXChildVisit_Recurse;
        }

        auto loc = cursor_location(cursor);

        VariableRow row;
        row.id = next_id++;
        row.usr = cursor_usr(cursor);
        row.name = cursor_spelling(cursor);
        row.type = cursor_type_spelling(cursor);
        row.file_id = map.get_id(loc.filename);
        row.line = loc.line;

        // Determine scope
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        CXCursorKind parent_kind = clang_getCursorKind(parent);

        if (parent_kind == CXCursor_FunctionDecl ||
            parent_kind == CXCursor_CXXMethod ||
            parent_kind == CXCursor_Constructor ||
            parent_kind == CXCursor_Destructor) {
            // Local variable — route to function_id or method_id based on parent kind.
            std::string parent_usr = cursor_usr(parent);
            EntityMap::CallableRef ref = entities.resolve_callable(parent_usr);
            row.function_id = ref.is_method ? 0 : ref.id;
            row.method_id = ref.is_method ? ref.id : 0;

            CX_StorageClass storage = clang_Cursor_getStorageClass(cursor);
            row.scope_kind = (storage == CX_SC_Static) ? "static_local" : "local";
        } else {
            // Global variable
            row.function_id = 0;
            row.method_id = 0;
            row.scope_kind = "global";
        }

        row.namespace_id = 0;  // TODO: Track namespace

        CX_StorageClass storage = clang_Cursor_getStorageClass(cursor);
        switch (storage) {
            case CX_SC_Static: row.storage_class = "static"; break;
            case CX_SC_Extern: row.storage_class = "extern"; break;
            case CX_SC_Register: row.storage_class = "register"; break;
            default: row.storage_class = "none"; break;
        }

        // Check if const
        CXType type = clang_getCursorType(cursor);
        row.is_const = clang_isConstQualifiedType(type) != 0;
        row.is_inline = false;  // TODO
        row.is_system = is_in_system_header(cursor);

        variables.push_back(row);

        return CXChildVisit_Recurse;
    });

    return variables;
}

std::vector<ParameterRow> build_parameters_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_parameters_table_with_map(tu, map, entities);
}

std::vector<ParameterRow> build_parameters_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<ParameterRow> parameters;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // Collect parameters using shared EntityMap for function_id
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_ParmDecl) {
            return CXChildVisit_Recurse;
        }

        // Get parent function or method
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        EntityMap::CallableRef ref = entities.resolve_callable(parent_usr);
        if (ref.id == 0) {
            return CXChildVisit_Recurse;  // Parent callable not in entity map
        }

        ParameterRow row;
        row.id = next_id++;
        row.function_id = ref.is_method ? 0 : ref.id;
        row.method_id = ref.is_method ? ref.id : 0;
        row.name = cursor_spelling(cursor);
        row.type = cursor_type_spelling(cursor);

        // Get parameter index
        int index = 0;
        visit_children(parent, [&](CXCursor child, CXCursor) -> CXChildVisitResult {
            if (clang_getCursorKind(child) == CXCursor_ParmDecl) {
                if (clang_equalCursors(child, cursor)) {
                    return CXChildVisit_Break;
                }
                index++;
            }
            return CXChildVisit_Continue;
        });
        row.index = index;

        // Check for default argument
        bool has_default = false;
        visit_children(cursor, [&](CXCursor child, CXCursor) -> CXChildVisitResult {
            if (clang_isExpression(clang_getCursorKind(child))) {
                has_default = true;
                return CXChildVisit_Break;
            }
            return CXChildVisit_Continue;
        });
        row.has_default = has_default;
        row.is_system = is_in_system_header(cursor);

        parameters.push_back(row);

        return CXChildVisit_Recurse;
    });

    return parameters;
}

std::vector<EnumRow> build_enums_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_enums_table_with_map(tu, map, entities);
}

std::vector<EnumRow> build_enums_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<EnumRow> enums;

    CXCursor root = tu.cursor();

    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_EnumDecl) {
            return CXChildVisit_Recurse;
        }

        std::string usr = cursor_usr(cursor);
        int64_t id = entities.get_enum_id(usr);
        if (id == 0) {
            return CXChildVisit_Recurse;  // Not in entity map
        }

        auto loc = cursor_location(cursor);

        EnumRow row;
        row.id = id;  // Use shared ID from EntityMap
        row.usr = usr;
        row.name = cursor_spelling(cursor);

        // Get underlying type
        CXType enum_type = clang_getEnumDeclIntegerType(cursor);
        row.underlying_type = to_string(clang_getTypeSpelling(enum_type));

        // Check if scoped (enum class)
        row.is_scoped = clang_EnumDecl_isScoped(cursor) != 0;

        row.file_id = map.get_id(loc.filename);
        row.line = loc.line;
        row.is_system = is_in_system_header(cursor);

        enums.push_back(row);

        return CXChildVisit_Recurse;
    });

    return enums;
}

std::vector<EnumValueRow> build_enum_values_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_enum_values_table_with_map(tu, map, entities);
}

std::vector<EnumValueRow> build_enum_values_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<EnumValueRow> values;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // Collect enum values using shared EntityMap for enum_id
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        if (clang_getCursorKind(cursor) != CXCursor_EnumConstantDecl) {
            return CXChildVisit_Recurse;
        }

        // Get parent enum
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        int64_t enum_id = entities.get_enum_id(parent_usr);
        if (enum_id == 0) {
            return CXChildVisit_Recurse;  // Parent enum not in entity map
        }

        EnumValueRow row;
        row.id = next_id++;
        row.enum_id = enum_id;  // Use shared ID from EntityMap
        row.name = cursor_spelling(cursor);
        row.value = clang_getEnumConstantDeclValue(cursor);
        row.is_system = is_in_system_header(cursor);

        values.push_back(row);

        return CXChildVisit_Recurse;
    });

    return values;
}

std::vector<CallRow> build_calls_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    return build_calls_table_with_map(tu, map);
}

std::vector<CallRow> build_calls_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<CallRow> calls;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // First, find all function definitions and collect calls within them
    visit_children(root, [&](CXCursor func_cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind func_kind = clang_getCursorKind(func_cursor);

        // Only process function/method definitions
        if (func_kind != CXCursor_FunctionDecl &&
            func_kind != CXCursor_CXXMethod &&
            func_kind != CXCursor_Constructor &&
            func_kind != CXCursor_Destructor) {
            return CXChildVisit_Recurse;
        }

        // Only process definitions, not declarations
        if (!is_definition(func_cursor)) {
            return CXChildVisit_Recurse;
        }

        std::string caller_usr = cursor_usr(func_cursor);

        // Visit children of this function to find call expressions
        visit_children(func_cursor, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
            if (clang_getCursorKind(cursor) != CXCursor_CallExpr) {
                return CXChildVisit_Recurse;
            }

            // Get the callee (the function being called)
            CXCursor callee = clang_getCursorReferenced(cursor);
            if (clang_Cursor_isNull(callee)) {
                return CXChildVisit_Recurse;
            }

            CXCursorKind callee_kind = clang_getCursorKind(callee);

            // Only track calls to functions/methods
            if (callee_kind != CXCursor_FunctionDecl &&
                callee_kind != CXCursor_CXXMethod &&
                callee_kind != CXCursor_Constructor &&
                callee_kind != CXCursor_Destructor) {
                return CXChildVisit_Recurse;
            }

            auto loc = cursor_location(cursor);

            CallRow row;
            row.id = next_id++;
            row.caller_usr = caller_usr;
            row.callee_usr = cursor_usr(callee);
            row.callee_name = cursor_spelling(callee);
            row.file_id = map.get_id(loc.filename);
            row.line = loc.line;
            row.column = loc.column;

            // Check if it's a virtual call
            row.is_virtual = (callee_kind == CXCursor_CXXMethod &&
                              clang_CXXMethod_isVirtual(callee) != 0);

            row.is_system = is_in_system_header(cursor);

            calls.push_back(row);

            return CXChildVisit_Recurse;
        });

        return CXChildVisit_Recurse;
    });

    return calls;
}

std::vector<InheritanceRow> build_inheritance_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    return build_inheritance_table_with_map(tu, map);
}

std::vector<InheritanceRow> build_inheritance_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<InheritanceRow> inheritance;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // Find all class/struct declarations and their base specifiers
    visit_children(root, [&](CXCursor class_cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(class_cursor);

        // Only process class/struct declarations
        if (kind != CXCursor_ClassDecl && kind != CXCursor_StructDecl) {
            return CXChildVisit_Recurse;
        }

        std::string derived_usr = cursor_usr(class_cursor);
        std::string derived_name = cursor_spelling(class_cursor);

        // Visit children to find base specifiers
        visit_children(class_cursor, [&](CXCursor child, CXCursor) -> CXChildVisitResult {
            if (clang_getCursorKind(child) != CXCursor_CXXBaseSpecifier) {
                return CXChildVisit_Continue;
            }

            // Get the base class type and cursor
            CXType base_type = clang_getCursorType(child);
            CXCursor base_class = clang_getTypeDeclaration(base_type);

            auto loc = cursor_location(class_cursor);

            InheritanceRow row;
            row.id = next_id++;
            row.derived_usr = derived_usr;
            row.derived_name = derived_name;
            row.base_usr = cursor_usr(base_class);
            row.base_name = to_string(clang_getTypeSpelling(base_type));

            // Get access specifier
            row.access = access_string(clang_getCXXAccessSpecifier(child));

            // Get file info
            row.file_id = map.get_id(loc.filename);

            // Check for virtual inheritance
            row.is_virtual = clang_isVirtualBase(child) != 0;

            row.is_system = is_in_system_header(class_cursor);

            inheritance.push_back(row);

            return CXChildVisit_Continue;
        });

        return CXChildVisit_Recurse;
    });

    return inheritance;
}

std::vector<StringLiteralRow> build_string_literals_table(const TranslationUnit& tu) {
    FileMap map = build_file_map(tu);
    EntityMap entities = build_entity_map(tu);
    return build_string_literals_table_with_map(tu, map, entities);
}

std::vector<StringLiteralRow> build_string_literals_table_with_map(const TranslationUnit& tu, const FileMap& map, const EntityMap& entities) {
    std::vector<StringLiteralRow> strings;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // Context for tracking enclosing function
    struct VisitorContext {
        std::vector<StringLiteralRow>& strings;
        const FileMap& map;
        const EntityMap& entities;
        int64_t& next_id;
        std::string current_func_usr;  // USR of enclosing function (empty if global)
        int64_t current_func_id;       // ID of enclosing function (0 if global)
    };

    VisitorContext ctx{strings, map, entities, next_id, "", 0};

    // Recursive visitor that tracks function context
    std::function<CXChildVisitResult(CXCursor, CXCursor, VisitorContext&)> visit_recursive;
    visit_recursive = [&visit_recursive](CXCursor cursor, CXCursor, VisitorContext& ctx) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        // Track enclosing function
        std::string saved_func_usr = ctx.current_func_usr;
        if (kind == CXCursor_FunctionDecl ||
            kind == CXCursor_CXXMethod ||
            kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor) {
            if (is_definition(cursor)) {
                ctx.current_func_usr = cursor_usr(cursor);
            }
        }

        // Capture string literals
        if (kind == CXCursor_StringLiteral) {
            auto loc = cursor_location(cursor);

            StringLiteralRow row;
            row.id = ctx.next_id++;
            row.file_id = ctx.map.get_id(loc.filename);
            row.line = loc.line;
            row.column = loc.column;
            row.function_usr = ctx.current_func_usr;
            row.is_system = is_in_system_header(cursor);

            // Get the string content
            // Note: clang_getCursorSpelling returns the literal as written (with quotes)
            std::string literal = cursor_spelling(cursor);

            // Check for wide string (L"...")
            row.is_wide = (!literal.empty() && literal[0] == 'L');

            // Extract content (remove quotes and prefix)
            if (!literal.empty()) {
                size_t start = literal.find('"');
                size_t end = literal.rfind('"');
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    row.content = literal.substr(start + 1, end - start - 1);
                } else {
                    row.content = literal;
                }
            }

            ctx.strings.push_back(row);
        }

        // Visit children
        clang_visitChildren(cursor,
            [](CXCursor child, CXCursor parent, CXClientData data) -> CXChildVisitResult {
                auto& ctx = *static_cast<VisitorContext*>(data);
                // Call our recursive function
                std::function<CXChildVisitResult(CXCursor, CXCursor, VisitorContext&)>* fn = nullptr;
                // We need to access the outer function - use a different approach
                return CXChildVisit_Recurse;  // Let clang handle recursion
            },
            &ctx);

        // Restore function context
        ctx.current_func_usr = saved_func_usr;

        return CXChildVisit_Recurse;
    };

    // Start visiting from root
    visit_children(root, [&](CXCursor cursor, CXCursor parent) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        // Track enclosing function
        if (kind == CXCursor_FunctionDecl ||
            kind == CXCursor_CXXMethod ||
            kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor) {
            if (is_definition(cursor)) {
                std::string func_usr = cursor_usr(cursor);
                EntityMap::CallableRef enclosing = entities.resolve_callable(func_usr);

                // Visit this function's children to find string literals
                visit_children(cursor, [&](CXCursor child, CXCursor) -> CXChildVisitResult {
                    if (clang_getCursorKind(child) == CXCursor_StringLiteral) {
                        auto loc = cursor_location(child);

                        StringLiteralRow row;
                        row.id = next_id++;
                        row.file_id = map.get_id(loc.filename);
                        row.line = loc.line;
                        row.column = loc.column;
                        row.function_usr = func_usr;
                        row.func_id = enclosing.is_method ? 0 : enclosing.id;
                        row.method_id = enclosing.is_method ? enclosing.id : 0;
                        row.is_system = is_in_system_header(child);

                        // Get the string content
                        std::string literal = cursor_spelling(child);
                        row.is_wide = (!literal.empty() && literal[0] == 'L');

                        // Extract content (remove quotes and prefix)
                        if (!literal.empty()) {
                            size_t start = literal.find('"');
                            size_t end = literal.rfind('"');
                            if (start != std::string::npos && end != std::string::npos && end > start) {
                                row.content = literal.substr(start + 1, end - start - 1);
                            } else {
                                row.content = literal;
                            }
                        }

                        strings.push_back(row);
                    }
                    return CXChildVisit_Recurse;
                });
            }
        }
        // Also capture global string literals (outside functions)
        else if (kind == CXCursor_StringLiteral) {
            auto loc = cursor_location(cursor);

            StringLiteralRow row;
            row.id = next_id++;
            row.file_id = map.get_id(loc.filename);
            row.line = loc.line;
            row.column = loc.column;
            row.function_usr = "";  // Global scope
            row.func_id = 0;        // No enclosing free function
            row.method_id = 0;      // No enclosing method
            row.is_system = is_in_system_header(cursor);

            std::string literal = cursor_spelling(cursor);
            row.is_wide = (!literal.empty() && literal[0] == 'L');

            if (!literal.empty()) {
                size_t start = literal.find('"');
                size_t end = literal.rfind('"');
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    row.content = literal.substr(start + 1, end - start - 1);
                } else {
                    row.content = literal;
                }
            }

            strings.push_back(row);
        }

        return CXChildVisit_Recurse;
    });

    return strings;
}

// ============================================================================
// Virtual Table Registration
// ============================================================================

void register_tables(xsql::Database& db, const TranslationUnit& tu,
                     const std::string& schema) {
    // Build shared maps ONCE for consistent IDs across all tables
    FileMap file_map = build_file_map(tu);
    EntityMap entity_map = build_entity_map(tu);

    // Pre-build all table data (captures by value avoid dangling reference issues)
    auto files_data = std::make_shared<std::vector<FileRow>>(build_files_table_with_map(tu, file_map));
    auto functions_data = std::make_shared<std::vector<FunctionRow>>(build_functions_table_with_map(tu, file_map, entity_map));
    auto classes_data = std::make_shared<std::vector<ClassRow>>(build_classes_table_with_map(tu, file_map, entity_map));
    auto methods_data = std::make_shared<std::vector<MethodRow>>(build_methods_table_with_map(tu, file_map, entity_map));
    auto fields_data = std::make_shared<std::vector<FieldRow>>(build_fields_table_with_map(tu, file_map, entity_map));
    auto variables_data = std::make_shared<std::vector<VariableRow>>(build_variables_table_with_map(tu, file_map, entity_map));
    auto parameters_data = std::make_shared<std::vector<ParameterRow>>(build_parameters_table_with_map(tu, file_map, entity_map));
    auto enums_data = std::make_shared<std::vector<EnumRow>>(build_enums_table_with_map(tu, file_map, entity_map));
    auto enum_values_data = std::make_shared<std::vector<EnumValueRow>>(build_enum_values_table_with_map(tu, file_map, entity_map));
    auto calls_data = std::make_shared<std::vector<CallRow>>(build_calls_table_with_map(tu, file_map));
    auto inheritance_data = std::make_shared<std::vector<InheritanceRow>>(build_inheritance_table_with_map(tu, file_map));
    auto string_literals_data = std::make_shared<std::vector<StringLiteralRow>>(build_string_literals_table_with_map(tu, file_map, entity_map));

    // Use schema as table name prefix (e.g., "main" -> "main_functions")
    // Empty schema means no prefix (backward compatible)
    std::string prefix = schema.empty() ? "" : schema + "_";

    // Files table
    auto files_def = xsql::cached_table<FileRow>((prefix + "files").c_str())
        .cache_builder([files_data](std::vector<FileRow>& cache) {
            cache = *files_data;
        })
        .column_int64("id", [](const FileRow& r) { return r.id; })
        .column_text("path", [](const FileRow& r) { return r.path; })
        .column_int("is_main_file", [](const FileRow& r) { return r.is_main_file ? 1 : 0; })
        .column_int64("mtime", [](const FileRow& r) { return static_cast<int64_t>(r.mtime); })
        .column_int("is_header", [](const FileRow& r) { return r.is_header ? 1 : 0; })
        .column_int("is_system", [](const FileRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(files_def);

    // Functions table
    auto functions_def = xsql::cached_table<FunctionRow>((prefix + "functions").c_str())
        .cache_builder([functions_data](std::vector<FunctionRow>& cache) {
            cache = *functions_data;
        })
        .column_int64("id", [](const FunctionRow& r) { return r.id; })
        .column_text("usr", [](const FunctionRow& r) { return r.usr; })
        .column_text("name", [](const FunctionRow& r) { return r.name; })
        .column_text("qualified_name", [](const FunctionRow& r) { return r.qualified_name; })
        .column_text("return_type", [](const FunctionRow& r) { return r.return_type; })
        .column_int64("file_id", [](const FunctionRow& r) { return r.file_id; })
        .column_int("line", [](const FunctionRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const FunctionRow& r) { return static_cast<int>(r.column); })
        .column_int("end_line", [](const FunctionRow& r) { return static_cast<int>(r.end_line); })
        .column_int("end_column", [](const FunctionRow& r) { return static_cast<int>(r.end_column); })
        .column_int("is_definition", [](const FunctionRow& r) { return r.is_definition ? 1 : 0; })
        .column_int("is_inline", [](const FunctionRow& r) { return r.is_inline ? 1 : 0; })
        .column_int("is_variadic", [](const FunctionRow& r) { return r.is_variadic ? 1 : 0; })
        .column_int("is_static", [](const FunctionRow& r) { return r.is_static ? 1 : 0; })
        .column_int("is_system", [](const FunctionRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(functions_def);

    // Classes table
    auto classes_def = xsql::cached_table<ClassRow>((prefix + "classes").c_str())
        .cache_builder([classes_data](std::vector<ClassRow>& cache) {
            cache = *classes_data;
        })
        .column_int64("id", [](const ClassRow& r) { return r.id; })
        .column_text("usr", [](const ClassRow& r) { return r.usr; })
        .column_text("name", [](const ClassRow& r) { return r.name; })
        .column_text("qualified_name", [](const ClassRow& r) { return r.qualified_name; })
        .column_text("kind", [](const ClassRow& r) { return r.kind; })
        .column_int64("file_id", [](const ClassRow& r) { return r.file_id; })
        .column_int("line", [](const ClassRow& r) { return static_cast<int>(r.line); })
        .column_int("is_definition", [](const ClassRow& r) { return r.is_definition ? 1 : 0; })
        .column_int("is_abstract", [](const ClassRow& r) { return r.is_abstract ? 1 : 0; })
        .column_int64("namespace_id", [](const ClassRow& r) { return r.namespace_id; })
        .column_int("is_system", [](const ClassRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(classes_def);

    // Methods table
    auto methods_def = xsql::cached_table<MethodRow>((prefix + "methods").c_str())
        .cache_builder([methods_data](std::vector<MethodRow>& cache) {
            cache = *methods_data;
        })
        .column_int64("id", [](const MethodRow& r) { return r.id; })
        .column_text("usr", [](const MethodRow& r) { return r.usr; })
        .column_int64("class_id", [](const MethodRow& r) { return r.class_id; })
        .column_text("name", [](const MethodRow& r) { return r.name; })
        .column_text("qualified_name", [](const MethodRow& r) { return r.qualified_name; })
        .column_text("return_type", [](const MethodRow& r) { return r.return_type; })
        .column_text("access", [](const MethodRow& r) { return r.access; })
        .column_int("is_virtual", [](const MethodRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_pure_virtual", [](const MethodRow& r) { return r.is_pure_virtual ? 1 : 0; })
        .column_int("is_static", [](const MethodRow& r) { return r.is_static ? 1 : 0; })
        .column_int("is_const", [](const MethodRow& r) { return r.is_const ? 1 : 0; })
        .column_int("is_override", [](const MethodRow& r) { return r.is_override ? 1 : 0; })
        .column_int("is_final", [](const MethodRow& r) { return r.is_final ? 1 : 0; })
        .column_int("line", [](const MethodRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const MethodRow& r) { return static_cast<int>(r.column); })
        .column_int("end_line", [](const MethodRow& r) { return static_cast<int>(r.end_line); })
        .column_int("end_column", [](const MethodRow& r) { return static_cast<int>(r.end_column); })
        .column_int("is_system", [](const MethodRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(methods_def);

    // Fields table
    auto fields_def = xsql::cached_table<FieldRow>((prefix + "fields").c_str())
        .cache_builder([fields_data](std::vector<FieldRow>& cache) {
            cache = *fields_data;
        })
        .column_int64("id", [](const FieldRow& r) { return r.id; })
        .column_int64("class_id", [](const FieldRow& r) { return r.class_id; })
        .column_text("name", [](const FieldRow& r) { return r.name; })
        .column_text("type", [](const FieldRow& r) { return r.type; })
        .column_text("access", [](const FieldRow& r) { return r.access; })
        .column_int("is_static", [](const FieldRow& r) { return r.is_static ? 1 : 0; })
        .column_int("is_mutable", [](const FieldRow& r) { return r.is_mutable ? 1 : 0; })
        .column_int("bit_width", [](const FieldRow& r) { return r.bit_width; })
        .column_int64("offset_bits", [](const FieldRow& r) { return r.offset_bits; })
        .column_int("is_system", [](const FieldRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(fields_def);

    // Variables table
    auto variables_def = xsql::cached_table<VariableRow>((prefix + "variables").c_str())
        .cache_builder([variables_data](std::vector<VariableRow>& cache) {
            cache = *variables_data;
        })
        .column_int64("id", [](const VariableRow& r) { return r.id; })
        .column_text("usr", [](const VariableRow& r) { return r.usr; })
        .column_text("name", [](const VariableRow& r) { return r.name; })
        .column_text("type", [](const VariableRow& r) { return r.type; })
        .column_int64("file_id", [](const VariableRow& r) { return r.file_id; })
        .column_int("line", [](const VariableRow& r) { return static_cast<int>(r.line); })
        .column_int64("function_id", [](const VariableRow& r) { return r.function_id; })
        .column_int64("method_id", [](const VariableRow& r) { return r.method_id; })
        .column_int64("namespace_id", [](const VariableRow& r) { return r.namespace_id; })
        .column_text("scope_kind", [](const VariableRow& r) { return r.scope_kind; })
        .column_text("storage_class", [](const VariableRow& r) { return r.storage_class; })
        .column_int("is_const", [](const VariableRow& r) { return r.is_const ? 1 : 0; })
        .column_int("is_inline", [](const VariableRow& r) { return r.is_inline ? 1 : 0; })
        .column_int("is_system", [](const VariableRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(variables_def);

    // Parameters table
    auto parameters_def = xsql::cached_table<ParameterRow>((prefix + "parameters").c_str())
        .cache_builder([parameters_data](std::vector<ParameterRow>& cache) {
            cache = *parameters_data;
        })
        .column_int64("id", [](const ParameterRow& r) { return r.id; })
        .column_int64("function_id", [](const ParameterRow& r) { return r.function_id; })
        .column_int64("method_id", [](const ParameterRow& r) { return r.method_id; })
        .column_text("name", [](const ParameterRow& r) { return r.name; })
        .column_text("type", [](const ParameterRow& r) { return r.type; })
        .column_int("index_", [](const ParameterRow& r) { return r.index; })
        .column_int("has_default", [](const ParameterRow& r) { return r.has_default ? 1 : 0; })
        .column_int("is_system", [](const ParameterRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(parameters_def);

    // Enums table
    auto enums_def = xsql::cached_table<EnumRow>((prefix + "enums").c_str())
        .cache_builder([enums_data](std::vector<EnumRow>& cache) {
            cache = *enums_data;
        })
        .column_int64("id", [](const EnumRow& r) { return r.id; })
        .column_text("usr", [](const EnumRow& r) { return r.usr; })
        .column_text("name", [](const EnumRow& r) { return r.name; })
        .column_text("underlying_type", [](const EnumRow& r) { return r.underlying_type; })
        .column_int("is_scoped", [](const EnumRow& r) { return r.is_scoped ? 1 : 0; })
        .column_int64("file_id", [](const EnumRow& r) { return r.file_id; })
        .column_int("line", [](const EnumRow& r) { return static_cast<int>(r.line); })
        .column_int("is_system", [](const EnumRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(enums_def);

    // Enum values table
    auto enum_values_def = xsql::cached_table<EnumValueRow>((prefix + "enum_values").c_str())
        .cache_builder([enum_values_data](std::vector<EnumValueRow>& cache) {
            cache = *enum_values_data;
        })
        .column_int64("id", [](const EnumValueRow& r) { return r.id; })
        .column_int64("enum_id", [](const EnumValueRow& r) { return r.enum_id; })
        .column_text("name", [](const EnumValueRow& r) { return r.name; })
        .column_int64("value", [](const EnumValueRow& r) { return r.value; })
        .column_int("is_system", [](const EnumValueRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(enum_values_def);

    // Calls table (function call graph)
    auto calls_def = xsql::cached_table<CallRow>((prefix + "calls").c_str())
        .cache_builder([calls_data](std::vector<CallRow>& cache) {
            cache = *calls_data;
        })
        .column_int64("id", [](const CallRow& r) { return r.id; })
        .column_text("caller_usr", [](const CallRow& r) { return r.caller_usr; })
        .column_text("callee_usr", [](const CallRow& r) { return r.callee_usr; })
        .column_text("callee_name", [](const CallRow& r) { return r.callee_name; })
        .column_int64("file_id", [](const CallRow& r) { return r.file_id; })
        .column_int("line", [](const CallRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const CallRow& r) { return static_cast<int>(r.column); })
        .column_int("is_virtual", [](const CallRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_system", [](const CallRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(calls_def);

    // Inheritance table (class hierarchy)
    auto inheritance_def = xsql::cached_table<InheritanceRow>((prefix + "inheritance").c_str())
        .cache_builder([inheritance_data](std::vector<InheritanceRow>& cache) {
            cache = *inheritance_data;
        })
        .column_int64("id", [](const InheritanceRow& r) { return r.id; })
        .column_text("derived_usr", [](const InheritanceRow& r) { return r.derived_usr; })
        .column_text("derived_name", [](const InheritanceRow& r) { return r.derived_name; })
        .column_text("base_usr", [](const InheritanceRow& r) { return r.base_usr; })
        .column_text("base_name", [](const InheritanceRow& r) { return r.base_name; })
        .column_text("access", [](const InheritanceRow& r) { return r.access; })
        .column_int64("file_id", [](const InheritanceRow& r) { return r.file_id; })
        .column_int("is_virtual", [](const InheritanceRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_system", [](const InheritanceRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(inheritance_def);

    // String literals table
    auto string_literals_def = xsql::cached_table<StringLiteralRow>((prefix + "string_literals").c_str())
        .cache_builder([string_literals_data](std::vector<StringLiteralRow>& cache) {
            cache = *string_literals_data;
        })
        .column_int64("id", [](const StringLiteralRow& r) { return r.id; })
        .column_text("content", [](const StringLiteralRow& r) { return r.content; })
        .column_int64("file_id", [](const StringLiteralRow& r) { return r.file_id; })
        .column_int("line", [](const StringLiteralRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const StringLiteralRow& r) { return static_cast<int>(r.column); })
        .column_text("function_usr", [](const StringLiteralRow& r) { return r.function_usr; })
        .column_int64("func_id", [](const StringLiteralRow& r) { return r.func_id; })
        .column_int64("method_id", [](const StringLiteralRow& r) { return r.method_id; })
        .column_int("is_wide", [](const StringLiteralRow& r) { return r.is_wide ? 1 : 0; })
        .column_int("is_system", [](const StringLiteralRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(string_literals_def);
}

// ============================================================================
// Project Mode - Multiple TU Registration
// ============================================================================

void register_project_tables(xsql::Database& db,
                             const std::vector<const TranslationUnit*>& tus,
                             const std::string& schema) {
    // Build combined row data from all TUs
    // Each TU is processed with its own file_map/entity_map, but we deduplicate
    // the results based on USR (for entities) or path (for files)
    auto files_data = std::make_shared<std::vector<FileRow>>();
    auto functions_data = std::make_shared<std::vector<FunctionRow>>();
    auto classes_data = std::make_shared<std::vector<ClassRow>>();
    auto methods_data = std::make_shared<std::vector<MethodRow>>();
    auto fields_data = std::make_shared<std::vector<FieldRow>>();
    auto variables_data = std::make_shared<std::vector<VariableRow>>();
    auto parameters_data = std::make_shared<std::vector<ParameterRow>>();
    auto enums_data = std::make_shared<std::vector<EnumRow>>();
    auto enum_values_data = std::make_shared<std::vector<EnumValueRow>>();
    auto calls_data = std::make_shared<std::vector<CallRow>>();
    auto inheritance_data = std::make_shared<std::vector<InheritanceRow>>();
    auto string_literals_data = std::make_shared<std::vector<StringLiteralRow>>();

    // Track seen items for deduplication
    std::set<std::string> seen_files;
    std::set<std::string> seen_functions;  // by USR
    std::set<std::string> seen_classes;    // by USR
    std::set<std::string> seen_methods;    // by USR
    std::set<std::string> seen_variables;  // by USR
    std::set<std::string> seen_enums;      // by USR
    std::set<std::string> seen_inheritance; // by derived_usr:base_usr

    std::unordered_map<std::string, int64_t> global_file_ids;
    std::unordered_map<std::string, int64_t> global_function_ids;
    std::unordered_map<std::string, int64_t> global_method_ids;
    std::unordered_map<std::string, int64_t> global_class_ids;
    std::unordered_map<std::string, int64_t> global_enum_ids;
    int64_t next_file_id = 1;
    int64_t next_function_id = 1;
    int64_t next_method_id = 1;
    int64_t next_class_id = 1;
    int64_t next_enum_id = 1;

    auto assign_global_id = [](std::unordered_map<std::string, int64_t>& ids,
                               const std::string& key,
                               int64_t& next_id) {
        auto [it, inserted] = ids.emplace(key, next_id);
        if (inserted) {
            ++next_id;
        }
        return it->second;
    };

    auto lookup_file_path = [](const std::unordered_map<int64_t, std::string>& paths_by_id,
                               int64_t local_id) -> std::string {
        if (local_id == 0) {
            return {};
        }
        auto it = paths_by_id.find(local_id);
        return (it != paths_by_id.end()) ? it->second : std::string{};
    };

    auto lookup_entity_usr = [](const std::unordered_map<int64_t, std::string>& usr_by_id,
                                int64_t local_id) -> std::string {
        if (local_id == 0) {
            return {};
        }
        auto it = usr_by_id.find(local_id);
        return (it != usr_by_id.end()) ? it->second : std::string{};
    };

    for (const auto* tu : tus) {
        if (!tu) continue;

        // Build maps for this TU
        FileMap file_map = build_file_map(*tu);
        EntityMap entity_map = build_entity_map(*tu);

        std::unordered_map<int64_t, std::string> local_file_paths;
        for (const auto& [path, id] : file_map.path_to_id) {
            local_file_paths[id] = path;
        }

        // Free-function USRs only — method USRs go in local_method_usrs so that
        // child rows can route `function_id` vs `method_id` without collisions.
        std::unordered_map<int64_t, std::string> local_function_usrs;
        for (const auto& [usr, id] : entity_map.free_func_usr_to_id) {
            local_function_usrs[id] = usr;
        }

        std::unordered_map<int64_t, std::string> local_method_usrs;
        for (const auto& [usr, id] : entity_map.method_usr_to_id) {
            local_method_usrs[id] = usr;
        }

        std::unordered_map<int64_t, std::string> local_class_usrs;
        for (const auto& [usr, id] : entity_map.class_usr_to_id) {
            local_class_usrs[id] = usr;
        }

        std::unordered_map<int64_t, std::string> local_enum_usrs;
        for (const auto& [usr, id] : entity_map.enum_usr_to_id) {
            local_enum_usrs[id] = usr;
        }

        // Files (deduplicate by path)
        auto tu_files = build_files_table_with_map(*tu, file_map);
        for (auto& row : tu_files) {
            row.id = assign_global_id(global_file_ids, row.path, next_file_id);
            if (seen_files.find(row.path) == seen_files.end()) {
                seen_files.insert(row.path);
                files_data->push_back(std::move(row));
            }
        }

        // Functions (deduplicate by USR)
        auto tu_functions = build_functions_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_functions) {
            std::string key = row.usr.empty()
                ? tu->path() + "#function#" + std::to_string(row.id)
                : row.usr;
            row.id = assign_global_id(global_function_ids, key, next_function_id);

            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }

            if (row.usr.empty() || seen_functions.find(row.usr) == seen_functions.end()) {
                if (!row.usr.empty()) seen_functions.insert(row.usr);
                functions_data->push_back(std::move(row));
            }
        }

        // Classes (deduplicate by USR)
        auto tu_classes = build_classes_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_classes) {
            std::string key = row.usr.empty()
                ? tu->path() + "#class#" + std::to_string(row.id)
                : row.usr;
            row.id = assign_global_id(global_class_ids, key, next_class_id);

            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }

            if (row.usr.empty() || seen_classes.find(row.usr) == seen_classes.end()) {
                if (!row.usr.empty()) seen_classes.insert(row.usr);
                classes_data->push_back(std::move(row));
            }
        }

        // Methods (deduplicate by USR). Methods live in their own id space
        // (disjoint from free functions) so children that reference them carry
        // a separate `method_id` FK.
        auto tu_methods = build_methods_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_methods) {
            // row.usr is guaranteed non-empty: build_methods_table_with_map
            // drops any cursor not registered in entity_map.method_usr_to_id.
            row.id = assign_global_id(global_method_ids, row.usr, next_method_id);

            std::string class_usr = lookup_entity_usr(local_class_usrs, row.class_id);
            if (!class_usr.empty()) {
                row.class_id = assign_global_id(global_class_ids, class_usr, next_class_id);
            }
            if (seen_methods.find(row.usr) == seen_methods.end()) {
                seen_methods.insert(row.usr);
                methods_data->push_back(std::move(row));
            }
        }

        // Fields (keep all - they're tied to classes)
        auto tu_fields = build_fields_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_fields) {
            std::string class_usr = lookup_entity_usr(local_class_usrs, row.class_id);
            if (!class_usr.empty()) {
                row.class_id = assign_global_id(global_class_ids, class_usr, next_class_id);
            }
            fields_data->push_back(std::move(row));
        }

        // Variables (deduplicate by USR)
        auto tu_variables = build_variables_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_variables) {
            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }

            std::string function_usr = lookup_entity_usr(local_function_usrs, row.function_id);
            if (!function_usr.empty()) {
                row.function_id = assign_global_id(global_function_ids, function_usr, next_function_id);
            }

            std::string method_usr = lookup_entity_usr(local_method_usrs, row.method_id);
            if (!method_usr.empty()) {
                row.method_id = assign_global_id(global_method_ids, method_usr, next_method_id);
            }

            if (row.usr.empty() || seen_variables.find(row.usr) == seen_variables.end()) {
                if (!row.usr.empty()) seen_variables.insert(row.usr);
                variables_data->push_back(std::move(row));
            }
        }

        // Parameters (keep all - they're tied to free functions or methods)
        auto tu_parameters = build_parameters_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_parameters) {
            std::string function_usr = lookup_entity_usr(local_function_usrs, row.function_id);
            if (!function_usr.empty()) {
                row.function_id = assign_global_id(global_function_ids, function_usr, next_function_id);
            }

            std::string method_usr = lookup_entity_usr(local_method_usrs, row.method_id);
            if (!method_usr.empty()) {
                row.method_id = assign_global_id(global_method_ids, method_usr, next_method_id);
            }
            parameters_data->push_back(std::move(row));
        }

        // Enums (deduplicate by USR)
        auto tu_enums = build_enums_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_enums) {
            std::string key = row.usr.empty()
                ? tu->path() + "#enum#" + std::to_string(row.id)
                : row.usr;
            row.id = assign_global_id(global_enum_ids, key, next_enum_id);

            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }

            if (row.usr.empty() || seen_enums.find(row.usr) == seen_enums.end()) {
                if (!row.usr.empty()) seen_enums.insert(row.usr);
                enums_data->push_back(std::move(row));
            }
        }

        // Enum values (keep all - they're tied to enums)
        auto tu_enum_values = build_enum_values_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_enum_values) {
            std::string enum_usr = lookup_entity_usr(local_enum_usrs, row.enum_id);
            if (!enum_usr.empty()) {
                row.enum_id = assign_global_id(global_enum_ids, enum_usr, next_enum_id);
            }
            enum_values_data->push_back(std::move(row));
        }

        // Calls (keep all - each call site is unique)
        auto tu_calls = build_calls_table_with_map(*tu, file_map);
        for (auto& row : tu_calls) {
            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }
            calls_data->push_back(std::move(row));
        }

        // Inheritance (deduplicate by derived_usr:base_usr pair)
        auto tu_inheritance = build_inheritance_table_with_map(*tu, file_map);
        for (auto& row : tu_inheritance) {
            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }
            std::string key = row.derived_usr + ":" + row.base_usr;
            if (seen_inheritance.find(key) == seen_inheritance.end()) {
                seen_inheritance.insert(key);
                inheritance_data->push_back(std::move(row));
            }
        }

        // String literals (keep all - each occurrence is unique). The builder
        // has already routed the enclosing callable into either func_id (free
        // function) or method_id (method); we just remap the one that's set.
        auto tu_strings = build_string_literals_table_with_map(*tu, file_map, entity_map);
        for (auto& row : tu_strings) {
            std::string file_path = lookup_file_path(local_file_paths, row.file_id);
            if (!file_path.empty()) {
                row.file_id = assign_global_id(global_file_ids, file_path, next_file_id);
            }

            std::string function_usr = lookup_entity_usr(local_function_usrs, row.func_id);
            if (!function_usr.empty()) {
                row.func_id = assign_global_id(global_function_ids, function_usr, next_function_id);
            }

            std::string method_usr = lookup_entity_usr(local_method_usrs, row.method_id);
            if (!method_usr.empty()) {
                row.method_id = assign_global_id(global_method_ids, method_usr, next_method_id);
            }
            string_literals_data->push_back(std::move(row));
        }
    }

    // Parent IDs (files, functions, classes, methods, enums) are already
    // assigned from stable USR/path keys above and are naturally gap-free
    // because only emitted rows allocate into their global id maps. Child
    // tables below have no stable primary key, so resequence their ids.
    int64_t id = 1;
    for (auto& row : *fields_data) {
        row.id = id++;
    }
    id = 1;
    for (auto& row : *variables_data) {
        row.id = id++;
    }
    id = 1;
    for (auto& row : *parameters_data) {
        row.id = id++;
    }
    id = 1;
    for (auto& row : *enum_values_data) {
        row.id = id++;
    }
    id = 1;
    for (auto& row : *calls_data) {
        row.id = id++;
    }
    id = 1;
    for (auto& row : *inheritance_data) {
        row.id = id++;
    }
    id = 1;
    for (auto& row : *string_literals_data) {
        row.id = id++;
    }

    // Register tables (same as single TU, but with combined data)
    std::string prefix = schema.empty() ? "" : schema + "_";

    // Files table
    auto files_def = xsql::cached_table<FileRow>((prefix + "files").c_str())
        .cache_builder([files_data](std::vector<FileRow>& cache) {
            cache = *files_data;
        })
        .column_int64("id", [](const FileRow& r) { return r.id; })
        .column_text("path", [](const FileRow& r) { return r.path; })
        .column_int("is_main_file", [](const FileRow& r) { return r.is_main_file ? 1 : 0; })
        .column_int64("mtime", [](const FileRow& r) { return static_cast<int64_t>(r.mtime); })
        .column_int("is_header", [](const FileRow& r) { return r.is_header ? 1 : 0; })
        .column_int("is_system", [](const FileRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(files_def);

    // Functions table
    auto functions_def = xsql::cached_table<FunctionRow>((prefix + "functions").c_str())
        .cache_builder([functions_data](std::vector<FunctionRow>& cache) {
            cache = *functions_data;
        })
        .column_int64("id", [](const FunctionRow& r) { return r.id; })
        .column_text("usr", [](const FunctionRow& r) { return r.usr; })
        .column_text("name", [](const FunctionRow& r) { return r.name; })
        .column_text("qualified_name", [](const FunctionRow& r) { return r.qualified_name; })
        .column_text("return_type", [](const FunctionRow& r) { return r.return_type; })
        .column_int64("file_id", [](const FunctionRow& r) { return r.file_id; })
        .column_int("line", [](const FunctionRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const FunctionRow& r) { return static_cast<int>(r.column); })
        .column_int("end_line", [](const FunctionRow& r) { return static_cast<int>(r.end_line); })
        .column_int("end_column", [](const FunctionRow& r) { return static_cast<int>(r.end_column); })
        .column_int("is_definition", [](const FunctionRow& r) { return r.is_definition ? 1 : 0; })
        .column_int("is_inline", [](const FunctionRow& r) { return r.is_inline ? 1 : 0; })
        .column_int("is_variadic", [](const FunctionRow& r) { return r.is_variadic ? 1 : 0; })
        .column_int("is_static", [](const FunctionRow& r) { return r.is_static ? 1 : 0; })
        .column_int("is_system", [](const FunctionRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(functions_def);

    // Classes table
    auto classes_def = xsql::cached_table<ClassRow>((prefix + "classes").c_str())
        .cache_builder([classes_data](std::vector<ClassRow>& cache) {
            cache = *classes_data;
        })
        .column_int64("id", [](const ClassRow& r) { return r.id; })
        .column_text("usr", [](const ClassRow& r) { return r.usr; })
        .column_text("name", [](const ClassRow& r) { return r.name; })
        .column_text("qualified_name", [](const ClassRow& r) { return r.qualified_name; })
        .column_text("kind", [](const ClassRow& r) { return r.kind; })
        .column_int64("file_id", [](const ClassRow& r) { return r.file_id; })
        .column_int("line", [](const ClassRow& r) { return static_cast<int>(r.line); })
        .column_int("is_definition", [](const ClassRow& r) { return r.is_definition ? 1 : 0; })
        .column_int("is_abstract", [](const ClassRow& r) { return r.is_abstract ? 1 : 0; })
        .column_int64("namespace_id", [](const ClassRow& r) { return r.namespace_id; })
        .column_int("is_system", [](const ClassRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(classes_def);

    // Methods table
    auto methods_def = xsql::cached_table<MethodRow>((prefix + "methods").c_str())
        .cache_builder([methods_data](std::vector<MethodRow>& cache) {
            cache = *methods_data;
        })
        .column_int64("id", [](const MethodRow& r) { return r.id; })
        .column_text("usr", [](const MethodRow& r) { return r.usr; })
        .column_int64("class_id", [](const MethodRow& r) { return r.class_id; })
        .column_text("name", [](const MethodRow& r) { return r.name; })
        .column_text("qualified_name", [](const MethodRow& r) { return r.qualified_name; })
        .column_text("return_type", [](const MethodRow& r) { return r.return_type; })
        .column_text("access", [](const MethodRow& r) { return r.access; })
        .column_int("is_virtual", [](const MethodRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_pure_virtual", [](const MethodRow& r) { return r.is_pure_virtual ? 1 : 0; })
        .column_int("is_static", [](const MethodRow& r) { return r.is_static ? 1 : 0; })
        .column_int("is_const", [](const MethodRow& r) { return r.is_const ? 1 : 0; })
        .column_int("is_override", [](const MethodRow& r) { return r.is_override ? 1 : 0; })
        .column_int("is_final", [](const MethodRow& r) { return r.is_final ? 1 : 0; })
        .column_int("line", [](const MethodRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const MethodRow& r) { return static_cast<int>(r.column); })
        .column_int("end_line", [](const MethodRow& r) { return static_cast<int>(r.end_line); })
        .column_int("end_column", [](const MethodRow& r) { return static_cast<int>(r.end_column); })
        .column_int("is_system", [](const MethodRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(methods_def);

    // Fields table
    auto fields_def = xsql::cached_table<FieldRow>((prefix + "fields").c_str())
        .cache_builder([fields_data](std::vector<FieldRow>& cache) {
            cache = *fields_data;
        })
        .column_int64("id", [](const FieldRow& r) { return r.id; })
        .column_int64("class_id", [](const FieldRow& r) { return r.class_id; })
        .column_text("name", [](const FieldRow& r) { return r.name; })
        .column_text("type", [](const FieldRow& r) { return r.type; })
        .column_text("access", [](const FieldRow& r) { return r.access; })
        .column_int("is_static", [](const FieldRow& r) { return r.is_static ? 1 : 0; })
        .column_int("is_mutable", [](const FieldRow& r) { return r.is_mutable ? 1 : 0; })
        .column_int("bit_width", [](const FieldRow& r) { return r.bit_width; })
        .column_int64("offset_bits", [](const FieldRow& r) { return r.offset_bits; })
        .column_int("is_system", [](const FieldRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(fields_def);

    // Variables table
    auto variables_def = xsql::cached_table<VariableRow>((prefix + "variables").c_str())
        .cache_builder([variables_data](std::vector<VariableRow>& cache) {
            cache = *variables_data;
        })
        .column_int64("id", [](const VariableRow& r) { return r.id; })
        .column_text("usr", [](const VariableRow& r) { return r.usr; })
        .column_text("name", [](const VariableRow& r) { return r.name; })
        .column_text("type", [](const VariableRow& r) { return r.type; })
        .column_int64("file_id", [](const VariableRow& r) { return r.file_id; })
        .column_int("line", [](const VariableRow& r) { return static_cast<int>(r.line); })
        .column_int64("function_id", [](const VariableRow& r) { return r.function_id; })
        .column_int64("method_id", [](const VariableRow& r) { return r.method_id; })
        .column_int64("namespace_id", [](const VariableRow& r) { return r.namespace_id; })
        .column_text("scope_kind", [](const VariableRow& r) { return r.scope_kind; })
        .column_text("storage_class", [](const VariableRow& r) { return r.storage_class; })
        .column_int("is_const", [](const VariableRow& r) { return r.is_const ? 1 : 0; })
        .column_int("is_inline", [](const VariableRow& r) { return r.is_inline ? 1 : 0; })
        .column_int("is_system", [](const VariableRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(variables_def);

    // Parameters table
    auto parameters_def = xsql::cached_table<ParameterRow>((prefix + "parameters").c_str())
        .cache_builder([parameters_data](std::vector<ParameterRow>& cache) {
            cache = *parameters_data;
        })
        .column_int64("id", [](const ParameterRow& r) { return r.id; })
        .column_int64("function_id", [](const ParameterRow& r) { return r.function_id; })
        .column_int64("method_id", [](const ParameterRow& r) { return r.method_id; })
        .column_text("name", [](const ParameterRow& r) { return r.name; })
        .column_text("type", [](const ParameterRow& r) { return r.type; })
        .column_int("index_", [](const ParameterRow& r) { return r.index; })
        .column_int("has_default", [](const ParameterRow& r) { return r.has_default ? 1 : 0; })
        .column_int("is_system", [](const ParameterRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(parameters_def);

    // Enums table
    auto enums_def = xsql::cached_table<EnumRow>((prefix + "enums").c_str())
        .cache_builder([enums_data](std::vector<EnumRow>& cache) {
            cache = *enums_data;
        })
        .column_int64("id", [](const EnumRow& r) { return r.id; })
        .column_text("usr", [](const EnumRow& r) { return r.usr; })
        .column_text("name", [](const EnumRow& r) { return r.name; })
        .column_text("underlying_type", [](const EnumRow& r) { return r.underlying_type; })
        .column_int("is_scoped", [](const EnumRow& r) { return r.is_scoped ? 1 : 0; })
        .column_int64("file_id", [](const EnumRow& r) { return r.file_id; })
        .column_int("line", [](const EnumRow& r) { return static_cast<int>(r.line); })
        .column_int("is_system", [](const EnumRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(enums_def);

    // Enum values table
    auto enum_values_def = xsql::cached_table<EnumValueRow>((prefix + "enum_values").c_str())
        .cache_builder([enum_values_data](std::vector<EnumValueRow>& cache) {
            cache = *enum_values_data;
        })
        .column_int64("id", [](const EnumValueRow& r) { return r.id; })
        .column_int64("enum_id", [](const EnumValueRow& r) { return r.enum_id; })
        .column_text("name", [](const EnumValueRow& r) { return r.name; })
        .column_int64("value", [](const EnumValueRow& r) { return r.value; })
        .column_int("is_system", [](const EnumValueRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(enum_values_def);

    // Calls table
    auto calls_def = xsql::cached_table<CallRow>((prefix + "calls").c_str())
        .cache_builder([calls_data](std::vector<CallRow>& cache) {
            cache = *calls_data;
        })
        .column_int64("id", [](const CallRow& r) { return r.id; })
        .column_text("caller_usr", [](const CallRow& r) { return r.caller_usr; })
        .column_text("callee_usr", [](const CallRow& r) { return r.callee_usr; })
        .column_text("callee_name", [](const CallRow& r) { return r.callee_name; })
        .column_int64("file_id", [](const CallRow& r) { return r.file_id; })
        .column_int("line", [](const CallRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const CallRow& r) { return static_cast<int>(r.column); })
        .column_int("is_virtual", [](const CallRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_system", [](const CallRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(calls_def);

    // Inheritance table
    auto inheritance_def = xsql::cached_table<InheritanceRow>((prefix + "inheritance").c_str())
        .cache_builder([inheritance_data](std::vector<InheritanceRow>& cache) {
            cache = *inheritance_data;
        })
        .column_int64("id", [](const InheritanceRow& r) { return r.id; })
        .column_text("derived_usr", [](const InheritanceRow& r) { return r.derived_usr; })
        .column_text("derived_name", [](const InheritanceRow& r) { return r.derived_name; })
        .column_text("base_usr", [](const InheritanceRow& r) { return r.base_usr; })
        .column_text("base_name", [](const InheritanceRow& r) { return r.base_name; })
        .column_text("access", [](const InheritanceRow& r) { return r.access; })
        .column_int64("file_id", [](const InheritanceRow& r) { return r.file_id; })
        .column_int("is_virtual", [](const InheritanceRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_system", [](const InheritanceRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(inheritance_def);

    // String literals table
    auto string_literals_def = xsql::cached_table<StringLiteralRow>((prefix + "string_literals").c_str())
        .cache_builder([string_literals_data](std::vector<StringLiteralRow>& cache) {
            cache = *string_literals_data;
        })
        .column_int64("id", [](const StringLiteralRow& r) { return r.id; })
        .column_text("content", [](const StringLiteralRow& r) { return r.content; })
        .column_int64("file_id", [](const StringLiteralRow& r) { return r.file_id; })
        .column_int("line", [](const StringLiteralRow& r) { return static_cast<int>(r.line); })
        .column_int("column", [](const StringLiteralRow& r) { return static_cast<int>(r.column); })
        .column_text("function_usr", [](const StringLiteralRow& r) { return r.function_usr; })
        .column_int64("func_id", [](const StringLiteralRow& r) { return r.func_id; })
        .column_int64("method_id", [](const StringLiteralRow& r) { return r.method_id; })
        .column_int("is_wide", [](const StringLiteralRow& r) { return r.is_wide ? 1 : 0; })
        .column_int("is_system", [](const StringLiteralRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(string_literals_def);
}

} // namespace clangsql
