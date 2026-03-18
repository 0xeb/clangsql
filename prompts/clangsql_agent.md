# CLANGSQL Agent Guide

SQL interface for C/C++ source code analysis using Clang AST.

## Key Concepts

### Why Filter System Headers?
Every C++ file includes system headers (`<vector>`, `<string>`, etc.). These add thousands of symbols to the database. Almost always, you want user code only:
```sql
WHERE is_system = 0
```

### Declaration vs Definition
C++ separates declaration (announces existence) from definition (provides implementation):
- A header declares `void foo();`
- A source file defines `void foo() { ... }`

Use `is_definition = 1` to find implementations:
```sql
SELECT name FROM functions WHERE is_definition = 1 AND is_system = 0;
```

### USR (Unified Symbol Resolution)
Each symbol has a unique `usr` string that's consistent across translation units. Use it to:
- Match the same symbol across files
- Join calls to functions: `calls.callee_usr = functions.usr`
- Track inheritance: `inheritance.base_usr = classes.usr`

Use `id` for simple same-table foreign keys (e.g., `methods.class_id = classes.id`).

### Translation Units and Schemas
Each source file is a translation unit. When analyzing multiple files:
```bash
clangsql file1.cpp:a file2.cpp:b -i
```
Tables become prefixed: `a_functions`, `b_functions`. Use USR to correlate symbols across schemas.

## CLI Usage

```bash
clangsql main.cpp -e "SELECT name FROM functions"      # Single query
clangsql main.cpp -i                                    # Interactive REPL
clangsql main.cpp --http 8080                           # HTTP server mode
clangsql main.cpp -- -std=c++17 -I./include            # Pass Clang flags
```

## Project Mode

Analyze entire codebases with a unified schema. Files are deduplicated by USR, giving you a single view across all translation units.

```bash
# Analyze all .cpp files in a directory
clangsql --project ./src -i

# Custom file patterns
clangsql --project ./src --pattern "*.cpp" --pattern "*.cxx" -i

# Exclude directories
clangsql --project ./src --exclude build --exclude third_party -i

# With Clang flags
clangsql --project ./src -- -std=c++17 -I./include -DDEBUG
```

### AST Caching (Speed Up Re-parses)

Enable caching to avoid re-parsing unchanged files. Cache validates source AND all include file mtimes.

```bash
# Enable caching (disabled by default)
clangsql --project ./src --cache -i

# With verbose output (shows cache hits/misses)
clangsql --project ./src --cache --cache-verbose -i

# Custom cache directory
clangsql --project ./src --cache-dir C:/my/cache -i

# Clear all cached files
clangsql --clear-cache
```

**Cache location:** `%LOCALAPPDATA%\clangsql\cache` (Windows) or `~/.cache/clangsql` (Linux/macOS)

**When to use:** Enable `--cache` for repeated analysis of the same project. First parse is slightly slower (writes cache), but subsequent runs are much faster for unchanged files.

### How Deduplication Works
- **Functions, classes, methods, variables, enums**: Deduplicated by USR (same symbol = one row)
- **Files**: Deduplicated by normalized path
- **Calls, inheritance, string_literals**: NOT deduplicated (each occurrence preserved with `file_id`)

### Project Mode Queries

```sql
-- Files in the project
SELECT path FROM files WHERE is_system = 0;

-- Functions with their source files
SELECT f.name, fi.path
FROM functions f
JOIN files fi ON f.file_id = fi.id
WHERE f.is_system = 0;

-- Calls by source file
SELECT fi.path, c.callee_name, COUNT(*) as call_count
FROM calls c
JOIN files fi ON c.file_id = fi.id
WHERE c.is_system = 0
GROUP BY fi.path, c.callee_name
ORDER BY call_count DESC;

-- String literals across the project
SELECT value, COUNT(*) as occurrences
FROM string_literals
WHERE is_system = 0
GROUP BY value
ORDER BY occurrences DESC LIMIT 20;

-- Functions per file
SELECT fi.path, COUNT(f.id) as func_count
FROM files fi
LEFT JOIN functions f ON f.file_id = fi.id
WHERE fi.is_system = 0
GROUP BY fi.id
ORDER BY func_count DESC;
```

