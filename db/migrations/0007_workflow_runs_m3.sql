BEGIN;

SET search_path TO dispatch, public;

-- Allow EVENT as an alias used by the dispatcher API (mapped from definition triggers).
ALTER TABLE workflow_runs DROP CONSTRAINT IF EXISTS workflow_runs_trigger_type_check;
ALTER TABLE workflow_runs
    ADD CONSTRAINT workflow_runs_trigger_type_check
    CHECK (
        trigger_type IN (
            'MANUAL',
            'CRON',
            'ROBOT_EVENT',
            'DEVICE_EVENT',
            'EVENT'
        )
    );

CREATE INDEX IF NOT EXISTS workflow_runs_state_created_idx
    ON workflow_runs (state, created_at DESC);

CREATE INDEX IF NOT EXISTS node_runs_waiting_event_idx
    ON node_runs (state)
    WHERE state = 'WAITING_EVENT';

COMMENT ON TABLE workflow_runs IS
    'M3 execution instances bound to an immutable workflow_versions row';

COMMIT;
