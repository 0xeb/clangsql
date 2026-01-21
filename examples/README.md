# clangsql Examples

This directory contains example projects demonstrating clangsql usage.

## sample_project/

A multi-file C++ project with classes, methods, functions, enums, and structs.

### Structure

```
sample_project/
├── CMakeLists.txt           # CMake build with compile_commands.json
├── include/
│   └── sample/
│       ├── math_utils.hpp   # Calculator classes, math functions
│       └── string_utils.hpp # StringUtils class, enums, structs
└── src/
    ├── main.cpp             # Application entry point
    ├── math_utils.cpp       # Math implementations
    └── string_utils.cpp     # String implementations
```

### Build the Example

```bash
cd sample_project
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --config Release
```

### Query with clangsql

```bash
# Single file queries
clangsql src/math_utils.cpp -I./include -e "SELECT name FROM functions WHERE is_system = 0"
clangsql src/math_utils.cpp -I./include -e "SELECT name FROM classes WHERE is_system = 0"
clangsql src/math_utils.cpp -I./include -e "SELECT c.name, m.name FROM classes c JOIN methods m ON m.class_id = c.id WHERE c.is_system = 0"

# Multi-file queries (schema-prefixed tables)
clangsql src/math_utils.cpp src/string_utils.cpp -I./include \
    -e "SELECT name FROM math_utils_classes WHERE is_system = 0"

# Cross-schema queries
clangsql src/math_utils.cpp src/string_utils.cpp -I./include \
    -e "SELECT m.name as math_class, s.name as string_class FROM math_utils_classes m, string_utils_classes s WHERE m.is_system = 0 AND s.is_system = 0"

# Custom schema names
clangsql src/math_utils.cpp:math src/string_utils.cpp:str -I./include \
    -e "SELECT name FROM math_functions WHERE is_system = 0"
```

### What's in This Example

| Entity | Count | Description |
|--------|-------|-------------|
| Classes | 4 | Calculator, ScientificCalculator, StringUtils, StringConfig |
| Methods | ~15 | add, subtract, multiply, divide, power, factorial, is_prime, to_upper, to_lower, trim, split, join, contains |
| Free Functions | 5 | gcd, lcm, reverse, starts_with, ends_with |
| Enums | 1 | ResultCode |
| Structs | 1 | StringConfig |

### Sample Queries

```sql
-- List all user-defined classes
SELECT name, kind, is_abstract FROM classes WHERE is_system = 0;

-- Find virtual methods
SELECT c.name as class, m.name as method
FROM classes c
JOIN methods m ON m.class_id = c.id
WHERE m.is_virtual = 1 AND c.is_system = 0;

-- Function extent (line numbers)
SELECT name, line, end_line FROM functions WHERE is_system = 0 AND is_definition = 1;

-- Count entities per file
SELECT
    (SELECT COUNT(*) FROM classes WHERE is_system = 0) as classes,
    (SELECT COUNT(*) FROM methods WHERE is_system = 0) as methods,
    (SELECT COUNT(*) FROM functions WHERE is_system = 0) as functions;
```
