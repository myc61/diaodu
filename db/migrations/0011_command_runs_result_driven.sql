BEGIN;

SET search_path TO dispatch, public;

CREATE TABLE IF NOT EXISTS command_runs (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    command_id uuid NOT NULL UNIQUE DEFAULT gen_random_uuid(),
    workflow_run_id uuid NOT NULL
        REFERENCES workflow_runs(id) ON DELETE CASCADE,
    node_run_id uuid NOT NULL
        REFERENCES node_runs(id) ON DELETE CASCADE,
    robot_id uuid NOT NULL REFERENCES robots(id),
    capability_definition_id uuid
        REFERENCES capability_definitions(id) ON DELETE SET NULL,
    operation_kind text NOT NULL,
    endpoint_name text NOT NULL,
    correlation_id text NOT NULL DEFAULT '',
    state text NOT NULL DEFAULT 'CREATED'
        CHECK (
            state IN (
                'CREATED',
                'DISPATCHING',
                'ACTIVE',
                'SUCCEEDED',
                'FAILED',
                'TIMED_OUT',
                'CANCELLED',
                'UNCERTAIN'
            )
        ),
    request_payload jsonb NOT NULL DEFAULT '{}'::jsonb,
    last_feedback jsonb,
    result_payload jsonb,
    error_data jsonb,
    dispatched_at timestamptz,
    completed_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS command_runs_workflow_idx
    ON command_runs (workflow_run_id, created_at);

CREATE INDEX IF NOT EXISTS command_runs_node_idx
    ON command_runs (node_run_id, created_at);

CREATE INDEX IF NOT EXISTS command_runs_active_idx
    ON command_runs (robot_id, state)
    WHERE state IN ('CREATED', 'DISPATCHING', 'ACTIVE', 'UNCERTAIN');

COMMENT ON TABLE command_runs IS
    'Durable correlation between workflow nodes and ROS service/action/navigation results';

COMMIT;
