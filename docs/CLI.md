# clangsql CLI Reference

SQL interface for C/C++ source code analysis using libclang.

## Quick Start

```bash
# Single file query
clangsql main.cpp -e "SELECT name FROM functions WHERE is_system = 0"

# Interactive REPL
clangsql main.cpp -i

# With compiler flags
clangsql main.cpp -std=c++17 -I./include -e "SELECT * FROM classes"
```

## Usage

```
clangsql <files...> [options] [-- clang-args...]
clangsql --remote host:port [options]
```

### Local Modes (require source files + libclang)

| Mode | Command | Description |
|------|---------|-------------|
| Query | `clangsql file.cpp -e "SQL"` | Execute query, exit |
| REPL | `clangsql file.cpp -i` | Interactive mode |
| Server | `clangsql file.cpp --server [port]` | Host SQL over TCP |

### Remote Mode (thin client, NO libclang required)

| Mode | Command | Description |
|------|---------|-------------|
| Query | `clangsql --remote host:port -q "SQL"` | Remote query |
| REPL | `clangsql --remote host:port -i` | Remote interactive |

## Options

```
Local Options:
  -e <sql>           Execute SQL query and exit
  -i                 Interactive mode (REPL)
  --server [port]    Start server (default: 13337)
  --token <token>    Auth token for server mode
  -h, --help         Show help
  --version          Show version

Remote Options:
  --remote host:port Connect to remote server
  -q <sql>           Execute SQL query (remote)
  -i                 Interactive mode (remote)
  --token <token>    Auth token for remote connection

Clang args (auto-detected or after --):
  -I<path>           Include path
  -D<name>[=value]   Define macro
  -std=c++XX         Language standard
  -isystem <path>    System include path
  -W*, -f*, etc.     Other clang flags
```

## Multi-File Support

Multiple source files create schema-prefixed tables:

```bash
# Two files → schema prefixes
clangsql main.cpp utils.cpp -e "SELECT * FROM main_functions"
clangsql main.cpp utils.cpp -e "SELECT * FROM utils_functions"

# Custom schema names
clangsql main.cpp:m utils.cpp:u -e "SELECT * FROM m_functions"

# Cross-file queries
clangsql main.cpp utils.cpp -e "
  SELECT m.name, u.name
  FROM main_calls mc
  JOIN utils_functions u ON mc.callee_name = u.name
"
```

Single file mode has no prefix:
```bash
clangsql main.cpp -e "SELECT * FROM functions"  # No prefix
```

## Tables

### Core Entities

| Table | Description | Key Columns |
|-------|-------------|-------------|
| `files` | Translation unit files | path, is_main_file, is_header, is_system |
| `functions` | Free functions | name, qualified_name, return_type, line, is_system |
| `classes` | Classes/structs/unions | name, kind, is_abstract, is_system |
| `methods` | Class methods | name, class_id, is_virtual, is_override, access |
| `fields` | Class fields | name, class_id, type, access, offset_bits |
| `variables` | Variables | name, type, scope_kind, function_id |
| `parameters` | Function parameters | name, function_id, type, index |
| `enums` | Enumerations | name, underlying_type, is_scoped |
| `enum_values` | Enum constants | name, enum_id, value |

### Relationships

| Table | Description | Key Columns |
|-------|-------------|-------------|
| `calls` | Call graph | caller_usr, callee_name, callee_usr, is_virtual |
| `inheritance` | Class hierarchy | derived_name, base_name, access, is_virtual |

### System Header Filtering

All tables include `is_system` column:
```sql
-- User code only
SELECT * FROM functions WHERE is_system = 0;

-- System headers only
SELECT * FROM classes WHERE is_system = 1;

-- Count both
SELECT
  SUM(CASE WHEN is_system = 0 THEN 1 ELSE 0 END) as user_count,
  SUM(is_system) as system_count
FROM functions;
```

## REPL Commands

```
clangsql> .tables          List all tables
clangsql> .schema <table>  Show table schema
clangsql> .attached        List attached TUs
clangsql> .quit            Exit
```

## Example Queries

### Find Functions

```sql
-- Functions with many parameters
SELECT f.name, COUNT(p.id) as params
FROM functions f
JOIN parameters p ON p.function_id = f.id
WHERE f.is_system = 0
GROUP BY f.id
HAVING params > 3;

-- Static functions
SELECT name, file_id, line FROM functions
WHERE is_static = 1 AND is_system = 0;
```

### Analyze Classes

```sql
-- Classes with virtual methods
SELECT DISTINCT c.name
FROM classes c
JOIN methods m ON m.class_id = c.id
WHERE m.is_virtual = 1 AND c.is_system = 0;

-- Inheritance depth
WITH RECURSIVE hierarchy AS (
  SELECT derived_usr, base_usr, 1 as depth
  FROM inheritance WHERE is_system = 0
  UNION ALL
  SELECT i.derived_usr, i.base_usr, h.depth + 1
  FROM inheritance i
  JOIN hierarchy h ON i.derived_usr = h.base_usr
)
SELECT derived_usr, MAX(depth) as max_depth
FROM hierarchy GROUP BY derived_usr;
```

### Call Graph Analysis

```sql
-- Find all callers of a function
SELECT DISTINCT f.name as caller
FROM calls c
JOIN functions f ON c.caller_usr = f.usr
WHERE c.callee_name = 'process_data';

-- Functions that call virtual methods
SELECT DISTINCT f.name
FROM functions f
JOIN calls c ON c.caller_usr = f.usr
WHERE c.is_virtual = 1;
```

## Server Mode

### Starting a Server

```bash
# Start server on default port (13337)
clangsql main.cpp utils.cpp --server

# Custom port
clangsql main.cpp --server 8080

# With auth token (required for non-localhost bind)
clangsql main.cpp --server 8080 --token mysecret
```

### Connecting as Client

```bash
# Single query
clangsql --remote localhost:13337 -q "SELECT name FROM functions"

# Interactive
clangsql --remote localhost:13337 -i

# With auth token
clangsql --remote localhost:13337 --token mysecret -q "SELECT * FROM classes"
```

### Benefits of Server Mode

1. **No libclang on client** - Remote mode is a thin TCP client
2. **Parse once, query many** - AST cached in server memory
3. **Cross-platform** - Query from Linux/macOS against Windows-parsed code
4. **Shared access** - Multiple clients can query same parsed code

## compile_commands.json Support

```bash
# Load flags from compile_commands.json
clangsql --compile-commands build/compile_commands.json main.cpp -e "..."

# Auto-detect (looks in current dir and build/)
clangsql main.cpp -e "..."  # Uses compile_commands.json if found
```

## Environment

Requires libclang in PATH:
```bash
# Windows
set PATH=C:\Program Files\LLVM\bin;%PATH%

# Linux/macOS
export PATH=/usr/lib/llvm-18/bin:$PATH
```
