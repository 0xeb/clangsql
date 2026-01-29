# CLANGSQL TODO

## Feature Requests

### Text Extraction / Source View

**Goal**: Provide a way to extract the actual source text of entities (functions, classes, etc.) from extent to extent.

**Use Cases**:
- Get the full body of a function as text
- Extract source code for any entity with extents
- Text view of declarations, definitions, statements

**Requirements**:
1. Tables should expose extent information:
   - `start_line`, `start_column`
   - `end_line`, `end_column`
   - Or combined: `extent_start`, `extent_end`

2. A helper function or virtual table to extract text given extents:
   ```sql
   -- Option A: Function
   SELECT get_source_text(file_path, start_line, start_col, end_line, end_col)
   FROM functions WHERE name = 'main';

   -- Option B: Virtual table with text column
   SELECT name, body_text FROM functions WHERE name = 'main';

   -- Option C: Separate source_text table
   SELECT s.text
   FROM functions f
   JOIN source_text s ON s.file = f.file
     AND s.start_line = f.extent_start_line
     AND s.end_line = f.extent_end_line;
   ```

3. Generic pattern for all entities with extents (functions, classes, enums, typedefs, etc.)

**Implementation Notes**:
- libclang provides `clang_getCursorExtent()` for cursor extents
- `clang_getExpansionLocation()` gives line/column info
- Need to read source file and extract text between extents
- Consider caching source file contents for performance

**Priority**: Medium
**Status**: Proposed
