# clangsql

SQL interface for Clang AST databases. Query C/C++ source code using SQL.

Part of the [xsql](https://github.com/allthingsida/xsql) family (idasql, pdbsql, clangsql).

## Features

- Parse C/C++ source files with libclang
- Query AST entities via SQL: functions, classes, methods, variables, enums
- Multi-translation-unit support with ATTACH syntax
- Cross-platform: Windows, macOS, Linux
- Same API patterns as idasql/pdbsql

## Quick Start

```bash
# Parse a file and list functions
clangsql main.cpp -e "SELECT name FROM functions"

# With compiler flags
clangsql main.cpp -- -std=c++17 -I./include

# Interactive mode
clangsql main.cpp -i
```

## Building

### Prerequisites

- CMake 3.20+
- C++17 compiler
- libclang (via vcpkg or system)

### Build with vcpkg (recommended)

```bash
vcpkg install llvm[clang] nlohmann-json
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

### Build as part of xsql monorepo

```bash
cd xsql
cmake -B build
cmake --build build
```

### Standalone build

```bash
git submodule update --init external/libxsql
cmake -B build
cmake --build build
```

## Example Queries

### Call Graph Analysis

```sql
-- Who calls what (use USR for cross-table joins)
SELECT f.name as caller, c.callee_name as calls
FROM calls c
JOIN functions f ON c.caller_usr = f.usr
WHERE c.is_system = 0;
```
```
caller | calls
-------+-------------
main   | Circle
main   | process_data
main   | add
```

### Class Hierarchy

```sql
-- Find inheritance tree from base class
WITH RECURSIVE hierarchy AS (
    SELECT usr, name, 0 as depth
    FROM classes WHERE name = 'Shape'
    UNION ALL
    SELECT c.usr, c.name, h.depth + 1
    FROM hierarchy h
    JOIN inheritance i ON i.base_usr = h.usr
    JOIN classes c ON c.usr = i.derived_usr
)
SELECT name, depth FROM hierarchy ORDER BY depth;
```
```
name   | depth
-------+------
Shape  | 0
Circle | 1
```

### Virtual Methods

```sql
-- Find all virtual and pure virtual methods
SELECT qualified_name, access,
    CASE WHEN is_pure_virtual THEN 'pure'
         WHEN is_virtual THEN 'virtual'
    END as virt
FROM methods
WHERE is_virtual = 1 AND is_system = 0;
```
```
qualified_name | access | virt
---------------+--------+--------
Shape::~Shape  | public | virtual
Shape::area    | public | pure
Shape::draw    | public | virtual
Circle::area   | public | virtual
Circle::draw   | public | virtual
```

### Code Metrics

```sql
-- Largest functions by line count
SELECT name, end_line - line + 1 as lines
FROM functions
WHERE is_definition = 1 AND is_system = 0
ORDER BY lines DESC;
```
```
name         | lines
-------------+------
main         | 9
process_data | 6
add          | 3
```

### Multi-File Analysis

```bash
# Attach multiple translation units with prefixes
clangsql "file1.cpp:a" "file2.cpp:b" -i
```
```sql
-- Compare functions across files
SELECT 'a' as tu, name FROM a_functions WHERE is_system = 0
UNION ALL
SELECT 'b', name FROM b_functions WHERE is_system = 0;
```
```
tu | name
---+-------------
a  | main
a  | process_data
b  | multiply
b  | get_version
```

### Class Layout

```sql
-- Field offsets in bits
SELECT name, type, access, offset_bits
FROM fields WHERE is_system = 0;
```
```
name    | type   | access    | offset_bits
--------+--------+-----------+------------
x       | double | public    | 0
y       | double | public    | 64
name_   | string | protected | 64
radius_ | double | private   | 320
```

## Tables

### Core Entities
- `files` - Source files (path, is_main_file, is_header, is_system)
- `functions` - Functions with USR, name, return_type, line positions
- `parameters` - Function parameters (name, type, index_, has_default)
- `classes` - Classes, structs, unions (kind, is_abstract)
- `fields` - Class data members (type, access, offset_bits)
- `methods` - Class methods (access, is_virtual, is_pure_virtual, is_const)
- `variables` - Variables (scope_kind: global/local, storage_class)
- `enums` - Enums (underlying_type, is_scoped for enum class)
- `enum_values` - Enum constants (name, value)

### Relationships
- `calls` - Call graph (caller_usr, callee_usr, callee_name, is_virtual)
- `inheritance` - Class hierarchy (derived_usr, base_usr, access, is_virtual)

All tables have `is_system` column (1=system header, 0=user code). Always filter with `WHERE is_system = 0` for user code only.

## Programmatic Usage

```cpp
#include <clangsql/clangsql.hpp>

// Parse a file
clangsql::Index index;
clangsql::TranslationUnit tu(index.get(), "main.cpp", {"-std=c++17"});

// Query with SQL
clangsql::Session session;
session.attach("main.cpp", "main");
auto result = session.database().exec("SELECT name FROM main.functions");
```

## CMake Integration

```cmake
find_package(clangsql CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE clangsql::clangsql)
```

## License

MIT License - see [LICENSE](LICENSE)

## Related Projects

- [idasql](https://github.com/allthingsida/idasql) - SQL interface for IDA databases
- [pdbsql](https://github.com/0xeb/pdbsql) - SQL interface for PDB files
- [libxsql](https://github.com/0xeb/libxsql) - Shared virtual table framework
