BEGIN;

SET search_path TO dispatch, public;

-- Bind workflows to a scene (map workspace) and declare how they start.
ALTER TABLE workflow_definitions
    ADD COLUMN IF NOT EXISTS scene_id uuid REFERENCES scenes(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS trigger_type text NOT NULL DEFAULT 'MANUAL',
    ADD COLUMN IF NOT EXISTS trigger_config jsonb NOT NULL DEFAULT '{}'::jsonb;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_constraint
        WHERE conname = 'workflow_definitions_trigger_type_check'
    ) THEN
        ALTER TABLE workflow_definitions
            ADD CONSTRAINT workflow_definitions_trigger_type_check
            CHECK (
                trigger_type IN (
                    'MANUAL',
                    'EVENT',
                    'CRON',
                    'MANUAL_OR_EVENT'
                )
            );
    END IF;
END $$;

CREATE INDEX IF NOT EXISTS workflow_definitions_scene_idx
    ON workflow_definitions (scene_id);

COMMENT ON COLUMN workflow_definitions.scene_id IS
    'Optional scene scope: editor loads robots/stations/actions from this scene';
COMMENT ON COLUMN workflow_definitions.trigger_config IS
    'e.g. {"event_name":"point_b_completed","cron":"0 */5 * * *"}';

COMMIT;
