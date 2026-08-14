BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE workflow_runs
    ADD COLUMN IF NOT EXISTS archived_at timestamptz;

CREATE INDEX IF NOT EXISTS workflow_runs_archived_created_idx
    ON workflow_runs (archived_at, created_at DESC);

-- A navigation run may own outbox rows. History cleanup must remove the whole
-- run aggregate atomically instead of leaving orphaned commands.
ALTER TABLE command_outbox
    DROP CONSTRAINT IF EXISTS command_outbox_workflow_run_id_fkey;
ALTER TABLE command_outbox
    ADD CONSTRAINT command_outbox_workflow_run_id_fkey
    FOREIGN KEY (workflow_run_id) REFERENCES workflow_runs(id) ON DELETE CASCADE;
ALTER TABLE command_outbox
    DROP CONSTRAINT IF EXISTS command_outbox_node_run_id_fkey;
ALTER TABLE command_outbox
    ADD CONSTRAINT command_outbox_node_run_id_fkey
    FOREIGN KEY (node_run_id) REFERENCES node_runs(id) ON DELETE CASCADE;

COMMENT ON COLUMN workflow_runs.archived_at IS
    'Engineer-hidden workflow history; terminal archived runs may be purged';

COMMIT;
