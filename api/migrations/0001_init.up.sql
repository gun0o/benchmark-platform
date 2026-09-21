-- M1.3: machines, runs, measurements.
--
-- One row of `measurements` is one trial of one metric for one
-- (machine, workload, thread_count, working_set_bytes) configuration, i.e. exactly one
-- object from a run envelope's `results` array with the machine block flattened back in.

CREATE TABLE IF NOT EXISTS machines (
    id              text PRIMARY KEY,          -- 32 hex chars, see schema machine.id
    hostname        text        NOT NULL,
    cpu_model       text        NOT NULL,
    physical_cores  integer     NOT NULL,
    logical_cpus    integer     NOT NULL,
    l1d_kb          integer     NOT NULL,
    l2_kb           integer     NOT NULL,
    l3_kb           integer     NOT NULL,
    memory_bytes    bigint      NOT NULL,
    os              text        NOT NULL,
    kernel          text        NOT NULL,
    compiler        text        NOT NULL,
    compiler_flags  text        NOT NULL,
    virtualized     text        NOT NULL DEFAULT '',
    first_seen      timestamptz NOT NULL,
    last_seen       timestamptz NOT NULL
);

CREATE TABLE IF NOT EXISTS runs (
    id              uuid PRIMARY KEY,
    machine_id      text        NOT NULL REFERENCES machines(id),
    engine_version  text        NOT NULL,
    engine_git_sha  text        NOT NULL,
    started_at      timestamptz NOT NULL,
    finished_at     timestamptz NOT NULL,
    argv            jsonb       NOT NULL DEFAULT '[]'::jsonb,
    ingested_at     timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS measurements (
    id                 bigserial PRIMARY KEY,
    run_id             uuid             NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    machine_id         text             NOT NULL REFERENCES machines(id),
    workload           text             NOT NULL,
    metric             text             NOT NULL,
    thread_count       integer          NOT NULL,
    working_set_bytes  bigint           NOT NULL,
    trial              integer          NOT NULL,
    value              double precision NOT NULL,
    unit               text             NOT NULL,
    recorded_at        timestamptz      NOT NULL,
    duration_ns        bigint           NOT NULL,
    params             jsonb
);

-- The filter set every read endpoint uses, in the order it narrows fastest.
CREATE INDEX IF NOT EXISTS measurements_filter_idx
    ON measurements (machine_id, metric, workload, thread_count, working_set_bytes);
CREATE INDEX IF NOT EXISTS measurements_run_idx ON measurements (run_id);
CREATE INDEX IF NOT EXISTS runs_machine_idx ON runs (machine_id, started_at DESC);
