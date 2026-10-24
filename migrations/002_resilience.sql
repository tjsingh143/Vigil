-- Forward-only, repeatable addition to 001. Apply as schema owner, never daemon.
BEGIN;
ALTER TABLE vigil.schema_version DROP CONSTRAINT IF EXISTS schema_version_version_check;
ALTER TABLE vigil.schema_version ADD CONSTRAINT schema_version_version_check CHECK(version IN (1,2));
CREATE TABLE IF NOT EXISTS vigil.run_origins (
    run_id uuid PRIMARY KEY REFERENCES vigil.runs,
    spool_id uuid NOT NULL,
    run_no bigint NOT NULL CHECK(run_no BETWEEN 1 AND 9223372036854775806),
    run_token bytea NOT NULL UNIQUE CHECK(octet_length(run_token)=32),
    boot_id uuid NOT NULL,
    started_utc_ms bigint NOT NULL CHECK(started_utc_ms BETWEEN 0 AND 253402300799999),
    started_ms bigint NOT NULL CHECK(started_ms BETWEEN 0 AND 9223372036854775806),
    UNIQUE(spool_id,run_no)
);
CREATE TABLE IF NOT EXISTS vigil.replay_receipts (
    run_id uuid NOT NULL,
    batch_id bigint NOT NULL CHECK(batch_id BETWEEN 1 AND 9223372036854775806),
    payload bytea NOT NULL CHECK(octet_length(payload) BETWEEN 1 AND 160000),
    record_count smallint NOT NULL CHECK(record_count BETWEEN 0 AND 32),
    PRIMARY KEY(run_id,batch_id),
    FOREIGN KEY(run_id,batch_id) REFERENCES vigil.checkpoints
);
-- Internal invoker validator. JSON is checked before conversion to JSONB so
-- duplicate keys cannot disappear. Error messages never include the payload.
CREATE OR REPLACE FUNCTION vigil.validate_replay(raw bytea) RETURNS jsonb
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE
    t text; b jsonb; o jsonb; x jsonb; k text; n numeric;
    first_seq bigint; i bigint := 0;
