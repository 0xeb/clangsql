# clangsql Examples

Example projects demonstrating clangsql usage.

## Building Examples

Examples are built from the clangsql root:

```bash
cmake -B build -DCLANGSQL_BUILD_EXAMPLES=ON
cmake --build build --config Release
```

## sample_project/

Multi-file C++ project with classes, methods, functions, enums, and structs.

### Query Examples

```bash
cd examples/sample_project

# Basic queries
clangsql src/math_utils.cpp -I./include -e "SELECT name FROM functions WHERE is_system = 0"

# With compile_commands.json
clangsql src/main.cpp --compile-commands ../../build/compile_commands.json \
    -e "SELECT name FROM functions"
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
clangsql src/main.cpp --cache -e "SELECT COUNT(*) FROM functions"
clangsql --clear-cache
```

### Multi-file

```bash
clangsql src/math_utils.cpp src/string_utils.cpp -I./include \
    -e "SELECT name FROM math_utils_classes"
```

### Common Queries

```sql
SELECT name, kind FROM classes WHERE is_system = 0;
SELECT c.name, m.name FROM classes c JOIN methods m ON m.class_id = c.id WHERE m.is_virtual = 1;
SELECT name, line, end_line FROM functions WHERE is_definition = 1;
```
