RECURSE_FOR_TESTS(
    ut_backup
    ut_base
    ut_base_reboots
    ut_bsvolume
    ut_bsvolume_reboots
    ut_cdc_stream
    ut_cdc_stream_reboots
    ut_column_build
    ut_compaction
    ut_export
    ut_export_reboots_s3
    ut_external_data_source
    ut_external_data_source_reboots
    ut_external_table
    ut_external_table_reboots
    ut_extsubdomain
    ut_extsubdomain_reboots
    ut_filestore_reboots
    ut_index
    ut_index_build
    ut_index_build_reboots
    ut_login
    ut_move
    ut_move_reboots
    ut_olap
    ut_olap_reboots
    ut_pq_reboots
    ut_reboots
    ut_replication
    ut_replication_reboots
    ut_restore
    ut_rtmr
    ut_rtmr_reboots
    ut_ru_calculator
    ut_sequence
    ut_sequence_reboots
    ut_serverless
    ut_split_merge
    ut_split_merge_reboots
    ut_stats
    ut_subdomain
    ut_subdomain_reboots
    ut_topic_splitmerge
    ut_ttl
    ut_user_attributes
    ut_user_attributes_reboots
)

LIBRARY()

SRCS(
    defs.h
    schemeshard.cpp
    schemeshard__borrowed_compaction.cpp
    schemeshard__compaction.cpp
    schemeshard__clean_pathes.cpp
    schemeshard__conditional_erase.cpp
    schemeshard__describe_scheme.cpp
    schemeshard__delete_tablet_reply.cpp
    schemeshard__find_subdomain_path_id.cpp
    schemeshard__fix_bad_paths.cpp
    schemeshard__init.cpp
    schemeshard__init_populator.cpp
    schemeshard__init_root.cpp
    schemeshard__init_schema.cpp
    schemeshard__serverless_storage_billing.cpp
    schemeshard__sync_update_tenants.cpp
    schemeshard__login.cpp
    schemeshard__monitoring.cpp
    schemeshard__notify.cpp
    schemeshard__operation.cpp
    schemeshard__operation.h
    schemeshard__operation_blob_depot.cpp
    schemeshard__operation_side_effects.cpp
    schemeshard__operation_side_effects.h
    schemeshard__operation_memory_changes.cpp
    schemeshard__operation_db_changes.cpp
    schemeshard__operation_alter_bsv.cpp
    schemeshard__operation_alter_extsubdomain.cpp
    schemeshard__operation_alter_fs.cpp
    schemeshard__operation_alter_index.cpp
    schemeshard__operation_alter_kesus.cpp
    schemeshard__operation_alter_login.cpp
    schemeshard__operation_alter_olap_store.cpp
    schemeshard__operation_alter_olap_table.cpp
    schemeshard__operation_alter_pq.cpp
    schemeshard__operation_alter_solomon.cpp
    schemeshard__operation_alter_subdomain.cpp
    schemeshard__operation_alter_table.cpp
    schemeshard__operation_alter_user_attrs.cpp
    schemeshard__operation_assign_bsv.cpp
    schemeshard__operation_cancel_tx.cpp
    schemeshard__operation_common.cpp
    schemeshard__operation_common.h
    schemeshard__operation_common_subdomain.h
    schemeshard__operation_consistent_copy_tables.cpp
    schemeshard__operation_copy_table.cpp
    schemeshard__operation_create_backup.cpp
    schemeshard__operation_create_bsv.cpp
    schemeshard__operation_create_external_data_source.cpp
    schemeshard__operation_create_external_table.cpp
    schemeshard__operation_create_extsubdomain.cpp
    schemeshard__operation_create_fs.cpp
    schemeshard__operation_create_index.cpp
    schemeshard__operation_create_indexed_table.cpp
    schemeshard__operation_create_kesus.cpp
    schemeshard__operation_create_lock.cpp
    schemeshard__operation_create_olap_store.cpp
    schemeshard__operation_create_olap_table.cpp
    schemeshard__operation_create_pq.cpp
    schemeshard__operation_create_replication.cpp
    schemeshard__operation_create_restore.cpp
    schemeshard__operation_create_rtmr.cpp
    schemeshard__operation_create_sequence.cpp
    schemeshard__operation_create_solomon.cpp
    schemeshard__operation_create_subdomain.cpp
    schemeshard__operation_create_table.cpp
    schemeshard__operation_drop_bsv.cpp
    schemeshard__operation_drop_external_data_source.cpp
    schemeshard__operation_drop_external_table.cpp
    schemeshard__operation_drop_extsubdomain.cpp
    schemeshard__operation_drop_fs.cpp
    schemeshard__operation_drop_indexed_table.cpp
    schemeshard__operation_drop_kesus.cpp
    schemeshard__operation_drop_lock.cpp
    schemeshard__operation_drop_olap_store.cpp
    schemeshard__operation_drop_olap_table.cpp
    schemeshard__operation_drop_pq.cpp
    schemeshard__operation_drop_replication.cpp
    schemeshard__operation_drop_sequence.cpp
    schemeshard__operation_drop_solomon.cpp
    schemeshard__operation_drop_subdomain.cpp
    schemeshard__operation_drop_table.cpp
    schemeshard__operation_drop_unsafe.cpp
    schemeshard__operation_mkdir.cpp
    schemeshard__operation_modify_acl.cpp
    schemeshard__operation_move_index.cpp
    schemeshard__operation_move_table.cpp
    schemeshard__operation_move_tables.cpp
    schemeshard__operation_move_table_index.cpp
    schemeshard__operation_part.cpp
    schemeshard__operation_part.h
    schemeshard__operation_rmdir.cpp
    schemeshard__operation_split_merge.cpp
    schemeshard__operation_just_reject.cpp
    schemeshard__operation_upgrade_subdomain.cpp
    schemeshard__operation_initiate_build_index.cpp
    schemeshard__operation_finalize_build_index.cpp
    schemeshard__operation_create_build_index.cpp
    schemeshard__operation_apply_build_index.cpp
    schemeshard__operation_cansel_build_index.cpp
    schemeshard__operation_drop_index.cpp
    schemeshard__operation_create_cdc_stream.cpp
    schemeshard__operation_alter_cdc_stream.cpp
    schemeshard__operation_drop_cdc_stream.cpp
    schemeshard__operation_allocate_pq.cpp
    schemeshard__operation_deallocate_pq.cpp
    schemeshard__pq_stats.cpp
    schemeshard__publish_to_scheme_board.cpp
    schemeshard__state_changed_reply.cpp
    schemeshard__table_stats.cpp
    schemeshard__table_stats_histogram.cpp
    schemeshard__upgrade_schema.cpp
    schemeshard__upgrade_access_database.cpp
    schemeshard__make_access_database_no_inheritable.cpp
    schemeshard_audit_log_fragment.cpp
    schemeshard_audit_log.cpp
    schemeshard_impl.cpp
    schemeshard_impl.h
    schemeshard_billing_helpers.cpp
    schemeshard_cdc_stream_scan.cpp
    schemeshard_domain_links.h
    schemeshard_domain_links.cpp
    schemeshard_effective_acl.h
    schemeshard_effective_acl.cpp
    schemeshard_identificators.cpp
    schemeshard_info_types.cpp
    schemeshard_info_types.h
    schemeshard_olap_types.cpp
    schemeshard_path_describer.cpp
    schemeshard_path_element.cpp
    schemeshard_path_element.h
    schemeshard_path.cpp
    schemeshard_path.h
    schemeshard_schema.h
    schemeshard_svp_migration.h
    schemeshard_svp_migration.cpp
    schemeshard_tx_infly.h
    schemeshard_tables_storage.cpp
    schemeshard_types.cpp
    schemeshard_types.h
    schemeshard_user_attr_limits.h
    schemeshard_utils.cpp
    schemeshard_utils.h
    schemeshard_export__cancel.cpp
    schemeshard_export__create.cpp
    schemeshard_export__forget.cpp
    schemeshard_export__get.cpp
    schemeshard_export__list.cpp
    schemeshard_export_flow_proposals.cpp
    schemeshard_export.cpp
    schemeshard_import__cancel.cpp
    schemeshard_import__create.cpp
    schemeshard_import__forget.cpp
    schemeshard_import__get.cpp
    schemeshard_import__list.cpp
    schemeshard_import_flow_proposals.cpp
    schemeshard_import.cpp
    schemeshard_build_index.cpp
    schemeshard_build_index_tx_base.cpp
    schemeshard_build_index__cancel.cpp
    schemeshard_build_index__forget.cpp
    schemeshard_build_index__list.cpp
    schemeshard_build_index__create.cpp
    schemeshard_build_index__get.cpp
    schemeshard_build_index__progress.cpp
    schemeshard_validate_ttl.cpp
    operation_queue_timer.h
    user_attributes.cpp
)

