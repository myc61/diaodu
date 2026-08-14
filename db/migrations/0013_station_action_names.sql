BEGIN;

SET search_path TO dispatch, public;

ALTER TABLE station_actions
    ADD COLUMN IF NOT EXISTS action_name text NOT NULL DEFAULT '';

UPDATE station_actions
SET action_name = capability_key
WHERE btrim(action_name) = '';

COMMENT ON COLUMN station_actions.action_name IS
    'Engineer-defined display name for distinguishing point actions in workflow editing and monitoring.';

COMMIT;