BEGIN
    IF octet_length(raw) NOT BETWEEN 1 AND 160000 THEN RAISE EXCEPTION 'invalid replay'; END IF;
    t := convert_from(raw,'UTF8');
    IF NOT (t IS JSON OBJECT WITH UNIQUE KEYS) THEN RAISE EXCEPTION 'invalid replay'; END IF;
    b := t::jsonb; o := b->'origin';
    IF jsonb_typeof(o) IS DISTINCT FROM 'object' OR b->>'format' IS DISTINCT FROM '2'
       OR jsonb_typeof(b->'records') IS DISTINCT FROM 'array'
       OR jsonb_array_length(b->'records')>32
       OR jsonb_typeof(b->'health') IS DISTINCT FROM 'object'
       OR jsonb_typeof(b->'local_loss') IS DISTINCT FROM 'object'
       OR jsonb_typeof(b->'kind') IS DISTINCT FROM 'string' OR b->>'kind' NOT IN ('flush','gap','recovery') OR NOT b ? 'kind'
       OR NOT b ? 'alarms' OR NOT (b->'alarms'='null'::jsonb OR
          (jsonb_typeof(b->'alarms')='array' AND jsonb_array_length(b->'alarms') BETWEEN 1 AND 193))
    THEN RAISE EXCEPTION 'invalid replay'; END IF;
    -- Require canonical UUID/token spellings, not PostgreSQL's permissive UUID parser.
    FOREACH k IN ARRAY ARRAY['run_id','spool_id','boot_id'] LOOP
        IF jsonb_typeof(o->k) IS DISTINCT FROM 'string' OR
           (o->>k) !~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'
        THEN RAISE EXCEPTION 'invalid origin'; END IF;
    END LOOP;
    IF jsonb_typeof(o->'run_token') IS DISTINCT FROM 'string' OR
       o->>'run_token' !~ '^[0-9a-f]{64}$' OR b->'run_id' IS DISTINCT FROM o->'run_id' OR
       b->'started_ms' IS DISTINCT FROM o->'started_ms' OR
       b->'started_utc_ms' IS DISTINCT FROM o->'started_utc_ms'
    THEN RAISE EXCEPTION 'invalid origin'; END IF;
    -- All numeric atoms use integer syntax and fit signed bigint. Detail values
    -- may be negative (temperature); named counters/times below may not.
    IF EXISTS (
        WITH RECURSIVE tree(v) AS (
            SELECT t::json UNION ALL
            SELECT c.v FROM tree CROSS JOIN LATERAL (
                SELECT value AS v FROM json_each(CASE WHEN json_typeof(tree.v)='object' THEN tree.v ELSE '{}'::json END)
                UNION ALL
                SELECT value FROM json_array_elements(CASE WHEN json_typeof(tree.v)='array' THEN tree.v ELSE '[]'::json END)
            ) c
        ) SELECT 1 FROM tree WHERE json_typeof(v)='number' AND
          (v::text !~ '^-?(0|[1-9][0-9]*)$' OR (v::text)::numeric NOT BETWEEN -9223372036854775808 AND 9223372036854775806)
    ) THEN RAISE EXCEPTION 'invalid integer'; END IF;
    FOREACH k IN ARRAY ARRAY['format','order_id','batch_id','started_ms','started_utc_ms','mono_ms','utc_ms','capture_ticket','revision','reading_drops','event_drops'] LOOP
        IF jsonb_typeof(b->k) IS DISTINCT FROM 'number' OR (b->>k)::numeric<0 THEN RAISE EXCEPTION 'invalid counter'; END IF;
    END LOOP;
    IF (b->>'order_id')::bigint<1 OR (b->>'batch_id')::bigint<1 OR
       (b->>'started_utc_ms')::bigint>253402300799999 OR (b->>'utc_ms')::bigint>253402300799999
    THEN RAISE EXCEPTION 'invalid range'; END IF;
    FOREACH k IN ARRAY ARRAY['run_no','started_ms','started_utc_ms'] LOOP
        IF jsonb_typeof(o->k) IS DISTINCT FROM 'number' OR (o->>k)::numeric<0 THEN RAISE EXCEPTION 'invalid origin'; END IF;
    END LOOP;
    IF (o->>'run_no')::bigint<1 THEN RAISE EXCEPTION 'invalid origin'; END IF;
    IF (SELECT count(*) FROM jsonb_object_keys(b->'health'))!=14 OR
       (SELECT count(*) FROM jsonb_object_keys(b->'local_loss'))!=4 THEN RAISE EXCEPTION 'invalid health'; END IF;
    FOREACH k IN ARRAY ARRAY['readings_queued','events_queued','readings_dropped','events_dropped','first_drop_ms','last_drop_ms','revision','outage_started_ms','outage_ms','outage_total_ms','outage_ended_ms','connection_failures','query_failures','reconnects'] LOOP
        IF jsonb_typeof(b->'health'->k) IS DISTINCT FROM 'number' OR (b->'health'->>k)::numeric<0 THEN RAISE EXCEPTION 'invalid health'; END IF;
    END LOOP;
    FOREACH k IN ARRAY ARRAY['reading_evicted','event_evicted','reading_rejected','event_rejected'] LOOP
        IF jsonb_typeof(b->'local_loss'->k) IS DISTINCT FROM 'number' OR (b->'local_loss'->>k)::numeric<0 THEN RAISE EXCEPTION 'invalid loss'; END IF;
    END LOOP;
    IF b->'health'->'revision' IS DISTINCT FROM b->'revision' OR
       (b->'local_loss'->>'reading_evicted')::numeric+(b->'local_loss'->>'reading_rejected')::numeric != (b->'health'->>'readings_dropped')::numeric OR
       (b->'local_loss'->>'event_evicted')::numeric+(b->'local_loss'->>'event_rejected')::numeric != (b->'health'->>'events_dropped')::numeric OR
       (b->>'reading_drops')::numeric>(b->'health'->>'readings_dropped')::numeric OR
       (b->>'event_drops')::numeric>(b->'health'->>'events_dropped')::numeric
    THEN RAISE EXCEPTION 'invalid loss'; END IF;
    FOR x IN SELECT value FROM (
        SELECT value,0 AS lane,ordinality FROM jsonb_array_elements(b->'records') WITH ORDINALITY
        UNION ALL SELECT value,1,ordinality FROM jsonb_array_elements(CASE WHEN b->'alarms'='null'::jsonb THEN '[]'::jsonb ELSE b->'alarms' END) WITH ORDINALITY
    ) entries ORDER BY lane,ordinality
    LOOP
        IF jsonb_typeof(x) IS DISTINCT FROM 'object' OR
           jsonb_typeof(x->'detail') IS DISTINCT FROM 'object' OR
           -- JSONB adds insignificant spaces; remove them by checking original
           -- detail text separately below instead of imposing a smaller wire limit.
           jsonb_typeof(x->'kind') IS DISTINCT FROM 'string' OR
           x->>'kind' NOT IN ('reading','heartbeat','rejected','alarm')
        THEN RAISE EXCEPTION 'invalid record'; END IF;
        FOREACH k IN ARRAY ARRAY['seq','utc_ms','mono_ms','pod','channel'] LOOP
            IF jsonb_typeof(x->k) IS DISTINCT FROM 'number' THEN RAISE EXCEPTION 'invalid record'; END IF;
        END LOOP;
        IF (x->>'utc_ms')::numeric NOT BETWEEN 0 AND 253402300799999 OR
           (x->>'mono_ms')::numeric<0 OR (x->>'pod')::numeric NOT BETWEEN 0 AND 127 OR
           (x->>'channel')::numeric NOT BETWEEN -3 AND 1 THEN RAISE EXCEPTION 'invalid record'; END IF;
        IF i<jsonb_array_length(b->'records') THEN
            IF i=0 THEN first_seq := (x->>'seq')::bigint; END IF;
            IF first_seq<1 OR (x->>'seq')::numeric != first_seq::numeric+i THEN RAISE EXCEPTION 'invalid sequence'; END IF;
        ELSIF x->>'seq'!='0' OR x->>'kind'!='alarm' THEN RAISE EXCEPTION 'invalid snapshot'; END IF;
        i := i+1;
    END LOOP;
    -- Preserve the original JSON spelling for the fixed detail byte bound.
    IF EXISTS (SELECT 1 FROM (
        SELECT value FROM json_array_elements(t::json->'records') UNION ALL
        SELECT value FROM json_array_elements(CASE WHEN json_typeof(t::json->'alarms')='array' THEN t::json->'alarms' ELSE '[]'::json END)
    ) e WHERE octet_length((value->'detail')::text)>=384)
    THEN RAISE EXCEPTION 'invalid detail'; END IF;
    RETURN b;
