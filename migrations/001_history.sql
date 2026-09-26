-- Apply with psql -v ON_ERROR_STOP=1 -f migrations/001_history.sql.
-- Owner provisions schema; daemon needs only USAGE/SELECT/INSERT and EXECUTE.
BEGIN;
CREATE SCHEMA IF NOT EXISTS vigil;
CREATE TABLE IF NOT EXISTS vigil.schema_version (
    version integer PRIMARY KEY CHECK (version = 1)
);
INSERT INTO vigil.schema_version VALUES (1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS vigil.runs (
    run_id uuid PRIMARY KEY,
    started_at timestamptz NOT NULL,
    started_ms bigint NOT NULL
);
CREATE TABLE IF NOT EXISTS vigil.history (
    run_id uuid NOT NULL REFERENCES vigil.runs,
    seq bigint NOT NULL,
    observed_at timestamptz NOT NULL,
    monotonic_ms bigint NOT NULL,
    kind text NOT NULL CHECK (kind IN ('reading','heartbeat','rejected','alarm')),
    pod smallint NOT NULL,
    channel smallint NOT NULL,
    detail jsonb NOT NULL,
    PRIMARY KEY (run_id, seq)
);
CREATE INDEX IF NOT EXISTS history_source_time ON vigil.history(pod,channel,observed_at);
CREATE TABLE IF NOT EXISTS vigil.checkpoints (
    run_id uuid NOT NULL REFERENCES vigil.runs,
    batch_id bigint NOT NULL,
    observed_at timestamptz NOT NULL,
    monotonic_ms bigint NOT NULL,
    kind text NOT NULL CHECK (kind IN ('flush','recovery','gap')),
    reading_drops bigint NOT NULL CHECK (reading_drops >= 0),
    event_drops bigint NOT NULL CHECK (event_drops >= 0),
    detail jsonb NOT NULL,
    alarms jsonb, -- complete state as of observation; NULL means no new snapshot
    PRIMARY KEY (run_id,batch_id)
);
-- A retry after a lost COMMIT response must not duplicate history or gap counts.
-- All writes are atomic under this one statement's transaction.
CREATE OR REPLACE FUNCTION vigil.write_batch(b jsonb) RETURNS void
LANGUAGE plpgsql SET search_path = pg_catalog AS $$
DECLARE
    rid uuid := (b->>'run_id')::uuid;
    bid bigint := (b->>'batch_id')::bigint;
BEGIN
    IF EXISTS (SELECT 1 FROM vigil.checkpoints WHERE run_id=rid AND batch_id=bid) THEN
        RETURN;
    END IF;
    INSERT INTO vigil.runs VALUES (rid,
        to_timestamp((b->>'started_utc_ms')::double precision/1000),
        (b->>'started_ms')::bigint) ON CONFLICT DO NOTHING;
    INSERT INTO vigil.history
    SELECT rid, x.seq, to_timestamp(x.utc_ms::double precision/1000),
           x.mono_ms, x.kind, x.pod, x.channel, x.detail
    FROM jsonb_to_recordset(b->'records') AS x(seq bigint,utc_ms bigint,
        mono_ms bigint,kind text,pod smallint,channel smallint,detail jsonb)
    ON CONFLICT DO NOTHING;
    INSERT INTO vigil.checkpoints VALUES (rid,bid,
        to_timestamp((b->>'utc_ms')::double precision/1000),
        (b->>'mono_ms')::bigint,b->>'kind',
        (b->>'reading_drops')::bigint,(b->>'event_drops')::bigint,
        b->'health',NULLIF(b->'alarms','null'::jsonb));
END;
$$;
COMMIT;