GENERATE_ENUM_SERIALIZATION(schemeshard_info_types.h)

GENERATE_ENUM_SERIALIZATION(schemeshard_types.h)

GENERATE_ENUM_SERIALIZATION(operation_queue_timer.h)

PEERDIR(
    contrib/libs/protobuf
    library/cpp/deprecated/enum_codegen
    library/cpp/html/pcdata
    library/cpp/json
    ydb/core/actorlib_impl
    ydb/core/audit
    ydb/core/base
    ydb/core/blob_depot
    ydb/core/blockstore/core
    ydb/core/engine
    ydb/core/engine/minikql
    ydb/core/external_sources
    ydb/core/filestore/core
    ydb/core/formats/arrow/compression
    ydb/core/kesus/tablet
    ydb/core/metering
    ydb/core/persqueue
    ydb/core/persqueue/config
    ydb/core/persqueue/events
    ydb/core/persqueue/writer
    ydb/core/protos
    ydb/core/scheme
    ydb/core/statistics
    ydb/core/sys_view/partition_stats
    ydb/core/tablet
    ydb/core/tablet_flat
    ydb/core/tx
    ydb/core/tx/datashard
    ydb/core/tx/scheme_board
    ydb/core/tx/tx_allocator_client
    ydb/core/util
    ydb/core/wrappers
    ydb/core/ydb_convert
    ydb/library/aclib
    ydb/library/aclib/protos
    ydb/library/login
    ydb/library/login/protos
    ydb/library/yql/minikql
    ydb/services/bg_tasks
)

YQL_LAST_ABI_VERSION()

IF (OS_WINDOWS)
    SRCS(
        schemeshard_import_scheme_getter_fallback.cpp
    )
ELSE()
    SRCS(
        schemeshard_import_scheme_getter.cpp
    )
ENDIF()

END()
