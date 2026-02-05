# clangsql

SQL interface for Clang AST databases. Query C/C++ source code using SQL.

Part of the [xsql](https://github.com/0xeb/xsql) project family.

## Features

- Parse C/C++ source files with libclang
- Query AST entities via SQL: functions, classes, methods, variables, enums
- Multi-translation-unit support with schema prefixes
- AI agent mode for natural language queries (Claude, Copilot)
- AST caching for fast repeated queries
- Server mode for remote analysis
- Cross-platform: Windows, macOS, Linux
- Consistent xsql API patterns

## Quick Start

```bash
# Parse a file and list functions
clangsql main.cpp -e "SELECT name FROM functions"

# With compiler flags
clangsql main.cpp -- -std=c++17 -I./include

# Interactive mode
clangsql main.cpp -i

# Multi-file with schema prefixes (each file gets prefixed tables)
clangsql lib/utils.cpp:utils src/main.cpp:main -i

# Glob patterns for multiple files
clangsql "src/**/*.cpp" -- -std=c++17 -I./include

# CMake project (uses compile_commands.json)
clangsql --compile-commands build/compile_commands.json -i
clangsql --build-dir build -i  # Auto-find compile_commands.json
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

## AST Caching

Enable caching to avoid re-parsing unchanged files. **Recommended for project mode** where you'll run multiple queries against the same codebase.

```bash
# Enable caching for single file
clangsql main.cpp --cache -e "SELECT name FROM functions"

# Enable caching for project mode (big speedup on re-runs!)
clangsql --project ./src --cache -i

# Show cache hit/miss messages
clangsql --project ./src --cache --cache-verbose -i

# Custom cache directory
clangsql --project ./src --cache-dir /path/to/cache -i

# Clear all cached files
clangsql --clear-cache
```

**Cache location:** `%LOCALAPPDATA%\clangsql\cache` (Windows) or `~/.cache/clangsql` (Linux/macOS)

**What's validated:** Cache is invalidated when source files, any included headers, compiler args, or Clang version change. Each file's mtime is checked, so only modified files are re-parsed.

## AI Agent Mode

Query your codebase using natural language.

### Prerequisites for AI Features

The AI agent requires one of these CLI tools installed and authenticated:

| Provider | CLI Tool | Install | Login |
|----------|----------|---------|-------|
| Claude (default) | [Claude Code](https://docs.anthropic.com/en/docs/claude-code) | `npm install -g @anthropic-ai/claude-code` | Run `claude`, then `/login` |
| GitHub Copilot | [Copilot CLI](https://github.com/features/copilot/cli/) | `npm install -g @github/copilot` | Run `copilot`, then `/login` |

**Important:** You must be logged in before using AI features.

### Usage

```bash
# Interactive AI session
clangsql main.cpp --agent -i

# Single-shot prompt
clangsql main.cpp --prompt "Find all virtual methods"

# Verbose mode shows generated SQL
clangsql main.cpp --agent -v -i

# Choose provider (claude or copilot)
clangsql main.cpp --agent --provider claude -i
```

The agent generates SQL queries based on your natural language questions and executes them against the parsed AST.

## Server Mode

Host a parsed AST over TCP for remote querying:

```bash
# Start server (default port 17198)
clangsql main.cpp --server

# Remote client (no libclang required)
clangsql --remote localhost:17198 -q "SELECT name FROM functions"
clangsql --remote localhost:17198 -i
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

clangsql supports parsing entire projects with multiple translation units. Each file gets a schema prefix, making tables like `utils_functions`, `main_classes`, etc.

**Cross-file queries work via USR (Unified Symbol Resolution)** - a unique identifier consistent across translation units.

```bash
# Schema prefix syntax: file.cpp:schema_name
clangsql lib/utils.cpp:utils src/main.cpp:main -i

# Glob patterns (recursive)
clangsql "src/**/*.cpp" -- -std=c++17

# CMake projects with compile_commands.json
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clangsql --compile-commands build/compile_commands.json -i
```

```sql
-- Find utils functions that are called from main
SELECT DISTINCT u.name as function_called
FROM utils_functions u
JOIN main_calls c ON c.callee_usr = u.usr
WHERE u.is_system = 0;
```
```
function_called
---------------
log_info
compute_area
```

```sql
-- Aggregate function counts across all schemas
SELECT COUNT(*) as total_functions FROM (
    SELECT name FROM utils_functions WHERE is_system = 0
    UNION ALL
    SELECT name FROM main_functions WHERE is_system = 0
);
```

```sql
-- Cross-file inheritance (class in main inherits from utils)
SELECT m.derived_name, u.name as base_class
FROM main_inheritance m
JOIN utils_classes u ON m.base_usr = u.usr;
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

- [libxsql](https://github.com/0xeb/libxsql) - Shared virtual table framework
