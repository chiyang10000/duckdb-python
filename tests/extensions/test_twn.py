import struct

import pytest

import duckdb

DECISION_RECORD_SIZE = 8712
EXECUTION_RECORD_SIZE = 304


def write_decision(path):
    record = bytearray(DECISION_RECORD_SIZE)
    struct.pack_into("<qiiiii", record, 0, 1000000001, 7, 11, 1234, 2, 1)
    struct.pack_into("<B", record, 32, 1)
    struct.pack_into("<i", record, 36, 2)
    struct.pack_into("<B", record, 40, 3)
    struct.pack_into("<?", record, 41, True)
    struct.pack_into("<ii", record, 44, 11, 99)
    struct.pack_into("<q", record, 56, 1000000002)
    struct.pack_into("<H", record, 64, 3)
    record[66:69] = b"abc"
    struct.pack_into("<bbhiqii", record, 184, 4, -1, 9, 10, 1000000003, 12345, 6)
    struct.pack_into("<iiiiii", record, 208, 8, 9, -10, 11, 12, 13)
    struct.pack_into("<bbhiqii", record, 232, 1, -1, 2, 3, 4, 5, 6)
    struct.pack_into("<bbhiqii", record, 256, 7, 1, 8, 9, 10, 11, 12)
    struct.pack_into("<qiii", record, 280, 1000000004, 20, 2, 3)
    struct.pack_into("<10i", record, 300, *range(100, 110))
    struct.pack_into("<10i", record, 340, *range(200, 210))
    struct.pack_into("<10i", record, 380, *range(300, 310))
    struct.pack_into("<10i", record, 420, *range(400, 410))
    struct.pack_into("<8i", record, 460, 1110, 1234, -1, 2, 500, 6, 7, 8)
    record[492:500] = b"CLORD\0\0\0"
    record[500:508] = b"ORIG\0\0\0\0"
    record[508:516] = b"ORDER\0\0\0"
    struct.pack_into("<2f", record, 516, 1.25, -2.5)
    struct.pack_into("<f", record, 6660, 3.75)
    path.write_bytes(record)


def execution_record(local_time_ns, key, clord_id, text):
    record = bytearray(EXECUTION_RECORD_SIZE)
    struct.pack_into("<q14i", record, 0, local_time_ns, 2, key, 0, 1, 3, -1, 10, 20, 4, 21, 6, 4, 20, 5)
    record[64:69] = b"12345"
    record[69:85] = clord_id.ljust(16, b"\0")
    record[85:101] = b"ORIG".ljust(16, b"\0")
    record[101:109] = b"ORDER".ljust(8, b"\0")
    record[109:125] = b"EXEC".ljust(16, b"\0")
    record[125:133] = b"ACCOUNT".ljust(8, b"\0")
    record[133:141] = b"2330".ljust(8, b"\0")
    record[141:149] = b"SUB".ljust(8, b"\0")
    record[149:173] = b"20260813-01:02:03.123".ljust(24, b"\0")
    record[173:301] = text.ljust(128, b"\0")
    return record


def test_read_twn_decision(tmp_path):
    path = tmp_path / "decision-bin"
    write_decision(path)

    connection = duckdb.connect()
    row = connection.execute(
        """
        SELECT local_time_ns, key, message_is_decision_enabled, message_body,
               action_side, account_position, pending_time, decision_price,
               orderbook_bid_prices, order_error_code, order_clord_id,
               state, hidden
        FROM read_twn_decision(?)
        """,
        [str(path)],
    ).fetchone()

    assert row == (
        1000000001,
        1234,
        True,
        b"abc",
        -1,
        -10,
        4,
        11,
        tuple(range(100, 110)),
        1110,
        "CLORD",
        [1.25, -2.5],
        [3.75],
    )


def test_read_twn_execution(tmp_path):
    path = tmp_path / "execution-bin"
    path.write_bytes(execution_record(100, 1234, b"A1", b"first") + execution_record(200, 5678, b"B2", b"second"))

    connection = duckdb.connect()
    rows = connection.execute(
        """
        SELECT local_time_ns, key, exec_type, clord_id, symbol, text
        FROM read_twn_execution(?)
        ORDER BY local_time_ns
        """,
        [str(path)],
    ).fetchall()

    assert rows == [
        (100, 1234, "3", "A1", "2330", "first"),
        (200, 5678, "3", "B2", "2330", "second"),
    ]


@pytest.mark.parametrize(
    ("function_name", "record_size"),
    [("read_twn_decision", DECISION_RECORD_SIZE), ("read_twn_execution", EXECUTION_RECORD_SIZE)],
)
def test_read_twn_rejects_misaligned_file(tmp_path, function_name, record_size):
    path = tmp_path / "bad-bin"
    path.write_bytes(bytes(record_size + 1))

    with pytest.raises(duckdb.InvalidInputException, match="not aligned"):
        duckdb.execute(f"SELECT count(*) FROM {function_name}(?)", [str(path)])


def test_read_twn_rejects_invalid_decision_lengths(tmp_path):
    path = tmp_path / "decision-bin"
    record = bytearray(DECISION_RECORD_SIZE)
    struct.pack_into("<i", record, 20, 1537)
    path.write_bytes(record)

    with pytest.raises(duckdb.InvalidInputException, match="exceeds capacity"):
        duckdb.execute("SELECT state FROM read_twn_decision(?)", [str(path)]).fetchall()

    struct.pack_into("<i", record, 20, 0)
    struct.pack_into("<H", record, 64, 119)
    path.write_bytes(record)
    with pytest.raises(duckdb.InvalidInputException, match="exceeds capacity"):
        duckdb.execute("SELECT message_body FROM read_twn_decision(?)", [str(path)]).fetchall()


@pytest.mark.parametrize("function_name", ["read_twn_decision", "read_twn_execution"])
def test_read_twn_empty_file(tmp_path, function_name):
    path = tmp_path / "empty-bin"
    path.write_bytes(b"")

    assert duckdb.execute(f"SELECT count(*) FROM {function_name}(?)", [str(path)]).fetchone() == (0,)
