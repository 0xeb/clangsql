#pragma once
/// @file tables.hpp
/// @brief Virtual table definitions for clangsql

#include <xsql/vtable.hpp>
#include <xsql/database.hpp>
#include <clangsql/parser.hpp>
#include <vector>
#include <string>

namespace clangsql {

// ============================================================================
// Table row types
// ============================================================================

/// Row for the 'files' table
struct FileRow {
    int64_t id;
    std::string path;
    bool is_main_file;
    std::time_t mtime;
    bool is_header;
    bool is_system;  // True if in system include path
};

/// Row for the 'functions' table
struct FunctionRow {
    int64_t id;
    std::string usr;
    std::string name;
    std::string qualified_name;
    std::string return_type;
    int64_t file_id;
    unsigned line;
    unsigned column;
    unsigned end_line;
    unsigned end_column;
    bool is_definition;
    bool is_inline;
    bool is_variadic;
    bool is_static;
    bool is_system;  // True if in system header
};

/// Row for the 'classes' table
struct ClassRow {
    int64_t id;
    std::string usr;
    std::string name;
    std::string qualified_name;
    std::string kind;  // "class", "struct", "union"
    int64_t file_id;
    unsigned line;
    bool is_definition;
    bool is_abstract;
    int64_t namespace_id;
    bool is_system;  // True if in system header
};

/// Row for the 'methods' table
struct MethodRow {
    int64_t id;
    std::string usr;
    int64_t class_id;
    std::string name;
    std::string qualified_name;
    std::string return_type;
    std::string access;  // "public", "protected", "private"
    bool is_virtual;
    bool is_pure_virtual;
    bool is_static;
    bool is_const;
    bool is_override;
    bool is_final;
    unsigned line;
    unsigned column;
    unsigned end_line;
    unsigned end_column;
    bool is_system;  // True if in system header
};

/// Row for the 'fields' table
struct FieldRow {
    int64_t id;
    int64_t class_id;
    std::string name;
    std::string type;
    std::string access;
    bool is_static;
    bool is_mutable;
    int bit_width;
    int64_t offset_bits;
    bool is_system;  // True if in system header
};

/// Row for the 'variables' table
struct VariableRow {
    int64_t id;
    std::string usr;
    std::string name;
    std::string type;
    int64_t file_id;
    unsigned line;
    int64_t function_id;  // NULL for globals
    int64_t namespace_id;
    std::string scope_kind;  // "global", "local", "static_local"
    std::string storage_class;
    bool is_const;
    bool is_inline;
    bool is_system;  // True if in system header
};

/// Row for the 'parameters' table
struct ParameterRow {
    int64_t id;
    int64_t function_id;
    std::string name;
    std::string type;
    int index;
    bool has_default;
    bool is_system;  // True if in system header
};

/// Row for the 'enums' table
struct EnumRow {
    int64_t id;
    std::string usr;
    std::string name;
    std::string underlying_type;
    bool is_scoped;
    int64_t file_id;
    unsigned line;
    bool is_system;  // True if in system header
};

/// Row for the 'enum_values' table
struct EnumValueRow {
    int64_t id;
    int64_t enum_id;
    std::string name;
    int64_t value;
    bool is_system;  // True if in system header
};

/// Row for the 'calls' table (function call graph)
struct CallRow {
    int64_t id;
    std::string caller_usr;     ///< USR of the calling function/method
    std::string callee_usr;     ///< USR of the called function/method
    std::string callee_name;    ///< Name of the called function (for display)
    unsigned line;              ///< Line of the call site
    unsigned column;            ///< Column of the call site
    bool is_virtual;            ///< True if calling a virtual method
    bool is_system;             ///< True if call is in system header
};

/// Row for the 'inheritance' table (class hierarchy)
struct InheritanceRow {
    int64_t id;
    std::string derived_usr;    ///< USR of the derived class
    std::string derived_name;   ///< Name of the derived class (for display)
    std::string base_usr;       ///< USR of the base class
    std::string base_name;      ///< Name of the base class (for display)
    std::string access;         ///< "public", "protected", "private"
    bool is_virtual;            ///< True if virtual inheritance
    bool is_system;             ///< True if in system header
};

// ============================================================================
// Table builders (populate from AST traversal)
// ============================================================================

/// Build file rows from a translation unit
std::vector<FileRow> build_files_table(const TranslationUnit& tu);

/// Build function rows from a translation unit
std::vector<FunctionRow> build_functions_table(const TranslationUnit& tu);

/// Build class rows from a translation unit
std::vector<ClassRow> build_classes_table(const TranslationUnit& tu);

/// Build method rows from a translation unit
std::vector<MethodRow> build_methods_table(const TranslationUnit& tu);

/// Build field rows from a translation unit
std::vector<FieldRow> build_fields_table(const TranslationUnit& tu);

/// Build variable rows from a translation unit
std::vector<VariableRow> build_variables_table(const TranslationUnit& tu);

/// Build parameter rows from a translation unit
std::vector<ParameterRow> build_parameters_table(const TranslationUnit& tu);

/// Build enum rows from a translation unit
std::vector<EnumRow> build_enums_table(const TranslationUnit& tu);

/// Build enum value rows from a translation unit
std::vector<EnumValueRow> build_enum_values_table(const TranslationUnit& tu);

/// Build call rows from a translation unit (function call graph)
std::vector<CallRow> build_calls_table(const TranslationUnit& tu);

/// Build inheritance rows from a translation unit (class hierarchy)
std::vector<InheritanceRow> build_inheritance_table(const TranslationUnit& tu);

// ============================================================================
// Virtual table registration
// ============================================================================

/// Register all clangsql virtual tables with the database
/// @param db The xsql database
/// @param tu The translation unit to expose
/// @param schema Schema prefix (e.g., "main" for ATTACH ... AS main)
void register_tables(xsql::Database& db, const TranslationUnit& tu,
                     const std::string& schema = "");

} // namespace clangsql