## Tables (12 total)

All tables have `is_system` column (1=system header, 0=user code).

### files
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| path | TEXT | Full file path |
| is_main_file | INT | 1=main TU file |
| is_header | INT | 1=header file |
| is_system | INT | 1=system include |

### functions
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| usr | TEXT | Unique symbol ID (for cross-TU matching) |
| name | TEXT | Function name |
| qualified_name | TEXT | Full namespace::name |
| return_type | TEXT | Return type |
| file_id | INT | FK to files |
| line, column | INT | Start position |
| end_line, end_column | INT | End position |
| is_definition | INT | 1=definition, 0=declaration |
| is_inline | INT | 1=inline |
| is_variadic | INT | 1=has ... |
| is_static | INT | 1=static linkage |

### classes
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| usr | TEXT | Unique symbol ID |
| name | TEXT | Class name |
| qualified_name | TEXT | Full name |
| kind | TEXT | "class", "struct", "union" |
| file_id | INT | FK to files |
| line | INT | Line number |
| is_definition | INT | 1=definition |
| is_abstract | INT | 1=has pure virtual |

### methods
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| usr | TEXT | Unique symbol ID |
| class_id | INT | FK to classes |
| name | TEXT | Method name |
| qualified_name | TEXT | Full name |
| return_type | TEXT | Return type |
| access | TEXT | "public", "protected", "private" |
| is_virtual | INT | 1=virtual |
| is_pure_virtual | INT | 1=pure virtual (= 0) |
| is_static | INT | 1=static method |
| is_const | INT | 1=const method |
| line, column, end_line, end_column | INT | Position |

### fields
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| class_id | INT | FK to classes |
| name | TEXT | Field name |
| type | TEXT | Field type |
| access | TEXT | Access level |
| is_static | INT | 1=static member |
| is_mutable | INT | 1=mutable |
| bit_width | INT | Bit field width (0=normal) |
| offset_bits | INT | Offset from class start |

### variables
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| usr | TEXT | Unique symbol ID |
| name | TEXT | Variable name |
| type | TEXT | Type |
| file_id | INT | FK to files |
| line | INT | Line number |
| function_id | INT | FK to functions (NULL for globals) |
| scope_kind | TEXT | "global", "local", "static_local" |
| storage_class | TEXT | "static", "extern", "none" |
| is_const | INT | 1=const |

### parameters
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| function_id | INT | FK to functions/methods |
| name | TEXT | Parameter name |
| type | TEXT | Parameter type |
| index_ | INT | 0-based position |
| has_default | INT | 1=has default value |

### enums
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| usr | TEXT | Unique symbol ID |
| name | TEXT | Enum name |
| underlying_type | TEXT | Integer type |
| is_scoped | INT | 1=enum class |
| file_id | INT | FK to files |
| line | INT | Line number |

### enum_values
| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| enum_id | INT | FK to enums |
| name | TEXT | Constant name |
| value | INT | Integer value |

### calls
Function call graph.

| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| caller_usr | TEXT | Calling function USR |
| callee_usr | TEXT | Called function USR |
| callee_name | TEXT | Called function name |
| line, column | INT | Call site position |
| is_virtual | INT | 1=virtual method call |
| file_id | INT | FK to files (project mode) |

### inheritance
Class hierarchy.

| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| derived_usr | TEXT | Derived class USR |
| derived_name | TEXT | Derived class name |
| base_usr | TEXT | Base class USR |
| base_name | TEXT | Base class name |
| access | TEXT | "public", "protected", "private" |
| is_virtual | INT | 1=virtual inheritance |
| file_id | INT | FK to files (project mode) |

### string_literals
String constants found in code, with direct FK to enclosing function for fast queries.

| Column | Type | Description |
|--------|------|-------------|
| id | INT | Primary key |
| content | TEXT | String content (unescaped) |
| file_id | INT | FK to files |
| line | INT | Line number |
| column | INT | Column number |
| function_usr | TEXT | Enclosing function USR (empty if global) |
| func_id | INT | **FK to functions** (0 if global) - fast queries! |
| is_wide | INT | 1=wide string (L"...") |
| is_system | INT | 1=system header |

