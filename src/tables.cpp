/// @file tables.cpp
/// @brief Virtual table implementations for clangsql

#include <clangsql/tables.hpp>
#include <clangsql/parser.hpp>
#include <xsql/vtable.hpp>
#include <xsql/database.hpp>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace clangsql {

// ============================================================================
// Shared File Map (used by all table builders)
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

// Forward declarations for _with_map variants
static FileMap build_file_map(const TranslationUnit& tu);
std::vector<FileRow> build_files_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<FunctionRow> build_functions_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<ClassRow> build_classes_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<MethodRow> build_methods_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<FieldRow> build_fields_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<VariableRow> build_variables_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<ParameterRow> build_parameters_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<EnumRow> build_enums_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<EnumValueRow> build_enum_values_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<CallRow> build_calls_table_with_map(const TranslationUnit& tu, const FileMap& map);
std::vector<InheritanceRow> build_inheritance_table_with_map(const TranslationUnit& tu, const FileMap& map);

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
// Build FileMap (must be called first)
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
    return build_functions_table_with_map(tu, map);
}

std::vector<FunctionRow> build_functions_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<FunctionRow> functions;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        // Only process function declarations (not methods - those go in methods table)
        if (kind != CXCursor_FunctionDecl) {
            return CXChildVisit_Recurse;
        }

        auto loc = cursor_location(cursor);
        auto extent = get_definition_extent(cursor);  // Includes body for definitions

        FunctionRow row;
        row.id = next_id++;
        row.usr = cursor_usr(cursor);
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
    return build_classes_table_with_map(tu, map);
}

std::vector<ClassRow> build_classes_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<ClassRow> classes;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        // Only process class/struct/union declarations
        if (kind != CXCursor_ClassDecl &&
            kind != CXCursor_StructDecl &&
            kind != CXCursor_UnionDecl) {
            return CXChildVisit_Recurse;
        }

        auto loc = cursor_location(cursor);

        ClassRow row;
        row.id = next_id++;
        row.usr = cursor_usr(cursor);
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
    return build_methods_table_with_map(tu, map);
}

std::vector<MethodRow> build_methods_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<MethodRow> methods;
    std::unordered_map<std::string, int64_t> class_usr_to_id;
    int64_t next_id = 1;

    // First pass: collect class USRs
    CXCursor root = tu.cursor();
    int64_t class_id = 1;
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl ||
            kind == CXCursor_UnionDecl) {
            std::string usr = cursor_usr(cursor);
            if (class_usr_to_id.find(usr) == class_usr_to_id.end()) {
                class_usr_to_id[usr] = class_id++;
            }
        }
        return CXChildVisit_Recurse;
    });

    // Second pass: collect methods
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_CXXMethod &&
            kind != CXCursor_Constructor &&
            kind != CXCursor_Destructor) {
            return CXChildVisit_Recurse;
        }

        // Get parent class
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        auto it = class_usr_to_id.find(parent_usr);
        if (it == class_usr_to_id.end()) {
            return CXChildVisit_Recurse;
        }

        auto loc = cursor_location(cursor);
        auto extent = get_definition_extent(cursor);  // Includes body for definitions

        MethodRow row;
        row.id = next_id++;
        row.usr = cursor_usr(cursor);
        row.class_id = it->second;
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
    return build_fields_table_with_map(tu, map);
}

std::vector<FieldRow> build_fields_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<FieldRow> fields;
    std::unordered_map<std::string, int64_t> class_usr_to_id;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // First pass: collect class USRs
    int64_t class_id = 1;
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl ||
            kind == CXCursor_UnionDecl) {
            std::string usr = cursor_usr(cursor);
            if (class_usr_to_id.find(usr) == class_usr_to_id.end()) {
                class_usr_to_id[usr] = class_id++;
            }
        }
        return CXChildVisit_Recurse;
    });

    // Second pass: collect fields
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_FieldDecl) {
            return CXChildVisit_Recurse;
        }

        // Get parent class
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        auto it = class_usr_to_id.find(parent_usr);
        if (it == class_usr_to_id.end()) {
            return CXChildVisit_Recurse;
        }

        FieldRow row;
        row.id = next_id++;
        row.class_id = it->second;
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
    return build_variables_table_with_map(tu, map);
}

std::vector<VariableRow> build_variables_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<VariableRow> variables;
    std::unordered_map<std::string, int64_t> func_usr_to_id;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // First pass: collect function USRs
    int64_t func_id = 1;
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_FunctionDecl ||
            kind == CXCursor_CXXMethod ||
            kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor) {
            std::string usr = cursor_usr(cursor);
            if (func_usr_to_id.find(usr) == func_usr_to_id.end()) {
                func_usr_to_id[usr] = func_id++;
            }
        }
        return CXChildVisit_Recurse;
    });

    // Second pass: collect variables
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
            // Local variable
            std::string parent_usr = cursor_usr(parent);
            auto it = func_usr_to_id.find(parent_usr);
            row.function_id = (it != func_usr_to_id.end()) ? it->second : 0;

            CX_StorageClass storage = clang_Cursor_getStorageClass(cursor);
            row.scope_kind = (storage == CX_SC_Static) ? "static_local" : "local";
        } else {
            // Global variable
            row.function_id = 0;
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
    return build_parameters_table_with_map(tu, map);
}

