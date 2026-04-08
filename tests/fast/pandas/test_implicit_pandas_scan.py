# simple DB API testcase

import pandas as pd
import pytest

import duckdb
from conftest import pandas_supports_arrow_backend


def make_dataframe(rows, use_pyarrow=False):
    df = pd.DataFrame(rows)
    if not use_pyarrow:
        return df
    if not pandas_supports_arrow_backend():
        pytest.skip("pyarrow-backed pandas DataFrame not available in this environment")
    return df.convert_dtypes(dtype_backend="pyarrow")


class TestImplicitPandasScan:
    @pytest.mark.parametrize("use_pyarrow", [False, True])
    def test_local_pandas_scan(self, duckdb_cursor, use_pyarrow):
        con = duckdb.connect()
        df = make_dataframe([{"COL1": "val1", "CoL2": 1.05}, {"COL1": "val3", "CoL2": 17}], use_pyarrow)  # noqa: F841
        r1 = con.execute("select * from df").fetchdf()
        assert r1["COL1"][0] == "val1"
        assert r1["COL1"][1] == "val3"
        assert r1["CoL2"][0] == 1.05
        assert r1["CoL2"][1] == 17

    @pytest.mark.parametrize("use_pyarrow", [False, True])
    def test_global_pandas_scan(self, duckdb_cursor, use_pyarrow):
        """Test that DuckDB can scan a module-level DataFrame variable."""
        con = duckdb.connect()
        global test_global_df
        test_global_df = make_dataframe([{"COL1": "val1", "CoL2": 1.05}, {"COL1": "val4", "CoL2": 17}], use_pyarrow)
        r1 = con.execute("select * from test_global_df").fetchdf()
        assert r1["COL1"][0] == "val1"
        assert r1["COL1"][1] == "val4"
        assert r1["CoL2"][0] == 1.05
        assert r1["CoL2"][1] == 17

    @pytest.mark.parametrize("use_pyarrow", [False, True])
    def test_rowid_hidden_but_orderable(self, duckdb_cursor, use_pyarrow):
        con = duckdb.connect()
        df = make_dataframe([{"a": 1}, {"a": 3}, {"a": 2}], use_pyarrow)  # noqa: F841

        star_columns = list(con.execute("select * from df limit 0").fetchnumpy().keys())
        assert star_columns == ["a"]

        descending = con.execute("select * from df order by rowid desc").fetchall()
        assert descending == [(2,), (3,), (1,)]

        with_rowid = con.execute("select rowid, * from df order by rowid").fetchall()
        assert with_rowid == [(0, 1), (1, 3), (2, 2)]

    @pytest.mark.parametrize("use_pyarrow", [False, True])
    def test_rowid_order_by_across_cte(self, duckdb_cursor, use_pyarrow):
        con = duckdb.connect()
        df = make_dataframe([{"a": 11}, {"a": 22}, {"a": 33}], use_pyarrow)  # noqa: F841

        result = con.execute(
            """
            with t as (
                select rowid, * from df
            )
            select * from t order by rowid desc
            """
        ).fetchall()
        assert result == [(2, 33), (1, 22), (0, 11)]

    @pytest.mark.parametrize("use_pyarrow", [False, True])
    def test_module_sql_rowid_hidden_but_orderable(self, duckdb_cursor, use_pyarrow):
        df = make_dataframe([{"a": 1}, {"a": 3}, {"a": 2}], use_pyarrow)  # noqa: F841

        star_columns = list(duckdb.sql("select * from df limit 0").fetchnumpy().keys())
        assert star_columns == ["a"]

        descending = duckdb.sql("select * from df order by rowid desc").fetchall()
        assert descending == [(2,), (3,), (1,)]

        with_rowid = duckdb.sql("select rowid, * from df order by rowid").fetchall()
        assert with_rowid == [(0, 1), (1, 3), (2, 2)]

    @pytest.mark.parametrize("use_pyarrow", [False, True])
    def test_module_sql_relation_chaining_rowid_negative(self, duckdb_cursor, use_pyarrow):
        df = make_dataframe([{"a": 1}, {"a": 3}, {"a": 2}], use_pyarrow)  # noqa: F841

        with pytest.raises(duckdb.BinderException, match='Referenced column "rowid" not found in FROM clause!'):
            duckdb.sql("from df").project("rowid, *").fetchall()