**Fast String Queries (use func_id, not line ranges!):**
```sql
-- All strings in a specific function (O(1) lookup)
SELECT content FROM string_literals WHERE func_id = 42;

-- Which function has the most string literals? (fast GROUP BY)
SELECT f.name, COUNT(*) as count
FROM string_literals s
JOIN functions f ON s.func_id = f.id
WHERE s.is_system = 0 AND f.is_system = 0
GROUP BY s.func_id
ORDER BY count DESC LIMIT 10;

-- Functions with hardcoded "password" strings
SELECT DISTINCT f.name
FROM string_literals s
JOIN functions f ON s.func_id = f.id
WHERE s.content LIKE '%password%' AND s.is_system = 0;

-- Global strings (not inside any function)
SELECT content FROM string_literals WHERE func_id = 0 AND is_system = 0;
```

## Common Queries

### Basic Queries

```sql
-- User functions only (exclude system headers)
SELECT name, return_type FROM functions WHERE is_system = 0;

-- Classes with method counts
SELECT c.name, COUNT(m.id) as methods
FROM classes c LEFT JOIN methods m ON m.class_id = c.id
WHERE c.is_system = 0 GROUP BY c.id ORDER BY methods DESC;

-- Virtual methods
SELECT c.name, m.name FROM methods m
JOIN classes c ON m.class_id = c.id
WHERE m.is_virtual = 1 AND m.is_system = 0;

-- Function size (line count)
SELECT name, end_line - line + 1 as lines
FROM functions WHERE is_definition = 1
ORDER BY lines DESC LIMIT 10;
```

### Call Graph Analysis

```sql
-- Who calls what
SELECT f.name as caller, c.callee_name
FROM calls c JOIN functions f ON c.caller_usr = f.usr
WHERE c.is_system = 0;

-- Most called functions
SELECT callee_name, COUNT(*) as calls
FROM calls WHERE is_system = 0
GROUP BY callee_name ORDER BY calls DESC LIMIT 10;

-- Functions calling a specific function
SELECT DISTINCT f.name
FROM functions f
JOIN calls c ON c.caller_usr = f.usr
WHERE c.callee_name = 'malloc';

-- Leaf functions (call nothing)
SELECT f.name FROM functions f
LEFT JOIN calls c ON c.caller_usr = f.usr
WHERE c.id IS NULL AND f.is_system = 0 AND f.is_definition = 1;

-- Call graph depth with recursive CTE
WITH RECURSIVE callgraph AS (
    SELECT usr, name, 0 as depth
    FROM functions WHERE name = 'main'
    UNION ALL
    SELECT f.usr, f.name, cg.depth + 1
    FROM callgraph cg
    JOIN calls c ON c.caller_usr = cg.usr
    JOIN functions f ON f.usr = c.callee_usr
    WHERE cg.depth < 5
)
SELECT DISTINCT name, MIN(depth) as min_depth
FROM callgraph GROUP BY name ORDER BY min_depth;
```

### Class Hierarchy

```sql
-- Inheritance relationships
SELECT derived_name, base_name, access
FROM inheritance WHERE is_system = 0;

-- Find all subclasses of a class
SELECT derived_name FROM inheritance WHERE base_name = 'BaseClass';

-- Classes with no base class (root classes)
SELECT c.name FROM classes c
LEFT JOIN inheritance i ON i.derived_usr = c.usr
WHERE i.id IS NULL AND c.is_system = 0;

-- Full hierarchy with recursive CTE
WITH RECURSIVE hierarchy AS (
    SELECT usr, name, 0 as depth
    FROM classes WHERE name = 'BaseClass'
    UNION ALL
    SELECT c.usr, c.name, h.depth + 1
    FROM hierarchy h
    JOIN inheritance i ON i.base_usr = h.usr
    JOIN classes c ON c.usr = i.derived_usr
)
SELECT name, depth FROM hierarchy ORDER BY depth, name;

-- Abstract classes (have pure virtual methods)
SELECT name FROM classes WHERE is_abstract = 1 AND is_system = 0;
```

### API Surface Analysis

