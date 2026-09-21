-- M5.1: the filter index has to end with `id`, or the list query cannot use it.
--
-- GET /v1/measurements is "filter on five columns, order by id, limit N" (keyset
-- pagination). The 0001 index (machine_id, metric, workload, thread_count,
-- working_set_bytes) can find the matching rows but not in id order, so the planner
-- preferred walking the primary key and filtering - which costs nothing while one
-- machine's rows are dense and degrades exactly when the table stops being dominated by
-- one machine. Appending `id` makes the index answer the filter, the cursor and the
-- ordering at once, and the row count it scans is then the page size.
DROP INDEX IF EXISTS measurements_filter_idx;
CREATE INDEX IF NOT EXISTS measurements_filter_idx
    ON measurements (machine_id, metric, workload, thread_count, working_set_bytes, id);
