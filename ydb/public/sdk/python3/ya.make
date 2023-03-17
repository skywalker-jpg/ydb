PY3_LIBRARY()

PY_SRCS(
    TOP_LEVEL
    ydb/__init__.py
    ydb/_apis.py
    ydb/_errors.py
    ydb/_grpc/__init__.py
    ydb/_grpc/common/__init__.py
    ydb/_session_impl.py
    ydb/_sp_impl.py
    ydb/_tx_ctx_impl.py
    ydb/_utilities.py
    ydb/aio/__init__.py
    ydb/aio/_utilities.py
    ydb/aio/connection.py
    ydb/aio/credentials.py
    ydb/aio/driver.py
    ydb/aio/iam.py
    ydb/aio/pool.py
    ydb/aio/resolver.py
    ydb/aio/scheme.py
    ydb/aio/table.py
    ydb/auth_helpers.py
    ydb/connection.py
    ydb/convert.py
    ydb/credentials.py
    ydb/dbapi/__init__.py
    ydb/dbapi/connection.py
    ydb/dbapi/cursor.py
    ydb/dbapi/errors.py
    ydb/default_pem.py
    ydb/driver.py
    ydb/export.py
    ydb/global_settings.py
    ydb/iam/__init__.py
    ydb/iam/auth.py
    ydb/import_client.py
    ydb/interceptor.py
    ydb/issues.py
    ydb/operation.py
    ydb/pool.py
    ydb/resolver.py
    ydb/scheme.py
    ydb/scheme_test.py
    ydb/scripting.py
    ydb/settings.py
    ydb/sqlalchemy/__init__.py
    ydb/sqlalchemy/types.py
    ydb/table.py
    ydb/table_test.py
    ydb/tornado/__init__.py
    ydb/tornado/tornado_helpers.py
    ydb/tracing.py
    ydb/types.py
    ydb/ydb_version.py
)

PEERDIR(
    contrib/python/grpcio
    contrib/python/protobuf
    ydb/public/api/grpc
    ydb/public/api/grpc/draft
    ydb/public/api/protos
)

END()
