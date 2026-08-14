BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE workflow_runs
    DROP CONSTRAINT IF EXISTS workflow_runs_trigger_type_check;
ALTER TABLE workflow_runs
    ADD CONSTRAINT workflow_runs_trigger_type_check
    CHECK (
        trigger_type IN (
            'MANUAL',
            'CRON',
            'ROBOT_EVENT',
            'DEVICE_EVENT',
            'EVENT',
            'SUBFLOW'
        )
    );

ALTER TABLE workflow_runs
    ADD COLUMN IF NOT EXISTS parent_run_id uuid
        REFERENCES workflow_runs(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS root_run_id uuid
        REFERENCES workflow_runs(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS source_run_id uuid
        REFERENCES workflow_runs(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS parent_node_run_id uuid
        REFERENCES node_runs(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS causation_event_id uuid,
    ADD COLUMN IF NOT EXISTS business_key text NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS workflow_runs_root_idx
    ON workflow_runs (root_run_id, created_at);
CREATE INDEX IF NOT EXISTS workflow_runs_parent_idx
    ON workflow_runs (parent_run_id, created_at)
    WHERE parent_run_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS workflow_runs_business_key_idx
    ON workflow_runs (business_key, created_at)
    WHERE business_key <> '';

CREATE TABLE IF NOT EXISTS workflow_signals (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    event_name text NOT NULL,
    source_run_id uuid REFERENCES workflow_runs(id) ON DELETE SET NULL,
    source_node_run_id uuid REFERENCES node_runs(id) ON DELETE SET NULL,
    target_run_id uuid REFERENCES workflow_runs(id) ON DELETE SET NULL,
    target_workflow_definition_id uuid
        REFERENCES workflow_definitions(id) ON DELETE SET NULL,
    business_key text NOT NULL DEFAULT '',
    deduplication_key text,
    payload jsonb NOT NULL DEFAULT '{}'::jsonb,
    occurred_at timestamptz NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX IF NOT EXISTS workflow_signals_deduplication_key_uq
    ON workflow_signals (deduplication_key)
    WHERE deduplication_key IS NOT NULL;
CREATE INDEX IF NOT EXISTS workflow_signals_route_idx
    ON workflow_signals (event_name, target_run_id, business_key, occurred_at);

ALTER TABLE workflow_runs
    DROP CONSTRAINT IF EXISTS workflow_runs_causation_event_id_fkey;
ALTER TABLE workflow_runs
    ADD CONSTRAINT workflow_runs_causation_event_id_fkey
    FOREIGN KEY (causation_event_id)
    REFERENCES workflow_signals(id) ON DELETE SET NULL;

CREATE TABLE IF NOT EXISTS robot_startup_profiles (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    robot_id uuid NOT NULL REFERENCES robots(id) ON DELETE CASCADE,
    name text NOT NULL,
    description text NOT NULL DEFAULT '',
    enabled boolean NOT NULL DEFAULT true,
    ssh_port integer NOT NULL DEFAULT 22 CHECK (ssh_port BETWEEN 1 AND 65535),
    ssh_username text NOT NULL,
    credential_reference text NOT NULL,
    known_hosts_reference text NOT NULL,
    steps jsonb NOT NULL DEFAULT '[]'::jsonb,
    readiness_checks jsonb NOT NULL DEFAULT '[]'::jsonb,
    stop_steps jsonb NOT NULL DEFAULT '[]'::jsonb,
    timeout_ms integer NOT NULL DEFAULT 120000
        CHECK (timeout_ms BETWEEN 1000 AND 3600000),
    version integer NOT NULL DEFAULT 1 CHECK (version > 0),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (robot_id, name, version)
);

CREATE INDEX IF NOT EXISTS robot_startup_profiles_robot_idx
    ON robot_startup_profiles (robot_id, enabled, name);

COMMENT ON TABLE workflow_signals IS
    'Durable cross-run signal envelope used for precise wake-up and event starts';
COMMENT ON TABLE robot_startup_profiles IS
    'Controlled per-robot SSH startup plans; references secrets but never stores key material';

COMMIT;
