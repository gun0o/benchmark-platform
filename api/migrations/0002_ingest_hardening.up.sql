-- M5.1: make ingest safe to repeat and safe to split.
--
-- A run larger than the ingest limit is chunked by the engine into several POSTs that
-- share one run_id, so "the run already exists" can no longer mean "there is nothing to
-- do". Idempotency moves down to the row: one measurement is one trial of one metric for
-- one configuration of one run, and that tuple is unique.
CREATE UNIQUE INDEX IF NOT EXISTS measurements_dedup_idx
    ON measurements (run_id, metric, thread_count, working_set_bytes, trial);
