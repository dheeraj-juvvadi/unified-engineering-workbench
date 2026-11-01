# Input formats

Two file types are accepted: **ASCII STL** (geometry) and a **nodal temperature CSV** (ANSYS-style result export).
Both parsers are strict: a file is either read completely or rejected with a message that names the line and
says what to do. Source: `src/core/stl_ascii.cpp`, `src/core/ansys_csv.cpp`; tests: `tests/test_parsers.cpp`.

Errors carry `line` (1-based; **0 = the whole file**, e.g. empty or binary input), a `message` that already starts
with `Line N:` when a line applies, and a `hint`.

## ASCII STL

Units: the format has none. The workbench **assumes millimetres** and shows coordinates unscaled.

Accepted:
- `solid <name>` ... `facet normal nx ny nz` / `outer loop` / three `vertex x y z` / `endloop` / `endfacet` ... `endsolid [name]`
- keywords in any letter case, any spaces/tabs, blank lines anywhere, LF or CRLF line ends, optional UTF-8 BOM,
  no newline after the last line
- numbers like `1`, `-2.5`, `+3`, `1.5e-3`, `1E+2` (read with `std::from_chars`; no locale involved)
- the normal stored in the file is passed through unchanged (it is not recomputed)

Rejected (each with the line of the problem):

| Problem | Line reported |
|---|---|
| empty file (0 bytes) / only whitespace | 0 |
| binary STL (control bytes such as NUL in the first 512 bytes, or size exactly 84 + 50*n) | 0, hint "Export as ASCII STL" |
| first word is not `solid` | that line |
| keyword out of order or missing (`outer loop`, `endloop`, `endfacet`) | that line |
| facet with 2 vertices, or 4 or more vertices | the line where it is noticed (`endloop` / extra `vertex`) |
| vertex or normal with fewer/more than 3 numbers, or text that is not a number | that line |
| `nan`, `inf`, `-inf` | that line ("not finite") |
| values that overflow a double (`1e999`) or exceed float range (`1e300`) | that line |
| solid with zero facets | the `endsolid` line |
| no `endsolid` (file truncated), also when it stops inside a facet | the last non-blank line |
| anything after `endsolid` (a second solid is not supported) | that line |
| more than 2,000,000 triangles, or more than 200 MB | the offending facet / 0 |

Result: float positions (9 per triangle), float normals (3 per triangle), triangle count and the bounding box
(computed in double).

## Nodal temperature CSV

Header (first non-blank line, exact and case-sensitive; spaces around names are tolerated):

```
node_id,x_m,y_m,z_m,temperature_C
```

Units are fixed: coordinates in **metres**, temperature in **degrees Celsius**.
One node per row: integer id (>= 0, unique), then three coordinates and the temperature as decimal numbers
(`0.125`, `-3`, `1.5e-2`). No quoting, no units in the cells, `.` as decimal separator.

Accepted: LF or CRLF, UTF-8 BOM, blank lines anywhere (skipped), no final newline, unsorted ids,
temperature exactly -273.15.

Rejected:

| Problem | Line reported |
|---|---|
| empty file (0 bytes) / only whitespace | 0 |
| header only, no data rows | the header line |
| wrong header (name, case, order, or column count) | header line |
| row with the wrong number of columns (short row, extra column, trailing comma) | that line, e.g. `Line 7: expected 5 columns (node_id,x_m,y_m,z_m,temperature_C), found 4` |
| empty cell, non-numeric cell (`abc`, `20 degC`), non-integer or negative `node_id` | that line, names the column |
| `nan` / `inf` / `1e999` | that line |
| temperature below -273.15 | that line |
| duplicate `node_id` | the second occurrence |
| more than 5,000,000 rows, or more than 200 MB | the offending row / 0 |

Result: nodes (`id, x, y, z, T`), `t_min`, `t_max` and the coordinate bounding box.

## Measured parse times

Machine: Linux container, 4 logical CPUs (`nproc` = 4), Intel(R) Xeon(R) Processor @ 2.10GHz,
g++ 13.3, `-O2`, single thread, input already in memory (file reading and JSON building not included).
Times are from three runs of `tests/test_parsers.cpp`; the near-limit rows from one run with `WB_BIG_TESTS=1`.

| Input | Size | Parse time |
|---|---|---|
| STL, 200,000 triangles (in the test suite) | 47.3 MB | 130 - 190 ms |
| CSV, 500,000 rows, ascending ids (in the test suite) | 13.7 MB | 140 - 205 ms |
| CSV, 500,000 rows, descending ids (hash-set duplicate check) | 14 MB | 175 - 230 ms |
| STL, 800,000 triangles (near the 200 MB limit) | 189 MB | about 540 ms |
| CSV, 5,000,000 rows (= row limit) | 155 MB | about 1.8 s |

Roughly 350 MB/s for STL and 85 MB/s for CSV text. Memory is the input text plus the result
(48 bytes per triangle; 48 bytes per CSV node), so a 200 MB STL needs about 400 MB in total while loading.
Inputs over the limits are rejected up front (size check) before any parsing.