EXCEPTION WHEN data_exception OR raise_exception THEN
    RAISE EXCEPTION USING ERRCODE='VG002', MESSAGE='invalid replay payload';
END;
$$;
CREATE OR REPLACE FUNCTION vigil.replay_batch(raw bytea)
RETURNS TABLE(run_id uuid,batch_id bigint,record_count smallint,disposition text)
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE
    b jsonb := vigil.validate_replay(raw);
    o jsonb := b->'origin'; rid uuid := (b->>'run_id')::uuid;
    bid bigint := (b->>'batch_id')::bigint;
    cnt smallint := jsonb_array_length(b->'records');
    prior vigil.replay_receipts%ROWTYPE;
    fresh boolean;
BEGIN
    PERFORM pg_advisory_xact_lock(hashtextextended(rid::text,0));
    INSERT INTO vigil.runs VALUES(rid,to_timestamp((b->>'started_utc_ms')::double precision/1000),(b->>'started_ms')::bigint)
    ON CONFLICT DO NOTHING;
    fresh := FOUND;
    IF fresh THEN
        BEGIN
            INSERT INTO vigil.run_origins VALUES(rid,(o->>'spool_id')::uuid,(o->>'run_no')::bigint,
                decode(o->>'run_token','hex'),(o->>'boot_id')::uuid,(o->>'started_utc_ms')::bigint,(o->>'started_ms')::bigint);
        EXCEPTION WHEN unique_violation THEN
            RAISE EXCEPTION USING ERRCODE='VG001',MESSAGE='origin conflict';
        END;
    END IF;
    IF NOT EXISTS(SELECT 1 FROM vigil.runs r JOIN vigil.run_origins v USING(run_id)
        WHERE r.run_id=rid AND r.started_at=to_timestamp((b->>'started_utc_ms')::double precision/1000)
        AND r.started_ms=(b->>'started_ms')::bigint AND v.spool_id=(o->>'spool_id')::uuid
        AND v.run_no=(o->>'run_no')::bigint AND v.run_token=decode(o->>'run_token','hex')
        AND v.boot_id=(o->>'boot_id')::uuid AND v.started_ms=(o->>'started_ms')::bigint
        AND v.started_utc_ms=(o->>'started_utc_ms')::bigint)
    THEN RAISE EXCEPTION USING ERRCODE='VG001',MESSAGE='origin conflict'; END IF;
    SELECT * INTO prior FROM vigil.replay_receipts r WHERE r.run_id=rid AND r.batch_id=bid;
    IF FOUND THEN
        IF prior.payload IS DISTINCT FROM raw OR prior.record_count!=cnt THEN
            RAISE EXCEPTION USING ERRCODE='VG001',MESSAGE='receipt conflict';
        END IF;
        RETURN QUERY SELECT rid,bid,cnt,'equal'::text;
        RETURN;
    END IF;
    IF EXISTS(SELECT 1 FROM vigil.checkpoints c WHERE c.run_id=rid AND c.batch_id=bid) THEN
        RAISE EXCEPTION USING ERRCODE='VG001',MESSAGE='unverified checkpoint';
    END IF;
    -- Reuse the unchanged v1 projection and atomic history/checkpoint writer.
    -- It ignores event conflicts, so independently compare every projected column.
    PERFORM vigil.write_batch(b);
    IF EXISTS(SELECT 1 FROM jsonb_to_recordset(b->'records') AS x(seq bigint,utc_ms bigint,
        mono_ms bigint,kind text,pod smallint,channel smallint,detail jsonb)
        LEFT JOIN vigil.history h ON h.run_id=rid AND h.seq=x.seq
        WHERE h.run_id IS NULL OR ROW(h.observed_at,h.monotonic_ms,h.kind,h.pod,h.channel,h.detail)
        IS DISTINCT FROM ROW(to_timestamp(x.utc_ms::double precision/1000),x.mono_ms,x.kind,x.pod,x.channel,x.detail))
    THEN RAISE EXCEPTION USING ERRCODE='VG001',MESSAGE='history conflict'; END IF;
    INSERT INTO vigil.replay_receipts VALUES(rid,bid,raw,cnt);
    RETURN QUERY SELECT rid,bid,cnt,'inserted'::text;
END;
$$;
REVOKE EXECUTE ON FUNCTION vigil.validate_replay(bytea),vigil.replay_batch(bytea) FROM PUBLIC;
INSERT INTO vigil.schema_version VALUES(2) ON CONFLICT DO NOTHING;
COMMIT;
