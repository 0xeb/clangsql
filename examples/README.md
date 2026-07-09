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
