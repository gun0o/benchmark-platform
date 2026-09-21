DROP INDEX IF EXISTS measurements_filter_idx;
CREATE INDEX IF NOT EXISTS measurements_filter_idx
    ON measurements (machine_id, metric, workload, thread_count, working_set_bytes);
