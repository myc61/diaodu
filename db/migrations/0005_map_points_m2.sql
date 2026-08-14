BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE stations
    ADD COLUMN IF NOT EXISTS tags text[] NOT NULL DEFAULT '{}',
    ADD COLUMN IF NOT EXISTS notes text NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS status text NOT NULL DEFAULT 'ACTIVE';

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'stations_status_check'
    ) THEN
        ALTER TABLE stations
            ADD CONSTRAINT stations_status_check
            CHECK (status IN ('ACTIVE', 'ARCHIVED'));
    END IF;
END $$;

ALTER TABLE capability_definitions
    ADD COLUMN IF NOT EXISTS motion_ownership text NOT NULL DEFAULT 'DISPATCHER';

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'capability_definitions_motion_ownership_check'
    ) THEN
        ALTER TABLE capability_definitions
            ADD CONSTRAINT capability_definitions_motion_ownership_check
            CHECK (motion_ownership IN ('DISPATCHER', 'ROBOT_INTERNAL'));
    END IF;
END $$;

UPDATE capability_definitions
SET motion_ownership = 'DISPATCHER'
WHERE capability_key = 'navigation'
  AND motion_ownership IS DISTINCT FROM 'DISPATCHER';

CREATE TABLE IF NOT EXISTS station_actions (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    station_id uuid NOT NULL REFERENCES stations(id) ON DELETE CASCADE,
    sequence_no integer NOT NULL CHECK (sequence_no > 0),
    parallel_group integer,
    capability_definition_id uuid
        REFERENCES capability_definitions(id) ON DELETE SET NULL,
    capability_key text NOT NULL,
    robot_selector_type text NOT NULL DEFAULT 'FIXED'
        CHECK (robot_selector_type IN ('FIXED', 'GROUP', 'RUNTIME_VAR')),
    robot_id uuid REFERENCES robots(id) ON DELETE SET NULL,
    robot_group text NOT NULL DEFAULT '',
    runtime_variable text NOT NULL DEFAULT '',
    parameters jsonb NOT NULL DEFAULT '{}'::jsonb,
    precondition jsonb,
    failure_policy text NOT NULL DEFAULT 'FAIL'
        CHECK (failure_policy IN ('FAIL', 'CONTINUE', 'RETRY')),
    retry_count integer NOT NULL DEFAULT 0 CHECK (retry_count >= 0),
    timeout_ms integer NOT NULL DEFAULT 30000 CHECK (timeout_ms > 0),
    success_event_name text NOT NULL DEFAULT '',
    post_navigation_station_id uuid REFERENCES stations(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (station_id, sequence_no)
);

CREATE INDEX IF NOT EXISTS station_actions_station_idx
    ON station_actions (station_id, sequence_no);

-- Sample ROBOT_INTERNAL capability: composite task that moves internally.
INSERT INTO capability_definitions (
    profile_id,
    capability_key,
    operation_kind,
    endpoint_name,
    ros_message_type,
    parameter_schema,
    request_template,
    timeout_ms,
    blocking_type,
    resource_claims,
    protocol_config,
    motion_ownership
)
VALUES (
    '11111111-1111-1111-1111-111111111111',
    'pick_and_place',
    'SERVICE',
    '/zj_humanoid/task/pick_and_place',
    'std_srvs/Trigger',
    '{
        "type":"object",
        "properties":{
            "source_label":{"type":"string"},
            "target_label":{"type":"string"}
        }
    }'::jsonb,
    '{"source":"${source_label}","target":"${target_label}"}'::jsonb,
    180000,
    'HARD',
    '[{"name":"navigation","access":"EXCLUSIVE"},{"name":"arm","access":"EXCLUSIVE"}]'::jsonb,
    '{"adapter":"ROS1_SERVICE"}'::jsonb,
    'ROBOT_INTERNAL'
)
ON CONFLICT (profile_id, capability_key) DO UPDATE
SET motion_ownership = EXCLUDED.motion_ownership,
    blocking_type = EXCLUDED.blocking_type,
    resource_claims = EXCLUDED.resource_claims,
    updated_at = now();

COMMIT;