```sql
-- Public methods (the API)
SELECT c.name, m.name, m.return_type
FROM methods m JOIN classes c ON m.class_id = c.id
WHERE m.access = 'public' AND c.is_system = 0
ORDER BY c.name, m.name;

-- Private methods (implementation details)
SELECT c.name, m.name
FROM methods m JOIN classes c ON m.class_id = c.id
WHERE m.access = 'private' AND c.is_system = 0;

-- Class fields layout with offsets
SELECT c.name, f.name, f.type, f.offset_bits
FROM fields f JOIN classes c ON f.class_id = c.id
WHERE c.is_system = 0 ORDER BY c.name, f.offset_bits;

-- Const methods (don't modify state)
SELECT c.name, m.name FROM methods m
JOIN classes c ON m.class_id = c.id
WHERE m.is_const = 1 AND c.is_system = 0;
```

### Code Metrics

```sql
-- Functions by complexity (parameter count)
SELECT f.name, COUNT(p.id) as param_count
FROM functions f
LEFT JOIN parameters p ON p.function_id = f.id
WHERE f.is_system = 0
GROUP BY f.id ORDER BY param_count DESC LIMIT 20;

-- Classes by size (field count)
SELECT c.name, COUNT(f.id) as field_count
FROM classes c LEFT JOIN fields f ON f.class_id = c.id
WHERE c.is_system = 0
GROUP BY c.id ORDER BY field_count DESC;

-- Files by function count
SELECT fi.path, COUNT(fu.id) as func_count
FROM files fi
LEFT JOIN functions fu ON fu.file_id = fi.id
WHERE fi.is_system = 0
GROUP BY fi.id ORDER BY func_count DESC;

-- Return type distribution
SELECT return_type, COUNT(*) as count
FROM functions WHERE is_system = 0
GROUP BY return_type ORDER BY count DESC LIMIT 10;
```

### Static Analysis Patterns

```sql
-- Unused parameters (unnamed)
SELECT f.name as function, p.type as param_type
FROM parameters p
JOIN functions f ON p.function_id = f.id
WHERE (p.name = '' OR p.name IS NULL) AND f.is_system = 0;

-- Methods that might be const (non-const, non-setter)
SELECT c.name, m.name
FROM methods m JOIN classes c ON m.class_id = c.id
WHERE m.is_const = 0 AND m.is_static = 0
  AND m.name NOT LIKE 'set%'
  AND m.name NOT LIKE 'operator%'
  AND c.is_system = 0;

-- Static variables (potential globals)
SELECT name, type, scope_kind
FROM variables WHERE storage_class = 'static' AND is_system = 0;

-- Large enums (many values)
SELECT e.name, COUNT(ev.id) as value_count
FROM enums e JOIN enum_values ev ON ev.enum_id = e.id
WHERE e.is_system = 0
GROUP BY e.id ORDER BY value_count DESC;
```

### Cross-File Analysis

When multiple files are attached with schema prefixes:

```sql
-- Attach files
-- clangsql file1.cpp:f1 file2.cpp:f2 -i

-- Find functions defined in both files (ODR candidates)
SELECT f1.name
FROM f1_functions f1
JOIN f2_functions f2 ON f1.name = f2.name
WHERE f1.is_definition = 1 AND f2.is_definition = 1;

-- Cross-file calls
SELECT f1.name as caller, c.callee_name
FROM f1_calls c
JOIN f1_functions f1 ON c.caller_usr = f1.usr
JOIN f2_functions f2 ON c.callee_usr = f2.usr;
```

## Tips

- **Always filter system headers**: `WHERE is_system = 0` unless you specifically want STL/OS symbols
- **Use USR for cross-table matching**: `calls.caller_usr = functions.usr`
- **Use id for parent-child**: `methods.class_id = classes.id`
- **Pattern matching**: `LIKE 'get%'`, `LIKE '%Handler%'`, `LIKE '%_t'`
- **Count first**: Before complex queries, `SELECT COUNT(*) FROM table WHERE is_system = 0`

## Common Mistakes

