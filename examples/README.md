# clangsql Examples

Example projects demonstrating clangsql usage.

## demo/

Multi-file demo project for quick testing.

### Setup

```bash
cd clangsql/examples/demo

# Generate compile_commands.json
cmake -B build -G Ninja
```

### Query Examples

```bash
# Single file query (no prefix)
clangsql main.cpp -e "SELECT name FROM functions WHERE is_system = 0"

# Multi-file (prefixed tables: main_*, handlers_*, network_*, utils_*)
clangsql main.cpp handlers.cpp network.cpp utils.cpp -e "SELECT name FROM main_functions WHERE is_system = 0"

# With compile_commands.json (auto-loads all files, prefixed tables)
clangsql --compile-commands build/compile_commands.json -e "SELECT * FROM main_calls WHERE is_system = 0"

# AI agent mode
clangsql main.cpp handlers.cpp network.cpp utils.cpp --agent -i
clangsql main.cpp --prompt "Find functions that handle network connections"
```

## sample_project/

Multi-file C++ project with classes, methods, functions, enums, and structs.

```bash
cd clangsql/examples/sample_project

# Build to generate compile_commands.json
cmake -B build -G Ninja
cmake --build build
```

### Query Examples

```bash
# Basic queries
clangsql src/math_utils.cpp -I./include -e "SELECT name FROM functions WHERE is_system = 0"

# With compile_commands.json
clangsql --compile-commands build/compile_commands.json -e "SELECT name FROM functions"

# Interactive mode
clangsql src/main.cpp -I./include -i
```

### Call Graph

```sql
SELECT caller_usr, callee_name, line FROM calls WHERE is_system = 0;
SELECT caller_usr FROM calls WHERE callee_name = 'add';
SELECT callee_name FROM calls WHERE is_virtual = 1;
```

### Inheritance

```sql
SELECT derived_name, base_name, access FROM inheritance WHERE is_system = 0;
SELECT derived_name FROM inheritance WHERE base_name LIKE '%Calculator%';
```

### AST Caching

```bash
clangsql src/main.cpp -I./include --cache -e "SELECT COUNT(*) FROM functions"
clangsql --clear-cache
```

### Multi-file with Schema Prefixes

```bash
clangsql src/math_utils.cpp:math src/string_utils.cpp:str -I./include \
    -e "SELECT name FROM math_functions"
```

### Common Queries

```sql
SELECT name, kind FROM classes WHERE is_system = 0;
SELECT c.name, m.name FROM classes c JOIN methods m ON m.class_id = c.id WHERE m.is_virtual = 1;
SELECT name, line, end_line FROM functions WHERE is_definition = 1;
```