std::vector<ParameterRow> build_parameters_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<ParameterRow> parameters;
    std::unordered_map<std::string, int64_t> func_usr_to_id;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // First pass: collect function USRs
    int64_t func_id = 1;
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_FunctionDecl ||
            kind == CXCursor_CXXMethod ||
            kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor) {
            std::string usr = cursor_usr(cursor);
            if (func_usr_to_id.find(usr) == func_usr_to_id.end()) {
                func_usr_to_id[usr] = func_id++;
            }
        }
        return CXChildVisit_Recurse;
    });

    // Second pass: collect parameters
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_ParmDecl) {
            return CXChildVisit_Recurse;
        }

        // Get parent function
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        auto it = func_usr_to_id.find(parent_usr);
        if (it == func_usr_to_id.end()) {
            return CXChildVisit_Recurse;
        }

        ParameterRow row;
        row.id = next_id++;
        row.function_id = it->second;
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
    return build_enums_table_with_map(tu, map);
}

std::vector<EnumRow> build_enums_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<EnumRow> enums;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind != CXCursor_EnumDecl) {
            return CXChildVisit_Recurse;
        }

        auto loc = cursor_location(cursor);

        EnumRow row;
        row.id = next_id++;
        row.usr = cursor_usr(cursor);
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
    return build_enum_values_table_with_map(tu, map);
}

std::vector<EnumValueRow> build_enum_values_table_with_map(const TranslationUnit& tu, const FileMap& map) {
    std::vector<EnumValueRow> values;
    std::unordered_map<std::string, int64_t> enum_usr_to_id;
    int64_t next_id = 1;

    CXCursor root = tu.cursor();

    // First pass: collect enum USRs
    int64_t enum_id = 1;
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        if (clang_getCursorKind(cursor) == CXCursor_EnumDecl) {
            std::string usr = cursor_usr(cursor);
            if (enum_usr_to_id.find(usr) == enum_usr_to_id.end()) {
                enum_usr_to_id[usr] = enum_id++;
            }
        }
        return CXChildVisit_Recurse;
    });

    // Second pass: collect enum values
    visit_children(root, [&](CXCursor cursor, CXCursor) -> CXChildVisitResult {
        if (clang_getCursorKind(cursor) != CXCursor_EnumConstantDecl) {
            return CXChildVisit_Recurse;
        }

        // Get parent enum
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        std::string parent_usr = cursor_usr(parent);
        auto it = enum_usr_to_id.find(parent_usr);
        if (it == enum_usr_to_id.end()) {
            return CXChildVisit_Recurse;
        }

        EnumValueRow row;
        row.id = next_id++;
        row.enum_id = it->second;
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

            InheritanceRow row;
            row.id = next_id++;
            row.derived_usr = derived_usr;
            row.derived_name = derived_name;
            row.base_usr = cursor_usr(base_class);
            row.base_name = to_string(clang_getTypeSpelling(base_type));

            // Get access specifier
            row.access = access_string(clang_getCXXAccessSpecifier(child));

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

// ============================================================================
// Virtual Table Registration
// ============================================================================

void register_tables(xsql::Database& db, const TranslationUnit& tu,
                     const std::string& schema) {
    // Build shared file map ONCE
    FileMap file_map = build_file_map(tu);

    // Pre-build all table data (captures by value avoid dangling reference issues)
    auto files_data = std::make_shared<std::vector<FileRow>>(build_files_table_with_map(tu, file_map));
    auto functions_data = std::make_shared<std::vector<FunctionRow>>(build_functions_table_with_map(tu, file_map));
    auto classes_data = std::make_shared<std::vector<ClassRow>>(build_classes_table_with_map(tu, file_map));
    auto methods_data = std::make_shared<std::vector<MethodRow>>(build_methods_table_with_map(tu, file_map));
    auto fields_data = std::make_shared<std::vector<FieldRow>>(build_fields_table_with_map(tu, file_map));
    auto variables_data = std::make_shared<std::vector<VariableRow>>(build_variables_table_with_map(tu, file_map));
    auto parameters_data = std::make_shared<std::vector<ParameterRow>>(build_parameters_table_with_map(tu, file_map));
    auto enums_data = std::make_shared<std::vector<EnumRow>>(build_enums_table_with_map(tu, file_map));
    auto enum_values_data = std::make_shared<std::vector<EnumValueRow>>(build_enum_values_table_with_map(tu, file_map));
    auto calls_data = std::make_shared<std::vector<CallRow>>(build_calls_table_with_map(tu, file_map));
    auto inheritance_data = std::make_shared<std::vector<InheritanceRow>>(build_inheritance_table_with_map(tu, file_map));

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
        .column_int("is_virtual", [](const InheritanceRow& r) { return r.is_virtual ? 1 : 0; })
        .column_int("is_system", [](const InheritanceRow& r) { return r.is_system ? 1 : 0; })
        .build();
    db.register_and_create_cached_table(inheritance_def);
}

} // namespace clangsql