```sql
-- WRONG: Missing system filter (returns thousands of STL symbols)
SELECT name FROM functions;

-- RIGHT: Filter to user code
SELECT name FROM functions WHERE is_system = 0;

-- WRONG: Joining calls.caller_id (doesn't exist)
SELECT * FROM calls c JOIN functions f ON c.caller_id = f.id;

-- RIGHT: Use USR
SELECT * FROM calls c JOIN functions f ON c.caller_usr = f.usr;

-- WRONG: Expecting declarations to have bodies
SELECT end_line - line FROM functions;  -- Many will be 0

-- RIGHT: Filter to definitions
SELECT end_line - line FROM functions WHERE is_definition = 1;
```

## Limitations

- **Read-only**: Analysis only, no code modification
- **No macro expansion tracking**: Macros are expanded before parsing
- **Templates**: Only declarations tracked, not instantiations
- **Inline definitions**: Header-defined functions appear per-TU
- **No control flow**: No CFG, basic blocks, or data flow analysis
- **No comments**: Source comments not preserved in AST

## Debugging Queries

If a query returns unexpected results:

1. **Check system filter**: Did you add `WHERE is_system = 0`?
2. **Check definition filter**: Are you looking at declarations? Add `is_definition = 1`
3. **Check counts**: `SELECT COUNT(*) FROM table WHERE is_system = 0`
4. **Inspect sample**: `SELECT * FROM table WHERE is_system = 0 LIMIT 5`
5. **Verify joins**: Print both sides before joining

---

## Server Modes

CLANGSQL supports two server protocols for remote queries: **HTTP REST** (recommended) and raw TCP.

---

### HTTP REST Server (Recommended)

Standard REST API that works with curl, any HTTP client, or LLM tools.

**Starting the server:**
```bash
# Default port 8081
clangsql main.cpp --http

# Custom port and bind address
clangsql main.cpp --http 9000 --bind 0.0.0.0

# With authentication
clangsql main.cpp --http 8081 --token mysecret
```

**HTTP Endpoints:**

| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/` | GET | No | Welcome message |
| `/help` | GET | No | API documentation (for LLM discovery) |
| `/query` | POST | Yes* | Execute SQL (body = raw SQL) |
| `/status` | GET | Yes* | Health check |
| `/shutdown` | POST | Yes* | Stop server |

*Auth required only if `--token` was specified.

**Example with curl:**
```bash
# Get API documentation
curl http://localhost:8081/help

# Execute SQL query
curl -X POST http://localhost:8081/query -d "SELECT name FROM functions WHERE is_system = 0 LIMIT 5"

# With authentication
curl -X POST http://localhost:8081/query \
     -H "Authorization: Bearer mysecret" \
     -d "SELECT * FROM classes"

# Check status
curl http://localhost:8081/status
```

**Response Format (JSON):**
```json
{"success": true, "columns": ["name"], "rows": [["main"]], "row_count": 1}
```

```json
{"success": false, "error": "no such table: bad_table"}
```

---

### MCP Server (Model Context Protocol)

Start an MCP server for integration with Claude Desktop and other MCP clients.

**Starting from the REPL:**
```
clangsql> .mcp start
MCP server started on port 9042
SSE endpoint: http://127.0.0.1:9042/sse

Available tools:
  clangsql_query  - Execute SQL query directly
  clangsql_agent  - Ask natural language question (AI-powered)
```

**REPL Commands:**

| Command | Description |
|---------|-------------|
| `.mcp` | Show status or start if not running |
| `.mcp start` | Start MCP server on random port |
| `.mcp stop` | Stop MCP server |
| `.mcp help` | Show MCP help |

**Claude Desktop Configuration:**
```json
{
  "mcpServers": {
    "clangsql": {
      "url": "http://127.0.0.1:<port>/sse"
    }
  }
}
```

---

### Dynamic HTTP Server (from REPL)

Start/stop the HTTP server dynamically from the agent REPL:

```
clangsql> .http start
HTTP server started on port 8142
URL: http://127.0.0.1:8142
```

**REPL Commands:**

| Command | Description |
|---------|-------------|
| `.http` | Show status or start if not running |
| `.http start` | Start HTTP server on random port |
| `.http stop` | Stop HTTP server |
| `.http help` | Show HTTP help |

Press Ctrl+C to stop the server and return to the REPL.
