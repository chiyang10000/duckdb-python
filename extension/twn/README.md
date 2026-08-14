# TWN persist reader

The built-in `twn` extension exposes the fixed-width TWN production persist
files as DuckDB table functions:

```sql
SELECT * FROM read_twn_decision('/path/to/decision-bin');
SELECT * FROM read_twn_execution('/path/to/execution-bin');
```

`read_twn_decision` requires 8712-byte records and
`read_twn_execution` requires 304-byte records. Both functions reject files
whose sizes are not record-aligned. Fixed-size character buffers are returned
as `VARCHAR` with trailing NUL bytes removed. The Format6 body is a `BLOB`,
orderbook levels are `INTEGER[10]`, and decision `state` and `hidden` are
`FLOAT[]` truncated to their persisted `state_size` and `hidden_size`.

The extension uses projection pushdown, so queries only decode requested
columns. In particular, `count(*)` does not materialize the large decision
state vectors.
